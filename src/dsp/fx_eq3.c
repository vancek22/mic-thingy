#include "dsp/fx_eq3.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "low",      "dB", -12.0f, 12.0f,   0.0f },
    { "mid",      "dB", -12.0f, 12.0f,   0.0f },
    { "high",     "dB", -12.0f, 12.0f,   0.0f },
    { "lowfreq",  "Hz", 50.0f,  500.0f,  120.0f },
    { "midfreq",  "Hz", 200.0f, 5000.0f, 1500.0f },
    { "midq",     "",   0.3f,   4.0f,    1.0f },
    { "highfreq", "Hz", 2000.0f, 12000.0f, 6000.0f },
};

typedef struct {
    float sr;
    float v[7];                 /* current parameter values, indexed by id */
    Biquad lo, mid, hi;
} EqState;

static void eq_update(EqState *s)
{
    biquad_lowshelf(&s->lo, s->sr, s->v[EQ_LOW_FREQ], s->v[EQ_LOW_DB]);
    biquad_peaking(&s->mid, s->sr, s->v[EQ_MID_FREQ], s->v[EQ_MID_Q], s->v[EQ_MID_DB]);
    biquad_highshelf(&s->hi, s->sr, s->v[EQ_HIGH_FREQ], s->v[EQ_HIGH_DB]);
}

static int eq_init(Fx *fx, int sr)
{
    EqState *s = (EqState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    for (int i = 0; i < 7; i++) s->v[i] = k_params[i].def;
    biquad_reset(&s->lo); biquad_reset(&s->mid); biquad_reset(&s->hi);
    eq_update(s);
    fx->state = s;
    return 0;
}

static void eq_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    EqState *s = (EqState *)fx->state;
    for (int i = 0; i < n; i++)
        buf[i] = biquad_tick(&s->hi, biquad_tick(&s->mid, biquad_tick(&s->lo, buf[i])));
}

static void eq_set_param(Fx *fx, int id, float v)
{
    EqState *s = (EqState *)fx->state;
    if (id < 0 || id >= 7) return;
    s->v[id] = vf_clampf(v, k_params[id].min, k_params[id].max);
    eq_update(s);                /* filter memory is kept, so no hard glitch */
}

static void eq_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_eq3_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "eq";
    fx->init = eq_init;
    fx->process = eq_process;
    fx->set_param = eq_set_param;
    fx->destroy = eq_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    atomic_init(&fx->bypass, 0);
}
