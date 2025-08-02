#include "kernel/types.h"
#include "user/user.h"


// 그냥 for문 계속 도는 cpu bound job
void busy_work(void) {
  int i, j;
  for(i = 0; i < 100000000; i++){
    for(j = 0; j < 100000000; j++){
      ;
    }
  }
}

int main(void) {
  int i, pid;
  int iterations = 50;  // 각 프로세스가 실행할 횟수

  // 3개의 자식 프로세스 생성
  for(i = 0; i < 3; i++){
    pid = fork();
    if(pid < 0){
      printf("fork failed\n");
      exit(1);
    }
    if(pid == 0) { // 자식 프로세스
      for(i = 0; i < iterations; i++){
        busy_work();
        printf("Child process, pid: %d, iteration: %d\n", getpid(), i);
      }
      exit(0);
    }
  }

  // 부모 프로세스도 iterations 번 busy_work() 실행
  for(i = 0; i < iterations; i++){
    busy_work();
    printf("Parent process, pid: %d, iteration: %d\n", getpid(), i);
  }

  // 자식 프로세스가 모두 종료될 때까지 기다림
  for(i = 0; i < 3; i++){
    wait(0);
  }

  exit(0);
}
