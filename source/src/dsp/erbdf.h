/* Inference for the "erbdf" denoiser trained by ml/ (ml/se/models/erbdf.py - the exact model; every layer here is
 * checked against torch by tests/erbdf_golden.h). Two stages per 10 ms frame:
 *   1. ERB-band envelope: log energy of ERB-spaced triangular bands -> 2 convolutions over frequency -> GRU ->
 *      one sigmoid gain per band, interpolated back to the bins; Y1 = X * gain.
 *   2. Deep filtering of the low bins: a second GRU + a small conv encoder/decoder predict `order` complex taps per
 *      bin that filter Y1 over the current and the previous frames: Y = Y1 + sum_i C_i * Y1(t - i).
 * Real arithmetic only (re / im arrays), no allocation, one call per frame. Weights come from
 * src/dsp/erbdf_weights.c (ml/export_erbdf_c.py). The "nn" effect (fx_nn.c) runs it when `model` is 1. */
#ifndef VF_ERBDF_H
#define VF_ERBDF_H

#define VF_ERB_MAXV     161     /* bins the net sees (50 Hz apart) */
#define VF_ERB_MAXNB    40      /* ERB bands */
#define VF_ERB_MAXH     192
#define VF_ERB_MAXC     32      /* conv channels */
#define VF_ERB_MAXD     160     /* bins with deep filtering (multiple of 4) */
#define VF_ERB_MAXORDER 8

typedef struct VfErbDfWeights {
    int V, nb, h, C, D, order, hf_lo, hf_hi;   /* bins above V take the mean stage-1 gain of hf_lo..hf_hi-1 */
    float compress;                            /* DF input = X * |X|^(compress-1) */
    const float *erb_fwd;                      /* [V][nb] bin power -> band mean */
    const float *erb_inv;                      /* [nb][V] band gains -> bin gains */
    const float *mean, *std_;                  /* [nb] */
    const float *ea1_w, *ea1_b;                /* conv1d 1 -> C, k3 s1: [C][1][3], [C] */
    const float *ea2_w, *ea2_b;                /* conv1d C -> C, k3 s2: [C][C][3], [C] */
    const float *la_w, *la_b;                  /* [h][C * na], na = (nb - 1) / 2 + 1 */
    const float *g_wih, *g_whh, *g_bih, *g_bhh;      /* GRU h -> h: [3h][h] x2, [3h] x2 */
    const float *oe_w, *oe_b;                  /* [nb][h + C * na] */
    const float *ov_w, *ov_b;                  /* [1][h], [1] */
    const float *eb1_w, *eb1_b;                /* conv1d 2 -> C, k3 s2: [C][2][3] */
    const float *eb2_w, *eb2_b;                /* conv1d C -> C, k3 s2 */
    const float *lb_w, *lb_b;                  /* [h][C * D / 4] */
    const float *gd_wih, *gd_whh, *gd_bih, *gd_bhh; /* GRU h -> h */
    const float *ld_w, *ld_b;                  /* [C * D / 4][h] */
    const float *u1_w, *u1_b;                  /* ConvTranspose1d C -> C, k3 s2: [C][C][3] (in, out, k) */
    const float *u2_w, *u2_b;                  /* ConvTranspose1d C -> 2 * order: [C][2 * order][3] */
} VfErbDfWeights;

extern const VfErbDfWeights vf_erbdf_default;  /* generated: src/dsp/erbdf_weights.c */

typedef struct VfErbDf {
    const VfErbDfWeights *w;
    float h1[VF_ERB_MAXH], h2[VF_ERB_MAXH];                                  /* GRU states */
    float hist_re[(VF_ERB_MAXORDER - 1) * VF_ERB_MAXD];                      /* previous stage-1 frames, oldest first */
    float hist_im[(VF_ERB_MAXORDER - 1) * VF_ERB_MAXD];
    /* scratch (kept here so the audio thread never allocates) */
    float fa[VF_ERB_MAXNB], band[VF_ERB_MAXNB], gb[VF_ERB_MAXNB], gbin[VF_ERB_MAXV];
    float c1[VF_ERB_MAXC * VF_ERB_MAXNB], a[VF_ERB_MAXC * VF_ERB_MAXNB];
    float xin[2 * VF_ERB_MAXD], b1[VF_ERB_MAXC * VF_ERB_MAXD / 2], b2[VF_ERB_MAXC * VF_ERB_MAXD / 4];
    float e[VF_ERB_MAXH], cat[VF_ERB_MAXH + VF_ERB_MAXC * VF_ERB_MAXNB];
    float dd[VF_ERB_MAXC * VF_ERB_MAXD / 4], u1[VF_ERB_MAXC * VF_ERB_MAXD / 2];
    float taps[2 * VF_ERB_MAXORDER * VF_ERB_MAXD];
    float y1r[VF_ERB_MAXV], y1i[VF_ERB_MAXV];
    float gi[3 * VF_ERB_MAXH], gh[3 * VF_ERB_MAXH];
} VfErbDf;

int  vf_erbdf_init(VfErbDf *n, const VfErbDfWeights *w);       /* 0 = ok, -1 = dimensions do not fit the buffers */
void vf_erbdf_reset(VfErbDf *n);                               /* zero GRU states and filter history */

/* One frame. re / im: raw spectrum of the Vorbis-windowed 960-sample frame, bins 0..V-1 (unnormalised FFT).
 * Writes the enhanced bins 0..V-1 to yr / yi, the mean stage-1 gain of the high band to *hf_gain (the caller scales
 * the bins above V with it), and returns the voice probability. floor_lin: lowest stage-1 gain applied to any bin
 * (0 = none; the net itself never sees it). */
float vf_erbdf_step(VfErbDf *n, const float *re, const float *im, float floor_lin, float *yr, float *yi, float *hf_gain);

#endif
