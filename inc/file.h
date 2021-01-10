#pragma once

#include <extent.h>
#include <sleeplock.h>

// in-memory copy of an inode
struct inode
{
  uint dev;  // Device number
  uint inum; // Inode number
  int ref;   // Reference count
  int valid; // Flag for if node is valid
  struct sleeplock lock;

  short type; // copy of disk inode
  short devid;
  uint size;
  struct extent data;
};

// table mapping device ID (devid) to device functions
struct devsw
{
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

// Device ids
enum
{
  CONSOLE = 1,
};

// File info struct:
// ref: https://courses.cs.washington.edu/courses/cse451/21wi/sections/21wi_section_1.pdf page 7
struct finfo
{
  int ref_ct;
  struct inode *ip;
  int offset;
  int access_permi;
};

/**
 * open the existing file with the given path and access mode.
 * */
int file_open(char *path, int mode);

/**
 * close the file with given fd.
 * */
int file_close(int fd);
