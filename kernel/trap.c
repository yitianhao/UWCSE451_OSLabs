#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
int validate_cow(uint64_t addr);

int num_page_faults = 0;

int grow_user_stack_ondemand(uint64_t addr);

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      // lab5: check if swap in is needed
      struct vregion* vr = va2vregion(&myproc()->vspace, addr);
      if (vr) {
        struct vpage_info* curr_info = va2vpage_info(vr, addr);
        if (curr_info->used && !curr_info->present) { // same as used but not present
          if (swap_in(curr_info->on_disk, addr) != -1) {
            vspaceinstall(myproc());
            return;
          }
          else panic("swap in failed\n");
        }
      }

      // lab3: check if it caused by copy on write
      if (validate_cow(addr) == 0 && (tf->err & 2)) {
        // it is caused by copy on write
        if (vspace_copy_on_write(&myproc()->vspace, addr) != -1) { // implemented in vspace.c
          vspaceinstall(myproc());
          return;
        } else {
          panic("err in vspace_copy_on_write");
        }
      }

      // lab3: checking if this page fault is valid for growing stack on-demand
      struct vregion stack = myproc()->vspace.regions[VR_USTACK];
      if (addr >= stack.va_base - 10 * PGSIZE && addr < stack.va_base) {
        if (grow_user_stack_ondemand(addr) != -1) return;  // correctly handled growing stack
        else panic("err in grow_user_stack_ondemand");
      }

      if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

int validate_cow(uint64_t addr) {
  struct vregion* curr_region = va2vregion(&myproc()->vspace, addr);
  if (curr_region == 0) {
    return -1;
  }
  if (curr_region->dir == VRDIR_DOWN) {
    if (addr < curr_region->va_base - curr_region->size) {
      return -1;
    }
  } else {
    if (addr > curr_region->va_base + curr_region->size) {
      return -1;
    }
  }
  struct vpage_info* curr_page = va2vpage_info(curr_region, addr);
  if (curr_page == 0) {
    return -1;
  }
  if (curr_page->copy_on_write) {
    return 0;
  }
  return -1;
}

int grow_user_stack_ondemand(uint64_t addr) {
  struct vregion* stack = &(myproc()->vspace.regions[VR_USTACK]);
  uint64_t prev_limit = stack->va_base - stack->size;
  uint64_t n = PGROUNDUP(prev_limit - addr);
  if (stack->size + n >= 10 * PGSIZE) return -1;
  // vregionaddmap handles everything including rounding to see if calling kalloc is needed
  int size = vregionaddmap(stack, prev_limit - n, n, VPI_PRESENT, VPI_WRITABLE);
  if (size < 0) return -1;
  stack->size += size;
  vspaceinvalidate(&(myproc()->vspace));
  return prev_limit;
}
