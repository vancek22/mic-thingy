#include "dsp/erbdf.h"

#include <math.h>
#include <string.h>
#include "dsp/simd.h"

#define FEAT_SCALE 480.0f                    /* n_fft / 2: power is normalised by the window energy, as in training */

static float sigm(float x) { return 1.0f / (1.0f + expf(-x)); }

int vf_erbdf_init(VfErbDf *n, const VfErbDfWeights *w)
{
    memset(n, 0, sizeof *n);
    if (!w || w->V <= 0 || w->V > VF_ERB_MAXV || w->nb < 2 || w->nb > VF_ERB_MAXNB || w->h <= 0 || w->h > VF_ERB_MAXH ||
        w->C <= 0 || w->C > VF_ERB_MAXC || w->D <= 0 || w->D > VF_ERB_MAXD || w->D % 4 != 0 || w->D > w->V ||
        w->order < 2 || w->order > VF_ERB_MAXORDER || w->hf_lo < 0 || w->hf_lo >= w->hf_hi || w->hf_hi > w->V) return -1;
    n->w = w;
    (void)vf_simd_avx2();
    return 0;
}

void vf_erbdf_reset(VfErbDf *n)
{
    memset(n->h1, 0, sizeof n->h1);
    memset(n->h2, 0, sizeof n->h2);
    memset(n->hist_re, 0, sizeof n->hist_re);
    memset(n->hist_im, 0, sizeof n->hist_im);
}

/* y = W x + b, W [rows][cols] row-major */
static void dense(float *y, const float *W, const float *b, const float *x, int rows, int cols)
{
    for (int r = 0; r < rows; r++) y[r] = vf_dot(W + (long)r * cols, x, cols) + b[r];
}

/* torch.nn.GRU maths (gate order r, z, n; b_hn inside the reset product), one step, in place on h */
static void gru(VfErbDf *n, float *h, const float *x, int in, const float *wih, const float *whh, const float *bih,
                const float *bhh)
{
    const int H = n->w->h;
    dense(n->gi, wih, bih, x, 3 * H, in);
    dense(n->gh, whh, bhh, h, 3 * H, H);
    for (int i = 0; i < H; i++) {
        const float r = sigm(n->gi[i] + n->gh[i]);
        const float z = sigm(n->gi[H + i] + n->gh[H + i]);
        const float c = tanhf(n->gi[2 * H + i] + r * n->gh[2 * H + i]);
        h[i] = (1.0f - z) * c + z * h[i];
    }
}

/* Conv1d, kernel 3, padding 1: in [cin][lin] -> out [cout][lout], lout = (lin - 1) / stride + 1; w [cout][cin][3]. */
static void conv3(float *out, const float *in, int cin, int lin, const float *w, const float *b, int cout, int stride, int relu)
{
    const int lout = (lin - 1) / stride + 1;
    for (int o = 0; o < cout; o++) {
        for (int j = 0; j < lout; j++) {
            float s = b[o];
            for (int c = 0; c < cin; c++) {
                const float *wk = w + ((long)o * cin + c) * 3;
                const float *x = in + (long)c * lin;
                for (int k = 0; k < 3; k++) {
                    const int p = j * stride + k - 1;
                    if (p >= 0 && p < lin) s += wk[k] * x[p];
                }
            }
            out[(long)o * lout + j] = relu && s < 0.0f ? 0.0f : s;
        }
    }
}

/* ConvTranspose1d, kernel 3, stride 2, padding 1, output_padding 1: in [cin][lin] -> out [cout][2 lin];
 * w [cin][cout][3]; every input sample i adds w[k] * in[i] to output position 2 i - 1 + k. */
static void convt3(float *out, const float *in, int cin, int lin, const float *w, const float *b, int cout)
{
    const int lout = 2 * lin;
    for (int o = 0; o < cout; o++)
        for (int j = 0; j < lout; j++) out[(long)o * lout + j] = b[o];
    for (int c = 0; c < cin; c++)
        for (int o = 0; o < cout; o++) {
            const float *wk = w + ((long)c * cout + o) * 3;
            for (int i = 0; i < lin; i++) {
                const float x = in[(long)c * lin + i];
                for (int k = 0; k < 3; k++) {
                    const int j = 2 * i - 1 + k;
                    if (j >= 0 && j < lout) out[(long)o * lout + j] += wk[k] * x;
                }
            }
        }
}

float vf_erbdf_step(VfErbDf *n, const float *re, const float *im, float floor_lin, float *yr, float *yi, float *hf_gain)
{
    const VfErbDfWeights *w = n->w;
    const int V = w->V, nb = w->nb, H = w->h, C = w->C, D = w->D, order = w->order;
    const int na = (nb - 1) / 2 + 1, D2 = D / 2, D4 = D / 4;

    /* --- band features: weighted mean power per ERB band, log10, standardised --- */
    for (int j = 0; j < nb; j++) n->band[j] = 0.0f;
    for (int k = 0; k < V; k++) {
        const float p = re[k] * re[k] + im[k] * im[k];
        const float *row = w->erb_fwd + (long)k * nb;
        for (int j = 0; j < nb; j++) n->band[j] += row[j] * p;
    }
    for (int j = 0; j < nb; j++) n->fa[j] = (log10f(n->band[j] / FEAT_SCALE + 1e-10f) - w->mean[j]) / w->std_[j];

    /* --- envelope encoder: conv over frequency, then dense; a = the post-ReLU conv output [C][na] --- */
    conv3(n->c1, n->fa, 1, nb, w->ea1_w, w->ea1_b, C, 1, 1);
    conv3(n->a, n->c1, C, nb, w->ea2_w, w->ea2_b, C, 2, 1);
    dense(n->e, w->la_w, w->la_b, n->a, H, C * na);

    /* --- deep-filter encoder: compressed complex spectrum of the low bins -> 2 stride-2 convs --- */
    for (int k = 0; k < D; k++) {
        const float m = powf(re[k] * re[k] + im[k] * im[k] + 1e-12f, 0.5f * (w->compress - 1.0f));
        n->xin[k] = re[k] * m;
        n->xin[D + k] = im[k] * m;
    }
    conv3(n->b1, n->xin, 2, D, w->eb1_w, w->eb1_b, C, 2, 1);            /* [C][D/2] */
    conv3(n->b2, n->b1, C, D2, w->eb2_w, w->eb2_b, C, 2, 1);            /* [C][D/4] */
    dense(n->cat, w->lb_w, w->lb_b, n->b2, H, C * D4);                   /* cat is free scratch here */
    for (int i = 0; i < H; i++) n->e[i] += n->cat[i];

    /* --- shared GRU, voice probability, band gains --- */
    for (int i = 0; i < H; i++) if (n->e[i] < 0.0f) n->e[i] = 0.0f;
    gru(n, n->h1, n->e, H, w->g_wih, w->g_whh, w->g_bih, w->g_bhh);
    const float vad = sigm(vf_dot(w->ov_w, n->h1, H) + w->ov_b[0]);
    memcpy(n->cat, n->h1, (size_t)H * sizeof(float));
    memcpy(n->cat + H, n->a, (size_t)(C * na) * sizeof(float));
    for (int j = 0; j < nb; j++) n->gb[j] = sigm(vf_dot(w->oe_w + (long)j * (H + C * na), n->cat, H + C * na) + w->oe_b[j]);
    for (int k = 0; k < V; k++) {
        float g = 0.0f;
        for (int j = 0; j < nb; j++) g += n->gb[j] * w->erb_inv[(long)j * V + k];
        n->gbin[k] = g;
    }
    float hf = 0.0f;
    for (int k = w->hf_lo; k < w->hf_hi; k++) hf += n->gbin[k];
    *hf_gain = hf / (float)(w->hf_hi - w->hf_lo);
    for (int k = 0; k < V; k++) {
        const float g = n->gbin[k] < floor_lin ? floor_lin : n->gbin[k];
        n->y1r[k] = re[k] * g;
        n->y1i[k] = im[k] * g;
    }

    /* --- deep-filter taps: second GRU on the same context, conv decoder with skip connections --- */
    gru(n, n->h2, n->h1, H, w->gd_wih, w->gd_whh, w->gd_bih, w->gd_bhh);
    dense(n->dd, w->ld_w, w->ld_b, n->h2, C * D4, H);
    for (int i = 0; i < C * D4; i++) {
        float v = n->dd[i] > 0.0f ? n->dd[i] : 0.0f;
        n->dd[i] = v + n->b2[i];
    }
    convt3(n->u1, n->dd, C, D4, w->u1_w, w->u1_b, C);                    /* [C][D/2] */
    for (int i = 0; i < C * D2; i++) {
        const float v = n->u1[i] > 0.0f ? n->u1[i] : 0.0f;
        n->u1[i] = v + n->b1[i];
    }
    convt3(n->taps, n->u1, C, D2, w->u2_w, w->u2_b, 2 * order);          /* [2 * order][D], channel = ri * order + tap */
    for (int i = 0; i < 2 * order * D; i++) n->taps[i] = tanhf(n->taps[i]);

    /* --- Y[k] = Y1[k] + sum_i C_i[k] * Y1[k, t - i] on the low bins --- */
    for (int k = 0; k < D; k++) {
        float ar = n->y1r[k], ai = n->y1i[k];
        for (int i = 0; i < order; i++) {
            const float sr = i == 0 ? n->y1r[k] : n->hist_re[(order - 1 - i) * D + k];
            const float si = i == 0 ? n->y1i[k] : n->hist_im[(order - 1 - i) * D + k];
            const float cr = n->taps[i * D + k], ci = n->taps[(order + i) * D + k];
            ar += cr * sr - ci * si;
            ai += cr * si + ci * sr;
        }
        yr[k] = ar;
        yi[k] = ai;
    }
    for (int k = D; k < V; k++) { yr[k] = n->y1r[k]; yi[k] = n->y1i[k]; }

    /* history: drop the oldest frame, append this stage-1 frame */
    for (int j = 0; j + 1 < order - 1; j++) {
        memcpy(n->hist_re + (long)j * D, n->hist_re + (long)(j + 1) * D, (size_t)D * sizeof(float));
        memcpy(n->hist_im + (long)j * D, n->hist_im + (long)(j + 1) * D, (size_t)D * sizeof(float));
    }
    memcpy(n->hist_re + (long)(order - 2) * D, n->y1r, (size_t)D * sizeof(float));
    memcpy(n->hist_im + (long)(order - 2) * D, n->y1i, (size_t)D * sizeof(float));
    return vad;
}
