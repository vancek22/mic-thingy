#include "dsp/spectrum.h"

#include <math.h>
#include <string.h>

#define N VF_SPEC_N
#define PI_F 3.14159265358979f

void vf_spec_init(VfSpectrum *s)
{
    memset(s, 0, sizeof *s);
    for (int k = 0; k < N / 2; k++) {
        s->cs[k] = cosf(2.0f * PI_F * (float)k / N);
        s->sn[k] = -sinf(2.0f * PI_F * (float)k / N);
    }
    int bits = 0;
    while ((1 << bits) < N) bits++;
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        s->rev[i] = (unsigned short)r;
    }
    for (int i = 0; i < N; i++) s->win[i] = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)N);
    for (int k = 0; k < VF_SPEC_BINS; k++) s->in_db[k] = s->out_db[k] = -120.0f;
}

void vf_spec_push(VfSpectrum *s, const float *in, const float *out, int n)
{
    if (n > N) { in += n - N; out += n - N; n = N; }
    memmove(s->in_hist, s->in_hist + n, (size_t)(N - n) * sizeof(float));
    memmove(s->out_hist, s->out_hist + n, (size_t)(N - n) * sizeof(float));
    memcpy(s->in_hist + (N - n), in, (size_t)n * sizeof(float));
    memcpy(s->out_hist + (N - n), out, (size_t)n * sizeof(float));
}

static void fft(VfSpectrum *s)
{
    float *re = s->re, *im = s->im;
    for (int i = 0; i < N; i++) {
        const int j = s->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int b = 0; b < N; b += len)
            for (int k = 0; k < half; k++) {
                const float wr = s->cs[k * step], wi = s->sn[k * step];
                const int a = b + k, c = a + half;
                const float tr = re[c] * wr - im[c] * wi, ti = re[c] * wi + im[c] * wr;
                re[c] = re[a] - tr; im[c] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
    }
}

static void one(VfSpectrum *s, const float *hist, float *db, float smooth)
{
    for (int i = 0; i < N; i++) { s->re[i] = hist[i] * s->win[i]; s->im[i] = 0.0f; }
    fft(s);
    /* a full-scale sine gives amplitude N/4 per side-band bin with a Hann window: scale to 0 dBFS */
    const float scale = 4.0f / (float)N;
    for (int k = 0; k < VF_SPEC_BINS; k++) {
        const float mag = sqrtf(s->re[k] * s->re[k] + s->im[k] * s->im[k]) * scale;
        float v = mag > 1e-6f ? 20.0f * log10f(mag) : -120.0f;
        if (v < -120.0f) v = -120.0f;
        db[k] = v > db[k] ? v : db[k] * smooth + v * (1.0f - smooth);       /* instant attack, smooth release */
    }
}

void vf_spec_update(VfSpectrum *s, float smooth)
{
    if (smooth < 0.0f) smooth = 0.0f;
    if (smooth > 0.99f) smooth = 0.99f;
    one(s, s->in_hist, s->in_db, smooth);
    one(s, s->out_hist, s->out_db, smooth);
}
