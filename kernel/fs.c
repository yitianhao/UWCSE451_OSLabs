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
  brelse(bp);
}

static int find_free_extent_block(uint dev);
static void update_bit_map(uint dev, uint blk_num, uint status);

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

static int find_free_extent_block(uint dev);
static void update_bit_map(uint dev, uint blk_num, uint status);
static uint find_free_lognode();
static void log_write(struct buf* bp);
static void copy_to_disk();
static void log_commit();
static void log_check();

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
  log_check();
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

// Update dinode
static void write_dinode(uint inum, struct dinode* dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  writei(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

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
  uint tot, m;
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }
  // read-only fs, writing to inode is an error
  uint new_size = max((off + n), ip->size);
  if (new_size > ip->data.nblocks * BSIZE) {
    panic("Exceeding Max Size");
  }
  // for (uint written = 0; written < (n + offset + BSIZE - 1) / BSIZE; written++) {
  //   // 1. find how much that I need to write
  //   uint to_write_size = n % (BSIZE - off);
  //   // 2. load the block
  //   // we need a function that translate index of block to blk_num
  //   uint blk_num = curr_blk_index + ip->data.startblkno;  // smth we need to implement
  //   struct buf* buffer = bread(ip->devid, blk_num);
  //   memmove((buffer->data + off % BSIZE), src, to_write_size);
  //   // if (log_write(ip, buffer)) {
  //   //   // write to log and then to disk
  //   //   // need to tell if the write was successful
  //   //   written_sofar += to_write_size;
  //   // } else {
  //   //   // fail to write
  //   //   return written_sofar;
  //   // }
  //   bwrite(buffer);
  //   brelse(buffer);
  //   off = 0;
  //   n -= to_write_size;
  //   curr_blk_index++;
  //   written_sofar += to_write_size;
  // }
  for (tot = 0; tot < n; tot += m, off += m, src += m) {
    struct buf* bp = bread(ip->dev, ip->data.startblkno + off / BSIZE);
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(bp->data + off % BSIZE, src, m);
    //bwrite(bp);
    log_write(bp);

    brelse(bp);
    if (ip->inum > INODEFILEINO) {
      struct dinode di;
      read_dinode(ip->inum, &di);
      di.size = max(n + off, ip->size);
      write_dinode(ip->inum, &di);
      log_commit();
      copy_to_disk();
    }
  }


  // update inodefile
  // if (ip->inum != icache.inodefile.inum) {
  //   struct dinode di;
  //   read_dinode(ip->inum, &di);
  //   di.size = new_size;
  //   write_dinode(ip->inum, &di);
  //   ip->valid = 0;
  // }
  ip->valid = 0;
  return n;
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
  // 1. find first free dinode struct
  for (inum = 2; inum < icache.inodefile.size / sizeof(struct dinode); inum++) {
    read_dinode(inum, &dip);
    if (dip.type == 0) {
      break;
    }
  }

  //cprintf("inum to create = %d\n", inum);

  // 2. if we are writing/expanding inodefile
  if (inum >= icache.inodefile.size / sizeof(struct dinode)) {
    struct dinode inodefile;
    read_dinode(INODEFILEINO, &inodefile);
    inodefile.size += sizeof(struct dinode);
    write_dinode(INODEFILEINO, &inodefile);
  }

  // 3. find the first free extent region for us to write on the given device
  int free_extent_num = find_free_extent_block(ROOTDEV);
  //cprintf("the startblkno: %d\n", free_extent_num);
  if (free_extent_num == -1) {
    // no more free space for file
    // return err
    unlocki(&icache.inodefile);
    //cprintf("not enough space for extent\n");
    return -1;
  }
  // 4. update bitmap for DEFAULTBLK number of extends
  for (int i = 0; i < DEFAULTBLK; i++) {
    update_bit_map(ROOTDEV, (uint) (free_extent_num + i), 1);
  }

  // 5. set up the new dinode and update the meta-data of dinode on disk
  dip.size = 0;
  dip.type = T_FILE;
  dip.data.nblocks = DEFAULTBLK;
  dip.data.startblkno = (uint) free_extent_num;
  dip.max_size = DEFAULTBLK * BSIZE;
  dip.devid = ROOTDEV;
  // update file_inode
  write_dinode(inum, &dip);

  // 6. connect to directory
  struct inode* dir = iget(ROOTDEV, ROOTINO);
  // calculate offset
  uint offset = inum * sizeof(struct dirent);
  // populate dirent struct and write of disk
  struct dirent new_file;
  new_file.inum = inum;
  char name[DIRSIZ];
  if (dir != nameiparent(path, name)) {
    unlocki(&icache.inodefile);
    panic("different dir");
  }
  strncpy(new_file.name, name, DIRSIZ);
  written = concurrent_writei(dir, (char*) &new_file, offset, sizeof(struct dirent));
  if (written != sizeof(struct dirent)) {
    unlocki(&icache.inodefile);
    //cprintf("written to dir file failed\n");
    return -1;
  }


  // if (inum >= dir->size / sizeof(struct dirent)) {
  //   struct dinode ddir;
  //   read_dinode(ROOTINO, &ddir);
  //   ddir.size += sizeof(struct dirent);
  //   write_dinode(ROOTINO, &ddir);
  // }

  log_commit();
  copy_to_disk();
  unlocki(&icache.inodefile);
  return 0;
}

static int is_free(struct buf* content, uint blk_num) {
  uint index = (blk_num % BPB) / 8;
  uint bit = blk_num % 8;
  return (content->data[index] & (1 << bit)) == 0;
}

// using bitmap to find the first
// dev = device id (usually ROOTDEV)
static int find_free_extent_block(uint dev) {
  for (uint i = sb.inodestart; i < sb.nblocks; i++) { // find blk by blk
    uint curr_block = BBLOCK(i, sb);  // current block in bitmap
    struct buf* content = bread(ROOTDEV, curr_block);
    if (is_free(content, i)) {
      uint all_free = 1;
      for (int j = 0; j < DEFAULTBLK && i + j < BSIZE; j++) {
        if (!is_free(content, i + j)) {
          all_free = 0;
          break;
        }
      }
      if (all_free) {
        brelse(content);
        return i;
      }
    }
    brelse(content);
  }
  return -1;
}

// update blk_num in bitmap to be used if status = 1
// to be free if status = 0
// dev = devid (use ROOTDEV)
// blk_num = block number that has its state changed
// the algorithm is symmetric to find_free_extent_block and is_free
static void update_bit_map(uint dev, uint blk_num, uint status) {
  // 1.1 find the block(in the bitmap) that 'blk_num' is in
  uint bitblk = BBLOCK(blk_num, sb);
  // 1.2 find the offset
  uint offset = (blk_num % BPB) / 8;
  // 1.3 find the bit in the byte
  uint bit_num = (blk_num) % 8;

  // 2. load correct block from disk
  struct buf* content = bread(dev, bitblk);
  // 3. update the bit  (can change to if/else, based on implementation of other functions)
  if (status) {
    if (content -> data[offset] & (1 << bit_num)) {
      panic("Already occupied");
    }
    content->data[offset] = content->data[offset] | (1 << bit_num);
  } else {
    if (!(content -> data[offset] & (1 << bit_num))) {
      panic("Already free-ed");
    }
    content->data[offset] = content->data[offset] & ~(1 << bit_num);
  }
  // 4. write to disk
  // bwrite(content);
  log_write(content);
  // log_commit();
  // copy_to_disk();
  brelse(content);
}

int file_delete(char* path) {
  struct inode* ip;
  struct dinode dip;
  // file-to-delete not found
  if ((ip = namei(path)) == 0) {
    return -1;
  }
  locki(&icache.inodefile);
  read_dinode(ip->inum, &dip);

  irelease(ip);
  if (ip->ref > 0) {
    unlocki(&icache.inodefile);
    return -1;
  }
  // file-to-delete is directory
  if (dip.type != T_FILE) {
    unlocki(&icache.inodefile);
    return -1;
  }

  //cprintf("inum to delete = %d\n", ip->inum);

  // // 2. update the size of inodefile
  if (ip->inum == icache.inodefile.size / sizeof(struct dinode)) {
    struct dinode inodefile;
    read_dinode(INODEFILEINO, &inodefile);
    inodefile.size -= sizeof(struct dinode);
    write_dinode(INODEFILEINO, &inodefile);
  }

  // 4. update bitmap to free the DEFAULTBLK number of extent blocks
  for (int i = 0; i < DEFAULTBLK; i++) {
    update_bit_map(ROOTDEV, (uint) (dip.data.startblkno + i), 0);
  }

  // 5. unlink from root directory
  struct inode* dir = iget(ROOTDEV, ROOTINO);
  // calculate offset
  uint offset = ip->inum * sizeof(struct dirent);
  // populate dirent struct and write of disk
  struct dirent file;
  memset(&file, 0, sizeof(struct dirent));
  uint written = concurrent_writei(dir, (char*) &file, offset, sizeof(struct dirent));
  if (written != sizeof(struct dirent)) {
    unlocki(&icache.inodefile);
    return -1;
  }

  // 1. release inum in inodefile
  memset(&dip, 0, sizeof(struct dinode));
  write_dinode(ip->inum, &dip);

  log_commit();
  copy_to_disk();
  unlocki(&icache.inodefile);
  return 0;
}

// ----------------------------log section--------------------------------------

//
static uint find_free_lognode() {
  // struct buf* log_meta_data = bread(ROOTDEV, sb.logstart);
  // uint off = sb.logstart;
  // for (; off < sb.logstart + BSIZE; off += sizeof(struct lognode)) {
  //   if (((log_meta_data->data[off] & (1 << 0)) != 0) == 0) {
  //     brelse(log_meta_data);
  //     return off;
  //   }
  // }
  // brelse(log_meta_data);
  // return -1;

  // 1. load the entire region
  struct buf* buffer;
  struct lognode nodes[LOG_SIZE];
  buffer = bread(ROOTDEV, sb.logstart);
  memmove(nodes, buffer->data, BSIZE);
  // 2. check all log structs
  for (int i = 0; i < LOG_SIZE; i++) {
    if (nodes[i].dirty_flag == 0) {
      brelse(buffer);
      return sb.logstart + i + 1;
    }
  }

  brelse(buffer);

  return -1;
}

static void log_write(struct buf* bp) {
  struct lognode node;
  if ((node.data = find_free_lognode()) == -1) {
    panic("not enough block in log\n");
  }
  cprintf("starting blkno = ");
  node.blk_write = bp->blockno;
  node.dirty_flag = 1;
  node.commit_flag = 0;

  // write the content of bp to log
  struct buf* log_data = bread(ROOTDEV, node.data);
  memmove(log_data->data, bp->data, BSIZE);
  bwrite(log_data);
  brelse(log_data);
  //bp->flags |= B_DIRTY;

  // write log meta-data
  struct buf* log_meta_data = bread(ROOTDEV, sb.logstart);
  uint off = (node.data - sb.logstart - 1) * sizeof(struct lognode);
  memmove(log_meta_data->data + off, &node, sizeof(struct lognode));
  bwrite(log_meta_data);
  brelse(log_meta_data);

}

static void log_commit() {
  // 1. load the entire region
  struct buf* buffer;
  struct lognode nodes[LOG_SIZE];
  buffer = bread(ROOTDEV, sb.logstart);
  memmove(nodes, buffer->data, BSIZE);
  // 2. check all log structs
  for (int i = 0; i < LOG_SIZE; i++) {
    nodes[i].commit_flag = 1;
  }
  memmove(buffer->data, nodes, BSIZE);
  bwrite(buffer);
  brelse(buffer);
}

// node: a ptr to a loaded node struct
// index: index of the struct in log meta data region
static void copy_to_disk() {

  // 1. load the entire region
  struct buf* buffer;
  struct lognode nodes[LOG_SIZE];
  buffer = bread(ROOTDEV, sb.logstart);
  memmove(nodes, buffer->data, BSIZE);
  // 2. check all log structs
  for (int i = 0; i < LOG_SIZE; i++) {
    // 1. copy data
    if (!nodes[i].commit_flag) {
      brelse(buffer);
      panic("Not commited");
      return;
    }

    if (nodes[i].dirty_flag == 0) continue;

    // 1.1 get data
    struct buf* b = bread(ROOTDEV, nodes[i].data);
    // 1.2 write to extent block
    b->blockno = nodes[i].blk_write;
    bwrite(b);
    brelse(b);

    nodes[i].commit_flag = 0;
    nodes[i].dirty_flag = 0;
  }
  memmove(buffer->data, nodes, BSIZE);
  bwrite(buffer);
  brelse(buffer);


  // 3. set commit flag
  // log_commit(0);
  // node->commit_flag = 0;
  // struct buf* log_meta_data = bread(ROOTDEV, sb.logstart);
  // uint off = (node->data - sb.logstart - 1) * sizeof(struct lognode);
  // memmove(log_meta_data->data + off, node, sizeof(struct lognode));
  // bwrite(log_meta_data);
  // brelse(log_meta_data);
}

// check the entire log region when system booted
static void log_check() {
  // 1. load the entire region
  struct buf* buffer;
  struct lognode nodes[LOG_SIZE];
  buffer = bread(ROOTDEV, sb.logstart);
  memmove(nodes, buffer->data, BSIZE);
  // 2. check all log structs
  for (int i = 0; i < LOG_SIZE; i++) {
    if (nodes[i].commit_flag == 0) {
      brelse(buffer);
      return;
    }
    if (nodes[i].commit_flag == 1 && nodes[i].dirty_flag == 1) {
      copy_to_disk(&(nodes[i]));
    }
  }
  brelse(buffer);
}
