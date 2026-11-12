#include "dsp/fx_compressor.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "threshold", "dB", -60.0f, 0.0f,    -20.0f },
    { "ratio",     "",   1.0f,   20.0f,   3.0f },
    { "attack",    "ms", 0.1f,   200.0f,  8.0f },
    { "release",   "ms", 10.0f,  2000.0f, 150.0f },
    { "makeup",    "dB", 0.0f,   24.0f,   3.0f },
    { "knee",      "dB", 0.0f,   24.0f,   6.0f },
};

#define DETECTOR_RELEASE_MS 20.0f   /* peak detector fall time (fixed) */

typedef struct {
    float sr;
    float thr_db, ratio, makeup_lin, knee_db;
    float atk, rel, det_rel;
    float env;        /* peak detector (linear) */
    float gr_db;      /* smoothed gain reduction (dB, <= 0) */
} CompState;

static int comp_init(Fx *fx, int sr)
{
    CompState *s = (CompState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->thr_db = k_params[COMP_THRESHOLD_DB].def;
    s->ratio = k_params[COMP_RATIO].def;
    s->atk = vf_coef_ms(k_params[COMP_ATTACK_MS].def, s->sr);
    s->rel = vf_coef_ms(k_params[COMP_RELEASE_MS].def, s->sr);
    s->makeup_lin = vf_db_to_lin(k_params[COMP_MAKEUP_DB].def);
    s->knee_db = k_params[COMP_KNEE_DB].def;
    s->det_rel = vf_coef_ms(DETECTOR_RELEASE_MS, s->sr);
    fx->state = s;
    return 0;
}

/* Static curve: gain reduction (dB, <= 0) for an input level in dB. */
static float gain_computer(const CompState *s, float lvl_db)
{
    const float slope = 1.0f / s->ratio - 1.0f;       /* <= 0 */
    const float over = lvl_db - s->thr_db;
    const float k = s->knee_db;
    if (2.0f * over < -k) return 0.0f;
    if (k > 0.0f && 2.0f * fabsf(over) <= k) {
        const float t = over + k * 0.5f;
        return slope * t * t / (2.0f * k);
    }
    return slope * over;
}

static void comp_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    CompState *s = (CompState *)fx->state;
    float env = s->env, gr = s->gr_db;
    for (int i = 0; i < n; i++) {
        const float x = buf[i];
        const float a = fabsf(x);
        env = a > env ? a : a + s->det_rel * (env - a);
        const float target = gain_computer(s, vf_lin_to_db(env));
        const float c = target < gr ? s->atk : s->rel;  /* more reduction = attack */
        gr = target + c * (gr - target);
        buf[i] = x * vf_db_to_lin(gr) * s->makeup_lin;
    }
    s->env = env;
    s->gr_db = gr;
    ctx->comp_gr_db = gr;
}

static void comp_set_param(Fx *fx, int id, float v)
{
    CompState *s = (CompState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    switch (id) {
    case COMP_THRESHOLD_DB: s->thr_db = v; break;
    case COMP_RATIO:        s->ratio = v; break;
    case COMP_ATTACK_MS:    s->atk = vf_coef_ms(v, s->sr); break;
    case COMP_RELEASE_MS:   s->rel = vf_coef_ms(v, s->sr); break;
    case COMP_MAKEUP_DB:    s->makeup_lin = vf_db_to_lin(v); break;
    case COMP_KNEE_DB:      s->knee_db = v; break;
    default: break;
    }
}

static void comp_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_compressor_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "comp";
    fx->init = comp_init;
    fx->process = comp_process;
    fx->set_param = comp_set_param;
    fx->destroy = comp_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    atomic_init(&fx->bypass, 0);
}
