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
  - 



### Risk Analysis:
  - For appending, if we introduce a fixed length array, we are still having a fixed size storage for files. We thought about having a linked list to solve the issue. i.e. we could have a extent struct pointer (pointing somewhere in extent) that will be stored in dinode. Modify the extent struct to store an array of startblkno and number of blocks (mapped via their index). At the same time, the struct will also store another extent struct pointer that point to another block. i.e. somewhat similar to the linked list for vspace region.
    Do we need to introduce this? Is a fixed size array good enough?
  - 
