# Lab 2 Design Doc: Multiprocessing

## Overview

The goal of this lab:
 - add synchronization to functions implemented in lab 1 such that they could be run concurrently.
 - implement fork, wait, exit, pipe and execute functions to create, run, and terminate new programs/processes.

## Major Parts
File Interface: to build on top of lab 1, such that the File Interface could be run concurrently safely.

Processes - fork: this function allows OS to create new processes based on existing processes. This will allow all processes run on the system to have a common root process.

wait/exit: these are responsible to terminate an existing process. They do sufficient cleaning needed when possible.

pipe: it creates a channel for inter-process communication. It is a one-way pipe, i.e. data could only be transmitted from one process to the other process (not the other way).

exec: it initializes an existing process' virtual address space, registers and etc. Then, it overrides the process with a program stored somewhere on the disk.

## In-depth Analysis and Implementation

### Part 1: File Interface Synchronization:
Most file operations need to access to the disk. We believe that a sleep lock for most functions would be a good choice, because disk access is slow and time consuming. We do not want to waste CPU clock cycles just to wait for disk access.

For open/read/write/stat we choose to use sleep lock (i.e. concurrent_writei, concurrent_readi, etc), these are functions that would need to access the disk.

For dup/close we choose to use a spinlock. They only operate on data stored in memory (e.g. the file info struct). There would not be time consuming waiting needed.

### Part 2: Procs

#### fork: 
- A new entry in the process table must be created via `allocproc`
- User memory must be duplicated via `vspacecopy`
- The trapframe must be duplicated in the new process
- All the opened files must be duplicated in the new process
    - copy parent's `fds` (the process file info pointer array)
    - increment `ref_ct` in file info struct of all affected files.
- Set the state of the new process to be `RUNNABLE`
- Return 0 in the child, while returning the child's pid in the parent
    - This can be done by calling `myproc` after `vspacecopy` and check if current process have the same pid as the pid returned by `allocproc`. If they are the same, we are in child proc.

#### wait:
 - while non of its children is in `ZOMBIE` state, i.e. all `RUNNABLE` or `WAITING` or `RUNNING`...
    - `sleep` on its proc struct
 - Loop trough `ptable` to find the `ZOMBIE` child
 - Clear the child's proc struct from `ptable`
 - return child's pid

#### exit:
 - call `vspacefree` to free the vspace used by the process
 - set proc state to `ZOMBIE`
 - if `parent` no longer exit, set its parent to be the root proc
 - call `wakeup` on its parent proc

#### pipe:
- Bookkeeping:
    - Have a pipe struct defined in file.h:
        - size_left: bytes left to be written
        - read_ptr: offset of read
        - write_ptr: offset of write
        - buffer
    - since everything is working in memory, a spinlock is good here
- edit `fileopen`/`filewrite`/`fileread`/`fileclose` to accommodate pipe
- `kllock` a page for the pipe struct
- `fileopen`: create two file info struct: 1 for read and 1 for write
- `filewrite`/`fileread`: write iff size_left > 0 and read iff read_ptr > write_ptr. Reset read_ptr, write_ptr and size_left when buffer is full. If size_left < 0, call `fileclose` on the pipe
- `fileclose`: if size_left >= 0, set it to a negative number and close the current fd. If size_left < 0, free the allocated page and close the current fd.

#### exec:
- `vspacefree` to free the existing unwanted vpace
- `vspaceinit` to initialize vspace of the proc, set proc->
- `vspaceloadcode` to load code from disk
- `vspacewriteova` to write initial arguments into the stack for use
- set all contents, i.e. registers, in `p->context` to 0
- set proc state to `RUNNABLE`

## Risk Analysis
- We are not really sure if we need to use `vspaceinstall`, since we could just change the proc->vspace to a new vspace initialized by `vspaceinit`
- May be having a circular buffer for pipe could be faster for reading and writing? Write do not need to wait for read when buffer is full. It could just overwrite whatever that has been read.

### Time Estimation
 - Synchronization (4 hours)
    - edit all file functions (4 hours)
 - system calls (14 hours)
    - sys_fork (2.5 hours)
    - sys_exit/sys_wait (3 hours)
    - sys_pipe
      - create new struct and edit existing file functions (3 hours)
      - implement the actual method (1.5 hours)
    - sys_exec (4 hours)
 - Edge cases and Error handling (5 hours)
