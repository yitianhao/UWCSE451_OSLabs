# Lab 2 Design Doc: Multiprocessing

## Overview

The goal of this lab:
 - add synchronization to functions implemented in lab 1 such that they could be run concurrently.
 - implement fork, wait, exit, pipe and execute functions to create, run, and terminate new programs/processes.

## Major Parts
File Interface: to build on top of lab 1, such that the File Interface could be run concurrently safely.

fork: this function allows OS to create new processes based on existing processes. This will allow all processes run on the system to have a common root process.

wait/exit: these are responsible to terminate an existing process. They do sufficient cleaning needed when possible.

pipe: it creates a channel for inter-process communication. It is a one-way pipe, i.e. data could only be transmitted from one process to the other process (not the other way).

exec: it initializes an existing process' virtual address space, registers and etc. Then, it overrides the process with a program stored somewhere on the disk.

## In-depth Analysis and Implementation

### Part 1: File Interface Synchronization:
For global file table `ftable` we add a `spinlock` for it. We made this decision similar to the locking philosophy of `ptable`.
- Since this is a single core system, it's enough to only lock the entire `ftable` or `ptable`, becuase
only one of the `finfo struct` or `proc struct` can run at the single time.
- We choose `spinlock` not `sleeplock` because there is no interrupts involved in this process, so it
is okay for interrupts to be disabled.
- For codes we modified, we add some locks when we writing data to some `finfo struct` in `ftable`.
The philosophy is: to keep the critical section as small as possible.

### Part 2: Process

#### fork:
- A new entry in the process table must be created via `allocproc`
- User memory must be initialized and duplicated via `vspaceinit` and `vspacecopy`
- The trapframe must be duplicated in the new process via `memmove`
- All the opened files must be duplicated in the new process
    - copy parent's `fds` (the process file info pointer array)
    - increment `ref_ct` in file info struct of all affected files.
    - if encounters a pipe file, increment it's `read_ref_ct` or `write_ref_ct`
- Set the state of the new process to be `RUNNABLE`
- Return 0 in the child, while returning the child's pid in the parent
    - This can be done by changing child process's `rax` in its trap frame.

#### wait:
 - Loop trough `ptable` to find the `ZOMBIE` child
 - While non of its children is in `ZOMBIE` state, i.e. all `RUNNABLE` or `WAITING` or `RUNNING`...
    - `sleep` on its proc struct
 - Clear the child's proc struct from `ptable` (i.e., set state to `UNUSED`) and cleanup the child proc
 - Return child's pid

#### exit:
 - Set all current process's children's parent to the root process `initproc`
 - Close up all the files open by the current process
 - Set proc state to `ZOMBIE`
 - Call `wakeup` on its parent proc
 - Call `sched` to yield the schedular

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
- Validate the given `char** argv` and the argument strings by looping through `argv` until `NULL`, also get `argc` in this process
- Create a new `vspace` to set up. (design decision: we cannot discard our current `vspace` until we are confident about the set up)
    - Initialize
    - Load code from `path` and get the first instruction pointer
    - Initialize user stack to start from `SZ_2G`
- Reversely to write initial arguments into the stack for use. (first argument strings, then `argv`)
    - Need correct alignment
- Set all registers
- Copy over the mock `vspace` and install current process

## Risk Analysis
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
