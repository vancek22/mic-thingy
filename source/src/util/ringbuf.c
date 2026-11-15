#include "util/ringbuf.h"

#include <stdlib.h>
#include <string.h>

int vf_ring_init(VfRing *r, unsigned min_capacity)
{
    unsigned cap = 1;
    while (cap < min_capacity) cap <<= 1;
    r->buf = (float *)calloc(cap, sizeof(float));
    if (!r->buf) return -1;
    r->cap = cap;
    r->mask = cap - 1;
    atomic_init(&r->write_pos, 0);
    atomic_init(&r->read_pos, 0);
    return 0;
}

void vf_ring_free(VfRing *r)
{
    free(r->buf);
    r->buf = NULL;
}

unsigned vf_ring_available(const VfRing *r)
{
    unsigned w = atomic_load_explicit(&((VfRing *)r)->write_pos, memory_order_acquire);
    unsigned rd = atomic_load_explicit(&((VfRing *)r)->read_pos, memory_order_acquire);
    return w - rd;
}

unsigned vf_ring_space(const VfRing *r)
{
    return r->cap - vf_ring_available(r);
}

unsigned vf_ring_write(VfRing *r, const float *src, unsigned n)
{
    unsigned w = atomic_load_explicit(&r->write_pos, memory_order_relaxed);
    unsigned rd = atomic_load_explicit(&r->read_pos, memory_order_acquire);
    if (r->cap - (w - rd) < n) return 0;
    const unsigned at = w & r->mask, first = n < r->cap - at ? n : r->cap - at;   /* n <= cap: checked above */
    memcpy(r->buf + at, src, first * sizeof(float));
    memcpy(r->buf, src + first, (n - first) * sizeof(float));
    atomic_store_explicit(&r->write_pos, w + n, memory_order_release);
    return n;
}

unsigned vf_ring_read(VfRing *r, float *dst, unsigned n)
{
    unsigned rd = atomic_load_explicit(&r->read_pos, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&r->write_pos, memory_order_acquire);
    unsigned avail = w - rd;
    if (n > avail) n = avail;
    if (n > r->cap) n = r->cap;                     /* cannot happen with one writer; keeps the copies in bounds */
    if (dst) {
        const unsigned at = rd & r->mask, first = n < r->cap - at ? n : r->cap - at;
        memcpy(dst, r->buf + at, first * sizeof(float));
        memcpy(dst + first, r->buf, (n - first) * sizeof(float));
    }
    atomic_store_explicit(&r->read_pos, rd + n, memory_order_release);
    return n;
}
