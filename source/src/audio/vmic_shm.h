/* Shared-memory ring between mic thingy and a virtual-microphone driver (see driver/README.md).
 *
 * The driver (kernel, WaveRT miniport) creates a section holding a VfVmicShared header followed
 * by `capacity` float samples (mono, 48 kHz); the application maps it and is the only writer of
 * `write_pos`, the driver's timer/DPC the only writer of `read_pos`. Positions count samples and
 * wrap as unsigned 32-bit values; capacity is a power of two. When the application stops
 * writing the driver plays silence (and counts an underrun); when the ring is full the
 * application drops the block (and counts an overrun) instead of blocking.
 *
 * This is the application side plus a reference consumer used by the tests; the kernel side
 * re-implements `consume` with Interlocked* operations (same ordering rules). */
#ifndef VF_VMIC_SHM_H
#define VF_VMIC_SHM_H

#include <stdatomic.h>

#define VF_VMIC_MAGIC   0x434D4656u      /* "VFMC" */
#define VF_VMIC_VERSION 1u

typedef struct VfVmicShared {
    unsigned magic;
    unsigned version;
    unsigned capacity;               /* samples, power of two */
    unsigned rate;                   /* Hz, 48000 */
    atomic_uint write_pos;           /* application -> driver */
    atomic_uint read_pos;            /* driver -> application */
    atomic_uint underruns;           /* driver found the ring empty */
    atomic_uint overruns;            /* application found the ring full */
    atomic_uint client_alive;        /* application heartbeat counter (driver may fade out when it stops) */
    float data[1];                   /* capacity samples follow */
} VfVmicShared;

/* Bytes needed for a ring of `capacity` samples. */
unsigned long vf_vmic_shm_size(unsigned capacity);
/* Initialises a header in caller-provided memory (driver: the section; tests: malloc). */
void vf_vmic_shm_init(VfVmicShared *s, unsigned capacity, unsigned rate);
int  vf_vmic_shm_valid(const VfVmicShared *s);

/* Application side (one writer). Returns n, or 0 if there was no room (overrun counted). */
unsigned vf_vmic_shm_write(VfVmicShared *s, const float *src, unsigned n);
unsigned vf_vmic_shm_level(const VfVmicShared *s);          /* samples waiting for the driver */

/* Driver side reference (one reader): fills dst with n samples, silence on underrun. */
void vf_vmic_shm_consume(VfVmicShared *s, float *dst, unsigned n);

#endif
