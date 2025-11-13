#include <stdlib.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>

#define MAX_THREADS 128

#define STACK_SIZE 32767

#define THREAD_TIMER_PERIOD 50000 // 50 ms 

#define JB_RBX 0
#define JB_RBP 1
#define JB_R12 2
#define JB_R13 3
#define JB_R14 4
#define JB_R15 5
#define JB_RSP 6
#define JB_PC  7

typedef enum thread_state {
    THREAD_EXITED,
    THREAD_READY,
    THREAD_RUNNING,
} thread_state_t;

typedef struct tcb {
    pthread_t thread_id;
    thread_state_t state;

    jmp_buf context;
    void *stack;
    
    void *(*start_routine)(void *);
    void *arg;
} tcb_t;

static tcb_t thread_table[MAX_THREADS];
static int ready = 0;
static int curr_thread = 0;

static long int i64_ptr_mangle(long int p);

static void init_threads();
static void schedule_threads();
static void thread_wrapper();

static void sigalrm_handler(int sig);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg);
void pthread_exit(void *value_ptr);
pthread_t pthread_self();

static long int i64_ptr_mangle(long int p) {
    // From Canvas
    long int ret;
    asm(" mov %1, %%rax;\n"
        " xor %%fs:0x30, %%rax;"
        " rol $0x11, %%rax;"
        " mov %%rax, %0;"
    : "=r"(ret)
    : "r"(p)
    : "%rax"
    );
    return ret;
}

static void init_threads() {
    if (ready) {
        return;
    }
    
    for (size_t i = 0; i < MAX_THREADS; i++) {
        thread_table[i].thread_id = 0;
        thread_table[i].state = THREAD_EXITED;
        thread_table[i].stack = NULL;
        thread_table[i].start_routine = NULL;
        thread_table[i].arg = NULL;
    }
    
    curr_thread = 0;
    thread_table[curr_thread].thread_id = 0;
    thread_table[curr_thread].state = THREAD_RUNNING;    
    thread_table[curr_thread].stack = NULL;
    thread_table[curr_thread].start_routine = NULL;
    thread_table[curr_thread].arg = NULL;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;

    sa.sa_flags = SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = THREAD_TIMER_PERIOD;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = THREAD_TIMER_PERIOD;
    setitimer(ITIMER_REAL, &timer, NULL);
    
    ready = 1;
}

static void thread_wrapper() {
    tcb_t *tcb = &thread_table[curr_thread];
    void *result = tcb->start_routine(tcb->arg);
    pthread_exit(result);
}

static void schedule_threads() {
    int prev_thread = curr_thread;
    
    if (thread_table[prev_thread].state != THREAD_EXITED) {
        int ret = setjmp(thread_table[prev_thread].context);
        if (ret != 0) {
            return;
        }
        
        thread_table[prev_thread].state = THREAD_READY;
    }
    
    int next_thread = -1;
    int search_start = (curr_thread + 1) % MAX_THREADS;
    
    for (int i = 0; i < MAX_THREADS; i++) {
        int index = (search_start + i) % MAX_THREADS;
        if (thread_table[index].state == THREAD_READY) {
            next_thread = index;
            break;
        }
    }
    
    if (next_thread == -1) {
        if (thread_table[curr_thread].state == THREAD_READY) {
            next_thread = curr_thread;
        } else {
            exit(1);
        }
    }
    
    curr_thread = next_thread;
    thread_table[curr_thread].state = THREAD_RUNNING;
    
    longjmp(thread_table[curr_thread].context, 1);
}

static void sigalrm_handler(int sig) {
    schedule_threads();
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void*), void *arg) {
    if (!ready) {
        init_threads();
    }
    
    int new_thread = -1;
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == THREAD_EXITED) {
            new_thread = i;
            break;
        }
    }
    
    if (new_thread == -1) {
        return -1;
    }
    
    void *stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        return -1;
    }
    
    tcb_t *tcb = &thread_table[new_thread];
    tcb->thread_id = new_thread;
    tcb->state = THREAD_READY;
    tcb->stack = stack;
    tcb->start_routine = start_routine;
    tcb->arg = arg;
    
    setjmp(tcb->context);
    
    void *stack_top = (void *)((unsigned long)stack + STACK_SIZE);
    
    long int *jb = (long int *)tcb->context; 
    jb[JB_PC] = i64_ptr_mangle((long int)thread_wrapper);
    
    unsigned long stack_address = (unsigned long)stack_top;
    stack_address &= ~0xFUL;
    jb[JB_RSP] = i64_ptr_mangle(stack_address);
    
    *thread = tcb->thread_id;
    
    return 0;
}

void pthread_exit(void *value_ptr) {
    tcb_t *tcb = &thread_table[curr_thread];
    tcb->state = THREAD_EXITED;
    
    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_EXITED) {
            schedule_threads();
            break;
        }
    }
    
    exit(0);
}

pthread_t pthread_self() {
    if (!ready) {
        init_threads();
    }
    return thread_table[curr_thread].thread_id;
}
