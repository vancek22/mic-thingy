#include "audio/sink.h"

#include <string.h>

#define SINK_CAPACITY  16384      /* ~340 ms */
#define HYST_HIGH      480        /* drop when the average level is this far above target */
#define HYST_LOW       240        /* repeat when this far below */
#define TARGET_STEP    240        /* raise target by this after an underrun */
#define AVG_COEF       0.02f      /* level smoothing per consumer callback */

int vf_sink_init(VfSink *s, int target)
{
    memset(s, 0, sizeof *s);
    if (vf_ring_init(&s->ring, SINK_CAPACITY) != 0) return -1;
    s->target = target;
    s->target_max = (int)s->ring.cap / 2;
    s->level_avg = (float)target;
    atomic_init(&s->underruns, 0);
    atomic_init(&s->overruns, 0);
    atomic_init(&s->dropped, 0);
    atomic_init(&s->repeated, 0);
    atomic_init(&s->level, 0);
    atomic_init(&s->flush, 0);
    return 0;
}

void vf_sink_free(VfSink *s) { vf_ring_free(&s->ring); }

void vf_sink_flush(VfSink *s) { atomic_store(&s->flush, 1); }

void vf_sink_write(VfSink *s, const float *src, int n)
{
    if (vf_ring_write(&s->ring, src, (unsigned)n) == 0)
        atomic_fetch_add_explicit(&s->overruns, 1, memory_order_relaxed);
}

void vf_sink_read(VfSink *s, float *dst, int n)
{
    if (atomic_exchange_explicit(&s->flush, 0, memory_order_acq_rel)) {   /* consumer-side discard */
        vf_ring_read(&s->ring, NULL, vf_ring_available(&s->ring));
        s->primed = 0;
    }
    int avail = (int)vf_ring_available(&s->ring);
    atomic_store_explicit(&s->level, avail, memory_order_relaxed);

    if (!s->primed) {                              /* wait for the cushion to build up */
        if (avail < s->target + n) { memset(dst, 0, (size_t)n * sizeof *dst); return; }
        s->primed = 1;
        s->level_avg = (float)avail;
    }

    s->level_avg += AVG_COEF * ((float)avail - s->level_avg);

    if (avail < n) {                               /* starved: play what we have, re-prime */
        int got = (int)vf_ring_read(&s->ring, dst, (unsigned)avail);
        memset(dst + got, 0, (size_t)(n - got) * sizeof *dst);
        atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
        if (s->target + TARGET_STEP <= s->target_max) s->target += TARGET_STEP;
        s->primed = 0;
        return;
    }

    if (s->level_avg > (float)(s->target + HYST_HIGH) && avail > n) {
        /* consumer clock slower than producer: skip one sample */
        vf_ring_read(&s->ring, dst, (unsigned)n);
        vf_ring_read(&s->ring, NULL, 1);
        s->level_avg -= 1.0f;
        atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed);
    } else if (s->level_avg < (float)(s->target - HYST_LOW) && n > 1) {
        /* consumer clock faster: repeat one sample */
        vf_ring_read(&s->ring, dst, (unsigned)(n - 1));
        dst[n - 1] = dst[n - 2];
        s->level_avg += 1.0f;
        atomic_fetch_add_explicit(&s->repeated, 1, memory_order_relaxed);
    } else {
        vf_ring_read(&s->ring, dst, (unsigned)n);
    }
}
