#include "dsp/simd.h"

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#define VF_HAVE_AVX2_KERNELS 1
#include <immintrin.h>
#endif

/* ---- portable versions (also what the compiler turns into SSE2 code) ---- */

static float dot_c(const float *a, const float *b, int n)
{
    float s[8] = { 0 };
    int i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) s[k] += a[i + k] * b[i + k];
    float t = ((s[0] + s[4]) + (s[1] + s[5])) + ((s[2] + s[6]) + (s[3] + s[7]));
    for (; i < n; i++) t += a[i] * b[i];
    return t;
}

static void axpy_c(float *y, const float *x, float g, int n)
{
    for (int i = 0; i < n; i++) y[i] += g * x[i];
}

#ifdef VF_HAVE_AVX2_KERNELS
__attribute__((target("avx2,fma")))
static float dot_avx2(const float *a, const float *b, int n)
{
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
    }
    for (; i + 8 <= n; i += 8) acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
    acc0 = _mm256_add_ps(acc0, acc1);
    __m128 lo = _mm256_castps256_ps128(acc0), hi = _mm256_extractf128_ps(acc0, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 1));
    float t = _mm_cvtss_f32(lo);
    for (; i < n; i++) t += a[i] * b[i];
    return t;
}

__attribute__((target("avx2,fma")))
static void axpy_avx2(float *y, const float *x, float g, int n)
{
    const __m256 vg = _mm256_set1_ps(g);
    int i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(y + i, _mm256_fmadd_ps(vg, _mm256_loadu_ps(x + i), _mm256_loadu_ps(y + i)));
    for (; i < n; i++) y[i] += g * x[i];
}

static int detect(void)
{
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
}
#endif

static int g_avx2 = -1;

int vf_simd_avx2(void)
{
#ifdef VF_HAVE_AVX2_KERNELS
    if (g_avx2 < 0) g_avx2 = detect();
    return g_avx2;
#else
    return 0;
#endif
}

float vf_dot(const float *a, const float *b, int n)
{
#ifdef VF_HAVE_AVX2_KERNELS
    if (vf_simd_avx2()) return dot_avx2(a, b, n);
#endif
    return dot_c(a, b, n);
}

void vf_axpy(float *y, const float *x, float g, int n)
{
#ifdef VF_HAVE_AVX2_KERNELS
    if (vf_simd_avx2()) { axpy_avx2(y, x, g, n); return; }
#endif
    axpy_c(y, x, g, n);
}
