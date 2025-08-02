#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]){
    char *buf = "Hello xv6!";
    int ret_val;
    ret_val = myfunction(buf);
    fprintf(1, "Return value : 0x%x\n", ret_val);
    exit(0);
};