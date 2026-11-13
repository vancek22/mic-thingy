#include "dsp/minphase.h"

#include <math.h>
#include <string.h>

#define N VF_MP_N
#define PI_F 3.14159265358979f

void vf_minphase_init(VfMinPhase *mp)
{
    memset(mp, 0, sizeof *mp);
    for (int k = 0; k < N / 2; k++) {
        mp->cs[k] = cosf(2.0f * PI_F * (float)k / N);
        mp->sn[k] = -sinf(2.0f * PI_F * (float)k / N);   /* forward transform: e^(-j...) */
    }
    int bits = 0;
    while ((1 << bits) < N) bits++;
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        mp->rev[i] = (unsigned short)r;
    }
}

/* in-place forward FFT of mp->re/im */
static void fft(VfMinPhase *mp)
{
    float *re = mp->re, *im = mp->im;
    for (int i = 0; i < N; i++) {
        const int j = mp->rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int s = 0; s < N; s += len) {
            for (int k = 0; k < half; k++) {
                const float wr = mp->cs[k * step], wi = mp->sn[k * step];
                const int a = s + k, b = a + half;
                const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

/* inverse FFT (scaled by 1/N) via conjugation */
static void ifft(VfMinPhase *mp)
{
    for (int i = 0; i < N; i++) mp->im[i] = -mp->im[i];
    fft(mp);
    const float inv = 1.0f / N;
    for (int i = 0; i < N; i++) { mp->re[i] *= inv; mp->im[i] = -mp->im[i] * inv; }
}

void vf_minphase_design(VfMinPhase *mp, const float *gains, int n_gains, float *h)
{
    /* last non-zero gain: the model leaves the bins above 20 kHz at 0 */
    int last = n_gains - 1;
    while (last > 0 && gains[last] <= 1e-6f) last--;

    /* magnitude on the FFT grid (linear interpolation of the input grid) */
    const float scale = (float)(n_gains - 1) / (float)(N / 2);
    for (int k = 0; k <= N / 2; k++) {
        const float pos = (float)k * scale;
        int j = (int)pos;
        if (j >= n_gains - 1) j = n_gains - 2;
        const float fr = pos - (float)j;
        float a = gains[j > last ? last : j], b = gains[j + 1 > last ? last : j + 1];
        float g = a + (b - a) * fr;
        mp->mag[k] = g < 0.001f ? 0.001f : g;
    }

    /* real cepstrum of the (symmetric) log magnitude */
    for (int k = 0; k <= N / 2; k++) mp->re[k] = logf(mp->mag[k]);
    for (int k = 1; k < N / 2; k++) mp->re[N - k] = mp->re[k];
    memset(mp->im, 0, sizeof mp->im);
    ifft(mp);                                   /* mp->re = cepstrum c[n] */

    /* fold onto the causal side: minimum phase */
    for (int n = 1; n < N / 2; n++) mp->re[n] *= 2.0f;
    for (int n = N / 2 + 1; n < N; n++) mp->re[n] = 0.0f;
    memset(mp->im, 0, sizeof mp->im);
    fft(mp);                                    /* complex log spectrum of the minimum-phase filter */

    for (int k = 0; k < N; k++) {               /* exp */
        const float e = expf(mp->re[k]), ph = mp->im[k];
        mp->re[k] = e * cosf(ph);
        mp->im[k] = e * sinf(ph);
    }
    ifft(mp);                                   /* impulse response */

    for (int i = 0; i < VF_MP_TAPS; i++) h[i] = mp->re[i];
    for (int i = 0; i < VF_MP_TAPER; i++) {     /* fade the tail */
        const float w = 0.5f * (1.0f + cosf(PI_F * (float)(i + 1) / (float)(VF_MP_TAPER + 1)));
        h[VF_MP_TAPS - VF_MP_TAPER + i] *= w;
    }
}
