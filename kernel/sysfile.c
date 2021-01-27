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

/*
 * arg0: int [file descriptor]
 *
 * duplicate the file descriptor arg0, must use the smallest unused file descriptor
 * returns a new file descriptor of the duplicated file, -1 otherwise
 *
 * dup is generally used by the shell to configure stdin/stdout between
 * two programs connected by a pipe (lab 2).  For example, "ls | more"
 * creates two programs, ls and more, where the stdout of ls is sent
 * as the stdin of more.  The parent (shell) first creates a pipe
 * creating two new open file descriptors, and then create the two children.
 * Child processes inherit file descriptors, so each child process can
 * use dup to install each end of the pipe as stdin or stdout, and then
 * close the pipe.
 *
 * Error conditions:
 * arg0 is not an open file descriptor
 * there is no available file descriptor
 */
int sys_dup(void)
{
  // LAB1
  int fd;

  // extract arg
  if (argint(0, &fd) < 0 || fd < 0 || fd >= NOFILE)
  {
    return -1;
  }

  return file_dup(fd);
}

/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer to write read bytes to]
 * arg2: int [number of bytes to read]
 *
 * reads up to arg2 bytes from the current position of the file descriptor
 * arg0 and places those bytes into arg1. The current position of the
 * file descriptor is updated by that number of bytes.
 *
 * returns number of bytes read, or -1 if there was an error.
 *
 * If there are insufficient available bytes to complete the request,
 * reads as many as possible before returning with that number of bytes.
 *
 * Fewer than arg2 bytes can be read in various conditions:
 * If the current position + arg2 is beyond the end of the file.
 * If this is a pipe or console device and fewer than arg2 bytes are available
 * If this is a pipe and the other end of the pipe has been closed.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for read
 * some address between [arg1,arg1+arg2-1] is invalid
 * arg2 is not positive
 */
int sys_read(void)
{
  int fd;
  int n;
  char *buff;
  if (argint(0, &fd) < 0 || fd < 0 || fd >= NOFILE || argint(2, &n) < 0 || n < 0 || argptr(1, &buff, n) < 0)
  {
    return -1;
  }
  return file_read(fd, buff, (uint)n);
}

/*
 * arg0: int [file descriptor]
 * arg1: char * [buffer of bytes to write to the given fd]
 * arg2: int [number of bytes to write]
 *
 * writes up to arg2 bytes from arg1 to the current position of
 * the file descriptor. The current position of the file descriptor
 * is updated by that number of bytes.
 *
 * returns number of bytes written, or -1 if there was an error.
 *
 * If the full write cannot be completed, writes as many as possible
 * before returning with that number of bytes. For example, if the disk
 * runs out of space.
 *
 * If writing to a pipe and the other end of the pipe is closed,
 * will return 0 rather than an error.
 *
 * Error conditions:
 * arg0 is not a file descriptor open for write
 * some address between [arg1,arg1+arg2-1] is invalid
 * arg2 is not positive
 *
 * note that for lab1, the file system does not support writing past
 * the end of the file. Normally this would extend the size of the file
 * allowing the write to complete, to the maximum extent possible
 * provided there is space on the disk.
 */

int sys_write(void)
{
  int fd;
  int n;
  char *src;

  if (argint(0, &fd) < 0 || fd < 0 || fd >= NOFILE || argint(2, &n) < 0 || n < 0 || argptr(1, &src, n) < 0)
  {
    return -1;
  }
  return file_write(fd, src, (uint)n);
  // you have to change the code in this function.
  // Currently it supports printing one character to the screen.

  // int n;
  // char *p;

  // if (argint(2, &n) < 0 || argptr(1, &p, n) < 0)
  //   return -1;
  // uartputc((int)(*p));
  // return 1;
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
  int fd;

  // extract arg
  if (argint(0, &fd) < 0 || fd < 0 || fd >= NOFILE)
  {
    return -1;
  }

  return file_close(fd);
}

/*
 * arg0: int [file descriptor]
 * arg1: struct stat *
 *
 * populates the struct stat pointer passed in to the function
 *
 * returns 0 on success, -1 otherwise
 * Error conditions:
 * if arg0 is not a valid file descriptor
 * if any address within the range [arg1, arg1+sizeof(struct stat)] is invalid
 */

int sys_fstat(void)
{
  int fd;
  struct stat *ptr;

  if (argint(0, &fd) < 0 || fd < 0 || fd >= NOFILE || argptr(1, (char **)&ptr, sizeof(struct stat)) < 0)
  {
    return -1;
  }

  return file_stat(fd, ptr);
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
  if (mode == O_CREATE || (mode == O_RDWR && strncmp(path, "console", 8) != 0) || (mode == O_WRONLY && strncmp(path, "console", 8) != 0))
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
  int* res;
  if (argptr(0, (char**)&res, 2 * sizeof(int)) < 0) {
    return -1;
  }
  return pipe_open(res);
}
