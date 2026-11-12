/* Common interface for every effect module.
 *
 * Lifecycle: init() runs on a normal thread (may allocate). process() and
 * set_param() run on the audio thread and must never allocate, lock, or do I/O.
 * destroy() runs on a normal thread after audio has stopped. */
#ifndef VF_FX_H
#define VF_FX_H

#include <stdatomic.h>

#define VF_SAMPLE_RATE 48000
#define VF_FRAME_SIZE  480   /* 10 ms @ 48 kHz; required by RNNoise */

/* Per-frame data shared along the chain (e.g. RNNoise writes the voice
 * probability, the noise gate reads it). Lives on the audio thread only. */
typedef struct FxFrameCtx {
    float vad_prob;          /* 0..1 */
} FxFrameCtx;

typedef struct Fx Fx;
struct Fx {
    const char *name;
    void *state;
    int  (*init)(Fx *fx, int sample_rate);                                /* 0 = ok */
    void (*process)(Fx *fx, float *buf, int n, FxFrameCtx *ctx);          /* in place */
    void (*set_param)(Fx *fx, int id, float value);                       /* may be NULL */
    void (*destroy)(Fx *fx);
    atomic_int bypass;                                                    /* 1 = skip */
};

#endif
