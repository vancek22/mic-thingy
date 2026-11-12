#include "dsp/fx_rnnoise.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "rnnoise.h"

/* RNNoise works on floats in 16-bit integer range (+-32768), not +-1.0. */
#define RN_SCALE 32768.0f

static const FxParamInfo k_params[] = {
    /* dry/wet mix. 0.95 keeps a -26 dB dry floor: fricative onsets that RNNoise
     * mistakes for noise lose ~10 dB instead of vanishing (see tools/run_audio_tests). */
    { "wet", "", 0.0f, 1.0f, 0.95f },
};

typedef struct {
    DenoiseState *st;
    float in[VF_FRAME_SIZE];
    float out[VF_FRAME_SIZE];
    float dry[RNNOISE_DELAY_SAMPLES];   /* delay line for the dry path (ring) */
    int   dry_pos;
    float wet;
} RnState;

static int rn_init(Fx *fx, int sample_rate)
{
    if (sample_rate != VF_SAMPLE_RATE || rnnoise_get_frame_size() != VF_FRAME_SIZE)
        return -1;
    RnState *s = (RnState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->st = rnnoise_create(NULL);          /* built-in weights */
    if (!s->st) { free(s); return -1; }
    s->wet = k_params[RNNOISE_PARAM_WET].def;
    fx->state = s;
    return 0;
}

static void rn_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    RnState *s = (RnState *)fx->state;
    if (n != VF_FRAME_SIZE) return;
    for (int i = 0; i < n; i++) s->in[i] = buf[i] * RN_SCALE;
    float vad = rnnoise_process_frame(s->st, s->out, s->in);
    ctx->vad_prob = vad;
    ctx->vad_valid = 1;
    const float w = s->wet, d = 1.0f - w;
    int p = s->dry_pos;
    for (int i = 0; i < n; i++) {
        const float delayed = s->dry[p];        /* input from RNNOISE_DELAY_SAMPLES ago */
        s->dry[p] = buf[i];
        if (++p == RNNOISE_DELAY_SAMPLES) p = 0;
        buf[i] = s->out[i] * w * (1.0f / RN_SCALE) + delayed * d;
    }
    s->dry_pos = p;
}

static void rn_set_param(Fx *fx, int id, float v)
{
    RnState *s = (RnState *)fx->state;
    if (id == RNNOISE_PARAM_WET) s->wet = vf_clampf(v, 0.0f, 1.0f);
}

static void rn_destroy(Fx *fx)
{
    RnState *s = (RnState *)fx->state;
    if (!s) return;
    rnnoise_destroy(s->st);
    free(s);
    fx->state = NULL;
}

void fx_rnnoise_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "rnnoise";
    fx->init = rn_init;
    fx->process = rn_process;
    fx->set_param = rn_set_param;
    fx->destroy = rn_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->latency = RNNOISE_DELAY_SAMPLES;
    atomic_init(&fx->bypass, 0);
}
