/* Lock-free single-producer / single-consumer ring buffer of floats.
 * One thread writes, one (other) thread reads; no locks, no allocation after init. */
#ifndef VF_RINGBUF_H
#define VF_RINGBUF_H

#include <stdatomic.h>

typedef struct VfRing {
    float *buf;
    unsigned cap;              /* power of two */
    unsigned mask;
    atomic_uint write_pos;     /* total samples written (wraps; unsigned arithmetic) */
    atomic_uint read_pos;      /* total samples read */
} VfRing;

int  vf_ring_init(VfRing *r, unsigned min_capacity);    /* control thread; 0 = ok */
void vf_ring_free(VfRing *r);

unsigned vf_ring_available(const VfRing *r);             /* readable samples (either side) */
unsigned vf_ring_space(const VfRing *r);                 /* writable samples (either side) */

/* Producer: writes all n samples or nothing; returns n or 0. */
unsigned vf_ring_write(VfRing *r, const float *src, unsigned n);
/* Consumer: reads up to n samples; returns the count read. dst may be NULL to skip. */
unsigned vf_ring_read(VfRing *r, float *dst, unsigned n);

#endif
