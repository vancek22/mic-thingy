#include "dsp/fx_rnnoise.h"

#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "rnnoise.h"
#include "dsp/minphase.h"
#include "dsp/simd.h"

/* RNNoise works on floats in 16-bit integer range (+-32768), not +-1.0. */
#define RN_SCALE 32768.0f

static const FxParamInfo k_params[] = {
    /* dry/wet mix. 0.95 keeps a -26 dB dry floor: fricative onsets that RNNoise
     * mistakes for noise lose ~10 dB instead of vanishing (see tools/run_audio_tests). */
    { "wet", "", 0.0f, 1.0f, 0.95f, 0 },
    /* 0/1, see third_party/rnnoise/PATCHES.md. Default 0: -10 ms for ~0.6 dB SDR. */
    { "lookahead", "", 0.0f, 1.0f, 0.0f, 1 },
    /* 0/1: the frame's RNNoise gains drive a short minimum-phase filter applied to the same frame:
     * ~0.5 ms instead of 10 ms. Slightly less clean than the overlap-add path (see CLAUDE.md). */
    { "lowlat", "", 0.0f, 1.0f, 0.0f, 1 },
};

typedef struct {
    DenoiseState *st;
    float in[VF_FRAME_SIZE];
    float out[VF_FRAME_SIZE];
    float dry[RNNOISE_DELAY_SAMPLES];   /* delay line for the dry path (ring, max delay) */
    int   dry_pos;
    int   dry_delay;                    /* current delay: matches RNNoise's output delay */
    float wet;

    /* low-latency mode */
    int   lowlat;
    VfMinPhase *mp;                     /* design work area (allocated in init) */
    float gains[481];                   /* per-bin gains of the current frame (rnnoise_get_freq_size()) */
    float h_cur[VF_MP_TAPS];            /* designed filter, tap 0 first */
    float hr_old[VF_MP_TAPS], hr_cur[VF_MP_TAPS];   /* the same, reversed: lets the dot product run forwards */
    float hist[VF_MP_TAPS - 1 + VF_FRAME_SIZE];
} RnState;

#define LL_XFADE 240                    /* samples over which the new filter replaces the old one */

static int rn_init(Fx *fx, int sample_rate)
{
    if (sample_rate != VF_SAMPLE_RATE || rnnoise_get_frame_size() != VF_FRAME_SIZE)
        return -1;
    RnState *s = (RnState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->st = rnnoise_create(NULL);          /* built-in weights */
    if (!s->st) { free(s); return -1; }
    s->mp = (VfMinPhase *)malloc(sizeof *s->mp);
    if (!s->mp || rnnoise_get_freq_size() != (int)(sizeof s->gains / sizeof s->gains[0])) {
        free(s->mp); rnnoise_destroy(s->st); free(s); return -1;
    }
    vf_minphase_init(s->mp);
    s->hr_old[VF_MP_TAPS - 1] = s->hr_cur[VF_MP_TAPS - 1] = 1.0f;   /* identity until the first frame designs a filter */
    (void)vf_simd_avx2();
    s->wet = k_params[RNNOISE_PARAM_WET].def;
    s->dry_delay = RNNOISE_DELAY_SAMPLES;
    fx->state = s;
    return 0;
}

/* Low-latency path: RNNoise only analyses the frame; its gains become a minimum-phase FIR
 * that filters the very same frame (cross-faded from the previous frame's filter). */
static void rn_process_ll(RnState *s, float *buf, int n, FxFrameCtx *ctx)
{
    for (int i = 0; i < n; i++) s->in[i] = buf[i] * RN_SCALE;
    ctx->vad_prob = rnnoise_analyze_frame(s->st, s->in, s->gains);
    ctx->vad_valid = 1;

    memcpy(s->hr_old, s->hr_cur, sizeof s->hr_old);
    vf_minphase_design(s->mp, s->gains, (int)(sizeof s->gains / sizeof s->gains[0]), s->h_cur);
    for (int k = 0; k < VF_MP_TAPS; k++) s->hr_cur[VF_MP_TAPS - 1 - k] = s->h_cur[k];

    const int T = VF_MP_TAPS;
    float *b = s->hist;                         /* [T-1 previous samples][this frame] */
    memcpy(b + T - 1, buf, (size_t)n * sizeof(float));
    const float w = s->wet, d = 1.0f - w;
    for (int i = 0; i < n; i++) {
        const float *x = b + T - 1 + i;        /* x[-k] = input k samples ago; b + i is the oldest sample in reach */
        const float yn = vf_dot(s->hr_cur, b + i, T);
        float y = yn;
        if (i < LL_XFADE) {
            const float yo = vf_dot(s->hr_old, b + i, T);
            const float f = (float)(i + 1) / (float)LL_XFADE;
            y = yo + (yn - yo) * f;
        }
        buf[i] = y * w + x[0] * d;
    }
    memmove(b, b + n, (size_t)(T - 1) * sizeof(float));
}

static void rn_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    RnState *s = (RnState *)fx->state;
    if (n != VF_FRAME_SIZE) return;
    if (s->lowlat) { rn_process_ll(s, buf, n, ctx); return; }
    for (int i = 0; i < n; i++) s->in[i] = buf[i] * RN_SCALE;
    float vad = rnnoise_process_frame(s->st, s->out, s->in);
    ctx->vad_prob = vad;
    ctx->vad_valid = 1;
    const float w = s->wet, d = 1.0f - w;
    int p = s->dry_pos;
    const int dl = s->dry_delay;
    for (int i = 0; i < n; i++) {
        int r = p - dl;                         /* input from dry_delay samples ago */
        if (r < 0) r += RNNOISE_DELAY_SAMPLES;
        const float delayed = s->dry[r];        /* read before write: r == p when dl is max */
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
    if (id == RNNOISE_PARAM_LOOKAHEAD) {
        const int on = v >= 0.5f;
        rnnoise_set_lookahead(s->st, on);
        s->dry_delay = on ? RNNOISE_DELAY_SAMPLES : RNNOISE_DELAY_NO_LOOKAHEAD;
        atomic_store_explicit(&fx->latency, s->dry_delay, memory_order_relaxed);
    }
    if (id == RNNOISE_PARAM_LOWLAT) s->lowlat = v >= 0.5f;
    if (id == RNNOISE_PARAM_LOOKAHEAD || id == RNNOISE_PARAM_LOWLAT)
        atomic_store_explicit(&fx->latency, s->lowlat ? RNNOISE_LL_DELAY : s->dry_delay, memory_order_relaxed);
}

static void rn_destroy(Fx *fx)
{
    RnState *s = (RnState *)fx->state;
    if (!s) return;
    rnnoise_destroy(s->st);
    free(s->mp);
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
    atomic_init(&fx->latency, RNNOISE_DELAY_SAMPLES);
    atomic_init(&fx->bypass, 0);
}
