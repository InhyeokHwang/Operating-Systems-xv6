#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NUM_LOOP 100000
#define NUM_THREAD 4
#define MAX_LEVEL 3

int parent;
int fcfs_pids[NUM_THREAD];
int fcfs_count[100] = {0};

int fork_children() // 자식 4개 만듦, 본인 포함 총 5개
{
  int i, p;
  for (i = 0; i < NUM_THREAD; i++) {
    if ((p = fork()) == 0) {
      return getpid();
    } 
  }
  return parent;
}

void exit_children() // 자식 종료, 부모는 자식 회수 대기
{
  if (getpid() != parent)
    exit(0);
  int status;
  while (wait(&status) != -1);
}

int main(int argc, char *argv[])
{
  int i, pid;
  int count[MAX_LEVEL] = {0};

  parent = getpid();

  printf("FCFS & MLFQ test start\n\n"); // 테스트 시작

  // [Test 1] FCFS test
  printf("[Test 1] FCFS Queue Execution Order\n");
  pid = fork_children(); // 자식 4개 생성

  if (pid != parent) // 자식의 경우
  {
    while(fcfs_count[pid] < NUM_LOOP) // 100000번 돌기
    {
      fcfs_count[pid]++;
    }

    printf("Process %d executed %d times\n", pid, fcfs_count[pid]); // fcfs이므로 먼저 생성된 순으로 끝남
  }
  exit_children(); // 자식 회수
  printf("[Test 1] FCFS Test Finished\n\n");

  // Switch to FCFS mode - should not be changed
  if(fcfsmode() == 0) printf("successfully changed to FCFS mode!\n");
  else printf("nothing has been changed\n"); // 이미 fcfsmode이므로 -1값이 반환되고 이 문장 출력됨

  // Switch to MLFQ mode
  if(mlfqmode() == 0) printf("successfully changed to MLFQ mode!\n"); // fcfs -> mlfq 모드로 전환됨
  else printf("nothing has been changed\n");

  // [Test 2] MLFQ test
  printf("\n[Test 2] MLFQ Scheduling\n");
  pid = fork_children(); // 자식 4개 생성

  if (pid != parent) // 자식의 경우
  {
    for (i = 0; i < NUM_LOOP; i++) // 100000번 돌기
    {
      int x = getlev(); // 현재 프로세스의 레벨
      if (x < 0 || x >= MAX_LEVEL) // 레벨 값이 이상한 경우
      {
        printf("Wrong level: %d\n", x);
        exit(1);
      }
      count[x]++; // 특정 레벨에서 실행된 횟수를 기록
    }

    printf("Process %d (MLFQ L0-L2 hit count):\n", pid);
    for (i = 0; i < MAX_LEVEL; i++)
      printf("L%d: %d\n", i, count[i]);
  }
  exit_children(); // 자식 수거

  printf("[Test 2] MLFQ Test Finished\n");
  printf("\nFCFS & MLFQ test completed!\n");
  exit(0);
}
