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
#define VF_MAX_PARAMS  8

/* Per-frame data shared along the chain. Lives on the audio thread only. */
typedef struct FxFrameCtx {
    float vad_prob;          /* 0..1, valid only when vad_valid != 0 (RNNoise ran) */
    int   vad_valid;
    float gate_gain;         /* linear gain applied by the gate (meter) */
    float comp_gr_db;        /* compressor gain reduction in dB, <= 0 (meter) */
} FxFrameCtx;

typedef struct FxParamInfo {
    const char *name;        /* console name, lowercase, no spaces */
    const char *unit;        /* "dB", "ms", "Hz", "" */
    float min, max, def;
} FxParamInfo;

typedef struct Fx Fx;
struct Fx {
    const char *name;
    void *state;
    int  (*init)(Fx *fx, int sample_rate);                                /* 0 = ok */
    void (*process)(Fx *fx, float *buf, int n, FxFrameCtx *ctx);          /* in place */
    void (*set_param)(Fx *fx, int id, float value);                       /* audio thread */
    void (*destroy)(Fx *fx);
    const FxParamInfo *params;                                            /* n_params entries */
    int n_params;
    atomic_int bypass;                                                    /* 1 = skip */
};

#endif
