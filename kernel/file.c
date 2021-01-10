//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <proc.h>

struct devsw devsw[NDEV];

// file table
struct
{
  struct finfo finfo[NFILE];
} ftable;

// return the smallest fd available for the current process,
// if there is no available fd, return -1.
int fd_available();

int file_open(char *path, int mode)
{
  // get the inode pointer of this file
  struct inode *ip;
  if ((ip = namei(path)) == 0)
  {
    return -1;
  }

  // get the current process
  struct proc *process = myproc();

  // find the smallest fd available for the current process
  int fd = fd_available();

  // there is no available file descriptor
  if (fd == -1)
    return -1;

  // find the finto struct with the existing ip (another process can also refer to it)
  // OR smallest index available in the global ftable,
  // and make the connection
  struct finfo *file;
  for (file = ftable.finfo; file < &ftable.finfo[NFILE]; file++)
  {
    if (file->ip == ip)
    {
      file->access_permi = mode;
      file->ref_ct++;
      // what is offset???
      process->fds[fd] = file;
      break;
    }
  }
  if (process->fds[fd] == NULL)
  {
    for (file = ftable.finfo; file < &ftable.finfo[NFILE]; file++)
    {
      if (file->ref_ct == 0)
      {
        file->access_permi = mode;
        file->ip = ip;
        file->ref_ct++;
        // what is offset???
        process->fds[fd] = file;
        break;
      }
    }
  }

  return fd;
}

int file_close(int fd)
{
  // get the current process
  struct proc *process = myproc();

  // not an open fd
  struct finfo *file = process->fds[fd];
  if (file == NULL)
    return -1;

  // close the connection between finfo and the current process;
  process->fds[fd] = NULL;
  file->ref_ct--;

  // when no process is using this inode, clean up
  if (file->ref_ct == 0)
  {
    irelease(file->ip);
    file->access_permi = 0;
    file->ip = 0;
  }

  return 0;
}

int file_dup(int fd)
{
  // get the current process
  struct proc *process = myproc();

  // not an open fd
  struct finfo *file = process->fds[fd];
  if (file == NULL)
    return -1;

  // get new fd, ready for duplicate
  int new_fd = fd_available();
  if (new_fd == -1)
    return -1;

  // make duplicate
  file->ref_ct++;
  process->fds[new_fd] = file;

  return new_fd;
}

int fd_available()
{
  int fd;
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (myproc()->fds[fd] == NULL)
    {
      break;
    }
  }

  if (fd == NOFILE)
  {
    return -1;
  }

  return fd;
}
