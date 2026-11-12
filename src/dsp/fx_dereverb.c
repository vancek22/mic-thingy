#include "dsp/fx_dereverb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "strength", "",   1.0f,  4.0f,  2.0f,  0 },
    { "depth",    "dB", 0.0f,  30.0f, 15.0f, 0 },
    { "reach",    "dB", 10.0f, 40.0f, 12.0f, 0 },
};

typedef struct {
    float sr, strength, depth_db, reach_db;
    float e;              /* smoothed signal power (3 ms) */
    float peak_db;        /* recent speech level, decays 10 dB/s */
    float gain;           /* smoothed linear gain */
    float k_e, k_atk, k_rel, decay_per_sample;
} DrState;

static int dr_init(Fx *fx, int sr)
{
    DrState *s = (DrState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->strength = k_params[DEREV_STRENGTH].def;
    s->depth_db = k_params[DEREV_DEPTH_DB].def;
    s->reach_db = k_params[DEREV_REACH_DB].def;
    s->k_e = vf_coef_ms(3.0f, s->sr);
    s->k_atk = vf_coef_ms(2.0f, s->sr);      /* gain falls quickly ... */
    s->k_rel = vf_coef_ms(40.0f, s->sr);     /* ... and recovers at speech onsets within a few ms */
    s->decay_per_sample = 10.0f / s->sr;
    s->peak_db = -60.0f;
    s->gain = 1.0f;
    fx->state = s;
    return 0;
}

static void dr_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    DrState *s = (DrState *)fx->state;
    for (int i = 0; i < n; i++) {
        const float x = buf[i];
        s->e = x * x + s->k_e * (s->e - x * x);
        const float lvl = 10.0f * log10f(s->e + 1e-12f);              /* dB re full scale */
        if (lvl > s->peak_db) s->peak_db = lvl; else s->peak_db -= s->decay_per_sample;
        if (s->peak_db < -70.0f) s->peak_db = -70.0f;
        const float thr = s->peak_db - s->reach_db;
        float red = 0.0f;
        if (lvl < thr) {
            red = (thr - lvl) * (s->strength - 1.0f);
            if (red > s->depth_db) red = s->depth_db;
        }
        const float target = vf_db_to_lin(-red);
        const float k = target < s->gain ? s->k_atk : s->k_rel;
        s->gain = target + k * (s->gain - target);
        buf[i] = x * s->gain;
    }
}

static void dr_set_param(Fx *fx, int id, float v)
{
    DrState *s = (DrState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == DEREV_STRENGTH) s->strength = v;
    else if (id == DEREV_DEPTH_DB) s->depth_db = v;
    else if (id == DEREV_REACH_DB) s->reach_db = v;
}

static void dr_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_dereverb_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "dereverb";
    fx->init = dr_init;
    fx->process = dr_process;
    fx->set_param = dr_set_param;
    fx->destroy = dr_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
