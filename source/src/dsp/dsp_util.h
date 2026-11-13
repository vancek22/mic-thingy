#ifndef VF_DSP_UTIL_H
#define VF_DSP_UTIL_H

#include <math.h>

static inline float vf_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float vf_db_to_lin(float db) { return powf(10.0f, db * 0.05f); }

static inline float vf_lin_to_db(float lin)
{
    return 20.0f * log10f(lin > 1e-9f ? lin : 1e-9f);
}

/* One-pole smoothing coefficient for a time constant (ms): y = x + c*(y - x). */
static inline float vf_coef_ms(float ms, float sample_rate)
{
    if (ms < 0.01f) ms = 0.01f;
    return expf(-1.0f / (ms * 0.001f * sample_rate));
}

#endif
