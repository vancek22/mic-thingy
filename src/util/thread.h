/* Minimal thread wrapper (Windows threads / pthreads); MinGW has no C11 <threads.h>. */
#ifndef VF_THREAD_H
#define VF_THREAD_H

typedef struct VfThread {
    void *handle;          /* HANDLE or pthread_t storage */
    int started;
    unsigned long long pt; /* pthread_t on POSIX */
} VfThread;

int  vf_thread_start(VfThread *t, void (*fn)(void *), void *arg);   /* 0 = ok */
void vf_thread_join(VfThread *t);                                    /* no-op if not started */

#endif
