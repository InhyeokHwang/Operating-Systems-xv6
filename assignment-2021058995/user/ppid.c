#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(void) {
    int my_pid = getpid();
    int parent_pid = getppid(); 
    printf("My student ID is 2021058995\n");
    printf("My pid is %d\n", my_pid);
    printf("My ppid is %d\n", parent_pid);
    exit(0);
}