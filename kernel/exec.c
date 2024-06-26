#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <trap.h>
#include <x86_64vm.h>

static uint64_t get_addr_and_copy(struct vspace vs, uint64_t addr, char* data, int data_size);

int exec(char *path, char **argv) {
  // 1. get argc and validated version of argv,
  // the reason is to get argc for step 5 to use (step 5 needs backward alignment)
  // for example, ['ls', null] has argc 2
  int argc = 0;
  uint64_t arg_addr = (uint64_t) argv;
  char** args = argv;
  for (; argc <= MAXARG; argc++) {  // Q: why while loop doesn't work
    uint64_t str_addr;
    // validate and use arg addr to get string addr (the starting addr of the string)
    if (fetchint64_t(arg_addr, (int64_t*)&str_addr) == -1) return -1;
    // validate and get string itself
    if (fetchstr(str_addr, &args[argc]) == -1 ) return -1;
    if (args[argc] == NULL) break;
    arg_addr += sizeof(char*);  // loop through every addr in argv
  }
  argc++;
  if (strncmp(args[0], path, strlen(path))) return -1;

  // 2. establish a (mock) new vspace for the current process,
  // if everthing goes well, we will replace it with the current one.
  // This is a design decision: if anything goes wrong in the process of setting up
  // this new vspace, we won't miss up the current vspace
  struct vspace vs;
  if (vspaceinit(&vs) != 0) {
    vspacefree(&vs);
    return -1;
  }

  // 3. load the program
  //uint64_t first_instruction;
  if (vspaceloadcode(&vs, path, &myproc()->tf->rip) == 0) {
    vspacefree(&vs);
    return -1;
  }

  // 4. initialize user stack
  uint64_t addr = SZ_2G; // also vs.regions[VR_USTACK].va_base
  if (vspaceinitstack(&vs, addr) != 0) {
    vspacefree(&vs);
    return -1;
  }

  // 5. set arguments to user stack
  int idx = argc - 2;
  while (idx >= 0) {
    addr = get_addr_and_copy(vs, addr, args[idx], strlen(args[idx]) + 1);
    // correcting the address stored in current stack, based on our alignment
    args[idx] = (char*)addr;
    idx--;
  }

  // 6. copy over args
  addr = get_addr_and_copy(vs, addr, (char*)args, argc * sizeof(char*));

  struct proc* p = myproc();

  // 7. set rdi and rsi for main
  p->tf->rdi = argc - 1;  // arg0 -> argc for main
  p->tf->rsi = addr;  // arg1 -> argv for main
  p->tf->rsp = addr - sizeof(char*);  // bottom of the stack

  // 8. copying vs over to current process and install the process
  // if (vspacecopy(&p->vspace, &vs) != 0) {
  //   vspacefree(&vs);
  //   return -1;
  // }
  // vspacefree(&vs);
  // vspaceinstall(p);

  struct vspace old_vs = p->vspace;
  p->vspace = vs;
  vspaceinstall(p);
  vspacefree(&old_vs);

  // pml4e_t* pgtbl = p->vspace.pgtbl;
  // vspacefree_wo_pgtbl(&p->vspace);
  // vspaceinit(&p->vspace);
  // vspaceinstall(p);
  // freevm(pgtbl);
  // if (vspacecopy(&p->vspace, &vs) != 0) {
  //   vspacefree(&vs);
  //   return -1;
  // }
  // vspacefree(&vs);
  // vspaceinstall(p);

  // struct vspace old_vs = p->vspace;
  // pml4e_t* pgtbl = p->vspace.pgtbl;
  // //vspacefree_wo_pgtbl(&p->vspace);
  // for (int i = 0; i < NREGIONS; i++) {
  //   p->vspace.regions[i] = vs.regions[i];
  // }
  // //vspaceinit(&p->vspace);
  // //vspaceinstall(p);
  // freevm(pgtbl);
  // //vspacefree(&vs);
  // (&(p->vspace))->pgtbl = vs.pgtbl;
  // vspaceinvalidate(&(p->vspace));
  // vspaceinstall(p);
  // vspacefree(&old_vs);

  return 0;
}

static uint64_t get_addr_and_copy(struct vspace vs, uint64_t addr, char* data, int data_size) {
  // update addr
  addr -= data_size;
  // make alignment
  while (addr % sizeof(char*) != 0) addr--;
  // copy over
  if (vspacewritetova(&vs, addr, data, data_size) != 0) {
    vspacefree(&vs);
    return -1;
  }

  return addr;
}
