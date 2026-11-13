#include "app/calibrate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/fx_compressor.h"
#include "dsp/fx_gate.h"

static float to_db(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }

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
    c->state = VF_CAL_SILENCE;
}

static void fail(VfCalibrate *c, const char *why)
{
    c->state = VF_CAL_FAILED;
    snprintf(c->why, sizeof c->why, "%s", why);
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
    if (nv < c->n_sp / 10) { fail(c, "no speech heard - speak normally during the second step"); return; }
    r->speech_db = to_db(percentile(tmp, nv, 0.5f));
    r->speech_p90_db = to_db(percentile(tmp, nv, 0.9f));
    r->snr_db = r->speech_db - r->noise_db;
    if (r->speech_db < -55.0f) { fail(c, "speech is extremely quiet - check the microphone"); return; }

    /* gate: the voice detector's silence behaviour plus a margin; noisy rooms get a stricter gate */
    float thr = r->vad_silence + 0.20f;
    if (thr < 0.30f) thr = 0.30f;
    if (thr > 0.80f) thr = 0.80f;
    r->gate_threshold = thr;
    /* compressor: starts 6 dB under a typical syllable peak; make-up brings that peak to about
     * -14 dBFS after a 3:1 compression (2/3 of the 6 dB above the threshold are removed) */
    r->comp_threshold_db = r->speech_db - 6.0f;
    if (r->comp_threshold_db < -50.0f) r->comp_threshold_db = -50.0f;
    if (r->comp_threshold_db > -6.0f) r->comp_threshold_db = -6.0f;
    float makeup = -14.0f - (r->speech_db - 4.0f);
    if (makeup < 0.0f) makeup = 0.0f;
    if (makeup > 18.0f) makeup = 18.0f;
    r->comp_makeup_db = makeup;
    c->state = VF_CAL_DONE;
}

void vf_cal_frame(VfCalibrate *c, const float *f)
{
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
    const int g = pipeline_find_fx(p, "gate"), k = pipeline_find_fx(p, "comp");
    if (g >= 0) pipeline_set_param(p, g, GATE_THRESHOLD, c->result.gate_threshold);
    if (k >= 0) {
        pipeline_set_param(p, k, COMP_THRESHOLD_DB, c->result.comp_threshold_db);
        pipeline_set_param(p, k, COMP_MAKEUP_DB, c->result.comp_makeup_db);
    }
}
