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

struct fcfs_queue fcfs_q; // FCFS QUEUE
struct spinlock fcfs_lock; //FCFS LOCK

struct mlfq_queue mlfq_q[3]; // MLFQ QUEUE
struct spinlock mlfq_lock; // MLFQ LOCK

struct spinlock mode_lock; // mode lock
struct spinlock boost_lock; // priority boost lock

static uint64 last_boost = 0; // 직전 priority boost 값


// 스케줄러가 확인하는 모드
int sched_mode = FCFS_MODE;

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

  // 새 프로세스의 필드를 현재 모드에 맞게 초기화
  if (sched_mode == FCFS_MODE) {
    // fcfs 모드
    p->priority     = -1;
    p->level        = -1;
    p->time_quantum = -1;
  } else {
    // mlfq 모드면
    p->priority     = 3;
    p->level        = 0;
    p->time_quantum = 0;
  }

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
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
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
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
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

  p = allocproc(); // 최초의 프로세스
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
  fcfs_queue_push(p); // 프로세스가 RUNNABLE 상태로 바뀌었으므로 ready queue에 넣음.

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
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
  // 자식이 RUNNABLE 상태가 되었으므로 ready queue에 넣음
  if(sched_mode == FCFS_MODE) {
    fcfs_queue_push(np);
  } else {
    mlfq_queue_push(np->level, np);
  }
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
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
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
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
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

// // Per-CPU process scheduler.
// // Each CPU calls scheduler() after setting itself up.
// // Scheduler never returns.  It loops, doing:
// //  - choose a process to run.
// //  - swtch to start running that process.
// //  - eventually that process transfers control
// //    via swtch back to the scheduler.
// void
// scheduler(void)
// {
//   struct proc *p;
//   struct cpu *c = mycpu();

//   c->proc = 0;
//   for(;;){ 
//     // The most recent process to run may have had interrupts
//     // turned off; enable them to avoid a deadlock if all
//     // processes are waiting.
//     intr_on();

//     int found = 0;
//     for(p = proc; p < &proc[NPROC]; p++) { // 순회하면서 실행 가능한 프로세스를 찾음
//       acquire(&p->lock);
//       if(p->state == RUNNABLE) {
//         // Switch to chosen process.  It is the process's job
//         // to release its lock and then reacquire it
//         // before jumping back to us.
//         p->state = RUNNING;
//         c->proc = p;

//         printf("CONTEXT SWITCH Process name: %s, pid: %d\n", p->name, p->pid);
//         swtch(&c->context, &p->context); // context switch 발생

//         // Process is done running for now.
//         // It should have changed its p->state before coming back.
//         c->proc = 0;
//         found = 1;
//       }
//       release(&p->lock);
//     }
//     if(found == 0) { // 실행할 프로세스가 하나도 없음
//       // nothing to run; stop running on this core until an interrupt.
//       intr_on();
//       asm volatile("wfi"); // wait for interrupt의 약자
//     }
//   }
// }

void
scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // deadlock 방지
    intr_on();

    if (sched_mode == FCFS_MODE) { // fcfs 모드의 경우
      if (!fcfs_queue_empty()) {
        p = fcfs_queue_pop();
        acquire(&p->lock);
        if (p->state == RUNNABLE) {
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context); // 실행
          c->proc = 0; // 끝
        }
        release(&p->lock);
      } else {
        intr_on();
        asm volatile("wfi");
      }

    } else { // mlfq 모드의 경우
      // priority boost
      if (ticks - last_boost >= 50) {
        priorityboost();
      }

      // 우선순위 가장 높은 level0 큐부터 검색
      p = 0;
      for(int level = 0; level < 3; level++){
        if(!mlfq_queue_empty(level)){
          p = mlfq_queue_pop(level);
          break;
        }
      }

      if (p) {
        acquire(&p->lock);
        if (p->state == RUNNABLE) {

          // 실제로 실행
          p->state = RUNNING;
          c->proc = p;
          swtch(&c->context, &p->context); // 실행
          c->proc = 0; // 끝

          // 실행 후 demotion
          if (p->state == RUNNABLE) {
            if (p->level == 0) { // L0→L1
              if (p->time_quantum >= 1) {
                p->level++;
                p->time_quantum = 0;
              }
            } else if(p->level == 1){ // L1→L2
              if (p->time_quantum >= 3) {
                p->level++;
                p->time_quantum = 0;
              }
            } else {
              // L2: quantum 5 소진 시 priority 감소
              if (p->time_quantum >= 5) {
                p->time_quantum = 0;
                if (p->priority > 0)
                  p->priority--;
              }
            }
            // 다시 해당 레벨 큐로 push
            mlfq_queue_push(p->level, p);
          }
        }
        release(&p->lock);
      } else {
        // idle
        intr_on();
        asm volatile("wfi");
      }
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
  int intena; // context switch 전후 interrupt 상태 저장/복원 용도
  struct proc *p = myproc();

  if(!holding(&p->lock)) // 현재 프로세스에 대해서 락이 잘 걸려있지 않으면 race condition 발생
    panic("sched p->lock");
  if(mycpu()->noff != 1)  
    panic("sched locks");
  if(p->state == RUNNING) //호출 전에 이미 RUNNABLE 로 만들었음 
    panic("sched running");
  if(intr_get()) // context switch 도중 interrupt 발생하면 안됨
    panic("sched interruptible");

  intena = mycpu()->intena; // context switch 하기 전에 interrupt 상태 저장
  swtch(&p->context, &mycpu()->context); // context switch 발생
  mycpu()->intena = intena; // 나중에 돌아오고 나서 여기서부터 이어서 실행, interrupt 상태 복원
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  if(sched_mode == FCFS_MODE){ // fcfs 모드에서 자발적 yield가 호출되어도 방금까지 실행중이던 프로세스가 가장 생성시간이 빨랐을 것이기 때문에 해당 프로세스를 다시 재개
    return;
  }
  struct proc *p = myproc(); // 현재 프로세스
  acquire(&p->lock); 
  p->state = RUNNABLE; // 현재 프로세스 RUNNABLE 설정
  sched(); //context switching
  release(&p->lock); 
}

int 
getlev(void)
{
  if(sched_mode == FCFS_MODE) // FCFS모드면 99 반환
    return 99;
  struct proc *p = myproc();
  return p->level;
}

int 
setpriority(int pid, int priority)
{
  struct proc *p;

  // priority 값이 0~3 범위를 벗어나면 -2 반환.
  if(priority < 0 || priority > 3)
    return -2;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->priority = priority;
      release(&p->lock);
      return 0; // 성공 시 0 반환
    }
    release(&p->lock);
  }
  return -1; // pid에 대응되는 프로세스가 존재하지 않음, -1 반환
}

int
fcfsmode(void)
{
  struct proc *p;

  // 모드 전환 중에는 다른 스케줄링이 일어나지 않도록 보호
  acquire(&mode_lock);

  if(sched_mode == FCFS_MODE){
    printf("already in FCFS mode.\n");
    release(&mode_lock);
    return -1;
  }

  // mlfq 초기화
  mlfq_queue_init();
  // fcfs 초기화
  fcfs_queue_init();

  sched_mode = FCFS_MODE;
  acquire(&tickslock);
    ticks = 0;
  release(&tickslock);

  // 이미 프로세스 테이블 상에서 pid순(생성시간 순)으로 되어있음
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state==RUNNING){
      p->state        = RUNNABLE;
      p->priority     = -1;
      p->level        = -1;
      p->time_quantum = -1;
      fcfs_queue_push(p);
    }
    release(&p->lock);
  }

  release(&mode_lock);
  return 0;
}

int
mlfqmode(void)
{
  struct proc *p;

  // 모드 스위칭 중에는 다른 어떤 프로세스도 진행되지 않도록 atomic하게 만들었다.
  acquire(&mode_lock);

  // 이미 MLFQ 모드인지 확인
  if(sched_mode == MLFQ_MODE){
    printf("already in MLFQ mode.\n");
    release(&mode_lock);
    return -1;
  }

  // fcfs 큐 초기화
  fcfs_queue_init();
  // mlfq 큐 초기화
  mlfq_queue_init();

  // MLFQ mode로 설정
  sched_mode = MLFQ_MODE; 

  // global tick값 초기화
  acquire(&tickslock);
  ticks = 0;
  release(&tickslock);
  
  // fcfs에서의 프로세스들을 mlfq의 L0 큐로 이동
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state==RUNNING){
      p->state        = RUNNABLE;
      p->priority     = 3;   // 최고 우선순위
      p->level        = 0;   // 레벨 0
      p->time_quantum = 0;   // 소비 시간 초기화
      mlfq_queue_push(0, p);
    }
    release(&p->lock);
  }

  release(&mode_lock);
  return 0;
}

void
priorityboost(void)
{
  struct proc *p;

  acquire(&boost_lock); // atomic 하게 만들어 큐가 꼬이는 일이 발생하지 않게 하자.

  // FCFS 모드에서는 호출되면 안됨
  if(sched_mode == FCFS_MODE){
    printf("cannot call priority boost in FCFS mode\n");
    return;
  }

  // 직전에 부스트가 발생한 시간 갱신
  last_boost = ticks;

  // mlfq 큐 초기화
  mlfq_queue_init();

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNABLE || p->state==RUNNING) {
      p->state        = RUNNABLE;
      p->level        = 0;
      p->priority     = 3;
      p->time_quantum = 0;
      mlfq_queue_push(0, p);
    }
    release(&p->lock);
  }
  release(&boost_lock);
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
    if(p == myproc())  // 자기 자신은 건너뛰고
      continue;

    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      // 쉘 프로세스만 FCFS 모드로 복원
      if (p->pid == 2) {
        acquire(&mode_lock);
        sched_mode = FCFS_MODE;
        release(&mode_lock);
      }

      p->state = RUNNABLE;
      if(sched_mode == FCFS_MODE){
        fcfs_queue_push(p);
      } else {
        mlfq_queue_push(p->level, p);
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
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
    return copyout(p->pagetable, dst, src, len);
  } else {
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


///////////////////////////////////FCFS queue functions///////////////////////////////////
// fcfs 큐 락 초기화, 처음 한 번만 호출되고 런타임에 호출되면 안됨
void
fcfs_queue_lock_init(void)
{
  initlock(&fcfs_lock, "fcfs_q"); 
  fcfs_queue_init();
}
// 큐 초기화
void
fcfs_queue_init(void) 
{
  acquire(&fcfs_lock);
  fcfs_q.head = 0;
  fcfs_q.tail = 0;
  release(&fcfs_lock);
}
// 큐가 비었는지 확인
int
fcfs_queue_empty(void)
{
  return (fcfs_q.head == fcfs_q.tail);
}
// 큐가 꽉 찼는지 확인
int
fcfs_queue_full(void)
{
  return (((fcfs_q.tail + 1) % NPROC) == fcfs_q.head);
}
// 큐에 프로세스 삽입
void
fcfs_queue_push(struct proc *p)
{
  acquire(&fcfs_lock);
  if(fcfs_queue_full()){
    panic("fcfs_queue_push: ready queue full");
  }
  fcfs_q.procs[fcfs_q.tail] = p;
  fcfs_q.tail = (fcfs_q.tail + 1) % NPROC;
  release(&fcfs_lock);
}
// 큐에서 프로세스 pop
struct proc *
fcfs_queue_pop(void)
{
  struct proc *p = 0;
  acquire(&fcfs_lock);
  if(!fcfs_queue_empty()){
    p = fcfs_q.procs[fcfs_q.head];
    fcfs_q.head = (fcfs_q.head + 1) % NPROC;
  }
  release(&fcfs_lock);
  return p;
}

///////////////////////////////////MLFQ queue functions///////////////////////////////////
// mlfq 큐 락 초기화, 처음 한 번만 호출되고 런타임에 호출되면 안됨
void
mlfq_queue_lock_init()
{
  initlock(&mlfq_lock, "mlfq_q"); // mlfq queue 락 초기화
  mlfq_queue_init();
}
// 큐 초기화
void
mlfq_queue_init(void) 
{
  acquire(&mlfq_lock);
  for(int i = 0; i < 3; i++){
    mlfq_q[i].head = 0;
    mlfq_q[i].tail = 0;
  }
  release(&mlfq_lock);
}
// 큐가 비었는지 확인
int
mlfq_queue_empty(int level)
{
  return (mlfq_q[level].head == mlfq_q[level].tail);
}
// 큐가 꽉 찼는지 확인
int
mlfq_queue_full(int level)
{
  int next = (mlfq_q[level].tail + 1) % NPROC;
  return (next == mlfq_q[level].head);
}
// 큐에 프로세스 삽입
void
mlfq_queue_push(int level, struct proc *p)
{
  acquire(&mlfq_lock);
  if(mlfq_queue_full(level)){
    panic("mlfq_queue_push: ready queue full");
  }
  int next = (mlfq_q[level].tail + 1) % NPROC;
  mlfq_q[level].procs[mlfq_q[level].tail] = p;
  mlfq_q[level].tail = next;
  release(&mlfq_lock);
}
// 큐에서 프로세스 pop
struct proc *
mlfq_queue_pop(int level)
{
  struct proc *p = 0;
  acquire(&mlfq_lock);
  if (!mlfq_queue_empty(level)) {
    p = mlfq_q[level].procs[mlfq_q[level].head];
    mlfq_q[level].head = (mlfq_q[level].head + 1) % NPROC;
  }
  release(&mlfq_lock);
  return p;
}