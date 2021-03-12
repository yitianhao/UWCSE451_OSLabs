Book-Keeping:
 - in kalloc.c
 - introduce an uchar array[256] in memory to keep track swap disk status
   if array[i] == 0 -> the 8 blocks from swap_start + 8 * i are free
   else they are not free
 - introduce a new field vpage_info->on_disk to record down the index of its physical page on disk.
 - rmb to update cow, copy on_disk field as well (in map_region).

1. Enviction: build a helper function in proc.c for 1.4
    1.1 in `kalloc`, if there is no more available pages, i.e. reach line 171, we need to call `swapout`
    1.2 use `get_random_user_page` to randomly pick a `user` page. Note: keep calling this function until we get a `user` page.
    1.3 find consequtive 8 free blocks on disk in swap region
        get its index in array 
    1.4 Loop through all processes: happen in proc.c (needs 2 parameters, core_map_entry*, uint pa, uchar status) (0 to evict, 1 to load back)
        1.4.1 use `va` from `core_map_entry` of envicting page
        1.4.2 use `page2pa` to get pa of the page
        1.4.3 for the current process, use `va2vpi_idx` to get the physical page index, if `pa of core_map_entry == physical page index`, mark `vpage_info->present = 0`, update `vpage_info->on_disk = index`  
        1.4.4 invalidate its vspace
        1.4.5 core_map_entry->ref_ct --
    1.5 use `P2V` to get kernal va of the pa that is going to be evicted. Use `bread()` and `bwrite()` to write data to disk. (`memmove` as well, remember to `brelse()`)
    1.6 mark `core_map_entry` to be available and ref_ct = 0
        and clean up all its other fields


2. swapback:
    - in trap
        When page_fault, check if `vpage_info->on_disk != 0`, then go to swapback (in kalloc)

    in kalloc
        1. kalloc a free page
        2. use V2P get pa of the page
        3. get core_map_entry of the page
        4. set user = 1
        5. same as enviction.1.4 but reverse order
        6. use bread() to load data from disk and memmove to the page in order.
    
