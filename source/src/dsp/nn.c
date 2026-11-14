#include "dsp/nn.h"

#include <math.h>
#include <string.h>
#include "dsp/simd.h"

static float sigm(float x) { return 1.0f / (1.0f + expf(-x)); }

int vf_nn_init(VfNn *n, const VfNnWeights *w)
{
    memset(n, 0, sizeof *n);
    if (!w || w->nb != VF_NN_NB || w->h <= 0 || w->h > VF_NN_MAXH) return -1;
    n->w = w;
    (void)vf_simd_avx2();
    return 0;
}

void vf_nn_reset(VfNn *n)
{
    memset(n->h1, 0, sizeof n->h1);
    memset(n->h2, 0, sizeof n->h2);
}

/* matrix [rows][cols] times vector + bias */
static void matvec(float *y, const float *W, const float *b, const float *x, int rows, int cols)
{
    for (int r = 0; r < rows; r++) y[r] = vf_dot(W + (long)r * cols, x, cols) + b[r];
}

static void gru(VfNn *n, float *h, const float *x, int in, const float *wih, const float *whh,
                const float *bih, const float *bhh)
{
    const int H = n->w->h;
    matvec(n->gi, wih, bih, x, 3 * H, in);
    matvec(n->gh, whh, bhh, h, 3 * H, H);
    for (int i = 0; i < H; i++) {
        const float r = sigm(n->gi[i] + n->gh[i]);
        const float z = sigm(n->gi[H + i] + n->gh[H + i]);
        const float c = tanhf(n->gi[2 * H + i] + r * n->gh[2 * H + i]);
        h[i] = (1.0f - z) * c + z * h[i];
    }
}

float vf_nn_step(VfNn *n, const float *feat, float *gains)
{
    const VfNnWeights *w = n->w;
    const int H = w->h, NB = w->nb;
    for (int k = 0; k < NB; k++) n->x[k] = (feat[k] - w->mean[k]) / w->std[k];
    float *d = n->cat, *o1 = n->cat + H, *o2 = n->cat + 2 * H;
    matvec(d, w->in_w, w->in_b, n->x, H, NB);
    for (int i = 0; i < H; i++) d[i] = tanhf(d[i]);
    gru(n, n->h1, d, H, w->g1_wih, w->g1_whh, w->g1_bih, w->g1_bhh);
    memcpy(o1, n->h1, (size_t)H * sizeof(float));
    gru(n, n->h2, n->cat, 2 * H, w->g2_wih, w->g2_whh, w->g2_bih, w->g2_bhh);
    memcpy(o2, n->h2, (size_t)H * sizeof(float));
    for (int k = 0; k < NB; k++) gains[k] = sigm(vf_dot(w->out_w + (long)k * 3 * H, n->cat, 3 * H) + w->out_b[k]);
    return sigm(vf_dot(w->out_w + (long)NB * 3 * H, n->cat, 3 * H) + w->out_b[NB]);
}
