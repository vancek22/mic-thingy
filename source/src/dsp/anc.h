/* Active noise cancellation - SKELETON (feed-forward FxLMS), not part of the voice chain.
 *
 * Idea: a microphone hears the ambient noise (the reference x); the loudspeaker plays an
 * anti-noise y = W * x that arrives at the ear in anti-phase. The filter W adapts with the
 * filtered-x LMS rule from the residual e measured at the ear (error microphone):
 *     y(n) = sum_k w[k] x(n-k)            e(n) = d(n) + (S * y)(n)
 *     w[k] += mu * e(n) * x'(n-k),        x' = S^ * x
 * S is the secondary path (loudspeaker -> ear microphone), S^ its estimate (identify it once
 * with vf_anc_identify() or supply it).
 *
 * Hard limits, said plainly:
 *  - it needs an ERROR microphone at the ear; with only one microphone (the usual headset) the
 *    adaptive mode cannot work and only the fixed "invert" mode is possible;
 *  - the noise must reach the ear later than the anti-noise can be produced: total delay of
 *    capture + processing + playback must be shorter than the acoustic path. WASAPI shared mode
 *    is 30-45 ms, an ear cup is ~0.1 ms away - so over a normal Windows audio path this only
 *    works for very low, steady frequencies (hum). Real ANC runs on a DSP with < 100 us latency.
 * This module is the algorithm and a simulation-tested building block, not a product feature. */
#ifndef VF_ANC_H
#define VF_ANC_H

#define VF_ANC_MAX_TAPS  512
#define VF_ANC_MAX_SEC   128

typedef struct VfAnc {
    int taps, sec_taps;
    float mu;
    float w[VF_ANC_MAX_TAPS];              /* anti-noise filter */
    float sec[VF_ANC_MAX_SEC];             /* S^ */
    float xh[VF_ANC_MAX_TAPS + VF_ANC_MAX_SEC];    /* reference history, newest first */
    float fxh[VF_ANC_MAX_TAPS];            /* filtered reference history, newest first */
    int invert_only;                       /* 1: y = -gain * x (no adaptation) */
    float gain;
} VfAnc;

/* taps <= VF_ANC_MAX_TAPS; sec (S^) may be NULL (identity, 1 tap); sec_taps <= VF_ANC_MAX_SEC. */
void vf_anc_init(VfAnc *a, int taps, float mu, const float *sec, int sec_taps);
void vf_anc_set_invert(VfAnc *a, int on, float gain);
/* One sample: x = reference, e = residual at the ear (ignored when invert_only or `adapt` is 0).
 * Returns the anti-noise sample y to play. */
float vf_anc_tick(VfAnc *a, float x, float e, int adapt);

/* Estimates S^ from a probe: plays `probe` through `play` (callback: gives the ear-microphone sample
 * for each probe sample) with NLMS. Offline helper for tests/tools. */
typedef float (*VfAncPlant)(void *ctx, float y);
void vf_anc_identify(float *sec_out, int sec_taps, VfAncPlant plant, void *ctx, int samples);

#endif
