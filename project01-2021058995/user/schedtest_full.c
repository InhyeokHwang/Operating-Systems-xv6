#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NUM_LOOP    100000
#define NUM_THREAD  4
#define MAX_LEVEL   3

int parent;

// 1) 단순 fork/exit 헬퍼: 자식 NUM_THREAD개 생성
int
fork_children(void)
{
  int i, pid;
  for (i = 0; i < NUM_THREAD; i++) {
    if ((pid = fork()) == 0)
      return getpid();    // 자식: 자신 PID 반환
  }
  return parent;          // 부모: parent 반환
}

// 2) 자식 회수 헬퍼
void
exit_children(void)
{
  if (getpid() != parent)
    exit(0);              // 자식은 즉시 종료
  int status;
  while (wait(&status) != -1)
    ;                     // 부모는 모든 자식 회수
}

int
main(int argc, char *argv[])
{
  int pid, i;
  int hits[MAX_LEVEL];

  parent = getpid();
  printf("=== FCFS & MLFQ extended test start ===\n\n");

  // ---------------------------------------------------------
  // [Test 1] 기본 FCFS 동작 검증
  //   - non-preemptive: fork된 순서대로 각 자식이 NUM_LOOP회 실행
  // ---------------------------------------------------------
  printf("[1] FCFS 기본 동작\n");
  pid = fork_children();
  if (pid != parent) {
    // 자식: NUM_LOOP 회 루프
    int ctr = 0;
    while (ctr++ < NUM_LOOP) ;
    printf(" Child %d done busy loop\n", pid);
  }
  exit_children();
  printf(" => [1] FCFS 확인 완료\n\n");

  // ---------------------------------------------------------
  // [Test 2] mode-switch 직후 getlev 검증
  //   - fcfsmode() 후 getlev() == 99
  //   - mlfqmode() 후 getlev() == 0
  // ---------------------------------------------------------
  printf("[2] 모드 전환 직후 필드 초기화 확인\n");
  if (fcfsmode() != 0) printf("  fcfsmode: 이미 FCFS 모드\n");
  printf("   getlev() after fcfsmode() == %d  (기대: 99)\n", getlev());

  if (mlfqmode() != 0) printf("  mlfqmode: 이미 MLFQ 모드\n");
  printf("   getlev() after mlfqmode() == %d  (기대: 0)\n\n", getlev());

  // ---------------------------------------------------------
  // [Test 3] 기본 MLFQ 동작 검증
  //   - 각 레벨별 실행 횟수 누적
  // ---------------------------------------------------------
  printf("[3] MLFQ 기본 스케줄링 분포\n");
  pid = fork_children();
  if (pid != parent) {
    for (i = 0; i < MAX_LEVEL; i++) hits[i] = 0;
    for (i = 0; i < NUM_LOOP; i++) {
      int lvl = getlev();
      if (lvl < 0 || lvl >= MAX_LEVEL) {
        printf(" Wrong level: %d\n", lvl);
        exit(1);
      }
      hits[lvl]++;
    }
    printf(" Child %d level hits: ", pid);
    for (i = 0; i < MAX_LEVEL; i++)
      printf("L%d=%d ", i, hits[i]);
    printf("\n");
  }
  exit_children();
  printf(" => [3] MLFQ 분포 확인 완료\n\n");

  // ---------------------------------------------------------
  // [Test 4] 자발적 yield 테스트
  //   - FCFS 모드: yield() 호출해도 아무 변화 없음을 확인
  //   - MLFQ 모드: yield() 호출 시 즉시 재스케줄링 됨을 확인
  // ---------------------------------------------------------
  printf("[4] 자발적 yield 테스트\n");

  // -- FCFS 모드에서 yield --
  fcfsmode();
  pid = fork_children();
  if (pid != parent) {
    printf("  [FCFS] child %d calling yield() twice...\n", pid);
    yield();
    yield();
    printf("  [FCFS] still here after yield(), pid=%d\n", pid);
  }
  exit_children();

  // -- MLFQ 모드에서 yield, 이 경우 사이에 timer interrupt로 인한 yield가 발생하고 demotion이 일어날 수도 있다는 점.. --
  mlfqmode();
  pid = fork_children();
  if (pid != parent) {
    printf("  [MLFQ] child %d calling yield() twice...\n", pid);
    printf("    before getlev()=%d, pid=%d\n", getlev(), pid);
    yield();
    printf("    after 1st yield getlev()=%d, pid=%d\n", getlev(), pid);
    yield();
    printf("    after 2nd yield getlev()=%d, pid=%d\n", getlev(), pid);
  }
  exit_children();
  printf(" => [4] yield 동작 확인 완료\n\n");

  // ---------------------------------------------------------
  // [Test 5] setpriority 테스트
  //   - 정상 호출, 범위 벗어난 호출, 잘못된 PID 호출 검증
  // ---------------------------------------------------------
  printf("[5] setpriority 테스트\n");

  pid = fork();
  if (pid == 0) {
    // 자식은 잠깐 대기
    for (i = 0; i < 1000000; i++) ;
    printf("  child %d exiting\n", getpid());
    exit(0);
  }
  // 부모: 정상범위 설정
  if (setpriority(pid, 2) == 0)
    printf("  setpriority(%d,2) 성공\n", pid);
  else
    printf("  setpriority(%d,2) 실패\n", pid);
  // 범위 벗어난 priority
  printf("  setpriority(%d,-1) => %d (기대: -2)\n", pid, setpriority(pid, -1));
  // 잘못된 PID
  printf("  setpriority(9999,2) => %d (기대: -1)\n", setpriority(9999,2));
  wait(0);
  printf(" => [4] setpriority 확인 완료\n\n");

  // ---------------------------------------------------------
  // [6] priority boost 동작 확인
  //   - MLFQ 모드에서 일정 시간(틱) 경과 후 레벨이 다시 0으로 부스트 되는지 확인
  // ---------------------------------------------------------
  printf("[6] priority boost 테스트\n");
  mlfqmode();
  pid = fork_children();
  if (pid != parent) {
    // 짧은 busy loop를 반복하면서 priority boost가 발생하는지 확인
    for (i = 0; i < 200; i++) {
      for (int j = 0; j < 500000000; j++) ;  
      if (i % 10 == 0)
        printf("  child %d at iteration %d, level=%d\n",
               getpid(), i, getlev());
    }
  }
  exit_children();
  printf(" => [6] priority boost 확인 완료\n\n");

  printf("=== 모든 테스트 완료 ===\n");
  exit(0);
}
