/* VfSink: hands processed audio from the engine's audio thread to another
 * audio clock (a monitor device, the PipeWire virtual source). The two clocks
 * drift, so the consumer keeps the buffer level near `target` by dropping or
 * repeating a single sample now and then (inaudible at these rates).
 * Lock-free: one producer thread, one consumer thread. */
#ifndef VF_SINK_H
#define VF_SINK_H

#include <stdatomic.h>
#include "util/ringbuf.h"

typedef struct VfSink {
    VfRing ring;
    /* consumer-thread state */
    int   target;            /* desired fill level, samples */
    int   target_max;
    int   primed;
    float level_avg;
    /* statistics (read by the UI) */
    atomic_uint underruns;   /* consumer found the buffer empty (target is raised) */
    atomic_uint overruns;    /* producer found the buffer full (block dropped) */
    atomic_uint dropped;     /* single samples dropped for drift */
    atomic_uint repeated;    /* single samples repeated for drift */
    atomic_int  level;       /* last fill level seen by the consumer */
    atomic_int  flush;       /* set by any thread: the consumer discards what is buffered and re-primes */
} VfSink;

/* target: initial fill level in samples (e.g. 960 = 20 ms). 0 = ok. */
int  vf_sink_init(VfSink *s, int target);
void vf_sink_free(VfSink *s);

void vf_sink_write(VfSink *s, const float *src, int n);   /* producer */
void vf_sink_read(VfSink *s, float *dst, int n);          /* consumer: always fills n */
void vf_sink_flush(VfSink *s);                            /* e.g. when a consumer switches to this sink */
#endif
