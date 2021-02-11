# Lab 3: Address Space Management

## Overview

The goal of this lab:
- Create user-level heap
- Grow user stack on-demand
- Optimize fork to be copy-on-write

## Major Parts
- Create user-level heap
- Grow user stack on-demand
- Optimize fork to be copy-on-write

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

### Part 2: Grow User Stack On-demand
- if `rcr2() > vs.regions[VR_USTACK].va_base - 10 * 4096` it's a valid page fault and we need to handle it.
- check limit of the current allocated stack by `vs.regions[VR_HEAP].va_base + roundup(size / 4096) * 4096`
- if `rcr2()` is within the current limit, jump to increment
- else, call `vregionaddmap`
  - store the value of `myproc()->vspace` into a variable `vs`
  - get `struct vregion *vr = &(vs.regions[VR_USTACK])`
  - get `uint64_t sz = roundup((limit - rcr2()) / 4096) * 4096`
  - set `short present = 1`
  - set `short writable = 1`
- increment `vs.regions[VR_USTACK].size` by `va_base - rcr2()`

### Part 3: Make Copy-on-write Fork
- during fork
  - introduce a new field for `struct vpage_info` named `short copy_on_write`, initialized to be false
  - introduce another new field for `struct vpage_info` named `int ref_ct`, initialized to be 1
  - recursively go through all the `vpage_info` of the parent process
    - if `writable`, change to be read-only and set `copy_on_write` to be true
    - increment `ref_ct` by 1
  - make a new linked list `vpi_page` for child and copy the parent's `vspace` to child's
- in `free_page_desc_list`
  - if `ref_ct` is not 0, skip the step of `kfree`
- in `trap`
  - check the corresponding `vpage_info` of the address of trap has `copy_on_write` permission
    - if false, return error
    - if true,
      - decrement `ref_ct` by 1
      - allocate a new page with `kalloc` and copy data using `copy_vpi_page`
      -
      - set `writable` to be true, `copy_on_write` to be false
      -
