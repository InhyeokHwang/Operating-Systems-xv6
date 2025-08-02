#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);


extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;



// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->thread_count = 0;
  p->is_thread    = 0;        // 기본적으로 “스레드 아님”
  p->group        = p;        // 자기 자신을 루트로
  p->gprev        = 0;     // 그룹 리스트 링크 해제
  p->gnext        = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}



// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  // 1) 루트 프로세스이면 소속된 쓰레드 모두 해제
  if (p->group == p) {
    for (struct proc *q = p->gnext; q; q = q->gnext) {
      // 각 쓰레드 freethread 호출
      freethread(q);
    }
    // 2) 루트의 페이지테이블 전체 해제
    if (p->pagetable) {
      proc_freepagetable(p->pagetable, p->sz, p);
      p->pagetable = 0;
    }
  } 
  // 4) 루트까지 해제 후 메타데이터 리셋
  p->sz     = 0;
  p->pid    = 0;
  p->parent = 0;
  p->name[0]= '\0';
  p->chan   = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state  = UNUSED;
  p->group = 0;
  p->gprev = p->gnext = 0;
  p->is_thread = 0;
  p->thread_count = 0;
  p->ustack = 0;
}

void
freethread(struct proc *p)
{
  // 1) 사용자 트랩프레임 매핑 해제
  if (p->trapframe_va) {
    uvmunmap(p->pagetable, p->trapframe_va, 1, 0);
    p->trapframe_va = 0;
  }
  // 2) 커널 측 trapframe 해제
  if (p->trapframe) {
    kfree((void*)p->trapframe);
    p->trapframe = 0;
  }
  // 3) 스레드 슬롯 UNUSED로
  acquire(&p->lock);
  p->state = UNUSED;
  release(&p->lock);
  // 4) 메타데이터 초기화
  p->ustack       = 0;
  p->group        = 0;
  p->gprev        = p->gnext = 0;
  p->is_thread    = 0;
  p->thread_count = 0;
}




struct proc*
allocthread(struct proc *p) // 프로세스를 인자로 받음
{
  struct proc *np;

  for(np = proc; np < &proc[NPROC]; np++) {
    acquire(&np->lock);
    if(np->state == UNUSED) {
      goto found;
    } else {
      release(&np->lock);
    }
  }
  return 0;

found:
  np->pid = allocpid();
  np->state = USED;
  np->is_thread = 1;
  
  // process's user page table.
  np->pagetable = p->pagetable;

  // Allocate a trapframe page.
  if((np->trapframe = (struct trapframe *)kalloc()) == 0){
    freethread(np);
    release(&np->lock);
    return 0;
  }

  // 쓰레드 수 증가
  p->thread_count++;
  // 쓰레드의 트랩프레임 매핑
  if(mappages(np->pagetable, np->trapframe_va = TRAPFRAME - PGSIZE*(p->thread_count), PGSIZE, (uint64)(np->trapframe), PTE_R | PTE_W) < 0){ 
    panic("panic: thread trapframe mapping failed");
    return 0;
  }


  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&np->context, 0, sizeof(np->context));
  np->context.ra = (uint64)forkret;
  np->context.sp = np->kstack + PGSIZE;

  return np;
}




// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate(); // 최상위(level 2) 페이지 테이블 생성
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE, // mappages의 walk를 통해 level 1과 level 0 페이지 테이블을 생성함
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, p->trapframe_va = TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz, struct proc *p)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME - (p->thread_count * PGSIZE), 1 + p->thread_count, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  //추가추가
  struct proc *root  = p->group;
  //추가추가

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  // p->sz = sz;

  // update the canonical sz
  root->sz = sz;
  // now propagate that new size into every member’s p->sz
  for(struct proc *q = root; q; q = q->gnext){
    q->sz = sz;
    // since the list is finite, you’ll stop once gnext==NULL
  }

  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// Exit the current process or thread. Does not return.
void
exit(int status)
{
  struct proc *p = myproc();
  struct proc *parent = p->parent;
  int is_thread = (p->group != p);

  if (is_thread) {
    // ─── 쓰레드 종료: 그룹 리스트에서 분리하고 join_parent()만 깨움 ─────────
    if (p->gprev)   p->gprev->gnext = p->gnext;
    if (p->gnext)   p->gnext->gprev = p->gprev;
    p->gprev = p->gnext = 0;

    // join() 대기 중인 부모(루트) 깨우기
    acquire(&wait_lock);
    wakeup(parent);
    release(&wait_lock);
  } else {
    // ─── 프로세스 종료: 공유 자원 정리 ───────────────────────────────
    if (p == initproc)
      panic("init exiting");

    // 열린 파일 모두 닫기
    for (int fd = 0; fd < NOFILE; fd++) {
      if (p->ofile[fd]) {
        fileclose(p->ofile[fd]);
        p->ofile[fd] = 0;
      }
    }
    // cwd 반납
    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    // ─── 자식 스레드들 종료 신호 ─────────────────────────────
    // 같은 그룹에 속한 모든 쓰레드에 killed 세팅
    for (struct proc *q = p->gnext; q; q = q->gnext) {
      acquire(&q->lock);
      q->killed = 1;
      if (q->state == SLEEPING)
        q->state = RUNNABLE;
      release(&q->lock);
    }

    // 자식 프로세스 및 남은 쓰레드를 init으로 재배치 후 부모 깨우기
    acquire(&wait_lock);
    reparent(p);
    wakeup(parent);
    release(&wait_lock);
  }

  // ─── 공통: ZOMBIE 설정 및 스케줄링 ─────────────────────────────
  acquire(&p->lock);
  p->xstate = status;
  p->state  = ZOMBIE;

  // 컨텍스트 스위치: 더 이상 돌아오지 않음
  sched();
  panic("exit should not return");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();
  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){ // 자식 찾음
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);
        
        havekids = 1;
        if(pp->state == ZOMBIE){ //자식이 좀비라면(자식이 죽었는데 reaped 되지 않음)
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp); 
          
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;

  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);


  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p, *target = 0;

  // 1) pid에 해당하는 proc 찾기
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      target = p;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }
  if (!target)
    return -1;

  // 2) 같은 그룹의 루트(root) 결정
  struct proc *root = (target->group == target
                      ? target
                      : target->group);

  // 3) 루트만 temp_killed 플래그 세팅
  acquire(&root->lock);
  root->temp_killed = 1;
  if(root->state == SLEEPING)
    root->state = RUNNABLE;
  release(&root->lock);

  return 0;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    // 목적지가 유저 메모리라면, 페이지 테이블 검사(copyout) 후 복사
    return copyout(p->pagetable, dst, src, len);
  } else {
    // 목적지가 커널 메모리라면, 그냥 memmove로 빠르게 복사
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int
clone(void(*fcn)(void *, void *), void *arg1, void *arg2, void *stack)
{
  int i;
  struct proc *np; //쓰레드
  int tid; //쓰레드 id
  struct proc *proc = myproc()->group; // 프로세스
  uint64 stack_base = (uint64)stack; // 스택의 시작되는 주소

  
  // 새로운 프로세스 영역 할당
  if ((np = allocthread(proc)) == 0){
    return -1;
  }

  np->sz = proc->sz;

  // doubly linked list로 group 관리
  np->group = proc;
  np->gnext = proc->gnext;
  if(proc->gnext) {
    proc->gnext->gprev = np;
  }
  proc->gnext = np;
  np->gprev = proc;

  // 트랩프레임 영역 초기화
  memset(np->trapframe, 0, sizeof(*np->trapframe));

  // 진입점 설정
  np->trapframe->epc = (uint64)fcn;
  // a0/a1: fcn(arg1,arg2)
  np->trapframe->a0  = (uint64)arg1;
  np->trapframe->a1  = (uint64)arg2;

  // 쓰레드 스택 주소 보관 (join에서 사용) 새로 할당된 페이지의 top 부분임
  np->ustack = stack_base;


  // 쓰레드 스택 초기화
  uint64 sp = stack_base + PGSIZE;

  // 스택에 값 담기
  // *(uint64*)sRet = 0xFFFFFFFF;
  uint64 sRet = sp -3*sizeof(uint64); // va
  uint64 tmp_ret  = 0xFFFFFFFF;                             
  if (copyout(np->pagetable, sRet, (char *)&tmp_ret, sizeof(tmp_ret)) < 0)
    panic("clone: copyout failed for ret");

  // *(uint64*)sArg1 = (uint64)arg1;
  uint64 sArg1 = sp -2*sizeof(uint64); // va
  uint64 tmp_arg1 = (uint64)arg1;
  if (copyout(np->pagetable, sArg1, (char *)&tmp_arg1, sizeof(tmp_arg1)) < 0)
    panic("clone: copyout failed for arg1");

  // *(uint64*)sArg2 = (uint64)arg2;
  uint64 sArg2 = sp - sizeof(uint64); // va
  uint64 tmp_arg2 = (uint64)arg2;
  if (copyout(np->pagetable, sArg2, (char *)&tmp_arg2, sizeof(tmp_arg2)) < 0)
    panic("clone: copyout failed for arg2");

  np->trapframe->sp = sp - 3*sizeof(uint64); 

  // 열린 파일과 cwd(current working directory) 복제
  for (i = 0; i < NOFILE; i++) // 파일 디스크립터 테이블 순회
    if (proc->ofile[i]) // 파일이 열려 있다면
      np->ofile[i] = filedup(proc->ofile[i]); // 복사
  np->cwd = idup(proc->cwd); // 현재 작업 중인 디렉토리 복사

  // 프로세스 이름 복사
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  tid = np->pid;

  release(&np->lock);

  // 부모 프로세스 설정
  acquire(&wait_lock);
  np->parent = proc;
  release(&wait_lock);

  // RUNNABLE 상태로 만듦
  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return tid;
}



int
join(void **stack)
{
  struct proc *p;
  struct proc *cur = myproc();
  int haveThread;

  acquire(&wait_lock);
  for (;;) {
    haveThread = 0;

    // Scan all procs for threads of this parent in same pagetable
    for (p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);

        if (p->parent != cur) {
            release(&p->lock);
            continue;
        }

        haveThread = 1;
        // Found a zombie thread

        if (p->state == ZOMBIE) {
          int pid = p->pid;
          void *tstack = (void*)p->ustack;

          // Decrement thread count
          cur->thread_count--;

          // Use freethread to cleanup thread-specific resources
          release(&p->lock);
          *stack = tstack;
          release(&wait_lock);
          if(p->is_thread){
            freethread(p);
          }else{ //exec을 호출한 경우임
            freeproc(p);
          }

          if(cur->thread_count == 0 && cur->temp_killed){
            cur->temp_killed = 0;
            cur->killed = 1;
          }
          return pid;
        }
        release(&p->lock);
    }

    // 2) 더 이상 남은 스레드가 없으면 -1 리턴
    if (!haveThread || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    sleep(cur, &wait_lock);
  }
}