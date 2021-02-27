# Lab 4: File System

## Overview

The goal of this lab:
- Add more functionalities with synchronization enabled to the file system (e.g., file write to disk, file creation, file deletion)
- Add functionalities to enable a crash-safe file system

## Major Parts
- writeable file system
  - file write, file append, file create, file delete
- add synchronizations to above functionalities
  - add locks when appropriate
- enable a crash-safe file system
  - add logging layer

## In-depth Analysis and Implementation

### Part 1: Write to a file
- in `fs.c/writei`
  - write to file content:
    - loop thourgh all affected bytes similar to readi:
      - for each iteration:
        - 1. get the `buf` of associated block, using `bget`
        - 2. find number of bytes to be written on this page:
          - `m = min(n - tot, BSIZE - off % BSIZE)`
        - 3. `memmove(bp->data + offset % BSIZE, src, m)
        - 4. update the change to disk using bwrite
        - 5. `brelse(buf)`
  - update meta data of the `dinode`:
    - 1. update ip->size
    - 2. find offset of the affected file's meta-data in icache:
      - offset = ip->inum * size of block + 2 * size of short
      - create a buffer that stores the new size
      - `concurrent_writei(&icache.inodefile, buffer, offset, size of (uint))`
        - note: if `inum == inodefile.inode`, we will not update metadata (dinode)
- in `sys_open`:
  - enalbe `WRITE`

### Part 2: Append to a file
  - Book keeping:
    - instead of having a single extent struct, we will have an array of extent structs that keep track of
      multiple extents.
    - default size of every extent chunck = 20 blocks
    - introduce a new variable `uint max_size` to dinode to keep track with the maximum writable size
    - introduce a sleeplock to `superblock`
  - in `fs.c/writei`:
    - check `ip->size + n < ip->max_size`:
      - if yes, continue to write
      - else:
        - acquire `superblock.sleeplock`
        - use `bread` to get current state of bitmap, since we know the block number of the start of bitmap region
        - set `dinode.data[max_size / 20 * size of block].startblkno = superblock.bmapstart`
        - set its nblocks = 20
        - set all bits bitmap[superblock.bmapstart] to bitmap[superblock.bmapstart + 20] = 0
        - increment max_size by 20 * size of block
        - update bmapstart by finding the index of next bit in bitmap that = 1
        - user `bwrite` to update the bitmap region
        - continue to write

### Part 3: Create files
  - first use dirlookup to check if the file exists:
    - if exists, open normally and return
  - when creating file:
    - acquire `superblock.sleeplock`
    - for inum = 2 to maximum possible number of inodes on disk (`size of inodes / 64`):
      - use `read_dinode(inum)` to get the struct. If size == 0, we have found a free dinode
    - similar to append to file, use bitmap and `superblock.bmapstart` to get free blocks in extent
    - initialize the first extent struct in the free dinode found with the free blocks found
    - update `inodefile` like how we did for `writei`
    - update directory:
      - create a `struct dirent dir`, inum = inum we have found earlier, num = userinput
      - offset = inum * size of dirent
      - loop through `icache.inode` to find the inode with inum == 1 (i.e. root directory)
      - `concurrent_writei(&root directory, dir, offset, size of (dirent))`

### Part 4: Delete files
  - first use dirlookup to check if the file exists:
    - if does not exist, return error
  - when deleting:
    - acquire `superblock.sleeplock`
    - from offset we got, use `read_dinode` to get the dinode struct
    - use `bread` to get current state of bitmap
    - for every `block` in extent that is used by the file, set corresponding bit in bitmap to 1 and update bitmap
      using bwrite
    - to update inodes section in disk:
      - zero out the dinode struct by using `concurrent_writei` to inodefile like how we update inodefile in `write`
    - update directory (by zeroing out bits used to represent the file) like how we update in `create files`

### NOTE:
  - Since we are repeating a lot of stpes, we could write a few helper functions that update bitmap, inodefile and directory

### Part 5: Logging layer
  - book keeping:
    - in `superblock`, add `uint logstart`
    - make a log_data struct with the following fields for disk
      - `short commit_flag`
      - `uint data_ptr`
      - `uint inum`
      - `uint offset`
      - `uint blknum`
      - all fields of `dinode` except padding
      - padding up to 64
    - make a log_matadata struct with the following fields for disk, occupying 1 blocks (up to 8 files total possible)
      - `struct log_data array[8]`
  - in `mkfs.c`: update to add new log part for the disk, the log part will be placed
  in between of superblock and bitmap
  - `log_write()`: write to log
    - populate the block returned by `read_log_struct`:
      - find a free block (`commit_flag == 0`) in the log part, set the block number to `data_ptr`
      - use `bwrite` to write data from input to the block on disk with block number equals to `data_ptr`
  - `read_log_struct`
    - `bread` to read the start of the log
    - loop through the log part, 1 blocks at a time to find a free block
    - return the log_data struct and the index of the struct in the log_metadata
  - `commit_tx()`
    - use the index returned by `log_write` to find the log_data struct in the log_metadata struct
    - populate the log_data struct:
      - use input to set `inum`, `blknum`, `offset`
      - set `commit_flag = 1`
      - use bwrite to update
  - `copy_to_disk()`: copy from log to disk
    - `bread` to read the start of log_data
      - `bwrite` to the actual data on disk with `blknum`, `offset`, and data from `data_ptr`
      - `bwrite` to the metadata of the file using what is stored in the log_data
      - `bwrite` to the log_data: set `commit_flag = 0`
  - `log_check()`: check in when opening file
    - `bread` the start of the log_metadata, loop through it to find the same `inum`
      - if `commit_flag == 1`, call `copy_to_disk()`


### Risk Analysis:
## Unanswered Questions:
  - For appending, if we introduce a fixed length array, we are still having a fixed size storage for files. We thought about having a linked list to solve the issue. i.e. we could have a extent struct pointer (pointing somewhere in extent) that will be stored in dinode. Modify the extent struct to store an array of startblkno and number of blocks (mapped via their index). At the same time, the struct will also store another extent struct pointer that point to another block. i.e. somewhat similar to the linked list for vspace region.
    Do we need to introduce this? Is a fixed size array good enough?
  - for log part, we are confusing to allow 8 or 16 (or more?) block logging at the "same time"? We're having 8 blocks right now, is this enough?

## Staging of Work
1. create file + delete file
2. log + write(append)

## Time Estimation
- file write (5 hours)
- file append (5 hours)
- file create (5 hours)
- file delete (5 hours )
- add logging layer (20 hours)
- edge cases and error handling (5 hours)

