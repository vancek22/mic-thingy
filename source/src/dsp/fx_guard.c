#include "dsp/fx_guard.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "threshold", "dB", -30.0f, 0.0f,    -10.0f, 0 },
    { "ratio",     "",   2.0f,   20.0f,   8.0f,   0 },
    { "release",   "ms", 50.0f,  2000.0f, 400.0f, 0 },
};

typedef struct {
    float sr, thr, ratio, atk, rel;
    float gain;            /* smoothed linear gain */
} GuardState;

static int guard_init(Fx *fx, int sr)
{
    GuardState *s = (GuardState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->thr = vf_db_to_lin(k_params[GUARD_THRESHOLD_DB].def);
    s->ratio = k_params[GUARD_RATIO].def;
    s->atk = vf_coef_ms(0.3f, s->sr);
    s->rel = vf_coef_ms(k_params[GUARD_RELEASE_MS].def, s->sr);
    s->gain = 1.0f;
    fx->state = s;
    return 0;
}

static void guard_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    GuardState *s = (GuardState *)fx->state;
    const float expo = 1.0f / s->ratio - 1.0f;
    for (int i = 0; i < n; i++) {
        const float a = fabsf(buf[i]);
        const float target = a > s->thr ? powf(a / s->thr, expo) : 1.0f;     /* static curve */
        s->gain = target < s->gain ? target + s->atk * (s->gain - target)      /* fast attack */
                                   : target + s->rel * (s->gain - target);     /* slow recovery */
        buf[i] *= s->gain;
    }
}

static void guard_set_param(Fx *fx, int id, float v)
{
    GuardState *s = (GuardState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == GUARD_THRESHOLD_DB) s->thr = vf_db_to_lin(v);
    else if (id == GUARD_RATIO) s->ratio = v;
    else if (id == GUARD_RELEASE_MS) s->rel = vf_coef_ms(v, s->sr);
}

static void guard_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_guard_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "guard";
    fx->init = guard_init;
    fx->process = guard_process;
    fx->set_param = guard_set_param;
    fx->destroy = guard_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
