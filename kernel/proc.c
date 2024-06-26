#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>
#include <fs.h>
#include <file.h>
#include <vspace.h>

// process table
struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// to test crash safety in lab5,
// we trigger restarts in the middle of file operations
void reboot(void)
{
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
loop:
  asm volatile("hlt");
  goto loop;
}

void pinit(void) { initlock(&ptable.lock, "ptable"); }

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->killed = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trap_frame *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 8;
  *(uint64_t *)sp = (uint64_t)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)forkret;
  memset(p->fds, 0, sizeof p->fds);

  return p;
}

// Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_out_initcode_start[], _binary_out_initcode_size[];

  p = allocproc();

  initproc = p;
  assertm(vspaceinit(&p->vspace) == 0, "error initializing process's virtual address descriptor");
  vspaceinitcode(&p->vspace, _binary_out_initcode_start, (int64_t)_binary_out_initcode_size);
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FLAGS_IF;
  p->tf->rip = VRBOT(&p->vspace.regions[VR_CODE]); // beginning of initcode.S
  p->tf->rsp = VRTOP(&p->vspace.regions[VR_USTACK]);

  safestrcpy(p->name, "initcode", sizeof(p->name));

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  // 1. create a new entry in the process table
  struct proc *p = myproc();
  struct proc *child = allocproc();
  acquire(&ptable.lock);
  if (child == 0) {
    release(&ptable.lock);
    return -1;
  }
  child->parent = p;

  // 2. duplicate user memory
  if (vspaceinit(&child->vspace) != 0) {
    release(&ptable.lock);
    return -1;
  }
  // 2.1 copy-on-write
  if (vspacecopy(&child->vspace, &p->vspace) != 0) {
    release(&ptable.lock);
    return -1;
  }
  // if (vspacemapregions(&child->vspace, &p->vspace) != 0) {
  //   release(&ptable.lock);
  //   return -1;
  // }
  vspaceinstall(myproc());

  // 3. duplicate trap frame
  memmove(child->tf, p->tf, sizeof(*p->tf));

  // 4. duplicate all the open files
  for (int fd = 0; fd < NOFILE; fd++)
  {
    struct finfo* curr_file = p->fds[fd];
    if (curr_file != NULL)
    {
      child->fds[fd] = &(*curr_file); // off by one bug
      curr_file->ref_ct += 1;
      if (curr_file->type == PIPE) {
        struct pipe* curr_pipe = (struct pipe*) (curr_file->ip);
        acquire(&curr_pipe->lock);
        if (curr_file->access_permi == O_RDONLY) {
          curr_pipe->read_ref_ct++;
        } else {
          curr_pipe->write_ref_ct++;
        }
        release(&curr_pipe->lock);
      }
    }
  }

  // 5. change child's state
  child->state = RUNNABLE;
  child->tf->rax = 0; // return for child process
  release(&ptable.lock);
  return child->pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc* p = myproc();
  acquire(&ptable.lock);

  // 1. set all its running children's parent to root
  for (struct proc* curr = ptable.proc; curr < &ptable.proc[NPROC]; curr++) {
    if (curr->parent->pid == p->pid && curr->state != UNUSED) {
      curr->parent = initproc;
    }
  }

  // 2. close up all opened files
  for (int i = 0; i < NOFILE; i++) {
    if (p->fds[i] != NULL) {
      release(&ptable.lock);
      file_close(i);
      acquire(&ptable.lock);
    }
  }

  // 3. set its state to ZOMBIE
  p->state = ZOMBIE;
  p->killed = 0;
  p->chan = 0;

  // 5. wake its parent up
  wakeup1(p->parent);
  sched();
  release(&ptable.lock);
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  int child_count = 0;
  struct proc* zombie = NULL;
  struct proc* p = myproc();
  // Scan through table looking for exited children.
  while (zombie == NULL) {
    acquire(&ptable.lock);
    child_count = 0;
    for (struct proc* curr = ptable.proc; curr < &ptable.proc[NPROC]; curr++) {
      if (curr->parent->pid == p->pid && curr->state != UNUSED) {
        child_count++;
        if (curr->state == ZOMBIE) {
          zombie = curr;
        }
      }
    }
    // if no child
    if (child_count == 0) {
      release(&ptable.lock);
      return -1;
    }
    // if there is no zombie children currently
    if (zombie == NULL) {
      sleep(p, &ptable.lock);
    }
    release(&ptable.lock);
  }

  // cleanup the child proc
  kfree(zombie->kstack);
  vspacefree(&(zombie->vspace));
  int child_pid = zombie->pid;
  acquire(&ptable.lock);
  zombie->state = UNUSED;
  release(&ptable.lock);
  return child_pid;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      mycpu()->proc = p;
      vspaceinstall(p);
      p->state = RUNNING;
      swtch(&mycpu()->scheduler, p->context);
      vspaceinstallkern();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      mycpu()->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1) {
    cprintf("pid : %d\n", myproc()->pid);
    cprintf("ncli : %d\n", mycpu()->ncli);
    cprintf("intena : %d\n", mycpu()->intena);

    panic("sched locks");
  }
  if (myproc()->state == RUNNING)
    panic("sched running");
  if (readeflags() & FLAGS_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&myproc()->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  if (myproc() == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  myproc()->chan = chan;
  myproc()->state = SLEEPING;
  sched();

  // Tidy up.
  myproc()->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {[UNUSED] = "unused", [EMBRYO] = "embryo", [SLEEPING] = "sleep ", [RUNNABLE] = "runble", [RUNNING] = "run   ", [ZOMBIE] = "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint64_t pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state != 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint64_t *)p->context->rbp, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

struct proc *findproc(int pid)
{
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
      return p;
  }
  return 0;
}

// implemented for lab3: user heap
int sbrk(int n) {
  struct vregion* heap = &(myproc()->vspace.regions[VR_HEAP]);
  uint64_t prev_brk = heap->size + heap->va_base;
  // vregionaddmap handles everything including rounding to see if calling kalloc is needed
  int size = vregionaddmap(heap, prev_brk, n, VPI_PRESENT, VPI_WRITABLE);
  if (size < 0) return -1;
  heap->size += size;
  vspaceinvalidate(&(myproc()->vspace));
  return prev_brk;
}

// evicting_page: core_map_entry of the envciting/loading page
// swap_array_index: index of the swap array where the data of the page is going to store/ is storing on disk
// in: 1 -> load back, 0 -> swap out
int update_vspace(struct core_map_entry* evicting_page, uint va, int swap_array_index, int in, uint ppn) {
  struct proc* p;
  char lk = 0;
  // loop through all processes
  if (!holding(&ptable.lock)) {
    acquire(&ptable.lock);
    lk = 1;
  }
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    // check if the process' vspace will be affected
    struct vregion* curr_region = va2vregion(&p->vspace, va);
    if (curr_region == 0) {
      curr_region = va2vregion(&p->vspace, va - 1);
      if (curr_region == 0) {
        continue;  // such va does not exist in this proc
      }
    }
    struct vpage_info* curr_info = va2vpage_info(curr_region, va);
    if (in) {
      if (curr_info->present != 0 || curr_info->on_disk != swap_array_index) {
        continue;
      }
      curr_info->on_disk = 0;
      evicting_page->ref_ct++;
      curr_info->present = 1;
      curr_info->ppn = ppn;
      update_swap_ref_ct(-1, swap_array_index);
      vspaceinvalidate(&p->vspace);
    } else {
      if (curr_info->present != 1 || curr_info->ppn != ppn) {
        continue;  // different page
      }
      // affected page
      curr_info->present = in;
      curr_info->on_disk = swap_array_index;
      evicting_page->ref_ct--;
      curr_info->ppn = 0;
      update_swap_ref_ct(1, swap_array_index);
      vspacemarknotpresent(&p->vspace, va);
    }
    // vspaceinvalidate(&p->vspace);

  }
  if (lk) {
    release(&ptable.lock);
  }
  if (evicting_page->ref_ct != 0 && !in) {
    return -1;
  }
  return 0;
}
