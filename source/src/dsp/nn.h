/* Inference for the small recurrent denoiser trained by tools/nn/train.py (see tools/nn/nnlib.py for the
 * exact model): standardise -> dense(NB->H) tanh -> GRU(H) -> GRU(H) on [dense, gru1]
 * -> dense([dense, gru1, gru2] -> NB+1): NB sigmoid gains and one sigmoid voice probability.
 * GRU maths follow torch.nn.GRU: gate order (r, z, n), b_hn inside the reset product, h' = (1-z) n + z h.
 * Weights are row-major [out][in] as torch stores them. No allocation; one call per 10 ms frame. */
#ifndef VF_NN_H
#define VF_NN_H

#define VF_NN_NB   161          /* spectrum bins the net sees and predicts (0..8 kHz at 50 Hz) */
#define VF_NN_MAXH 192

typedef struct VfNnWeights {
    int nb, h;
    const float *mean, *std;                   /* [nb] */
    const float *in_w, *in_b;                  /* [h][nb], [h] */
    const float *g1_wih, *g1_whh, *g1_bih, *g1_bhh;   /* [3h][h], [3h][h], [3h], [3h] */
    const float *g2_wih, *g2_whh, *g2_bih, *g2_bhh;   /* [3h][2h], [3h][h], [3h], [3h] */
    const float *out_w, *out_b;                /* [nb+1][3h], [nb+1] */
} VfNnWeights;

extern const VfNnWeights vf_nn_default;        /* generated: src/dsp/nn_weights.c (tools/nn/export.py) */

typedef struct VfNn {
    const VfNnWeights *w;
    float h1[VF_NN_MAXH], h2[VF_NN_MAXH];
    float cat[3 * VF_NN_MAXH];                 /* [dense, gru1, gru2] */
    float x[VF_NN_NB];
    float gi[3 * VF_NN_MAXH], gh[3 * VF_NN_MAXH];
} VfNn;

/* 0 = ok, -1 = the weights do not fit the compiled limits. */
int   vf_nn_init(VfNn *n, const VfNnWeights *w);
void  vf_nn_reset(VfNn *n);                    /* clears the recurrent state */
/* feat: raw log10 power of bins 0..NB-1 (see fx_nn.c); writes NB gains in 0..1, returns the voice probability. */
float vf_nn_step(VfNn *n, const float *feat, float *gains);

#endif
