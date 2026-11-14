#include "dsp/fx_nn.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "dsp/fft960.h"

#define FN   (2 * VF_FRAME_SIZE)     /* 960 */
#define NBIN (FN / 2 + 1)            /* 481 */

static const FxParamInfo k_params[] = {
    { "wet",   "",   0.0f,   1.0f,  1.0f,   0 },
    { "floor", "dB", -60.0f, 0.0f,  -40.0f, 0 },
};

typedef struct {
    VfFft fft;
    VfNn nn;
    float win[FN];
    float prev[VF_FRAME_SIZE];       /* previous input block = first half of the frame, also the delayed dry signal */
    float ola[VF_FRAME_SIZE];        /* second half of the previous output frame */
    float re[FN], im[FN];
    float feat[VF_NN_NB], gains[VF_NN_NB];
    float wet, floor_lin;
} NnState;

static int nn_init(Fx *fx, int sr)
{
    if (sr != VF_SAMPLE_RATE) return -1;
    NnState *s = (NnState *)calloc(1, sizeof *s);
    if (!s) return -1;
    const VfNnWeights *w = fx->state ? (const VfNnWeights *)fx->state : &vf_nn_default;
    if (vf_nn_init(&s->nn, w) != 0) { free(s); return -1; }
    vf_fft_init(&s->fft);
    for (int i = 0; i < FN; i++) {
        const double a = sin(3.14159265358979323846 * (i + 0.5) / FN);
        s->win[i] = (float)sin(0.5 * 3.14159265358979323846 * a * a);
    }
    s->wet = k_params[NN_PARAM_WET].def;
    s->floor_lin = vf_db_to_lin(k_params[NN_PARAM_FLOOR].def);
    fx->state = s;
    return 0;
}

static void nn_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    NnState *s = (NnState *)fx->state;
    if (n != VF_FRAME_SIZE) return;
    for (int i = 0; i < VF_FRAME_SIZE; i++) {
        s->re[i] = s->prev[i] * s->win[i];
        s->re[VF_FRAME_SIZE + i] = buf[i] * s->win[VF_FRAME_SIZE + i];
    }
    memset(s->im, 0, sizeof s->im);
    vf_fft(&s->fft, s->re, s->im, 0);
    const VfNnWeights *wt = s->nn.w;
    const int nb = wt->nb;
    for (int k = 0; k < nb; k++)
        s->feat[k] = log10f((s->re[k] * s->re[k] + s->im[k] * s->im[k]) / (float)VF_FRAME_SIZE + 1e-10f);
    const float vad = vf_nn_step(&s->nn, s->feat, s->gains);
    ctx->vad_prob = vad;
    ctx->vad_valid = 1;

    float hf = 0.0f;                             /* above the net's band: mean gain of hf_lo..hf_hi-1 */
    for (int k = wt->hf_lo; k < wt->hf_hi; k++) hf += s->gains[k];
    hf /= (float)(wt->hf_hi - wt->hf_lo);
    const float fl = s->floor_lin;
    for (int k = 0; k < NBIN; k++) {
        float g = k < nb ? s->gains[k] : hf;
        if (g < fl) g = fl;
        s->re[k] *= g; s->im[k] *= g;
        if (k > 0 && k < FN / 2) { s->re[FN - k] *= g; s->im[FN - k] *= g; }
    }
    vf_fft(&s->fft, s->re, s->im, 1);

    const float w = s->wet, d = 1.0f - w, sc = 1.0f / (float)FN;
    for (int i = 0; i < VF_FRAME_SIZE; i++) {
        const float y = s->ola[i] + s->re[i] * sc * s->win[i];
        s->ola[i] = s->re[VF_FRAME_SIZE + i] * sc * s->win[VF_FRAME_SIZE + i];
        const float dry = s->prev[i];
        s->prev[i] = buf[i];
        buf[i] = y * w + dry * d;
    }
}

static void nn_set_param(Fx *fx, int id, float v)
{
    NnState *s = (NnState *)fx->state;
    if (id == NN_PARAM_WET) s->wet = vf_clampf(v, 0.0f, 1.0f);
    if (id == NN_PARAM_FLOOR) s->floor_lin = vf_db_to_lin(vf_clampf(v, -60.0f, 0.0f));
}

static void nn_destroy(Fx *fx)
{
    free(fx->state);
    fx->state = NULL;
}

void fx_nn_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "nn";
    fx->init = nn_init;
    fx->process = nn_process;
    fx->set_param = nn_set_param;
    fx->destroy = nn_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, NN_DELAY_SAMPLES);
    atomic_init(&fx->bypass, 1);
}

void fx_nn_use_weights(Fx *fx, const VfNnWeights *w)
{
    fx->state = (void *)w;            /* picked up by init, replaced by the real state there */
}
