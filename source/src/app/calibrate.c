#include "app/calibrate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/fx_compressor.h"
#include "dsp/fx_deesser.h"
#include "dsp/fx_eq3.h"
#include "dsp/fx_gate.h"
#include "dsp/fx_hum.h"
#include "dsp/fx_rnnoise.h"

#define NBIN (VF_FFT_N / 2 + 1)
#define BIN_HZ 50.0f                          /* 48000 / 960 */
#define SR 48000.0

/* Goertzel frequencies: hum candidates (50/60 Hz and two harmonics) and neighbours for the noise reference */
static const double k_gf[12] = { 50, 100, 150, 60, 120, 180, 40, 75, 135, 85, 165, 195 };

static float to_db(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int cmpf(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float percentile(float *v, int n, float p)
{
    if (n <= 0) return 0.0f;
    qsort(v, (size_t)n, sizeof *v, cmpf);
    int i = (int)(p * (float)(n - 1) + 0.5f);
    return v[i < 0 ? 0 : i >= n ? n - 1 : i];
}

void vf_cal_init(VfCalibrate *c) { memset(c, 0, sizeof *c); }

void vf_cal_start(VfCalibrate *c)
{
    memset(c, 0, sizeof *c);
    vf_fft_init(&c->fft);
    for (int i = 0; i < VF_FFT_N; i++) {
        const double a = sin(3.14159265358979323846 * (i + 0.5) / VF_FFT_N);
        c->win[i] = (float)sin(0.5 * 3.14159265358979323846 * a * a);
    }
    for (int k = 0; k < 12; k++) c->gcoef[k] = 2.0 * cos(2.0 * 3.14159265358979323846 * k_gf[k] / SR);
    c->state = VF_CAL_SILENCE;
}

static void fail(VfCalibrate *c, const char *why)
{
    c->state = VF_CAL_FAILED;
    snprintf(c->why, sizeof c->why, "%s", why);
}

static double band_mean(const double *psd, float lo, float hi)
{
    double s = 0;
    int n = 0;
    for (int k = (int)(lo / BIN_HZ); k < NBIN && (float)k * BIN_HZ < hi; k++) { s += psd[k]; n++; }
    return n ? s / n : 0.0;
}

static float db10(double p) { return p > 1e-20 ? (float)(10.0 * log10(p)) : -200.0f; }

/* pitch of one frame by normalised autocorrelation, 70..400 Hz; 0 if not clearly periodic */
static float frame_f0(const float *x, int n)
{
    double e0 = 0;
    for (int i = 0; i < n; i++) e0 += (double)x[i] * x[i];
    if (e0 < 1e-9) return 0.0f;
    int best = 0;
    double bv = 0;
    for (int lag = 120; lag <= 686; lag++) {                   /* 400 .. 70 Hz at 48 kHz */
        double s = 0, e1 = 0;
        for (int i = 0; i + lag < n; i += 2) { s += (double)x[i] * x[i + lag]; e1 += (double)x[i + lag] * x[i + lag]; }
        const double r = s / sqrt(e0 * 0.5 * e1 + 1e-12);
        if (r > bv) { bv = r; best = lag; }
    }
    return bv > 0.6 ? (float)(SR / best) : 0.0f;
}

static void analyse_frame(VfCalibrate *c)
{
    for (int i = 0; i < VF_FFT_N; i++) { c->re[i] = c->hist[i] * c->win[i]; c->im[i] = 0.0f; }
    vf_fft(&c->fft, c->re, c->im, 0);
    double tot = 0;
    for (int k = 0; k < NBIN; k++) tot += (double)c->re[k] * c->re[k] + (double)c->im[k] * c->im[k];
    if (c->state == VF_CAL_SILENCE) {
        for (int k = 0; k < NBIN; k++) c->noise_psd[k] += (double)c->re[k] * c->re[k] + (double)c->im[k] * c->im[k];
        c->n_noise_fr++;
        if (c->n_nfe < (int)(sizeof c->nfe / sizeof c->nfe[0])) c->nfe[c->n_nfe++] = db10(tot);
    } else if (c->state == VF_CAL_SPEECH && c->last_vad > 0.6f) {
        for (int k = 0; k < NBIN; k++) c->speech_psd[k] += (double)c->re[k] * c->re[k] + (double)c->im[k] * c->im[k];
        c->n_speech_fr++;
        double lo = 0, hi = 0;
        for (int k = 6; k < 60; k++) lo += (double)c->re[k] * c->re[k] + (double)c->im[k] * c->im[k];      /* 300-3000 Hz */
        for (int k = 100; k < 200; k++) hi += (double)c->re[k] * c->re[k] + (double)c->im[k] * c->im[k];   /* 5-10 kHz */
        const int cap = (int)(sizeof c->sib / sizeof c->sib[0]);
        if (c->n_sib < cap) {
            c->sib[c->n_sib] = db10(hi);
            c->vlo[c->n_sib] = db10(lo);
            c->sibl[c->n_sib++] = db10(4.0 * hi / ((double)VF_FFT_N * VF_FFT_N)) + 8.0f;   /* band RMS (Parseval) + crest, ~peak dBFS */
        }
        const float f0 = frame_f0(c->hist, VF_FFT_N);
        if (f0 > 0.0f && c->n_f0 < cap) c->f0s[c->n_f0++] = f0;
    }
}

void vf_cal_audio(VfCalibrate *c, const float *in, int n)
{
    if (c->state != VF_CAL_SILENCE && c->state != VF_CAL_SPEECH) return;
    for (int i = 0; i < n; i++) {
        if (c->state == VF_CAL_SILENCE) {
            const double x = in[i];
            for (int k = 0; k < 12; k++) {
                const double s0 = x + c->gcoef[k] * c->gs1[k] - c->gs2[k];
                c->gs2[k] = c->gs1[k];
                c->gs1[k] = s0;
            }
            c->gn++;
        }
        c->hist[c->hist_fill++] = in[i];
        if (c->hist_fill == VF_FFT_N) {
            analyse_frame(c);
            memmove(c->hist, c->hist + VF_FFT_N / 2, (VF_FFT_N / 2) * sizeof(float));
            c->hist_fill = VF_FFT_N / 2;
            c->audio_frames++;
        }
    }
}

static double goertzel_pow(const VfCalibrate *c, int k)
{
    return c->gs1[k] * c->gs1[k] + c->gs2[k] * c->gs2[k] - c->gcoef[k] * c->gs1[k] * c->gs2[k];
}

static void analyse_audio(VfCalibrate *c, VfCalResult *r)
{
    if (c->n_noise_fr < 100 || c->n_speech_fr < 100) return;
    r->has_audio = 1;
    /* hum: harmonics vs neighbouring frequencies (power per Goertzel bin) */
    if (c->gn > 0) {
        const double ref = (goertzel_pow(c, 6) + goertzel_pow(c, 7) + goertzel_pow(c, 8) + goertzel_pow(c, 9) +
                            goertzel_pow(c, 10) + goertzel_pow(c, 11)) / 6.0 + 1e-30;
        const double h50 = (goertzel_pow(c, 0) + goertzel_pow(c, 1) + goertzel_pow(c, 2)) / 3.0;
        const double h60 = (goertzel_pow(c, 3) + goertzel_pow(c, 4) + goertzel_pow(c, 5)) / 3.0;
        const double h = h50 > h60 ? h50 : h60;
        r->hum_db = (float)(10.0 * log10(h / ref + 1e-30));
        r->hum_hz = r->hum_db > 15.0f ? (h50 > h60 ? 50 : 60) : 0;
    }
    /* noise impulsiveness */
    float tmp[VF_CAL_SPEECH_S * 100 + 8];
    memcpy(tmp, c->nfe, (size_t)c->n_nfe * sizeof(float));
    const float med = percentile(tmp, c->n_nfe, 0.5f), p99 = percentile(tmp, c->n_nfe, 0.99f);
    r->transient_db = p99 - med;
    /* spectral balance of the voice: noise power is subtracted (both are per-frame means) */
    double sp[NBIN];
    for (int k = 0; k < NBIN; k++) {
        const double s = c->speech_psd[k] / c->n_speech_fr - c->noise_psd[k] / c->n_noise_fr;
        sp[k] = s > c->speech_psd[k] / c->n_speech_fr * 0.01 ? s : c->speech_psd[k] / c->n_speech_fr * 0.01;
    }
    const double ref = band_mean(sp, 300, 1000);
    r->low_db = db10(band_mean(sp, 100, 250) / ref);
    r->mid_db = db10(band_mean(sp, 2000, 4000) / ref);
    r->high_db = db10(band_mean(sp, 5000, 8000) / ref);
    r->rumble_db = db10(band_mean(c->noise_psd, 20, 80) / c->n_noise_fr) - db10(band_mean(c->speech_psd, 20, 80) / c->n_speech_fr);
    if (c->n_f0 > 20) {
        memcpy(tmp, c->f0s, (size_t)c->n_f0 * sizeof(float));
        r->f0_hz = percentile(tmp, c->n_f0, 0.5f);
    }
    if (c->n_sib > 20) {
        memcpy(tmp, c->sib, (size_t)c->n_sib * sizeof(float));
        const float hi95 = percentile(tmp, c->n_sib, 0.95f);
        memcpy(tmp, c->vlo, (size_t)c->n_sib * sizeof(float));
        r->sib_db = hi95 - percentile(tmp, c->n_sib, 0.5f);
        memcpy(tmp, c->sibl, (size_t)c->n_sib * sizeof(float));
        r->sib_level_db = percentile(tmp, c->n_sib, 0.9f);
    }

    /* --- proposals --- */
    /* high-pass: a bit above the rumble, well below the voice's pitch */
    float hp = r->f0_hz > 0.0f ? 0.55f * r->f0_hz : 80.0f;
    if (r->rumble_db > -3.0f && hp < 100.0f) hp = 100.0f;
    r->hp_hz = clampf(hp, 60.0f, 140.0f);
    /* EQ: move half-way towards the typical voice, only where the mic is clearly off (> 4 dB) */
    const float dl = r->low_db - VF_CAL_T_LOW, dm = r->mid_db - VF_CAL_T_MID, dh = r->high_db - VF_CAL_T_HIGH;
    r->eq_low_db = fabsf(dl) > 4.0f ? clampf(-0.5f * dl, -6.0f, 6.0f) : 0.0f;
    r->eq_mid_db = fabsf(dm) > 4.0f ? clampf(-0.5f * dm, -6.0f, 6.0f) : 0.0f;
    r->eq_high_db = fabsf(dh) > 4.0f ? clampf(-0.5f * dh, -6.0f, 6.0f) : 0.0f;
    /* de-esser: sibilants clearly louder than the vowels' band */
    r->deess = r->sib_db > 3.0f;              /* a normal voice (TTS) measures about 0 dB */
    r->deess_thr_db = clampf(r->sib_level_db - 6.0f, -50.0f, -5.0f);
}

static void finish(VfCalibrate *c)
{
    VfCalResult *r = &c->result;
    memset(r, 0, sizeof *r);
    if (c->n_sil < 100 || c->n_sp < 100) { fail(c, "too few frames"); return; }

    float tmp[VF_CAL_SPEECH_S * 100 + 8];
    memcpy(tmp, c->sil_in, (size_t)c->n_sil * sizeof(float));
    r->noise_db = to_db(percentile(tmp, c->n_sil, 0.5f));
    memcpy(tmp, c->sil_vad, (size_t)c->n_sil * sizeof(float));
    r->vad_silence = percentile(tmp, c->n_sil, 0.95f);

    /* speech level: peaks of the frames the detector calls voice */
    int nv = 0;
    for (int i = 0; i < c->n_sp; i++) if (c->sp_vad[i] > 0.5f) tmp[nv++] = c->sp_in[i];
    r->voiced_pct = 100.0f * (float)nv / (float)c->n_sp;
    if (nv < c->n_sp / 10) { fail(c, "no speech heard - read the text aloud during the second step"); return; }
    r->speech_db = to_db(percentile(tmp, nv, 0.5f));
    r->speech_p90_db = to_db(percentile(tmp, nv, 0.9f));
    r->snr_db = r->speech_db - r->noise_db;
    if (r->speech_db < -55.0f) { fail(c, "speech is extremely quiet - check the microphone"); return; }

    /* gate: the voice detector's silence behaviour plus a margin; noisy rooms get a stricter gate */
    r->gate_threshold = clampf(r->vad_silence + 0.20f, 0.30f, 0.80f);
    /* compressor: starts 6 dB under a typical syllable peak; make-up brings that peak to about
     * -14 dBFS after a 3:1 compression (2/3 of the 6 dB above the threshold are removed) */
    r->comp_threshold_db = clampf(r->speech_db - 6.0f, -50.0f, -6.0f);
    r->comp_makeup_db = clampf(-14.0f - (r->speech_db - 4.0f), 0.0f, 18.0f);
    /* denoiser strength: a quiet room keeps more of the natural voice */
    r->rn_wet = r->snr_db > 40.0f ? 0.85f : r->snr_db > 25.0f ? 0.95f : 1.0f;
    r->hp_hz = 80.0f;
    analyse_audio(c, r);
    c->state = VF_CAL_DONE;
}

void vf_cal_frame(VfCalibrate *c, const float *f)
{
    c->last_vad = f[VF_M_VAD];
    if (c->state == VF_CAL_SILENCE) {
        if (c->n_sil < VF_CAL_SILENCE_S * 100) {
            c->sil_in[c->n_sil] = f[VF_M_PEAK_IN];
            c->sil_vad[c->n_sil++] = f[VF_M_VAD];
        }
        if (++c->frames >= VF_CAL_SILENCE_S * 100) { c->state = VF_CAL_SPEECH; c->frames = 0; }
    } else if (c->state == VF_CAL_SPEECH) {
        if (c->n_sp < VF_CAL_SPEECH_S * 100) {
            c->sp_in[c->n_sp] = f[VF_M_PEAK_IN];
            c->sp_vad[c->n_sp++] = f[VF_M_VAD];
        }
        if (++c->frames >= VF_CAL_SPEECH_S * 100) finish(c);
    }
}

float vf_cal_progress(const VfCalibrate *c)
{
    const float total = (float)(VF_CAL_SILENCE_S + VF_CAL_SPEECH_S) * 100.0f;
    switch (c->state) {
    case VF_CAL_SILENCE: return (float)c->frames / total;
    case VF_CAL_SPEECH:  return ((float)VF_CAL_SILENCE_S * 100.0f + (float)c->frames) / total;
    case VF_CAL_DONE:    return 1.0f;
    default:             return 0.0f;
    }
}

void vf_cal_apply(const VfCalibrate *c, Pipeline *p)
{
    if (c->state != VF_CAL_DONE) return;
    const VfCalResult *r = &c->result;
    const int g = pipeline_find_fx(p, "gate"), k = pipeline_find_fx(p, "comp"), rn = pipeline_find_fx(p, "rnnoise");
    if (g >= 0) pipeline_set_param(p, g, GATE_THRESHOLD, r->gate_threshold);
    if (k >= 0) {
        pipeline_set_param(p, k, COMP_THRESHOLD_DB, r->comp_threshold_db);
        pipeline_set_param(p, k, COMP_MAKEUP_DB, r->comp_makeup_db);
    }
    if (rn >= 0) pipeline_set_param(p, rn, RNNOISE_PARAM_WET, r->rn_wet);
    if (!r->has_audio) return;
    const int hp = pipeline_find_fx(p, "highpass"), hum = pipeline_find_fx(p, "hum"), eq = pipeline_find_fx(p, "eq"),
              de = pipeline_find_fx(p, "deesser");
    if (hp >= 0) pipeline_set_param(p, hp, 0, r->hp_hz);
    if (hum >= 0 && r->hum_hz) {
        pipeline_set_param(p, hum, HUM_FREQ, (float)r->hum_hz);
        pipeline_set_bypass(p, hum, 0);
    }
    if (eq >= 0) {
        pipeline_set_param(p, eq, EQ_LOW_DB, r->eq_low_db);
        pipeline_set_param(p, eq, EQ_MID_DB, r->eq_mid_db);
        pipeline_set_param(p, eq, EQ_HIGH_DB, r->eq_high_db);
        pipeline_set_param(p, eq, EQ_LOW_FREQ, 150.0f);
        pipeline_set_param(p, eq, EQ_MID_FREQ, 3000.0f);
        pipeline_set_param(p, eq, EQ_MID_Q, 0.8f);
        pipeline_set_param(p, eq, EQ_HIGH_FREQ, 6000.0f);
    }
    if (de >= 0 && r->deess) {
        pipeline_set_param(p, de, DEESS_THRESHOLD_DB, r->deess_thr_db);
        pipeline_set_bypass(p, de, 0);
    }
}
