#include "dsp/fx_limiter.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "ceiling", "dB", -12.0f, 0.0f,    -1.0f, 0 },
    { "release", "ms", 5.0f,   1000.0f, 60.0f, 0 },
    { "lookahead", "ms", 0.0f, 1.5f,    1.5f,  0 },
};

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
    unsigned dq[RING];    /* monotonic queue of target indices (window minimum) */
    unsigned head, tail;
    int   d;              /* current look-ahead, samples (0..LIMITER_LOOKAHEAD) */
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
    s->d = LIMITER_LOOKAHEAD;
    s->sum = s->d;
    s->env = 1.0f;
    fx->state = s;
    return 0;
}

static void lim_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    LimState *s = (LimState *)fx->state;
    const float c = s->ceiling_lin;
    unsigned idx = s->idx;
    float gmin = 1.0f;                            /* deepest gain reduction of this frame (meter) */
    for (int i = 0; i < n; i++, idx++) {
        const unsigned w = idx & MASK;
        const float x = buf[i];
        const float a = fabsf(x);
        s->dly[w] = x;
        const float t = a > c ? c / a : 1.0f;
        s->tgt[w] = t;

        /* min over the last D+1 targets with a monotonic queue: O(1) per sample instead of O(D) */
        while (s->tail != s->head && s->tgt[s->dq[(s->tail - 1) & MASK] & MASK] >= t) s->tail--;
        s->dq[s->tail++ & MASK] = idx;
        while ((unsigned)(idx - s->dq[s->head & MASK]) > (unsigned)s->d) s->head++;
        float m = s->tgt[s->dq[s->head & MASK] & MASK];
        if (m > 1.0f) m = 1.0f;
        s->sum += (double)m - (double)s->mn[(idx - (unsigned)s->d) & MASK];
        s->mn[w] = m;

        const float g = s->d > 0 ? (float)(s->sum / s->d) : m;
        s->env = g < s->env ? g : g + s->rel * (s->env - g);
        if (s->env < gmin) gmin = s->env;

        float y = s->dly[(idx - (unsigned)s->d) & MASK] * s->env;
        if (y > c) y = c; else if (y < -c) y = -c;   /* float round-off safety */
        buf[i] = y;
    }
    s->idx = idx;
    if (gmin < ctx->limiter_gain) ctx->limiter_gain = gmin;
}

static void lim_set_param(Fx *fx, int id, float v)
{
    LimState *s = (LimState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == LIM_CEILING_DB) s->ceiling_lin = vf_db_to_lin(v);
    else if (id == LIM_RELEASE_MS) s->rel = vf_coef_ms(v, s->sr);
    else if (id == LIM_LOOKAHEAD_MS) {
        /* new window length: restart the (tiny) history so the sums stay consistent */
        const int d = (int)(v * 0.001f * s->sr + 0.5f);
        s->d = d > LIMITER_LOOKAHEAD ? LIMITER_LOOKAHEAD : d;
        for (int i = 0; i < RING; i++) { s->tgt[i] = 1.0f; s->mn[i] = 1.0f; s->dly[i] = 0.0f; }
        s->head = s->tail = 0;
        s->sum = s->d;
        atomic_store_explicit(&fx->latency, s->d, memory_order_relaxed);
    }
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
    atomic_init(&fx->latency, LIMITER_LOOKAHEAD);
    atomic_init(&fx->bypass, 0);
}
