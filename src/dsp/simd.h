/* Two vector kernels with run-time CPU dispatch (AVX2 + FMA when the CPU has them, portable C
 * otherwise): dot product and y += g * x. Used by the long adaptive/FIR filters (echo
 * canceller, low-latency mode). No alignment requirements; any n >= 0. */
#ifndef VF_SIMD_H
#define VF_SIMD_H

float vf_dot(const float *a, const float *b, int n);
void  vf_axpy(float *y, const float *x, float g, int n);      /* y[i] += g * x[i] */
int   vf_simd_avx2(void);                                      /* 1 if the AVX2 kernels are in use */

#endif
