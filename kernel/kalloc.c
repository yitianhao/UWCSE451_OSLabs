// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include <cdefs.h>
#include <defs.h>
#include <fs.h>
#include <e820.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <spinlock.h>
#include <vspace.h>
#include <proc.h>

#define STACK_BOUND 2147483647 - 10 * 4096

int npages = 0;
int pages_in_use;
int pages_in_swap;
int free_pages;

struct core_map_entry *core_map = NULL;

struct core_map_entry *pa2page(uint64_t pa) {
  if (PGNUM(pa) >= npages) {
    cprintf("%x\n", pa);
    panic("pa2page called with invalid pa");
  }
  return &core_map[PGNUM(pa)];
}

uint64_t page2pa(struct core_map_entry *pp) {
  return (pp - core_map) << PT_SHIFT;
}

// --------------------------------------------------------------
// Detect machine's physical memory setup.
// --------------------------------------------------------------

void detect_memory(void) {
  uint32_t i;
  struct e820_entry *e;
  size_t mem = 0, mem_max = -KERNBASE;

  e = e820_map.entries;
  for (i = 0; i != e820_map.nr; ++i, ++e) {
    if (e->addr >= mem_max)
      continue;
    mem = max(mem, (size_t)(e->addr + e->len));
  }

  // Limit memory to 256MB.
  mem = min(mem, mem_max);
  npages = mem / PGSIZE;
  cprintf("E820: physical memory %dMB\n", mem / 1024 / 1024);
}

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
struct swap_stat swap_status[SWAPSIZE_PAGES]; // 1 byte = 8 blocks -> 1 page

struct {
  struct spinlock lock;
  int use_lock;
} kmem;

static void setrand(unsigned int);
static int swap_out();

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void mem_init(void *vstart) {
  void *vend;

  core_map = vstart;
  memset(vstart, 0, PGROUNDUP(npages * sizeof(struct core_map_entry)));
  vstart += PGROUNDUP(npages * sizeof(struct core_map_entry));

  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 1;

  vend = (void *)P2V((uint64_t)(npages * PGSIZE));
  freerange(vstart, vend);
  free_pages = (vend - vstart) >> PT_SHIFT;
  pages_in_use = 0;
  pages_in_swap = 0;
  kmem.use_lock = 1;
  setrand(1);
}

void freerange(void *vstart, void *vend) {
  char *p;
  p = (char *)PGROUNDUP((uint64_t)vstart);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(char *v) {
  struct core_map_entry *r;
  uint lock = 0;

  if ((uint64_t)v % PGSIZE || v < _end || V2P(v) >= (uint64_t)(npages * PGSIZE))
    panic("kfree");

  if (kmem.use_lock && !holding(&kmem.lock)) {
    acquire(&kmem.lock);
    lock = 1;
  }

  r = (struct core_map_entry *)pa2page(V2P(v));
  if (r->ref_ct > 1) {
    r->ref_ct--;
    if (kmem.use_lock)
      release(&kmem.lock);
    return;
  }
  r->ref_ct = 0;
  pages_in_use--;
  free_pages++;

  // Fill with junk to catch dangling refs.
  memset(v, 2, PGSIZE);

  r->available = 1;
  r->user = 0;
  r->va = 0;
  if (kmem.use_lock && lock)
    release(&kmem.lock);
}

void
mark_user_mem(uint64_t pa, uint64_t va)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 1;
  r->va = va;
}

void
mark_kernel_mem(uint64_t pa)
{
  // for user mem, add an mapping to proc_info
  struct core_map_entry *r = pa2page(pa);

  r->user = 0;
  r->va = 0;
}

char *kalloc(void) {

  int i;
  int lock = 0;

  if (kmem.use_lock && !holding(&kmem.lock)) {
    acquire(&kmem.lock);
    lock = 1;
  }

  for (i = 0; i < npages; i++) {
    if (core_map[i].available == 1) {
      core_map[i].available = 0;
      core_map[i].ref_ct = 1;
      pages_in_use++;
      free_pages--;
      if (kmem.use_lock && lock)
        release(&kmem.lock);
      return P2V(page2pa(&core_map[i]));
    }
  }

  if (swap_out() == 0) {
    if (kmem.use_lock && lock)
      release(&kmem.lock);
    return 0;
  } else {
    if (kmem.use_lock && lock)
      release(&kmem.lock);
    return kalloc();
  }
}


static unsigned long int next = 1;

// returns random integer from [0, limit)
static int rand(int limit) {
  next = next * 1103515245 + 12345;
  return (unsigned int)(next/65536) % limit;
}

// Sets the seed for random.
// Intended to be used before calling rand.
static void setrand(unsigned int seed) {
  next = seed;
}

struct core_map_entry * get_random_user_page() {
  int x = 100;
  while(x--) {
    int rand_index = rand(npages);
    if (core_map[rand_index].va != 0) {
      return &core_map[rand_index];
    }
  }
  panic("Tried 100 random indices for random user page, all failed");
}

// implemented for lab3 copy-on-write fork
// increase ref_count of the page that PA is in
void increment_pp_ref_ct(uint64_t pa) {
  if (kmem.use_lock) {
    acquire(&kmem.lock);
  }
  struct core_map_entry* curr = pa2page(pa);
  curr->ref_ct++;
  if (kmem.use_lock) {
    release(&kmem.lock);
  }
}

// implemented for lab3 copy-on-write fork
// copy on write: we are writing, so need to make a copy
// decrease ref_count of the page that PA is in.
// if ref_count is 1 before decrement -> we can just use the page, return 0
// else return 1
int cow_copy_out_page(uint64_t pa, struct vpage_info* curr_page) {
  if (kmem.use_lock) {
      acquire(&kmem.lock);
  }
  struct core_map_entry* curr = pa2page(pa);
  if (curr->ref_ct > 1) {
    curr->ref_ct--;
    if (kmem.use_lock) {
    release(&kmem.lock);
    }
    return 1;
  } else {
    // 2.1. Set current vpage_info to writable and in not copy_on_write mode
    curr_page->writable = 1;
    curr_page->copy_on_write = 0;
    // 2.2. Set page table permission to writable
    if (kmem.use_lock) {
    release(&kmem.lock);
    }
    return 0;
  }
}

static struct core_map_entry* get_rand_sat_page() {
  struct core_map_entry* res = get_random_user_page();
  while (res->user != 1 || PGNUM(page2pa(res)) == 0 || res->ref_ct == 0) {
    res = get_random_user_page();
  }
  return res;
}

static int swap_out() {
  int i;
  struct core_map_entry* evicted_page;
  if (kmem.use_lock && !holding(&kmem.lock)) {
    panic("must be locked");
  }
  // 1. find a free set of 8 blocks
  for (i = 0; i < SWAPSIZE_PAGES; i++) {
    if (swap_status[i].used == 0) {
      // we have found the free region
      // mark it as used
      swap_status[i].used = 1;
      pages_in_swap++;
      break;
    }
  }
  if (i == SWAPSIZE_PAGES) {
    // if swap section is full
    panic("SWAP REGION FULL");
    return 0;
  }

  // 2. get a random user page to evict
  evicted_page = get_rand_sat_page();

  // 3. update all vspace_info if this page is involved
  while (update_vspace(evicted_page, evicted_page->va, i, 0, PGNUM(page2pa(evicted_page)))) {
    evicted_page = get_rand_sat_page();
  }

  if (kmem.use_lock)
      release(&kmem.lock);

  // 4. copy out data from the page and write to disk
  swap_write(P2V(page2pa(evicted_page)), i);
  
  if (kmem.use_lock)
    acquire(&kmem.lock);

  // 6. mark the page as unused
  evicted_page->available = 1;
  evicted_page->user = 0;
  evicted_page->va = 0;

  return 1;
}

int swap_in(uint on_disk_idx, uint addr) {
  char* va = kalloc();
  if (kmem.use_lock)
    acquire(&kmem.lock);
  if (va == 0){
    if (kmem.use_lock) release(&kmem.lock);
    cprintf("fail in kalloc\n");
    return -1;
  }
  struct core_map_entry* swapped_in_page = pa2page(V2P(va));
  swapped_in_page->ref_ct = 0;
  mark_user_mem(V2P(va), (uint64_t) va);
  pages_in_swap--;

  update_vspace(swapped_in_page, addr, on_disk_idx, 1, PGNUM(page2pa(swapped_in_page)));
  if (kmem.use_lock)
    release(&kmem.lock);
  swap_read(va, on_disk_idx);
  swap_status[on_disk_idx].used = 0;
  return 1;
}

// increase/decrease ref_ct of a swap region
// direction = 1 to increase
// direction = -1 to decrease
void update_swap_ref_ct(int direction, int index) {
  int lock = 0;
  if (kmem.use_lock) {
    if (!holding(&kmem.lock)) {
      acquire(&kmem.lock);
      lock = 1;
    }
  }
  swap_status[index].ref_ct = swap_status[index].ref_ct + direction * 1;
  if (swap_status[index].ref_ct == 0) {
    swap_status[index].used = 0;
  }
  if (kmem.use_lock && lock)
    release(&kmem.lock);
}