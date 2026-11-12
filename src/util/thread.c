#include "util/thread.h"

#ifdef _WIN32
#include <windows.h>

typedef struct { void (*fn)(void *); void *arg; } Start;

static DWORD WINAPI tramp(LPVOID p)
{
    Start s = *(Start *)p;
    HeapFree(GetProcessHeap(), 0, p);
    s.fn(s.arg);
    return 0;
}

int vf_thread_start(VfThread *t, void (*fn)(void *), void *arg)
{
    t->started = 0;
    Start *s = (Start *)HeapAlloc(GetProcessHeap(), 0, sizeof *s);
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    t->handle = CreateThread(NULL, 0, tramp, s, 0, NULL);
    if (!t->handle) { HeapFree(GetProcessHeap(), 0, s); return -1; }
    t->started = 1;
    return 0;
}

void vf_thread_join(VfThread *t)
{
    if (!t->started) return;
    WaitForSingleObject((HANDLE)t->handle, INFINITE);
    CloseHandle((HANDLE)t->handle);
    t->started = 0;
}
#else
#include <pthread.h>
#include <stdlib.h>

typedef struct { void (*fn)(void *); void *arg; } Start;

static void *tramp(void *p)
{
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int vf_thread_start(VfThread *t, void (*fn)(void *), void *arg)
{
    t->started = 0;
    Start *s = (Start *)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn; s->arg = arg;
    pthread_t pt;
    if (pthread_create(&pt, NULL, tramp, s) != 0) { free(s); return -1; }
    t->pt = (unsigned long long)pt;
    t->started = 1;
    return 0;
}

void vf_thread_join(VfThread *t)
{
    if (!t->started) return;
    pthread_join((pthread_t)t->pt, NULL);
    t->started = 0;
}
#endif
