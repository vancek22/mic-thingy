#include "dsp/fx_warmth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "amount", "", 0.0f, 1.0f, 0.3f, 0 },
};

typedef struct {
    float sr, amount, drive, bias, bias_out, inv;
    Biquad shelf;
} WarmState;

static void update(WarmState *s)
{
    s->drive = 1.0f + 3.0f * s->amount;
    s->bias = 0.15f * s->amount;
    s->bias_out = tanhf(s->drive * s->bias);
    /* slope of tanh(d (x + b)) at x = 0, so small signals keep their level */
    s->inv = 1.0f / (s->drive * (1.0f - s->bias_out * s->bias_out));
    const float z1 = s->shelf.z1, z2 = s->shelf.z2;
    biquad_lowshelf(&s->shelf, s->sr, 150.0f, 4.0f * s->amount);
    s->shelf.z1 = z1; s->shelf.z2 = z2;
}

static int warm_init(Fx *fx, int sr)
{
    WarmState *s = (WarmState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->amount = k_params[WARMTH_AMOUNT].def;
    biquad_reset(&s->shelf);
    update(s);
    fx->state = s;
    return 0;
}

static void warm_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    WarmState *s = (WarmState *)fx->state;
    if (s->amount <= 0.0f) return;
    for (int i = 0; i < n; i++) {
        const float x = biquad_tick(&s->shelf, buf[i]);
        buf[i] = (tanhf(s->drive * (x + s->bias)) - s->bias_out) * s->inv;
    }
}

static void warm_set_param(Fx *fx, int id, float v)
{
    WarmState *s = (WarmState *)fx->state;
    if (id == WARMTH_AMOUNT) { s->amount = vf_clampf(v, 0.0f, 1.0f); update(s); }
}

static void warm_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_warmth_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "warmth";
    fx->init = warm_init;
    fx->process = warm_process;
    fx->set_param = warm_set_param;
    fx->destroy = warm_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 2;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
