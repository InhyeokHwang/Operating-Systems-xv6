#include "types.h"
#include "riscv.h"
#include "defs.h"

// Simple system call
int myfunction(char *str){
    printf("%s\n", str);
    return 0xABCDABCD;
}

// Wrapper for my_syscall
int sys_myfunction(void){
    char str[100];
    //Decode argument using argstr
    if(argstr(0, str, 100) < 0) //유효성 검증
        return -1;
    return myfunction(str);
}