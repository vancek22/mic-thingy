#include "dsp/fx_highpass.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "freq", "Hz", 20.0f, 300.0f, 80.0f },
};

/* 4th-order Butterworth = two cascaded sections with these Q values. */
#define Q1 0.54119610f
#define Q2 1.30656296f

typedef struct { Biquad s1, s2; float sr; } HpState;

static void hp_set_freq(HpState *s, float f)
{
    f = vf_clampf(f, k_params[0].min, k_params[0].max);
    biquad_highpass(&s->s1, s->sr, f, Q1);
    biquad_highpass(&s->s2, s->sr, f, Q2);
}

static int hp_init(Fx *fx, int sr)
{
    HpState *s = (HpState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    biquad_reset(&s->s1);
    biquad_reset(&s->s2);
    hp_set_freq(s, k_params[0].def);
    fx->state = s;
    return 0;
}

static void hp_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    HpState *s = (HpState *)fx->state;
    for (int i = 0; i < n; i++)
        buf[i] = biquad_tick(&s->s2, biquad_tick(&s->s1, buf[i]));
}

static void hp_set_param(Fx *fx, int id, float v)
{
    if (id == HP_FREQ) hp_set_freq((HpState *)fx->state, v);
}

static void hp_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_highpass_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "highpass";
    fx->init = hp_init;
    fx->process = hp_process;
    fx->set_param = hp_set_param;
    fx->destroy = hp_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    atomic_init(&fx->bypass, 0);
}
