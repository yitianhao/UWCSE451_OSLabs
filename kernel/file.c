//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
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

int file_read(int fd, char *dst, uint n)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL || (file->access_permi != O_RDONLY && file->access_permi != O_RDWR))
  {
    return -1;
  }

  // get the file inode
  struct inode *ip = file->ip;
  // get curr offset
  uint offset = file->offset;
  int read = concurrent_readi(ip, dst, offset, n);
  if (read == -1)
  {
    return -1;
  }
  file->offset = file->offset + read;
  return read;
}

int file_write(int fd, char *src, uint n)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL || (file->access_permi != O_WRONLY && file->access_permi != O_RDWR))
  {
    return -1;
  }

  // get the file inode
  struct inode *ip = file->ip;
  // get curr offset
  uint offset = file->offset;
  int written = concurrent_writei(ip, src, offset, n);
  if (written == -1)
  {
    return -1;
  }
  file->offset = file->offset + written;
  return written;
}

int file_stat(int fd, struct stat *st)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL)
  {
    return -1;
  }

  // get the file inode
  struct inode *ip = file->ip;
  concurrent_stati(ip, st);
  return 0;
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
