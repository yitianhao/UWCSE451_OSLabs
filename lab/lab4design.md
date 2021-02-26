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
    - inside a for-loop similar to `readi`,
  - update meta data of the `dinode`:
- in `sys_open`:

### Part 2: Append to a file

### Part 3: Create files

### Part 4: Delete files

### Part 5: Logging layer