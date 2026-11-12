#include "dsp/anc.h"

#include <math.h>
#include <string.h>

void vf_anc_init(VfAnc *a, int taps, float mu, const float *sec, int sec_taps)
{
    memset(a, 0, sizeof *a);
    a->taps = taps < 1 ? 1 : taps > VF_ANC_MAX_TAPS ? VF_ANC_MAX_TAPS : taps;
    a->mu = mu;
    a->gain = 1.0f;
    if (sec && sec_taps > 0) {
        a->sec_taps = sec_taps > VF_ANC_MAX_SEC ? VF_ANC_MAX_SEC : sec_taps;
        memcpy(a->sec, sec, (size_t)a->sec_taps * sizeof(float));
    } else {
        a->sec_taps = 1;
        a->sec[0] = 1.0f;
    }
}

void vf_anc_set_invert(VfAnc *a, int on, float gain)
{
    a->invert_only = on;
    a->gain = gain;
}

float vf_anc_tick(VfAnc *a, float x, float e, int adapt)
{
    /* e belongs to the previous output, so it pairs with the previous filtered reference: adapt first */
    if (adapt && !a->invert_only) {
        float p = 1e-6f;
        for (int k = 0; k < a->taps; k++) p += a->fxh[k] * a->fxh[k];
        const float g = a->mu * e / p;
        for (int k = 0; k < a->taps; k++) a->w[k] -= g * a->fxh[k];
    }
    memmove(a->xh + 1, a->xh, (size_t)(a->taps + a->sec_taps - 1) * sizeof(float));
    a->xh[0] = x;
    if (a->invert_only) return -a->gain * x;

    float fx = 0.0f;                                   /* x' = S^ * x */
    for (int k = 0; k < a->sec_taps; k++) fx += a->sec[k] * a->xh[k];
    memmove(a->fxh + 1, a->fxh, (size_t)(a->taps - 1) * sizeof(float));
    a->fxh[0] = fx;

    float y = 0.0f;
    for (int k = 0; k < a->taps; k++) y += a->w[k] * a->xh[k];
    return y;
}

void vf_anc_identify(float *sec_out, int sec_taps, VfAncPlant plant, void *ctx, int samples)
{
    float h[VF_ANC_MAX_SEC] = { 0 }, xb[VF_ANC_MAX_SEC] = { 0 };
    if (sec_taps > VF_ANC_MAX_SEC) sec_taps = VF_ANC_MAX_SEC;
    unsigned s = 987654321u;
    for (int n = 0; n < samples; n++) {
        s = s * 1664525u + 1013904223u;
        const float probe = 0.5f * (((s >> 8) & 0xffff) / 32768.0f - 1.0f);
        const float ear = plant(ctx, probe);
        memmove(xb + 1, xb, (size_t)(sec_taps - 1) * sizeof(float));
        xb[0] = probe;
        float est = 0.0f, p = 1e-6f;
        for (int k = 0; k < sec_taps; k++) { est += h[k] * xb[k]; p += xb[k] * xb[k]; }
        const float g = 0.5f * (ear - est) / p;
        for (int k = 0; k < sec_taps; k++) h[k] += g * xb[k];
    }
    memcpy(sec_out, h, (size_t)sec_taps * sizeof(float));
}
