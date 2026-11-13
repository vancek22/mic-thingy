#include "dsp/fx_voice.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "pitch", "st", -12.0f, 12.0f, 0.0f, 0 },
    { "robot", "",   0.0f,   1.0f,  0.0f, 1 },
    { "radio", "",   0.0f,   1.0f,  0.0f, 1 },
};

#define RING   4096
#define MASK   (RING - 1)
#define WIN    1440              /* 30 ms */
#define PI_F   3.14159265358979f

typedef struct {
    float sr, semitones, ratio;
    int robot, radio;
    float ring[RING];
    unsigned wp;
    float phase;                 /* 0..1, position of tap 1 in the window */
    float carrier;               /* robot modulator phase */
    Biquad hp, lp;               /* radio band-limit */
} VoState;

static int vo_init(Fx *fx, int sr)
{
    VoState *s = (VoState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->ratio = 1.0f;
    biquad_highpass(&s->hp, s->sr, 400.0f, 0.7f);
    biquad_lowpass(&s->lp, s->sr, 3400.0f, 0.7f);
    fx->state = s;
    return 0;
}

static inline float tap(const VoState *s, float delay)
{
    const float pos = (float)s->wp - delay;
    const float fl = floorf(pos);
    const float fr = pos - fl;
    const unsigned i0 = (unsigned)(int)fl & MASK, i1 = (i0 + 1) & MASK;
    return s->ring[i0] + (s->ring[i1] - s->ring[i0]) * fr;
}

static void vo_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    VoState *s = (VoState *)fx->state;
    const int shifting = s->semitones != 0.0f;
    const float dphase = (1.0f - s->ratio) / (float)WIN;
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        if (shifting) {
            s->ring[s->wp & MASK] = x;
            float p2 = s->phase + 0.5f;
            if (p2 >= 1.0f) p2 -= 1.0f;
            const float w1 = sinf(PI_F * s->phase), w2 = sinf(PI_F * p2);
            x = tap(s, s->phase * (float)WIN) * w1 * w1 + tap(s, p2 * (float)WIN) * w2 * w2;
            s->phase += dphase;
            if (s->phase >= 1.0f) s->phase -= 1.0f; else if (s->phase < 0.0f) s->phase += 1.0f;
            s->wp++;
        }
        if (s->robot) {
            x *= 1.4f * sinf(s->carrier);
            s->carrier += 2.0f * PI_F * 90.0f / s->sr;
            if (s->carrier > 2.0f * PI_F) s->carrier -= 2.0f * PI_F;
        }
        if (s->radio) x = tanhf(2.0f * biquad_tick(&s->lp, biquad_tick(&s->hp, x))) * 0.6f;
        buf[i] = x;
    }
}

static void vo_set_param(Fx *fx, int id, float v)
{
    VoState *s = (VoState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == VOICE_PITCH) {
        s->semitones = v;
        s->ratio = powf(2.0f, v / 12.0f);
        if (v == 0.0f) { memset(s->ring, 0, sizeof s->ring); s->phase = 0.0f; }
    } else if (id == VOICE_ROBOT) s->robot = v >= 0.5f;
    else if (id == VOICE_RADIO) s->radio = v >= 0.5f;
    atomic_store_explicit(&fx->latency, s->semitones != 0.0f ? VOICE_PITCH_DELAY : 0, memory_order_relaxed);
}

static void vo_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_voice_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "voice";
    fx->init = vo_init;
    fx->process = vo_process;
    fx->set_param = vo_set_param;
    fx->destroy = vo_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 2;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
