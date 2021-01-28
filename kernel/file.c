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
struct {
  struct spinlock lock;
  struct finfo finfo[NFILE];
} ftable;

// return the smallest fd available for the current process,
// if there is no available fd, return -1.
static int fd_available();

// transfer n bytes of data from src to dest
// caller should make sure all ptrs are valid
static void data_transfer(char* src, char* dest, size_t n);

// need initialization??? saw similar functions: finit, binit
int file_open(char *path, int mode)
{
  // get the inode pointer of this file
  // namei call namex, which already has inode locked
  struct inode *ip;
  if ((ip = namei(path)) == 0)
    return -1;

  // get the current process
  struct proc *process = myproc();

  // find the smallest fd available for the current process
  acquire(&ftable.lock);
  int fd = fd_available();

  // there is no available file descriptor
  if (fd == -1) {
    release(&ftable.lock);
    return -1;
  }
  // find the finto struct with the smallest index available in the global ftable,
  // and make the connection
  struct finfo *file;
  for (file = ftable.finfo; file < &ftable.finfo[NFILE]; file++) {
    if (file->ref_ct == 0) {
      file->access_permi = mode;
      file->ip = (void*) ip;
      file->ref_ct++;
      file->offset = 0;
      process->fds[fd] = file;
      file->type = FILE;
      break;
    }
  }

  release(&ftable.lock);
  return fd;
}

int pipe_open(int* res) {
  // get the current process
  struct proc* p = myproc();

  // find 2 available fd, lock them up so others would not use them.
  acquire(&ftable.lock);
  int read = fd_available();
  p->fds[read] = (void*) 1;
  int write = fd_available();
  if (read == -1 || write == -1) {
    p->fds[read] = NULL;
    release(&ftable.lock);
    return -1;
  }

  // find 2 file info struct
  struct finfo* fread = NULL;
  struct finfo* fwrite = NULL;
  for (struct finfo* file = ftable.finfo; file < &ftable.finfo[NFILE]; file++) {
    if (file->ref_ct == 0 && fread == NULL) {
      fread = file;
    } else if (file->ref_ct == 0 && fwrite == NULL) {
      fwrite = file;
      break;
    }
  }
  if (fwrite == NULL || fread == NULL) {
    p->fds[read] = NULL;
    release(&ftable.lock);
    return -1;
  }

  // create the pipe
  struct pipe* new_pipe = (struct pipe*) kalloc();
  if (new_pipe == 0) {
    p->fds[read] = NULL;
    release(&ftable.lock);
    return -1;
  }

  // init pipe
  memset(new_pipe, 0, PAGE_SIZE);
  new_pipe->read_off = 0;
  new_pipe->write_off = 0;
  new_pipe->size_left = sizeof(new_pipe->buff);
  new_pipe->read_ref_ct = 1;
  new_pipe->write_ref_ct = 1;

  // init read end finfo
  fread->access_permi = O_RDONLY;
  fread->ip = (void*) new_pipe;
  fread->offset = 0;
  fread->ref_ct = 1;
  fread->type = PIPE;
  // init write end finfo
  fwrite->access_permi = O_WRONLY;
  fwrite->ip = (void*) new_pipe;
  fwrite->offset = 0;
  fwrite->ref_ct = 1;
  fwrite->type = PIPE;

  p->fds[read] = fread;
  p->fds[write] = fwrite;
  res[0] = read;
  res[1] = write;
  release(&ftable.lock);
  return 0;
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
  acquire(&ftable.lock);
  file->ref_ct--;
  release(&ftable.lock);
  if (file->type == PIPE) {
    struct pipe* curr_pipe = (struct pipe*) file->ip;
    acquire(&curr_pipe->lock);
    if (file->access_permi == O_RDONLY) {
      curr_pipe->read_ref_ct--;
    } else {
      curr_pipe->write_ref_ct--;
    }
    wakeup(curr_pipe);
    release(&curr_pipe->lock);
  }
  // when no process is using this inode/pipe, clean up
  if (file->ref_ct == 0) {
    acquire(&ftable.lock);
    if (file->type == FILE) {
      irelease((struct inode*)file->ip);
    } else if (file->type == PIPE) {
      struct pipe* curr_pipe = (struct pipe*) file->ip;
      if (curr_pipe->write_ref_ct == 0 && curr_pipe->read_ref_ct == 0) {
        kfree((char*)curr_pipe);
      }
    }
    file->access_permi = 0;
    file->ip = 0;
    file->offset = 0;
    release(&ftable.lock);
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
  acquire(&ftable.lock);
  int new_fd = fd_available();
  if (new_fd == -1) {
    release(&ftable.lock);
    return -1;
  }

  // make duplicate
  file->ref_ct++;

  // if it is a pipe
  if (file->type == PIPE) {
    struct pipe* curr_pipe = (struct pipe*) file->ip;
    if (file->access_permi == O_RDONLY) {
      curr_pipe->read_ref_ct++;
    } else {
      curr_pipe->write_ref_ct++;
    }
  }
  release(&ftable.lock);
  process->fds[new_fd] = file;

  return new_fd;
}

int file_read(int fd, char *dst, uint n)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL || (file->access_permi != O_RDONLY && file->access_permi != O_RDWR))
    return -1;

  if (file->type == FILE) {
    // get the file inode
    struct inode *ip = (struct inode*) file->ip;
    // get curr offset
    uint offset = file->offset;
    int read = concurrent_readi(ip, dst, offset, n);
    if (read == -1)
      return -1;
    acquire(&ftable.lock);
    file->offset = file->offset + read;
    release(&ftable.lock);
    return read;

  } else if (file->type == PIPE) {
    // get pipe struct first
    struct pipe* curr_pipe = (struct pipe*) file->ip;
    acquire(&curr_pipe->lock);

    // check if there is anything to read
    size_t read = -1;
    int rest = 0;

    while (read == -1) {
      // decide how much to read and what to return
      size_t to_read = curr_pipe->write_off - curr_pipe->read_off;
      if (curr_pipe->write_ref_ct == 0) {  // if write end is closed
        if (to_read < n && to_read > 0) {
          read = to_read;
          rest = to_read;
        } else if (to_read == 0) { // closed and no more things to read
          rest = 0;
        } else { // read as usual
          rest = (size_t) n;
          read = n;
        }
        break;
      } else { // normal cases
        if (to_read == 0) {  // nothing to read
          rest = -1;
        } else if (to_read < n) {  // partial read
          read = to_read;
          rest = (int) to_read;
          break;
        } else {  // usual read
          read = (size_t) n;
          rest = (int) n;
          break;
        }
        sleep(curr_pipe, &curr_pipe->lock);
      }
    }
    // cleanups
    data_transfer(curr_pipe->buff + curr_pipe->read_off, dst, read);
    // if the page is full, reset it iff writing end is still opened
    if (curr_pipe->read_off == curr_pipe->write_off
        && curr_pipe->size_left == 0
        && curr_pipe->write_ref_ct > 0) {
      curr_pipe->read_off = 0;
      curr_pipe->write_off = 0;
      curr_pipe->size_left = sizeof(curr_pipe->buff);
      file->offset = 0;
    }
    // set offsets
    curr_pipe->read_off += read;
    wakeup(curr_pipe);
    acquire(&ftable.lock);
    file->offset += read;
    release(&ftable.lock);
    release(&curr_pipe->lock);
    return rest;
  }
  return -1;
}

int file_write(int fd, char *src, uint n)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL || (file->access_permi != O_WRONLY && file->access_permi != O_RDWR))
    return -1;

  if (file->type == FILE) {
    // get the file inode
    struct inode *ip = (struct inode*) file->ip;
    // get curr offset
    uint offset = file->offset;
    int written = concurrent_writei(ip, src, offset, n);
    if (written == -1)
      return -1;
    acquire(&ftable.lock);
    file->offset = file->offset + written;
    release(&ftable.lock);
    return written;

  } else if (file->type == PIPE) {
    // get pipe struct first
    struct pipe* curr_pipe = (struct pipe*) file->ip;
    acquire(&curr_pipe->lock);

    // check if there is any more space left
    size_t to_write = curr_pipe->size_left;
    size_t written = 0;
    int rest = 0;
    // decide how much to write
    while (rest == 0) {
      if (curr_pipe->read_ref_ct == 0) {  // nobody is waiting for read
        rest = -1;
        break;
      } else { // normal cases
        if (to_write == 0) {  // no space to write
          rest = 0;
        } else if (to_write < n) {  // partial read
          written = to_write;
          rest = (int) to_write;
          break;
        } else {  // usual read
          written = (size_t) n;
          rest = (int) n;
          break;
        }
        sleep(curr_pipe, &curr_pipe->lock);
      }
    }

    // cleanups
    data_transfer(src, curr_pipe->buff + curr_pipe->write_off, written);
    // set offsets
    curr_pipe->write_off += written;
    curr_pipe->size_left -= written;
    wakeup(curr_pipe);
    acquire(&ftable.lock);
    file->offset = curr_pipe->write_off;
    release(&ftable.lock);
    release(&curr_pipe->lock);
    return rest;
  }
  return -1;
}


int file_stat(int fd, struct stat *st)
{
  // get the curr process
  struct proc *process = myproc();

  struct finfo *file = process->fds[fd];
  if (file == NULL)
    return -1;

  if (file->type == FILE) {
    // get the file inode
    struct inode *ip = (struct inode*)file->ip;
    concurrent_stati(ip, st);
  } else {
    return -1;
  }
  return 0;
}

static int fd_available()
{
  int fd;
  for (fd = 0; fd < NOFILE; fd++) {
    if (myproc()->fds[fd] == NULL)
      break;
  }

  if (fd == NOFILE)
    return -1;

  return fd;
}

static void data_transfer(char* src, char* dest, size_t n) {
  char* upto = src + n;
  for (; src < upto; src++) {
    *dest = *src;
    dest++;
  }
}