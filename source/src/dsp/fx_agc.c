#include "dsp/fx_agc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "target",  "dB", -40.0f, -6.0f,  -20.0f,  0 },
    { "maxgain", "dB", 0.0f,   30.0f,  12.0f,   0 },
    { "speed",   "ms", 200.0f, 10000.0f, 1500.0f, 0 },
};

typedef struct {
    float target_db, max_gain_db, speed_ms;
    float gain_db;        /* gain applied at the end of the previous frame */
} AgcState;

static int agc_init(Fx *fx, int sr)
{
    (void)sr;
    AgcState *s = (AgcState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->target_db = k_params[AGC_TARGET_DB].def;
    s->max_gain_db = k_params[AGC_MAX_GAIN_DB].def;
    s->speed_ms = k_params[AGC_SPEED_MS].def;
    fx->state = s;
    return 0;
}

static void agc_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    AgcState *s = (AgcState *)fx->state;
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)buf[i] * buf[i];
    const float lvl = vf_lin_to_db((float)sqrt(e / n));
    const int speech = ctx->vad_valid ? ctx->vad_prob > 0.5f : lvl > -50.0f;

    float next = s->gain_db;
    if (speech) {
        float want = s->target_db - lvl;                          /* lvl is measured before our gain */
        if (want > s->max_gain_db) want = s->max_gain_db;
        if (want < -12.0f) want = -12.0f;
        const float frame_ms = 1000.0f * (float)n / 48000.0f;
        float k = 1.0f - expf(-frame_ms / s->speed_ms);
        if (want < next) k = fminf(1.0f, k * 4.0f);               /* loud parts come down faster */
        next += (want - next) * k;
    }
    /* linear gain ramp across the frame: no steps */
    const float g0 = vf_db_to_lin(s->gain_db), g1 = vf_db_to_lin(next);
    for (int i = 0; i < n; i++) buf[i] *= g0 + (g1 - g0) * ((float)(i + 1) / (float)n);
    s->gain_db = next;
}

static void agc_set_param(Fx *fx, int id, float v)
{
    AgcState *s = (AgcState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == AGC_TARGET_DB) s->target_db = v;
    else if (id == AGC_MAX_GAIN_DB) s->max_gain_db = v;
    else if (id == AGC_SPEED_MS) s->speed_ms = v;
}

static void agc_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_agc_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "agc";
    fx->init = agc_init;
    fx->process = agc_process;
    fx->set_param = agc_set_param;
    fx->destroy = agc_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
