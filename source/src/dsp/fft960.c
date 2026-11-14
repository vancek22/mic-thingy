#include "dsp/fft960.h"

#include <math.h>
#include <string.h>

void vf_fft_init(VfFft *f)
{
    const double pi2 = 6.283185307179586476925286766559;
    for (int k = 0; k < VF_FFT_N; k++) {
        f->tw_re[k] = (float)cos(pi2 * k / VF_FFT_N);
        f->tw_im[k] = (float)-sin(pi2 * k / VF_FFT_N);
    }
    f->nf = 0;
    int n = VF_FFT_N;
    while (n > 1) {
        int p = 4;
        if (n % 4) p = (n % 2) == 0 ? 2 : (n % 3) == 0 ? 3 : 5;
        f->factors[f->nf++] = p;
        n /= p;
    }
}

/* out[0..n) = DFT of in[0], in[stride], ...; factors are consumed from index fi. */
static void work(const VfFft *f, float *ore, float *oim, const float *ire, const float *iim, int stride, int n,
                 int fi, int inv)
{
    if (n == 1) { ore[0] = ire[0]; oim[0] = iim[0]; return; }
    const int p = f->factors[fi], m = n / p;
    for (int q = 0; q < p; q++)
        work(f, ore + q * m, oim + q * m, ire + q * stride, iim + q * stride, stride * p, m, fi + 1, inv);
    /* butterflies: combine p sub-DFTs of length m; twiddle step in the global table = N / n */
    const int tstep = VF_FFT_N / n;
    float tre[5], tim[5];
    int pw[25];
    for (int q = 0; q < p; q++)
        for (int r = 0; r < p; r++) pw[q * p + r] = ((q * r) % p) * (VF_FFT_N / p);
    for (int k = 0; k < m; k++) {
        for (int q = 0; q < p; q++) {                          /* t[q] = sub[q][k] * W_n^(q k) */
            const int idx = q * k * tstep;             /* < N: (p-1)(m-1)N/n */
            const float wr = f->tw_re[idx], wi = inv ? -f->tw_im[idx] : f->tw_im[idx];
            const float xr = ore[q * m + k], xi = oim[q * m + k];
            tre[q] = xr * wr - xi * wi;
            tim[q] = xr * wi + xi * wr;
        }
        for (int r = 0; r < p; r++) {                          /* out[k + r m] = sum_q t[q] W_p^(q r) */
            float sr = tre[0], si = tim[0];
            for (int q = 1; q < p; q++) {
                const int idx = pw[q * p + r];
                const float wr = f->tw_re[idx], wi = inv ? -f->tw_im[idx] : f->tw_im[idx];
                sr += tre[q] * wr - tim[q] * wi;
                si += tre[q] * wi + tim[q] * wr;
            }
            ore[k + r * m] = sr;                               /* written back after all k are read: see below */
            oim[k + r * m] = si;
        }
    }
}

void vf_fft(VfFft *f, float *re, float *im, int inverse)
{
    /* The in-place butterfly above overwrites sub-results it still needs, so run it from a copy of the input
     * into the scratch pair and then copy back. For p sub-DFTs the loop over k only reads ore[q*m + k] and
     * writes ore[k + r*m]: the same set of m*p slots, one k at a time, so it is safe. */
    memcpy(f->scratch_re, re, sizeof f->scratch_re);
    memcpy(f->scratch_im, im, sizeof f->scratch_im);
    work(f, re, im, f->scratch_re, f->scratch_im, 1, VF_FFT_N, 0, inverse);
}
