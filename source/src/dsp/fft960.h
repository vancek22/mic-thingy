/* Mixed-radix complex FFT for the neural denoiser's 960-point frames (960 = 2^6 * 3 * 5).
 * Plain recursive decimation in time with precomputed twiddles; no allocation after create. */
#ifndef VF_FFT960_H
#define VF_FFT960_H

#define VF_FFT_N 960

typedef struct VfFft {
    float tw_re[VF_FFT_N], tw_im[VF_FFT_N];     /* exp(-2 pi i k / N) */
    int factors[12];
    int nf;
    float scratch_re[VF_FFT_N], scratch_im[VF_FFT_N];
} VfFft;

void vf_fft_init(VfFft *f);
/* In-place transform of N complex samples. inverse = 1 computes the unscaled inverse (conjugate twiddles). */
void vf_fft(VfFft *f, float *re, float *im, int inverse);

#endif
