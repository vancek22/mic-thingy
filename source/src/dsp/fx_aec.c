#include "dsp/fx_aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "dsp/simd.h"

static const FxParamInfo k_params[] = {
    { "delay",    "ms", 0.0f,  200.0f, 40.0f, 0 },
    { "strength", "",   0.05f, 1.0f,   0.4f,  0 },
};

#define MAX_DELAY  9600                 /* 200 ms */
#define AEC_NEAR_K 30.0                /* weight of the unexplained (near-end) power in the step size */
#define HIST       (AEC_TAPS + MAX_DELAY + VF_FRAME_SIZE)
#define CAP        (HIST * 3)           /* linear history, compacted when full */

typedef struct {
    VfSink *ref;
    float w[AEC_TAPS];                  /* w[j] multiplies the reference j samples after the oldest in the window */
    float *xh;                          /* reference history, newest at xh[wi - 1] */
    int wi;
    int delay;                          /* samples */
    float mu;
    float pd, pe, pyf;                  /* smoothed powers: microphone, error (slow); echo estimate (fast) */
    int hold;                           /* samples left with adaptation frozen (double talk) */
    int conv;                           /* latched after 1 s of clearly working cancellation */
    int good_run, frozen_run;           /* sample counters for latching / unlatching conv */
    float sdy, syy;                     /* smoothed <mic, echo estimate> and <estimate, estimate> per frame */
    float apply;                        /* share of the echo estimate that is subtracted, 0..1 (end of the last frame) */
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
    (void)vf_simd_avx2();                /* run the CPU detection off the audio thread */
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

    float ybuf[VF_FRAME_SIZE];
    double fdy = 0.0, fyy = 0.0, fdd = 0.0;
    for (int i = 0; i < n; i++) {
        const float *xi = x + i - D;     /* newest reference sample of the filter window */
        const float *xs = xi - (AEC_TAPS - 1);   /* the weights are stored newest-last so both vectors run forwards */
        const float y = vf_dot(s->w, xs, AEC_TAPS);
        const float d = buf[i];
        const float e = d - y;
        ybuf[i] = y;
        fdy += (double)d * y;
        fyy += (double)y * y;
        fdd += (double)d * d;

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
            /* The step shrinks with the power of what is NOT explained by the reference (AEC_TAPS x error power next to
             * the reference energy): voice, keys or room noise that the double-talk detectors miss can then no longer
             * drag the weights around. With a real echo the error is a fraction of the reference, so it costs little. */
            const float g = s->mu * e / (float)(en + AEC_NEAR_K * (double)AEC_TAPS * s->pe + 1e-3);
            vf_axpy(s->w, xs, g, AEC_TAPS);
        }
        /* slide the energy window by one sample */
        const float newest = xi[1], oldest = xi[-(AEC_TAPS - 1)];
        en += (double)newest * newest - (double)oldest * oldest;
        if (en < 0.0) en = 0.0;
    }

    /* 3) subtract the estimate only as far as it is really in the microphone signal. A true echo estimate correlates
     * with the microphone (projection <d,y>/<y,y> near 1); weights that learned something else produce an estimate
     * that does not, and subtracting it would ADD the system sound to the microphone (headset: no echo at all, the
     * music was audible in the pauses). The share follows the smoothed projection, clamped to 0..1, ramped over the
     * frame; the output can therefore not get louder than the microphone on average. Useless weights fade out. */
    s->sdy += 0.25f * ((float)fdy - s->sdy);
    s->syy += 0.25f * ((float)fyy - s->syy);
    float want = s->syy > 1e-9f ? vf_clampf(s->sdy / s->syy, 0.0f, 1.0f) : 0.0f;
    /* ...and never when it would make this frame clearly louder than the microphone was (the voice just stopped and
     * the smoothed share is still high): |d - a y|^2 - |d|^2 = a^2 <y,y> - 2 a <d,y>. "Clearly" = by more than a tenth
     * of the frame's own energy, so a weak true echo under a loud voice (random sign of <d,y>) does not trip it. */
    if ((double)want * want * fyy - 2.0 * want * fdy > 0.1 * fdd)
        want = fyy > 1e-12 ? vf_clampf((float)(fdy / fyy), 0.0f, 1.0f) : 0.0f;
    const float a0 = s->apply;
    const int ramp = want < a0 ? 48 : n;                  /* down within 1 ms, up over the frame */
    for (int i = 0; i < n; i++) {
        const float a = i < ramp ? a0 + (want - a0) * (float)(i + 1) / (float)ramp : want;
        buf[i] -= a * ybuf[i];
    }
    s->apply = want;
    if (want < 0.3f && s->syy > 1e-9f)
        for (int k = 0; k < AEC_TAPS; k++) s->w[k] *= 0.97f;
}

static void aec_set_param(Fx *fx, int id, float v)
{
    AecState *s = (AecState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == AEC_DELAY_MS) { const int d = (int)(v * 48.0f); if (d != s->delay) { s->delay = d; s->conv = 0; memset(s->w, 0, sizeof s->w); s->sdy = s->syy = s->apply = 0.0f; } }
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
