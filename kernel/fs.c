// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  initsleeplock(&sb.lock, "sb lock");
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 1 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iget() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. irelease() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.



struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.valid = 1;
  icache.inodefile.ref = 1;

  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  icache.inodefile.data = di.data;

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);

  init_inodefile(dev);
}


// Reads the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
static void read_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));
  dip->max_size = dip->data.nblocks * BSIZE;

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->valid = 0;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void locki(struct inode *ip) {
  struct dinode dip;

  if(ip == 0 || ip->ref < 1)
    panic("locki");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {

    if (ip != &icache.inodefile)
      locki(&icache.inodefile);
    read_dinode(ip->inum, &dip);
    if (ip != &icache.inodefile)
      unlocki(&icache.inodefile);

    ip->type = dip.type;
    ip->devid = dip.devid;

    ip->size = dip.size;
    ip->max_size = dip.max_size;
    ip->data = dip.data;

    ip->valid = 1;

    if (ip->type == 0)
      panic("iget: no type");
  }
}

// Unlock the given inode.
void unlocki(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("unlocki");

  releasesleep(&ip->lock);
}

// threadsafe stati.
void concurrent_stati(struct inode *ip, struct stat *st) {
  locki(ip);
  stati(ip, st);
  unlocki(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

// threadsafe readi.
int concurrent_readi(struct inode *ip, char *dst, uint off, uint n) {
  int retval;

  locki(ip);
  retval = readi(ip, dst, off, n);
  unlocki(ip);

  return retval;
}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    bp = bread(ip->dev, ip->data.startblkno + off / BSIZE);
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(dst, bp->data + off % BSIZE, m);
    brelse(bp);
  }
  return n;
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

// Write data to inode.
// Returns number of bytes written.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }
  // read-only fs, writing to inode is an error
  uint new_size = off + n;
  if (new_size > ip->max_size) {
    panic("Exceeding Max Size");
  }
  uint offset = off;
  uint written_sofar = 0;
  uint curr_blk_index = off / BSIZE;
  for (uint written = 0; written < (n + offset + BSIZE - 1) / BSIZE; written++) {
    // 1. find how much that I need to write
    uint to_write_size = n % (BSIZE - off);
    // 2. load the block
    // we need a function that translate index of block to blk_num
    uint blk_num = index_to_blknum(ip, curr_blk_index);  // smth we need to implement
    struct buf* buffer = bget(ip->devid, blk_num);
    memmove((buffer->data + off % BSIZE), src, to_write_size);
    if (log_write(ip, buffer)) {
      // write to log and then to disk
      // need to tell if the write was successful
      written_sofar += to_write_size;
    } else {
      // fail to write
      return written_sofar;
    }
    off = 0;
    curr_blk_index++;
  }
  return written_sofar;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }
  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(namei("/"));

  while ((path = skipelem(path, name)) != 0) {
    locki(ip);
    if (ip->type != T_DIR) {
      unlocki(ip);
      goto notfound;
    }

    // Stop one level early.
    if (nameiparent && *path == '\0') {
      unlocki(ip);
      return ip;
    }

    if ((next = dirlookup(ip, name, 0)) == 0) {
      unlocki(ip);
      goto notfound;
    }

    unlocki(ip);
    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}

// create file, return 0 on success, -1 on error i.e. disk is full
int file_create(char* path) {
  if (namei(path) != 0) {
    return 0;
  }
  struct dinode dip;
  uint inum;
  int written;
  locki(&icache.inodefile);
  // find first free dinode struct
  for (inum = 2; inum < icache.inodefile.size / sizeof(dip); inum++) {
    read_dinode(inum, &dip);
    if (dip.type == 0) {
      break;
    }
  }
  // offset we need to write to
  uint offset = INODEOFF(inum);

  if (offset >= icache.inodefile.size) {  // we will need to extent the file
    // 1. write to the file and update bitmap as well
    dip.size = 0;
    dip.type = 0;
    written = writei(&icache.inodefile, (char*) &dip, offset, sizeof(dip));
    if (written != sizeof(dip)) {
      unlocki(&icache.inodefile);
      return -1;
    }
  }
  // find the first free extent region for us to write on the given device
  int free_extent_num = find_free_extent_block(dip.devid);
  if (free_extent_num == -1) {
    // no more free space for file
    // return err
    unlocki(&icache.inodefile);
    return -1;
  }
  dip.size = 0;
  dip.type = T_FILE;

  // subject to change after we finalize how to extent
  dip.data.nblocks = DEFAULTBLK;
  dip.data.startblkno = (uint) free_extent_num;
  dip.max_size = DEFAULTBLK * BSIZE;

  // update file_inode
  written = writei(&icache.inodefile, (char*) &dip, offset, sizeof(dip));
  if (written != sizeof(dip)) {
      unlocki(&icache.inodefile);
      return -1;
  }
  // update bitmap
  for (int i = 0; i < DEFAULTBLK; i++) {
    update_bit_map(dip.devid, (uint) free_extent_num, 1);
  }
  // connect to directory
  // find inode of root dir
  struct inode* dir = iget(dip.devid, ROOTINO);
  // calculate offset
  offset = inum * sizeof(struct dirent);
  // populate dirent struct and write of disk
  struct dirent new_file;
  new_file.inum = inum;
  for (int i = 0; i < DIRSIZ; i++) {
    new_file.name[i] = path[i];
    if (path[i] == '\0') {
      break;
    }
  }
  written = concurrent_writei(&dir, (char*) &new_file, offset, sizeof(struct dirent));
  if (written != sizeof(dip)) {
      unlocki(&icache.inodefile);
      return -1;
  }
  unlocki(&icache.inodefile);
  return 0;
}

// using bitmap to find the first 
int find_free_extent_block(uint dev) {
  int start = sb.inodestart;
  for (uint curr = BBLOCK(sb.inodestart, sb); curr < sb.inodestart; curr++) {
    struct buf* content = bread(dev, curr);
    for (int i = 0; i < BSIZE; i++) {  // check 8 blks by 8 blks
      uchar byte = content->data[i];
      for (int j = 0; j < 8; i++) {  // check blk by blk
        if (byte & 1 == 0) {  // if it is 0, i.e. free
          int count = 1;
          uchar b = byte;
          for (int k = j; k < 8; k++) {
            if (b & 1 == 0) {
              count++;
            }
            b = b >> 1;
          }
          if (content->data[i + 1] == content->data[i + 2] && content->data[i + 1] == 0) {
            count += 16;
          }
          b = content->data[i + 3];
          for (int k = j; k > 0; k--) {
            if (b & 1 == 0) {
              count++;
            }
            b = b >> 1;
          }
          if (count == DEFAULTBLK) {
            return start + j + i * 8;
          }
        }
        byte = byte >> 1;
      }
    }
    start += BPB;
  }
  return -1;
}

// update blk_num in bitmap to be used if status = 1
// to be free if status = 0
void update_bit_map(uint dev, uint blk_num, uint status) {
  // 1.1 find the block that 'blk_num' is in
  uint bitblk = blk_num / (BSIZE * 8);
  blk_num = blk_num % (BSIZE * 8);
  // 1.2 find the offset
  uint offset = blk_num / 8;
  blk_num = blk_num % 8;
  // 1.3 find the bit in the byte
  uint bit_num = blk_num;

  // 2. load correct block from disk
  struct buf* content = bread(dev, bitblk + sb.bmapstart);
  // 3. update the bit  (can change to if/else, based on implementation of other functions)
  content->data[offset] = content->data[offset] ^ (1 << (bit_num));
  // 4. write to disk
  bwrite(content);
  brelse(content);
}

