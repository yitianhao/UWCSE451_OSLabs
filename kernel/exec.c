#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <trap.h>
#include <x86_64.h>

int exec(char *path, char **argv) {
  // 1. establish a (mock) new vspace for the current process,
  // if everthing goes well, we will replace it with the current one.
  // This is a design decision: if anything goes wrong in the process of setting up
  // this new vspace, we won't miss up the current vspace
  struct vspace vs;
  if (vspaceinit(&vs) != 0) {
    vspacefree(&vs);
    return -1;
  }

  // 2. load the program
  uint64_t first_instruction;
  if (vspaceloadcode(&vs, path, &first_instruction) != 0) {
    vspacefree(&vs);
    return -1;
  }

  // 3. initialize user stack
  if (vspaceinitstack(&vs, SZ_2G) != 0) {
    vspacefree(&vs);
    return -1;
  }

  // 4. set arguments to user stack
  int idx = 0;
  uint64_t addr = SZ_2G; // also vs.regions[VR_USTACK].va_base
  while (argv[idx]) { idx++; } // now idx = array_len(argv)
  int argc = idx;
  idx--;

  while (idx >= 0) {
    // get to the correct starting position for copying
    addr -= strlen(argv[idx]) + 1;
    while (addr % sizeof(char*) != 0) addr--;

    // copy over
    if (vspacewritetova(&vs, addr, argv[idx], strlen(argv[idx]) + 1) != 0) {
      vspacefree(&vs);
      return -1;
    }

    // correcting the address stored in current stack, based on our alignment
    argv[idx] = (char*)addr;

    idx--;
  }

  // copy over argv (simplify this once we passed the tests)
  addr -= argc * (sizeof(char*) + 1);
  while (addr % sizeof(char*) != 0) addr--;
  if (vspacewritetova(&vs, addr, (char*)argv, argc * (sizeof(char*) + 1)) != 0) {
    vspacefree(&vs);
    return -1;
  }

  struct proc* p = myproc();

  // set rdi and rsi for main
  p->tf->rdi = argc;
  p->tf->rsi = addr;
  p->tf->rsp = addr - sizeof(char*);  // bottom of the stack
  p->tf->rip = first_instruction;

  // copying vs over to current process
  if (vspacecopy(&p->vspace, &vs) != 0) {
    vspacefree(&vs);
    return -1;
  }

  vspacefree(&vs);
  vspaceinstall(p);

  return 0;
}
