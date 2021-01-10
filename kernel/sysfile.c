//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

int sys_dup(void)
{
  // LAB1
  return -1;
}

int sys_read(void)
{
  // LAB1
  return -1;
}

int sys_write(void)
{
  // you have to change the code in this function.
  // Currently it supports printing one character to the screen.

  int n;
  char *p;

  if (argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  uartputc((int)(*p));
  return 1;
}

/*
 * arg0: int [file descriptor]
 *
 * closes the passed in file descriptor
 * returns 0 on successful close, -1 otherwise
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 */
int sys_close(void)
{
  // LAB1
  return -1;
}

int sys_fstat(void)
{
  // LAB1
  return -1;
}

/*
 * arg0: char * [path to the file]
 * arg1: int [mode for opening the file (see inc/fcntl.h)]
 *
 * Given a pathname for a file, sys_open() returns a file descriptor, a small,
 * nonnegative integer for use in subsequent system calls. The file descriptor
 * returned by a successful call will be the lowest-numbered file descriptor
 * not currently open for the process.
 *
 * Each open file maintains a current position, initially zero.
 *
 * returns -1 on error
 *
 * Errors:
 * arg0 points to an invalid or unmapped address
 * there is an invalid address before the end of the string
 * the file does not exist
 * since the file system is read only, flag O_CREATE is not permitted
 * there is no available file descriptor
 *
 * note that for lab1, the file system does not support file create
 */
int sys_open(void)
{
  // LAB1
  char *path; // arg0: path to the file
  int mode;   // arg1: mode for opening the file

  // extract args
  if (argstr(0, &path) < 0 || argint(1, &mode) < 0)
  {
    return -1;
  }

  // make sure the mode is read only
  if (mode == O_CREATE || mode == O_RDWR || mode == O_WRONLY)
  {
    return -1;
  }

  return file_open(path, mode); // implemented in file.c
}

int sys_exec(void)
{
  // LAB2
  return -1;
}

int sys_pipe(void)
{
  // LAB2
  return -1;
}
