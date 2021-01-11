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
  uint ref_ct; // ref_ct cannot be negative
  struct inode *ip;
  uint offset;  // offset cannot be negative
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

/**
 * duplicate the file with given fd.
 * */
int file_dup(int fd);

/**
 * Read n bytes from the file with fd.
 * Store data read at the buffer dst
 * Param:
 * fd (int) : file descriptor
 * dst(char*): place to store all data read
 * n (uint) : number of bytes to be read
 * 
 * Return:
 *  - the number of bytes read.
 *  - -1 if there is any error
 * 
 * Effect:
 *  - offest of the file will be updated
 * */
int file_read(int fd, char* dst, uint n);

/**
 * Write n bytes from src to the file with fd.
 * Param:
 * fd (int) : file descriptor
 * src (char*) : data to be written
 * n (uint) : number of bytes to be written
 * 
 * Return:
 *  - the number of bytes written
 *  - -1 if there is any error
 * 
 * Effect:
 *  - offset of the file fd will be updated
 * */
int file_write(int fd, char* src, uint n);

/**
 * Populate the struct st with info associates to fd
 * Param:
 * fd (int): file descriptor
 * st (struct stat*): file stat struct to be populated
 * 
 * Return:
 * - 0 if success
 * - -1 if there is an error
 * 
 * Effect:
 *  - st will be populated
 * */
int file_stat(int fd, struct stat *st);