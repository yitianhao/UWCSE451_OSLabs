#pragma once

#include "extent.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define INODEFILEINO 0 // inode file inum
#define ROOTINO 1      // root i-number
#define BSIZE 512      // block size
#define DEFAULTBLK 24

// Disk layout:
// [ boot block | super block | free bit map |
//                                          inode file | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;       // Size of file system image (blocks)
  uint nblocks;    // Number of data blocks
  uint logstart;
  uint bmapstart;  // Block number of first free map block
  uint inodestart; // Block number of the start of inode file
};

// On-disk inode structure
struct dinode {
  short type;         // File type
  short devid;        // Device number (T_DEV only)
  uint size;          // Size of file (bytes)
  uint max_size;
  struct extent data; // Data blocks of file on disk
  char pad[42];       // So disk inodes fit contiguosly in a block
};

// log meta data struct
struct lognode {
  uchar commit_flag;  // ready to start copying?
  uchar dirty_flag;   // finished writing to log?
  uint  data;         // associated data block


  // dinode meta data for us to update
  uint inum;
  uint offset;        // offset that we should update from
  uint blk_write;     // blk that the data we need to copy to

  uint new_size;      // new size
  char pad[42];
};

// offset of inode in inodefile
#define INODEOFF(inum) ((inum) * sizeof(struct dinode))

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
