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
  int fd;
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (process->fds[fd] == NULL)
    {
      break;
    }
  }

  // there is no available file descriptor
  if (fd == NOFILE)
  {
    return -1;
  }

  // find the finto struct with the smallest index available in the global ftable,
  // and make the connection
  struct finfo *file;
  for (file = ftable.finfo; file < &ftable.finfo[NFILE]; file++)
  {
    if (file->ref_ct == 0)
    {
      file->access_permi = mode;
      file->ip = ip;
      // what is offset???
      process->fds[fd] = file;
    }
  }

  return fd;
}
