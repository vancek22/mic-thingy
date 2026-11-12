#include "dsp/fx_gate.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "threshold", "",   0.0f,  1.0f,    0.5f },
    { "hold",      "ms", 0.0f,  1000.0f, 150.0f },
    { "attack",    "ms", 0.1f,  50.0f,   3.0f },
    { "release",   "ms", 5.0f,  1000.0f, 100.0f },
    { "floor",     "dB", -90.0f, 0.0f,   -50.0f },
};

typedef struct {
    float sr;
    float threshold, hold_ms, floor_lin;
    float atk, rel;           /* one-pole coefficients */
    int   open;
    int   hold_left;          /* samples */
    float gain;
} GateState;

static int gate_init(Fx *fx, int sr)
{
    GateState *s = (GateState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->threshold = k_params[GATE_THRESHOLD].def;
    s->hold_ms = k_params[GATE_HOLD_MS].def;
    s->atk = vf_coef_ms(k_params[GATE_ATTACK_MS].def, s->sr);
    s->rel = vf_coef_ms(k_params[GATE_RELEASE_MS].def, s->sr);
    s->floor_lin = vf_db_to_lin(k_params[GATE_FLOOR_DB].def);
    s->gain = 1.0f;
    s->open = 1;
    fx->state = s;
    return 0;
}

static void gate_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    GateState *s = (GateState *)fx->state;

    if (ctx->vad_valid) {
        float vad = ctx->vad_prob;
        /* Open at the threshold; keep the hold timer alive at a lower level so
         * a hesitant VAD inside a word does not chop it (hysteresis). */
        if (vad >= s->threshold) {
            s->open = 1;
            s->hold_left = (int)(s->hold_ms * 0.001f * s->sr);
        } else if (s->open) {
            if (vad >= s->threshold * 0.6f) {
                s->hold_left = (int)(s->hold_ms * 0.001f * s->sr);
            } else {
                s->hold_left -= n;
                if (s->hold_left <= 0) { s->open = 0; s->hold_left = 0; }
            }
        }
    } else {
        s->open = 1;                       /* no VAD info: never gate */
    }

    const float target = s->open ? 1.0f : s->floor_lin;
    float g = s->gain;
    for (int i = 0; i < n; i++) {
        const float c = target > g ? s->atk : s->rel;
        g = target + c * (g - target);
        buf[i] *= g;
    }
    s->gain = g;
    ctx->gate_gain = g;
}

static void gate_set_param(Fx *fx, int id, float v)
{
    GateState *s = (GateState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    switch (id) {
    case GATE_THRESHOLD:  s->threshold = v; break;
    case GATE_HOLD_MS:    s->hold_ms = v; break;
    case GATE_ATTACK_MS:  s->atk = vf_coef_ms(v, s->sr); break;
    case GATE_RELEASE_MS: s->rel = vf_coef_ms(v, s->sr); break;
    case GATE_FLOOR_DB:   s->floor_lin = vf_db_to_lin(v); break;
    default: break;
    }
}

static void gate_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_gate_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "gate";
    fx->init = gate_init;
    fx->process = gate_process;
    fx->set_param = gate_set_param;
    fx->destroy = gate_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    atomic_init(&fx->bypass, 0);
}
