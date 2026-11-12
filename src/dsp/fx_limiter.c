#include "dsp/fx_limiter.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "ceiling", "dB", -12.0f, 0.0f,    -1.0f },
    { "release", "ms", 5.0f,   1000.0f, 60.0f },
};

#define D     LIMITER_LOOKAHEAD
#define RING  128
#define MASK  (RING - 1)

/* Gain at output time n is the mean of D consecutive window-minima of the
 * per-sample target gain, so it never exceeds the target of the sample being
 * output (delayed by D). Release is then slowed with a one-pole that can only
 * rise towards that value, so the ceiling is always respected. */
typedef struct {
    float sr, ceiling_lin, rel;
    float dly[RING];      /* input history */
    float tgt[RING];      /* per-sample target gain */
    float mn[RING];       /* window minimum of tgt */
    double sum;           /* sum of the last D values of mn */
    float env;            /* smoothed gain */
    unsigned idx;
} LimState;

static int lim_init(Fx *fx, int sr)
{
    LimState *s = (LimState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->ceiling_lin = vf_db_to_lin(k_params[LIM_CEILING_DB].def);
    s->rel = vf_coef_ms(k_params[LIM_RELEASE_MS].def, s->sr);
    for (int i = 0; i < RING; i++) { s->tgt[i] = 1.0f; s->mn[i] = 1.0f; }
    s->sum = D;
    s->env = 1.0f;
    fx->state = s;
    return 0;
}

static void lim_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    LimState *s = (LimState *)fx->state;
    const float c = s->ceiling_lin;
    unsigned idx = s->idx;
    for (int i = 0; i < n; i++, idx++) {
        const unsigned w = idx & MASK;
        const float x = buf[i];
        const float a = fabsf(x);
        s->dly[w] = x;
        s->tgt[w] = a > c ? c / a : 1.0f;

        float m = 1.0f;                           /* min over the last D+1 targets */
        for (unsigned k = 0; k <= D; k++) {
            const float t = s->tgt[(idx - k) & MASK];
            if (t < m) m = t;
        }
        s->sum += (double)m - (double)s->mn[(idx - D) & MASK];
        s->mn[w] = m;

        const float g = (float)(s->sum / D);
        s->env = g < s->env ? g : g + s->rel * (s->env - g);

        float y = s->dly[(idx - D) & MASK] * s->env;
        if (y > c) y = c; else if (y < -c) y = -c;   /* float round-off safety */
        buf[i] = y;
    }
    s->idx = idx;
}

static void lim_set_param(Fx *fx, int id, float v)
{
    LimState *s = (LimState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == LIM_CEILING_DB) s->ceiling_lin = vf_db_to_lin(v);
    else if (id == LIM_RELEASE_MS) s->rel = vf_coef_ms(v, s->sr);
}

static void lim_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_limiter_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "limiter";
    fx->init = lim_init;
    fx->process = lim_process;
    fx->set_param = lim_set_param;
    fx->destroy = lim_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->latency = LIMITER_LOOKAHEAD;
    atomic_init(&fx->bypass, 0);
}
