#include <errno.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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
#define JB_PC 7

typedef enum thread_state {
    THREAD_EXITED,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
} thread_state_t;

typedef struct tcb {
    pthread_t thread_id;
    thread_state_t state;

    jmp_buf context;
    void *stack;

    void *(*start_routine)(void *);
    void *arg;

    void *retval;
    int joiner;
} tcb_t;

typedef struct my_sem {
    int value;
    int waiters[MAX_THREADS];
    int num_waiters;
    int initialized;
} my_sem_t;

static tcb_t thread_table[MAX_THREADS];
static int ready = 0;
static int curr_thread = 0;

static long int i64_ptr_mangle(long int p);

static void init_threads();
static void schedule_threads();
static void thread_wrapper();

static void sigalrm_handler(int sig);

/* locking helpers */
void lock();
void unlock();

/* actual thread functions */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
void pthread_exit(void *value_ptr);
pthread_t pthread_self();
int pthread_join(pthread_t thread, void **value_ptr);

/* semaphore functions */
int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);

static long int i64_ptr_mangle(long int p) {
    // From Canvas
    long int ret;
    asm(" mov %1, %%rax;\n"
        " xor %%fs:0x30, %%rax;"
        " rol $0x11, %%rax;"
        " mov %%rax, %0;"
        : "=r"(ret)
        : "r"(p)
        : "%rax");
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
        thread_table[i].retval = NULL;
        thread_table[i].joiner = -1;
    }

    curr_thread = 0;
    thread_table[curr_thread].thread_id = 0;
    thread_table[curr_thread].state = THREAD_RUNNING;
    thread_table[curr_thread].stack = NULL;
    thread_table[curr_thread].start_routine = NULL;
    thread_table[curr_thread].arg = NULL;
    thread_table[curr_thread].retval = NULL;
    thread_table[curr_thread].joiner = -1;

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

        if (thread_table[prev_thread].state == THREAD_RUNNING) {
            thread_table[prev_thread].state = THREAD_READY;
        }
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

static void sigalrm_handler(int sig) { schedule_threads(); }

void lock() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

void unlock() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
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
        return EAGAIN;
    }

    void *stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        return ENOMEM;
    }

    tcb_t *tcb = &thread_table[new_thread];
    tcb->thread_id = new_thread;
    tcb->state = THREAD_READY;
    tcb->stack = stack;
    tcb->start_routine = start_routine;
    tcb->arg = arg;
    tcb->retval = NULL;
    tcb->joiner = -1;

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
    tcb->retval = value_ptr;

    if (tcb->joiner >= 0 && tcb->joiner < MAX_THREADS) {
        int j = tcb->joiner;
        if (thread_table[j].state == THREAD_BLOCKED) {
            thread_table[j].state = THREAD_READY;
        }
        tcb->joiner = -1;
    }

    for (size_t i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_EXITED) {
            schedule_threads();
            break;
        }
    }

    schedule_threads();
}

pthread_t pthread_self() {
    if (!ready) {
        init_threads();
    }
    return thread_table[curr_thread].thread_id;
}

int pthread_join(pthread_t thread, void **value_ptr) {
    if (!ready) {
        init_threads();
    }

    if (thread < 0 || thread >= MAX_THREADS) {
        return ESRCH;
    }

    if (thread_table[thread].thread_id != thread) {
        return ESRCH;
    }

    if (thread == curr_thread) {
        return EDEADLK;
    }

    lock();

    if (thread_table[thread].state == THREAD_EXITED) {
        if (value_ptr) {
            *value_ptr = thread_table[thread].retval;
        }

        if (thread_table[thread].stack) {
            free(thread_table[thread].stack);
            thread_table[thread].stack = NULL;
        }

        thread_table[thread].thread_id = 0;
        thread_table[thread].start_routine = NULL;
        thread_table[thread].arg = NULL;
        thread_table[thread].retval = NULL;
        thread_table[thread].joiner = -1;

        unlock();
        return 0;
    }

    if (thread_table[thread].joiner != -1) {
        unlock();
        return -1;
    }

    thread_table[thread].joiner = curr_thread;
    thread_table[curr_thread].state = THREAD_BLOCKED;

    unlock();
    schedule_threads();
    lock();

    if (value_ptr) {
        *value_ptr = thread_table[thread].retval;
    }

    if (thread_table[thread].stack) {
        free(thread_table[thread].stack);
        thread_table[thread].stack = NULL;
    }

    thread_table[thread].thread_id = 0;
    thread_table[thread].start_routine = NULL;
    thread_table[thread].arg = NULL;
    thread_table[thread].retval = NULL;
    thread_table[thread].joiner = -1;

    unlock();
    return 0;
}

int sem_init(sem_t *sem, int pshared, unsigned int value) {
    if (!ready) {
        init_threads();
    }

    if (sem == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (pshared != 0) {
        errno = EINVAL;
        return -1;
    }

    my_sem_t *s = (my_sem_t *)sem;

    s->value = (int)value;
    s->num_waiters = 0;
    s->initialized = 1;

    for (int i = 0; i < MAX_THREADS; i++) {
        s->waiters[i] = -1;
    }

    return 0;
}

int sem_destroy(sem_t *sem) {
    if (sem == NULL) {
        errno = EINVAL;
        return -1;
    }

    my_sem_t *s = (my_sem_t *)sem;

    if (!s->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (s->num_waiters != 0) {
        errno = EBUSY;
        return -1;
    }

    s->value = 0;
    s->num_waiters = 0;
    s->initialized = 0;

    for (int i = 0; i < MAX_THREADS; i++) {
        s->waiters[i] = -1;
    }

    return 0;
}

int sem_wait(sem_t *sem) {
    if (!ready) {
        init_threads();
    }

    if (sem == NULL) {
        errno = EINVAL;
        return -1;
    }

    my_sem_t *s = (my_sem_t *)sem;

    if (!s->initialized) {
        errno = EINVAL;
        return -1;
    }

    lock();
    
    if (s->value > 0) {
        s->value--;
        unlock();
        return 0;
    }

    if (s->num_waiters >= MAX_THREADS) {
        unlock();
        errno = EAGAIN;
        return -1;
    }

    s->waiters[s->num_waiters++] = curr_thread;
    thread_table[curr_thread].state = THREAD_BLOCKED;
    
    unlock();
    schedule_threads();
    return 0;
}

int sem_post(sem_t *sem) {
    if (!ready) {
        init_threads();
    }

    if (sem == NULL) {
        errno = EINVAL;
        return -1;
    }

    my_sem_t *s = (my_sem_t *)sem;

    if (!s->initialized) {
        errno = EINVAL;
        return -1;
    }

    lock();

    if (s->num_waiters > 0) {
        int waiter = s->waiters[0];

        for (int i = 1; i < s->num_waiters; i++) {
            s->waiters[i - 1] = s->waiters[i];
        }

        s->num_waiters--;
        s->waiters[s->num_waiters] = -1;

        if (waiter >= 0 && waiter < MAX_THREADS) {
            if (thread_table[waiter].state == THREAD_BLOCKED) {
                thread_table[waiter].state = THREAD_READY;
            }
        }
    } else {
        s->value++;
    }

    unlock();
    return 0;
}
