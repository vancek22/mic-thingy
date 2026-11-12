#include "dsp/fx_aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"

static const FxParamInfo k_params[] = {
    { "delay",    "ms", 0.0f,  200.0f, 40.0f, 0 },
    { "strength", "",   0.05f, 1.0f,   0.4f,  0 },
};

#define MAX_DELAY  9600                 /* 200 ms */
#define HIST       (AEC_TAPS + MAX_DELAY + VF_FRAME_SIZE)
#define CAP        (HIST * 3)           /* linear history, compacted when full */

typedef struct {
    VfSink *ref;
    float w[AEC_TAPS];
    float *xh;                          /* reference history, newest at xh[wi - 1] */
    int wi;
    int delay;                          /* samples */
    float mu;
    float pd, pe, pyf;                  /* smoothed powers: microphone, error (slow); echo estimate (fast) */
    int hold;                           /* samples left with adaptation frozen (double talk) */
    int conv;                           /* latched after 1 s of clearly working cancellation */
    int good_run, frozen_run;           /* sample counters for latching / unlatching conv */
} AecState;

static int aec_init(Fx *fx, int sr)
{
    (void)sr;
    AecState *s = (AecState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->xh = (float *)calloc(CAP, sizeof(float));
    if (!s->xh) { free(s); return -1; }
    s->wi = HIST;
    s->delay = (int)(k_params[AEC_DELAY_MS].def * 48.0f);
    s->mu = k_params[AEC_STRENGTH].def;
    fx->state = s;
    return 0;
}

void fx_aec_attach(Fx *fx, VfSink *ref)
{
    if (fx->state) ((AecState *)fx->state)->ref = ref;
}

static void aec_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    AecState *s = (AecState *)fx->state;
    if (!s->ref || n > VF_FRAME_SIZE) return;

    /* 1) append the next n reference samples to the history */
    if (s->wi + n > CAP) {
        memmove(s->xh, s->xh + s->wi - HIST, (size_t)HIST * sizeof(float));
        s->wi = HIST;
    }
    float *x = s->xh + s->wi;           /* new reference samples land here */
    vf_sink_read(s->ref, x, n);
    s->wi += n;

    /* 2) cancel sample by sample. x[i - D - k] is the reference k samples before "now - D". */
    const int D = s->delay;
    float xmax = 0.0f;                   /* Geigel: largest reference magnitude in the filter window */
    for (int k = 0; k < AEC_TAPS + n; k++) {
        const float v = fabsf(x[-D - k]);
        if (v > xmax) xmax = v;
    }
    double en = 0.0;
    for (int k = 0; k < AEC_TAPS; k++) { const float v = x[-D - k]; en += (double)v * v; }

    for (int i = 0; i < n; i++) {
        const float *xi = x + i - D;     /* xi[-k] = reference k samples before sample i (after the bulk delay) */
        float y = 0.0f;
        for (int k = 0; k < AEC_TAPS; k++) y += s->w[k] * xi[-k];
        const float d = buf[i];
        const float e = d - y;
        buf[i] = e;

        /* double talk: (a) Geigel - the microphone is louder than an echo can be; (b) once converged,
         * the residual is as large as the echo estimate, so something else is in the signal */
        const float c = 0.003f;
        s->pd += c * (d * d - s->pd);
        s->pe += c * (e * e - s->pe);
        s->pyf += 0.02f * (y * y - s->pyf);
        if (!s->conv) {                                                /* converged = the echo is clearly reduced, for a full second */
            s->good_run = (xmax > 0.02f && s->pd > 1e-7f && s->pe < 0.1f * s->pd) ? s->good_run + 1 : 0;
            if (s->good_run > VF_SAMPLE_RATE) s->conv = 1;
        }
        const int geigel = fabsf(d) > 0.8f * xmax && fabsf(d) > 0.01f;
        const int near_end = s->conv && e * e > 0.05f * (float)(en / AEC_TAPS) + 1e-7f;   /* residual far above what a converged filter leaves */
        if (geigel || near_end) s->hold = 2400;                    /* 50 ms: bridges the gaps between syllables */
        else if (s->hold > 0) s->hold--;
        s->frozen_run = s->hold > 0 ? s->frozen_run + 1 : 0;
        if (s->frozen_run > 10 * VF_SAMPLE_RATE) { s->conv = 0; s->good_run = 0; s->frozen_run = 0; }   /* the room changed: learn again */
        if (s->hold == 0 && en > 1e-4) {
            const float g = s->mu * e / (float)(en + 1e-3);
            for (int k = 0; k < AEC_TAPS; k++) s->w[k] += g * xi[-k];
        }
        /* slide the energy window by one sample */
        const float newest = xi[1], oldest = xi[-(AEC_TAPS - 1)];
        en += (double)newest * newest - (double)oldest * oldest;
        if (en < 0.0) en = 0.0;
    }
}

static void aec_set_param(Fx *fx, int id, float v)
{
    AecState *s = (AecState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == AEC_DELAY_MS) { const int d = (int)(v * 48.0f); if (d != s->delay) { s->delay = d; s->conv = 0; memset(s->w, 0, sizeof s->w); } }
    else if (id == AEC_STRENGTH) s->mu = v;
}

static void aec_destroy(Fx *fx)
{
    AecState *s = (AecState *)fx->state;
    if (s) free(s->xh);
    free(s);
    fx->state = NULL;
}

void fx_aec_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "aec";
    fx->init = aec_init;
    fx->process = aec_process;
    fx->set_param = aec_set_param;
    fx->destroy = aec_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
