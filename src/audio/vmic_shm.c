#include "audio/vmic_shm.h"

#include <string.h>

unsigned long vf_vmic_shm_size(unsigned capacity)
{
    return (unsigned long)(sizeof(VfVmicShared) - sizeof(float) + (size_t)capacity * sizeof(float));
}

void vf_vmic_shm_init(VfVmicShared *s, unsigned capacity, unsigned rate)
{
    memset(s, 0, (size_t)vf_vmic_shm_size(capacity));
    s->magic = VF_VMIC_MAGIC;
    s->version = VF_VMIC_VERSION;
    s->capacity = capacity;
    s->rate = rate;
}

int vf_vmic_shm_valid(const VfVmicShared *s)
{
    return s && s->magic == VF_VMIC_MAGIC && s->version == VF_VMIC_VERSION && s->capacity >= 256 &&
           (s->capacity & (s->capacity - 1)) == 0;
}

unsigned vf_vmic_shm_level(const VfVmicShared *s)
{
    return atomic_load_explicit((atomic_uint *)&s->write_pos, memory_order_acquire) -
           atomic_load_explicit((atomic_uint *)&s->read_pos, memory_order_acquire);
}

unsigned vf_vmic_shm_write(VfVmicShared *s, const float *src, unsigned n)
{
    const unsigned w = atomic_load_explicit(&s->write_pos, memory_order_relaxed);
    const unsigned r = atomic_load_explicit(&s->read_pos, memory_order_acquire);
    if (s->capacity - (w - r) < n) {
        atomic_fetch_add_explicit(&s->overruns, 1, memory_order_relaxed);
        return 0;
    }
    const unsigned mask = s->capacity - 1;
    for (unsigned i = 0; i < n; i++) s->data[(w + i) & mask] = src[i];
    atomic_store_explicit(&s->write_pos, w + n, memory_order_release);
    atomic_fetch_add_explicit(&s->client_alive, 1, memory_order_relaxed);
    return n;
}

void vf_vmic_shm_consume(VfVmicShared *s, float *dst, unsigned n)
{
    const unsigned r = atomic_load_explicit(&s->read_pos, memory_order_relaxed);
    const unsigned w = atomic_load_explicit(&s->write_pos, memory_order_acquire);
    const unsigned avail = w - r;
    const unsigned mask = s->capacity - 1;
    const unsigned got = avail < n ? avail : n;
    for (unsigned i = 0; i < got; i++) dst[i] = s->data[(r + i) & mask];
    if (got < n) {
        memset(dst + got, 0, (size_t)(n - got) * sizeof *dst);
        atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&s->read_pos, r + got, memory_order_release);
}
