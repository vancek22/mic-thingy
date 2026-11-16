#include "dsp/fx_aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "dsp/fft960.h"
#include "dsp/minphase.h"
#include "dsp/simd.h"

static const FxParamInfo k_params[] = {
    { "delay",    "ms", 0.0f,  200.0f, 40.0f, 0 },
    { "strength", "",   0.05f, 1.0f,   0.4f,  0 },
    /* residual echo suppressor: maximum attenuation of what the adaptive filter leaves (nonlinear speaker, room tail
     * beyond the 43 ms of taps, clock drift). Default 18 dB; 0 = off (bit-exact old behaviour). */
    { "suppress", "dB", 0.0f, 30.0f, 18.0f, 0 },
};

#define MAX_DELAY  9600                 /* 200 ms */
#define AEC_NEAR_K 30.0                /* weight of the unexplained (near-end) power in the step size */
#define HIST       (AEC_TAPS + MAX_DELAY + VF_FRAME_SIZE)
#define CAP        (HIST * 3)           /* linear history, compacted when full */
#define AEC_WHITEN 0.7f                /* pre-emphasis for the adaptation path (speech is far from white) */
#define RES_BINS   481
#define RES_XFADE  240

typedef struct {
    VfSink *ref;
    float w[AEC_TAPS];                  /* w[j] multiplies the reference j samples after the oldest in the window */
    float *xh;                          /* reference history, newest at xh[wi - 1] */
    float *xw;                          /* the same, pre-emphasised: x[n] - A x[n-1] (used to adapt only) */
    float e_prev, pew;                  /* previous error sample, smoothed power of the pre-emphasised error */
    int wi;
    int delay;                          /* samples */
    float mu;
    float pd, pe, pyf;                  /* smoothed powers: microphone, error (slow); echo estimate (fast) */
    float lpd, lpe;                     /* the same over ~2 s, updated only while the reference plays (echo-cancelled-for-real witness) */
    int hold;                           /* samples left with adaptation frozen (double talk) */
    int conv;                           /* latched after 1 s of clearly working cancellation */
    int good_run, frozen_run;           /* sample counters for latching / unlatching conv */
    float sdy, syy;                     /* smoothed <mic, echo estimate> and <estimate, estimate> per frame */
    float apply;                        /* share of the echo estimate that is subtracted, 0..1 (end of the last frame) */

    /* residual echo suppressor: per-bin gains (481 bins, 50 Hz) -> 128-tap minimum-phase FIR on the same frame */
    float sup_db, gmin;
    VfFft fft;
    VfMinPhase mp;
    float win[VF_FFT_N];
    float re[VF_FFT_N], im[VF_FFT_N];
    float ehist[VF_FRAME_SIZE];         /* previous output frame (analysis window = previous + current) */
    float cres[RES_BINS];               /* learned residual-echo-to-reference power ratio per bin */
    float pn[RES_BINS];                 /* noise floor of the output per bin (lower envelope), not echo */
    float pxeff[RES_BINS];              /* reference power per bin, peak-held with decay (covers the reverb tail) */
    float gsm[RES_BINS];                /* smoothed gains */
    float h_cur[VF_MP_TAPS], hr_old[VF_MP_TAPS], hr_cur[VF_MP_TAPS];
    float hist[VF_MP_TAPS - 1 + VF_FRAME_SIZE];
} AecState;

static void res_reset(AecState *s)
{
    for (int k = 0; k < RES_BINS; k++) { s->cres[k] = 1.0f; s->pxeff[k] = 0.0f; s->gsm[k] = 1.0f; s->pn[k] = 1.0f; }
    s->lpd = s->lpe = 0.0f;
    memset(s->ehist, 0, sizeof s->ehist);
    memset(s->hist, 0, sizeof s->hist);
    memset(s->hr_old, 0, sizeof s->hr_old);
    memset(s->hr_cur, 0, sizeof s->hr_cur);
    s->hr_old[VF_MP_TAPS - 1] = s->hr_cur[VF_MP_TAPS - 1] = 1.0f;     /* identity until the first filter is designed */
}

/* power per bin (|X|^2, window 960 = previous + current frame) of two contiguous pieces */
static void res_power(AecState *s, const float *a, int na, const float *b, int nb, float *p)
{
    for (int i = 0; i < na; i++) { s->re[i] = a[i] * s->win[i]; s->im[i] = 0.0f; }
    for (int i = 0; i < nb; i++) { s->re[na + i] = b[i] * s->win[na + i]; s->im[na + i] = 0.0f; }
    vf_fft(&s->fft, s->re, s->im, 0);
    const float inv = 1.0f / ((float)VF_FFT_N * (float)VF_FFT_N);
    for (int k = 0; k < RES_BINS; k++) p[k] = (s->re[k] * s->re[k] + s->im[k] * s->im[k]) * inv;
}

/* Residual echo suppressor. After the NLMS subtraction the signal still holds what a 43 ms linear filter cannot remove
 * (speaker nonlinearity, the room tail, clock drift). Per bin we learn how much of the (peak-held) reference power is
 * left in the output while nobody else talks (adaptation frozen during double talk - the same `hold` as the NLMS), and
 * attenuate bins where that expected residue is as large as what is there. Bins where the microphone carries more
 * (your voice, keys, breath) are left alone, and with no echo path (headset) the ratio is learned as ~0 = no effect.
 * The gains become a minimum-phase FIR on this frame: no delay. */
static void res_stage(AecState *s, float *buf, int n, const float *x, int D)
{
    float pe[RES_BINS], px[RES_BINS], pes[RES_BINS], gains[RES_BINS];
    res_power(s, s->ehist, VF_FRAME_SIZE, buf, n, pe);
    res_power(s, x - D - VF_FRAME_SIZE, VF_FFT_N, NULL, 0, px);          /* reference over the same stretch, D samples earlier */
    memcpy(s->ehist, buf, (size_t)n * sizeof(float));

    const int adapt = s->hold == 0;
    for (int k = 0; k < RES_BINS; k++) {
        float h = 0.65f * s->pxeff[k];                                   /* ~ -1.9 dB per frame: the reverb tail keeps being covered */
        if (px[k] > h) h = px[k];
        s->pxeff[k] = h;
        /* floor: lower envelope of the output (down at once, up over ~10 s) - room noise is not echo */
        s->pn[k] += (pe[k] < s->pn[k] ? 0.3f : 0.001f) * (pe[k] - s->pn[k]);
        if (adapt && h > 4e-9f) {
            float r = (pe[k] - s->pn[k]) / h;
            if (r < 0.0f) r = 0.0f;
            if (r > 2.0f) r = 2.0f;
            /* what you say / type only ADDS power to the output, it never lowers the residue: follow decreases quickly,
             * increases slowly (the lower envelope of the ratio), so double talk is not learned as echo */
            s->cres[k] += (r < s->cres[k] ? 0.15f : 0.004f) * (r - s->cres[k]);
        }
    }
    /* conf: how well the NLMS echo estimate explains the microphone (the projection the subtraction already uses, 0..1).
     * A power ratio alone cannot tell an echo residue from your own voice / keys, so with no echo path (headset: the
     * estimate is unrelated to the microphone, conf ~ 0) the suppressor stays out of the way. */
    const float conf_a = vf_clampf((s->apply - 0.35f) * (1.0f / 0.45f), 0.0f, 1.0f);   /* a true echo projects at ~1; chance correlation stays below 0.35 */
    /* ...but that projection alone is high in about a third of the frames with NO echo path (2048 taps fit the room noise;
     * measured on the headset scenarios, mean 0.5 vs 0.7 with an echo), and the suppressor then changed what the nn and the
     * gate see. Second witness: over ~2 s, while the reference plays, the error power is clearly below the microphone power
     * only when the echo is really cancelled (>= 3-4 dB with an echo, 0-1 dB with a headset, speech or silence). */
    float conf_e = 0.0f;
    if (s->lpd > 1e-9f && s->lpe > 1e-13f) conf_e = vf_clampf((10.0f * log10f(s->lpd / s->lpe) - 2.0f) * 0.5f, 0.0f, 1.0f);
    const float conf = conf_e < conf_a ? conf_e : conf_a;
    for (int k = 0; k < RES_BINS; k++) {
        const float a = pe[k > 0 ? k - 1 : 0], b = pe[k], c = pe[k < RES_BINS - 1 ? k + 1 : RES_BINS - 1];
        pes[k] = (a + b + c) * (1.0f / 3.0f);
        const float r = (s->cres[k] * s->pxeff[k]) / (pes[k] + 1e-12f);   /* share of this bin that is echo residue */
        float g = 1.0f - 1.5f * r;
        if (g < s->gmin) g = s->gmin;
        if (g > 1.0f) g = 1.0f;
        s->gsm[k] += (g - s->gsm[k]) * (g < s->gsm[k] ? 0.6f : 0.25f);   /* down fast, up slower */
        gains[k] = 1.0f - conf * (1.0f - s->gsm[k]);                   /* no sign of an echo path: leave the signal alone */
    }

    memcpy(s->hr_old, s->hr_cur, sizeof s->hr_old);
    vf_minphase_design(&s->mp, gains, RES_BINS, s->h_cur);
    for (int k = 0; k < VF_MP_TAPS; k++) s->hr_cur[VF_MP_TAPS - 1 - k] = s->h_cur[k];
    const int T = VF_MP_TAPS;
    float *b = s->hist;                                                  /* [T-1 previous samples][this frame] */
    memcpy(b + T - 1, buf, (size_t)n * sizeof(float));
    for (int i = 0; i < n; i++) {
        const float yn = vf_dot(s->hr_cur, b + i, T);
        float y = yn;
        if (i < RES_XFADE) {
            const float yo = vf_dot(s->hr_old, b + i, T);
            y = yo + (yn - yo) * ((float)(i + 1) / (float)RES_XFADE);
        }
        buf[i] = y;
    }
    memmove(b, b + n, (size_t)(T - 1) * sizeof(float));
}

static int aec_init(Fx *fx, int sr)
{
    (void)sr;
    AecState *s = (AecState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->xh = (float *)calloc(CAP, sizeof(float));
    s->xw = (float *)calloc(CAP, sizeof(float));
    if (!s->xh || !s->xw) { free(s->xh); free(s->xw); free(s); return -1; }
    s->wi = HIST;
    s->delay = (int)(k_params[AEC_DELAY_MS].def * 48.0f);
    s->mu = k_params[AEC_STRENGTH].def;
    (void)vf_simd_avx2();                /* run the CPU detection off the audio thread */
    vf_fft_init(&s->fft);
    vf_minphase_init(&s->mp);
    for (int i = 0; i < VF_FFT_N; i++) s->win[i] = sinf(3.14159265f * ((float)i + 0.5f) / (float)VF_FFT_N);
    s->sup_db = k_params[AEC_SUPPRESS].def;
    s->gmin = powf(10.0f, -s->sup_db / 20.0f);
    res_reset(s);
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
        memmove(s->xw, s->xw + s->wi - HIST, (size_t)HIST * sizeof(float));
        s->wi = HIST;
    }
    float *x = s->xh + s->wi;           /* new reference samples land here */
    float *xw = s->xw + s->wi;
    vf_sink_read(s->ref, x, n);
    for (int i = 0; i < n; i++) xw[i] = x[i] - AEC_WHITEN * x[i - 1];
    s->wi += n;

    /* 2) cancel sample by sample. x[i - D - k] is the reference k samples before "now - D". */
    const int D = s->delay;
    float xmax = 0.0f;                   /* Geigel: largest reference magnitude in the filter window */
    for (int k = 0; k < AEC_TAPS + n; k++) {
        const float v = fabsf(x[-D - k]);
        if (v > xmax) xmax = v;
    }
    double en = 0.0, enw = 0.0;
    for (int k = 0; k < AEC_TAPS; k++) { const float v = x[-D - k], w = xw[-D - k]; en += (double)v * v; enw += (double)w * w; }

    float ybuf[VF_FRAME_SIZE];
    double fdy = 0.0, fyy = 0.0, fdd = 0.0;
    for (int i = 0; i < n; i++) {
        const float *xi = x + i - D;     /* newest reference sample of the filter window */
        const float *xs = xi - (AEC_TAPS - 1);   /* the weights are stored newest-last so both vectors run forwards */
        const float y = vf_dot(s->w, xs, AEC_TAPS);
        const float d = buf[i];
        const float e = d - y;
        const float ew = e - AEC_WHITEN * s->e_prev;
        s->e_prev = e;
        s->pew += (ew * ew > s->pew ? 0.1f : 0.003f) * (ew * ew - s->pew);   /* fast attack: a voice onset must slow the step at once */
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
            /* adapted on the pre-emphasised signals: the path is the same linear system, but NLMS converges far faster
             * on a flat spectrum than on speech or music */
            /* (a step that shrinks once converged gave 25 dB instead of 19 dB in a simulation with 20 ppm clock drift, but cost
             * 2.5 dB on a real headphones-to-microphone recording: two real devices drift far more and the filter must track) */
            const float g = s->mu * ew / (float)(enw + AEC_NEAR_K * (double)AEC_TAPS * s->pew + 1e-3);
            vf_axpy(s->w, xw + (xi - x) - (AEC_TAPS - 1), g, AEC_TAPS);
        }
        /* slide the energy window by one sample */
        const float newest = xi[1], oldest = xi[-(AEC_TAPS - 1)];
        en += (double)newest * newest - (double)oldest * oldest;
        if (en < 0.0) en = 0.0;
        const float newest_w = xw[i - D + 1], oldest_w = xw[i - D - (AEC_TAPS - 1)];
        enw += (double)newest_w * newest_w - (double)oldest_w * oldest_w;
        if (enw < 0.0) enw = 0.0;
    }

    /* 3) subtract the estimate only as far as it is really in the microphone signal. A true echo estimate correlates
     * with the microphone (projection <d,y>/<y,y> near 1); weights that learned something else produce an estimate
     * that does not, and subtracting it would ADD the system sound to the microphone (headset: no echo at all, the
     * music was audible in the pauses). The share follows the smoothed projection, clamped to 0..1, ramped over the
     * frame; the output can therefore not get louder than the microphone on average. */
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
    if (en > 1e-4) { s->lpd += 0.005f * (s->pd - s->lpd); s->lpe += 0.005f * (s->pe - s->lpe); }   /* same gate as the adaptation: the reference is playing */
    if (s->sup_db > 0.01f && n == VF_FRAME_SIZE) res_stage(s, buf, n, x, D);
    /* No weight decay while the share is low: with speakers the far end often goes quiet while you talk, the share drops,
     * and decayed weights could not be relearned before the double-talk freeze let go (measured: echo uncancelled for
     * 5+ s after the far end resumed). Unused weights cost nothing - the share keeps them out of the output. */
}

static void aec_set_param(Fx *fx, int id, float v)
{
    AecState *s = (AecState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == AEC_DELAY_MS) { const int d = (int)(v * 48.0f); if (d != s->delay) { s->delay = d; s->conv = 0; memset(s->w, 0, sizeof s->w); s->sdy = s->syy = s->apply = 0.0f; res_reset(s); } }
    else if (id == AEC_STRENGTH) s->mu = v;
    else if (id == AEC_SUPPRESS) {
        const int was_off = s->sup_db <= 0.01f;
        s->sup_db = v;
        s->gmin = powf(10.0f, -v / 20.0f);
        if (was_off && v > 0.01f) res_reset(s);                          /* switched on: no stale spectra */
    }
}

static void aec_destroy(Fx *fx)
{
    AecState *s = (AecState *)fx->state;
    if (s) { free(s->xh); free(s->xw); }
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
