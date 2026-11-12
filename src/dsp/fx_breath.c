#include "dsp/fx_breath.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "breath", "dB", 0.0f, 30.0f, 12.0f, 0 },
    { "clicks", "dB", 0.0f, 30.0f, 14.0f, 0 },
    { "sens",   "",   0.0f, 1.0f,  0.5f,  0 },
};

#define SUB 48                    /* 1 ms detector blocks */

typedef struct {
    float sr, breath_db, click_db, sens;
    Biquad hp;                    /* 3 kHz high-pass for the transient detector */
    float hf_bg;                  /* slow HF background level (RMS per 1 ms block) */
    float speech_db;              /* recent speech level (frame RMS while VAD is high) */
    int   click_hold;             /* samples left of a click duck */
    float breath_gain_db;         /* smoothed breath reduction, 0 .. -breath_db */
    float gain;                   /* smoothed linear gain (all ducks) */
} BrState;

static int br_init(Fx *fx, int sr)
{
    BrState *s = (BrState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->breath_db = k_params[BREATH_DEPTH_DB].def;
    s->click_db = k_params[BREATH_CLICK_DB].def;
    s->sens = k_params[BREATH_SENS].def;
    biquad_highpass(&s->hp, s->sr, 3000.0f, 0.7f);
    s->hf_bg = 1e-4f;
    s->speech_db = -30.0f;
    s->gain = 1.0f;
    fx->state = s;
    return 0;
}

static void br_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    BrState *s = (BrState *)fx->state;
    if (!ctx->vad_valid) return;                         /* no voice information: do nothing */
    const float vad = ctx->vad_prob;

    /* frame level and speech tracking */
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)buf[i] * buf[i];
    const float lvl = vf_lin_to_db((float)sqrt(e / n));
    if (vad > 0.8f) s->speech_db += (lvl - s->speech_db) * (lvl > s->speech_db ? 0.3f : 0.02f);

    /* breath: not clearly silence, not clearly speech, and well below the speech level */
    const float lo = 0.05f, hi = 0.35f + 0.3f * s->sens;
    const int breathy = vad > lo && vad < hi && lvl > -60.0f && lvl < s->speech_db - 8.0f;
    const float want = breathy ? -s->breath_db : 0.0f;
    s->breath_gain_db += (want - s->breath_gain_db) * (breathy ? 0.35f : 0.2f);   /* per frame (10 ms) */
    const float breath_lin = vf_db_to_lin(s->breath_gain_db);

    const float click_lin = vf_db_to_lin(-s->click_db);
    const float k_atk = vf_coef_ms(0.5f, s->sr), k_rel = vf_coef_ms(8.0f, s->sr);
    const float ratio = 12.0f - 8.0f * s->sens;          /* HF burst vs background (more sensitive = lower) */
    for (int b = 0; b + SUB <= n; b += SUB) {
        double hf = 0;
        float hx[SUB];
        for (int i = 0; i < SUB; i++) { hx[i] = biquad_tick(&s->hp, buf[b + i]); hf += (double)hx[i] * hx[i]; }
        const float hrms = (float)sqrt(hf / SUB);
        if (vad < 0.5f && hrms > 0.004f && hrms > s->hf_bg * ratio) s->click_hold = (int)(0.025f * s->sr);
        else s->hf_bg += (hrms - s->hf_bg) * 0.004f;     /* background follows, ~250 ms */
        const float target = (s->click_hold > 0 ? click_lin : 1.0f) * breath_lin;
        if (s->click_hold > 0) s->click_hold -= SUB;
        for (int i = 0; i < SUB; i++) {
            const float k = target < s->gain ? k_atk : k_rel;
            s->gain = target + k * (s->gain - target);
            buf[b + i] *= s->gain;
        }
    }
}

static void br_set_param(Fx *fx, int id, float v)
{
    BrState *s = (BrState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == BREATH_DEPTH_DB) s->breath_db = v;
    else if (id == BREATH_CLICK_DB) s->click_db = v;
    else if (id == BREATH_SENS) s->sens = v;
}

static void br_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_breath_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "breath";
    fx->init = br_init;
    fx->process = br_process;
    fx->set_param = br_set_param;
    fx->destroy = br_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
