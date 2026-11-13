#include "dsp/fx_deesser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "freq",      "Hz", 4000.0f, 10000.0f, 6500.0f, 0 },
    { "threshold", "dB", -50.0f,  -5.0f,    -28.0f,  0 },
    { "ratio",     "",   2.0f,    10.0f,    4.0f,    0 },
    { "range",     "dB", 0.0f,    20.0f,    10.0f,   0 },
};

typedef struct {
    float sr, freq, thr_db, ratio, range_db;
    float atk, rel, env;
    Biquad hp;                /* low-pass at freq; the band above is x - lp(x), so lp + band == x exactly */
} DeState;

static int de_init(Fx *fx, int sr)
{
    DeState *s = (DeState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->freq = k_params[DEESS_FREQ].def;
    s->thr_db = k_params[DEESS_THRESHOLD_DB].def;
    s->ratio = k_params[DEESS_RATIO].def;
    s->range_db = k_params[DEESS_RANGE_DB].def;
    s->atk = vf_coef_ms(0.5f, s->sr);
    s->rel = vf_coef_ms(30.0f, s->sr);
    biquad_lowpass(&s->hp, s->sr, s->freq, 0.7071f);
    fx->state = s;
    return 0;
}

static void de_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    DeState *s = (DeState *)fx->state;
    for (int i = 0; i < n; i++) {
        const float x = buf[i];
        const float h = x - biquad_tick(&s->hp, x);            /* sibilance band = input minus its low-pass: exact complement */
        const float a = fabsf(h);
        s->env = a > s->env ? a + s->atk * (s->env - a) : a + s->rel * (s->env - a);
        float g = 1.0f;
        if (s->env > 1e-5f) {
            const float over = vf_lin_to_db(s->env) - s->thr_db;
            if (over > 0.0f) {
                float red = over * (1.0f - 1.0f / s->ratio);
                if (red > s->range_db) red = s->range_db;
                g = vf_db_to_lin(-red);
            }
        }
        buf[i] = x - h * (1.0f - g);                            /* only the HF band is reduced */
    }
}

static void de_set_param(Fx *fx, int id, float v)
{
    DeState *s = (DeState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == DEESS_FREQ) {
        s->freq = v;
        const float z1 = s->hp.z1, z2 = s->hp.z2;
        biquad_lowpass(&s->hp, s->sr, v, 0.7071f);
        s->hp.z1 = z1; s->hp.z2 = z2;
    } else if (id == DEESS_THRESHOLD_DB) s->thr_db = v;
    else if (id == DEESS_RATIO) s->ratio = v;
    else if (id == DEESS_RANGE_DB) s->range_db = v;
}

static void de_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_deesser_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "deesser";
    fx->init = de_init;
    fx->process = de_process;
    fx->set_param = de_set_param;
    fx->destroy = de_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
