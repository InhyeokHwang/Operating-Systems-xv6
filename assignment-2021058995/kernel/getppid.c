#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "defs.h"
#include "proc.h"



uint64 sys_getppid(void) { //argument를 받고 있지 않기 때문에 따로 wrapper function은 불필요
    struct proc *p = myproc(); //현재 프로세스 구조체 포인터
    if(p->parent){ //부모 프로세스가 있다면
        return p->parent->pid;
    }else{ //부모 프로세스가 없다면
        return -1; //-1 반환
    }
}
