#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0) //사용자 모드에서 트랩이 발생한 것이 맞는지 확인
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec); //stvec 레지스터에 값을 씀(트랩 발생 시 제어가 넘어갈 핸들러의 주소). 커널 모드에서 발생한 트랩을 kernelvec으로 처리해야하기 때문

  struct proc *p = myproc(); //현재 프로세스 포인터
  
  // save user program counter.
  p->trapframe->epc = r_sepc(); //트랩이 발생하기 직전의 프로그램 카운터 저장
  
  if(r_scause() == 8){ //8은 ecall에 해당
    // system call

    if(killed(p)) //해당 프로세스가 이미 killed 된 프로세스인지 확인
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4; //시스템 콜이 끝난 이후 사용자 모드로 돌아갈 때 다음 명령어 실행 위치 (4바이트 명령어 크기)

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on(); //인터럽트 온

    syscall(); //시스템 콜 로직 실행, trapframe->a0에 시스템콜 함수 담김
  } else if((which_dev = devintr()) != 0){ //타이머 인터럽트나 외부장치 인터럽트의 경우
    // ok
  } else { //예상치 못한 트랩, 익셉션
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret(); 
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off(); //인터럽트 끄기 -> kerneltrap에서 usertrap으로 바뀌는 과정에서 예기치 못한 예외가 발생하면 안됨

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline); //trampoline_uservec의 주소를 계산, uservec - trampoline은 uservec함수가 trampoline 내에서 얼마나 떨어져 있는지 나타냄
  w_stvec(trampoline_uservec); //stvec 레지스터에 값을 씀 (트랩 발생 시 제어가 넘어갈 핸들러의 주소). stvec을 설정해야 추후에 유저모드로 복귀하고 다시 시스템 콜이 발생했을 때 CPU가 점프해야 될 위치를 알 수 있음
  //커널모드는 kernelvec으로 트랩을 처리하지만 유저모드는 uservec으로 트랩을 처리하기 때문에 모드 변경 과정에서 stvec이 갖고 있는 주소를 항상 바꿔줘야됨

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  // 커널 모드로 다시 복귀할 때 필요한 값들 저장
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap; // 다음에 트랩이 발생했을 때 호출할 함수(여기서는 usertrap)의 주소를 저장합니다. (유저모드에서 호출할 것이기 때문)
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid() 프로세스 코어 id

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x); //sstatus 레지스터는 현재 실행 모드, 인터럽트 상태 등 정보를 담고 있음

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc); //트랩이 발생할 때, 사용자 모드의 프로그램 카운터(현재 실행 중이던 명령어 주소)가 sepc에 저장됨. sret 명령어를 통해 사용자 모드로 전환 시 정확히 이 주소부터 실행을 재개할 수 있게 함

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable); //사용자 페이지 테이블 준비하기

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline); //userret 주소 계산
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

