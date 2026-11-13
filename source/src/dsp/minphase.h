/* Minimum-phase FIR design from a magnitude response (cepstral method), used by the
 * low-latency RNNoise mode: the per-bin gains RNNoise computes for a frame become a short
 * causal filter that is applied to that same frame - no overlap-add delay.
 *
 * vf_minphase_init() allocates (normal thread); vf_minphase_design() does not allocate,
 * so it may run on the audio thread. */
#ifndef VF_MINPHASE_H
#define VF_MINPHASE_H

#define VF_MP_N      1024            /* FFT size: 46.875 Hz per bin at 48 kHz */
#define VF_MP_TAPS   128             /* FIR length */
#define VF_MP_TAPER  32               /* last taps fade out (cosine) to limit truncation ripple */

typedef struct VfMinPhase {
    float cs[VF_MP_N / 2];           /* twiddles cos/sin(2 pi k / N) */
    float sn[VF_MP_N / 2];
    unsigned short rev[VF_MP_N];     /* bit reversal */
    float re[VF_MP_N], im[VF_MP_N];  /* work */
    float mag[VF_MP_N / 2 + 1];
} VfMinPhase;

void vf_minphase_init(VfMinPhase *mp);

/* gains: n_gains linear magnitudes at k * (48000 / (2 * (n_gains - 1))) Hz (RNNoise: 481 values,
 * 50 Hz apart; bins the model leaves at 0 above 20 kHz are replaced by the last real value).
 * Writes VF_MP_TAPS coefficients to h (h[0] = first tap). Gains are floored at 0.001 (-60 dB). */
void vf_minphase_design(VfMinPhase *mp, const float *gains, int n_gains, float *h);

#endif
