# Lab 3: Address Space Management

## Overview

The goal of this lab:
- Create user-level heap
- Grow user stack on-demand
- Optimize fork to be copy-on-write

## Major Parts
- Create user-level heap
  - implement `sbrk` to allow user to allocate memory dynamically on physical memory
- Grow user stack on-demand
  - instead allocating a fix stack space at the start, grow stack on-demand
- Optimize fork to be copy-on-write
  - Currently, fork duplicates every page
of user memory in the parent process.
  - Here, we reduce the cost of fork by allowing multiple processes to share the same physical memory, while at a logical level
still behaving as if the memory was copied
  - change to copy-on-write to save space and runtime

## In-depth Analysis and Implementation

### Part 1: Create User-Level Heap
- get `uint64_t from_va = vs.regions[VR_HEAP].size + vs.regions[VR_HEAP].va_base`
- if the current page is enough (i.e., `vs.regions[VR_HEAP].size % 4096 + n < 4096`), jump to increment
- call `vregionaddmap` to
  - store the value of `myproc()->vspace` into a variable `vs`
  - get `struct vregion *vr = &(vs.regions[VR_HEAP])`
  - get `uint64_t sz = roundup((n - remaining_size) / 4096) * 4096`
  - set `short present = 1`
  - set `short writable = 1`
- increment `vs.regions[VR_HEAP].size` by n
- calls `vspaceinvalidate` and `vspaceinstall`

### Part 2: Grow User Stack On-demand
- if `rcr2() > vs.regions[VR_USTACK].va_base - 10 * 4096` it's a valid page fault (`b2` in user mode) so that we can handle it.
- check limit of the current allocated stack by `vs.regions[VR_HEAP].va_base + roundup(size / 4096) * 4096`
- if `rcr2()` is within the current limit, jump to increment
- else, call `vregionaddmap`
  - store the value of `myproc()->vspace` into a variable `vs`
  - get `struct vregion *vr = &(vs.regions[VR_USTACK])`
  - get `uint64_t sz = roundup((limit - rcr2()) / 4096) * 4096`
  - set `short present = 1`
  - set `short writable = 1`
- increment `vs.regions[VR_USTACK].size` by `va_base - rcr2()`
- calls `vspaceinvalidate` and `vspaceinstall`

### Part 3: Make Copy-on-write Fork
- bookkeeping
  -  a new field for `struct vpage_info` named `short copy_on_write`, initialized to be false
  - a new field for `struct core_map_entry` named `int ref_ct`, initialized to be 1
  - add a lock for `struct core_map_entry`
- during fork
  - recursively go through all the `vpage_info` of the parent process
    - if `writable`, change to be read-only and set `copy_on_write` to be true
    - call `increment_pp_ref_ct`
  - make a new linked list `vpi_page` for child and copy the parent's `vspace` to child's
- in `kalloc.c`
  -  add a function `increment_pp_ref_ct` given `va` and `vspace`, increment the corresponding `ref_ct` of the physical page
- in `kfree`
  - decrement `ref_ct` by 1, if it's not 0, return; otherwise, do normal kfree.
- in `trap`
  - check the corresponding `vpage_info` of the address of trap has `copy_on_write` permission using `va2vregion` and `va2vpage_info`
    - if false, return error
    - if true,
      - get the old physical page index using `va2vpi_idx`
      - acquire `spinlock` of `core_map_entry` with the index above
      - if `ref_ct == 1`, set `vpage_info.writable = 1` and `vpage_info.copy_on_write = 0`, and return to user; else, decrement the old `ref_ct` by 1
      - release `spinlock`
      - allocate a new page with `kalloc`, use `mark_user_mem` to setup the new `core_map_entry` (also in this function, acquire lock and set `ref_ct = 1`)
      - copy data using `memmove`
      - set the `vpage_info.ppn` to newly allocated page number
      - set `vpage_info.writable = 1`, `vpage_info.copy_on_write = 0`, `vpage_info.present = 0`
      - calls `vspaceinvalidate` and `vspaceinstall`

## Risk Analysis
### Unanswered Questions
- Why is `vspaceinvalidate` and `vspaceinstall` necessary?
- Is there any other critical section that we need to take care of?
- Where should we implement `sbrk`?

### Staging of Work
First adding necessary fields and helper functions, then user-heap and stack-on-demand, finally fork copy-on-write.

### Time Estimation
- user-heap (10 hours)
- stack-on-demand (2 hours)
- copy-on-write (20 hours)
- edge cases and error handling (5 hours)