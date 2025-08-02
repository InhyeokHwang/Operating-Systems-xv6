#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//clone syscall wrapper
uint64
sys_clone(void)
{
  uint64 fcn_addr, arg1, arg2, stack_addr;
  struct proc *p = myproc();

  argaddr(0, &fcn_addr);
  argaddr(1, &arg1);
  argaddr(2, &arg2);
  argaddr(3, &stack_addr);

  if (fcn_addr >= p->sz)
    return -1;
  if (stack_addr % PGSIZE != 0 || stack_addr + PGSIZE > p->sz)
    return -1;

  // printf("[debug] sys_clone: fcn=0x%p, arg1=%p, arg2=%p, stack=%p\n",
  //   (void*)fcn_addr, (void*)arg1, (void*)arg2, (void*)stack_addr);
  return clone((void(*)(void*,void*))fcn_addr, (void*)arg1, (void*)arg2, (void*)stack_addr);
}

// 수정이 필요한가요????
uint64
sys_join(void)
{
    uint64 ustack;     // user‐space void** stack
    argaddr(0, &ustack);

    
    uint64 kstack;     // 커널이 돌려줄 유저 스택 VA
    int pid = join((void**)&kstack);
    if (pid < 0)
      return -1;

    if (copyout(myproc()->pagetable,
                ustack,
                (char*)&kstack,
                sizeof kstack) < 0)
      return -1;

    return pid;
}