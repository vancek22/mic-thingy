#include "app/stats.h"

#include <math.h>
#include <string.h>
#include "dsp/pipeline.h"

static float to_db(float lin) { return lin > 1e-6f ? 20.0f * log10f(lin) : -120.0f; }

void vf_stats_init(VfStats *s)
{
    memset(s, 0, sizeof *s);
    s->head = -1;
}

void vf_stats_frame(VfStats *s, const float *f)
{
    s->frames++;
    const float pin = f[VF_M_PEAK_IN], pout = f[VF_M_PEAK_OUT];
    if (pin > s->peak_in) s->peak_in = pin;
    if (pout > s->peak_out) s->peak_out = pout;
    if (f[VF_M_GR] < s->gr) s->gr = f[VF_M_GR];
    if (f[VF_M_VAD] > 0.5f) {
        s->voiced_frames++;
        s->voice_in_sum += to_db(pin);
        if (f[VF_M_LIM] < 0.84f) s->lim_frames++;           /* 0.84 = 1.5 dB of reduction */
    } else {
        s->unvoiced_frames++;
        const float cut = to_db(pin) - to_db(pout);
        s->cut_sum += cut > 0.0f ? (cut > 60.0f ? 60.0f : cut) : 0.0f;
    }
}

void vf_stats_tick(VfStats *s, unsigned xruns_total)
{
    VfStatSec q;
    memset(&q, 0, sizeof q);
    q.in_db = to_db(s->peak_in);
    q.out_db = to_db(s->peak_out);
    q.gr_db = s->gr;
    q.voiced = s->frames ? (float)s->voiced_frames / (float)s->frames : 0.0f;
    q.voice_in_db = s->voiced_frames ? (float)(s->voice_in_sum / s->voiced_frames) : -120.0f;
    q.noise_cut_db = s->unvoiced_frames ? (float)(s->cut_sum / s->unvoiced_frames) : 0.0f;
    q.limiter = s->voiced_frames ? (float)s->lim_frames / (float)s->voiced_frames : 0.0f;
    q.xruns = xruns_total >= s->last_xruns ? xruns_total - s->last_xruns : 0;
    s->last_xruns = xruns_total;
    s->head = (s->head + 1) % VF_STATS_SECONDS;
    s->sec[s->head] = q;
    if (s->count < VF_STATS_SECONDS) s->count++;
    s->frames = s->voiced_frames = s->unvoiced_frames = s->lim_frames = 0;
    s->peak_in = s->peak_out = 0.0f;
    s->gr = 0.0f;
    s->voice_in_sum = s->cut_sum = 0.0;
}

void vf_stats_quality(const VfStats *s, int seconds, VfQualityInfo *out)
{
    memset(out, 0, sizeof *out);
    out->verdict = VF_Q_NO_DATA;
    if (seconds > s->count) seconds = s->count;
    if (seconds < 3) return;
    double voiced_sec = 0, speech_sum = 0, cut_sum = 0, lim_sum = 0, cut_w = 0;
    float max_in = -120.0f;
    for (int i = 0; i < seconds; i++) {
        const VfStatSec *q = &s->sec[(s->head - i + VF_STATS_SECONDS) % VF_STATS_SECONDS];
        if (q->voiced > 0.0f) {
            voiced_sec += q->voiced;
            speech_sum += (double)q->voice_in_db * q->voiced;
            lim_sum += (double)q->limiter * q->voiced;
            if (q->in_db > max_in) max_in = q->in_db;
        }
        cut_sum += (double)q->noise_cut_db * (1.0 - q->voiced);
        cut_w += 1.0 - q->voiced;
    }
    out->voiced_pct = (float)(100.0 * voiced_sec / seconds);
    if (voiced_sec < 1.5) { out->verdict = VF_Q_NO_SPEECH; out->noise_cut_db = cut_w > 0 ? (float)(cut_sum / cut_w) : 0.0f; return; }
    out->speech_db = (float)(speech_sum / voiced_sec);
    out->limiter_pct = (float)(100.0 * lim_sum / voiced_sec);
    out->noise_cut_db = cut_w > 0 ? (float)(cut_sum / cut_w) : 0.0f;
    if (max_in >= -1.0f) out->verdict = VF_Q_TOO_LOUD;
    else if (out->speech_db < -30.0f) out->verdict = VF_Q_TOO_QUIET;
    else if (out->limiter_pct > 30.0f) out->verdict = VF_Q_LIMITER_BUSY;
    else out->verdict = VF_Q_OK;
}

int vf_stats_xrun_history(const VfStats *s, int minutes, unsigned *out)
{
    if (minutes < 1) return 0;
    if (minutes > VF_STATS_SECONDS / 60) minutes = VF_STATS_SECONDS / 60;
    for (int m = 0; m < minutes; m++) {
        unsigned sum = 0;
        for (int k = 0; k < 60; k++) {
            const int back = (minutes - 1 - m) * 60 + k;               /* seconds before "now" (newest = 0) */
            if (back >= s->count) continue;
            sum += s->sec[(s->head - back + VF_STATS_SECONDS * 2) % VF_STATS_SECONDS].xruns;
        }
        out[m] = sum;
    }
    return minutes;
}
