// user/thread.c

#include "kernel/types.h"
#include "user/user.h"
#include "user/thread.h"

#define STACK_SIZE 4096      /* 스택으로 쓸 실제 크기 (1 페이지) */
#define ALIGNMENT  4096      /* 페이지 크기 */
#define NULL 0

/*
 * alloc_stack: malloc 기반으로 page-aligned 스택 블록을 만든 뒤
 *   (aligned_ptr - sizeof(void*)) 위치에 원래 malloc 포인터를 저장.
 * 반환된 aligned_ptr을 나중에 free_stack() 에 넘기면 원포인터로 해제 가능.
 */
#define STACK_SZ (PGSIZE * N)  // N페이지 크기만큼 스택 할당

static void *
alloc_stack(void)
{
    // 스택 크기 + 정렬 오버헤드 + 원본 포인터 저장 공간
    uint64 tot = STACK_SIZE + ALIGNMENT - 1 + sizeof(void*);
    void *orig = malloc(tot);
    if (!orig)
        return 0;

    // orig 바로 다음 바이트를 기준으로 페이지 경계 위쪽으로
    uint64 raw = (uint64)orig + sizeof(void*);
    uint64 aligned = (raw + ALIGNMENT - 1) & ~(uint64)(ALIGNMENT - 1);

    // aligned 바로 앞에 orig 저장
    ((void**)aligned)[-1] = orig;
    return (void*)aligned;
}

static void 
free_stack(void *stack) {
    if (stack) {
        /* aligned 바로 앞에 저장된 orig를 꺼내서 free */
        void *orig = ((void**)stack)[-1];
        free(orig);
    }
}

int
thread_create(void (*start_routine)(void*, void*), void *arg1, void *arg2)
{
    void *stack = alloc_stack();
    if (!stack)
        return -1;
    int pid = clone(start_routine, arg1, arg2, stack);
    
    if (pid < 0) {
        free_stack(stack);
        return -1;
    }
    return pid;
}


int
thread_join(void)
{
    void *stack;
    int pid = join(&stack);
    if (pid < 0)
        return -1;

    free_stack(stack);
    return pid;
}