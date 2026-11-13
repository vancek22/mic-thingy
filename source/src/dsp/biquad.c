#include "dsp/biquad.h"

#include <math.h>

#define VF_PI 3.14159265358979323846

/* Coefficients are computed in double, stored as float; filter state is kept. */
static void set(Biquad *b, double b0, double b1, double b2, double a0, double a1, double a2)
{
    b->b0 = (float)(b0 / a0);
    b->b1 = (float)(b1 / a0);
    b->b2 = (float)(b2 / a0);
    b->a1 = (float)(a1 / a0);
    b->a2 = (float)(a2 / a0);
}

void biquad_reset(Biquad *b)
{
    b->b0 = 1.0f; b->b1 = b->b2 = b->a1 = b->a2 = 0.0f;
    b->z1 = b->z2 = 0.0f;
}

void biquad_highpass(Biquad *b, float sr, float freq, float q)
{
    double w = 2.0 * VF_PI * freq / sr, c = cos(w), al = sin(w) / (2.0 * q);
    set(b, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

void biquad_lowshelf(Biquad *b, float sr, float freq, float gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2.0 * VF_PI * freq / sr;
    double c = cos(w), s = sin(w), al = s / 2.0 * sqrt(2.0);   /* shelf slope S = 1 */
    double t = 2.0 * sqrt(A) * al;
    set(b, A * ((A + 1) - (A - 1) * c + t),
           2 * A * ((A - 1) - (A + 1) * c),
           A * ((A + 1) - (A - 1) * c - t),
           (A + 1) + (A - 1) * c + t,
           -2 * ((A - 1) + (A + 1) * c),
           (A + 1) + (A - 1) * c - t);
}

void biquad_highshelf(Biquad *b, float sr, float freq, float gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2.0 * VF_PI * freq / sr;
    double c = cos(w), s = sin(w), al = s / 2.0 * sqrt(2.0);
    double t = 2.0 * sqrt(A) * al;
    set(b, A * ((A + 1) + (A - 1) * c + t),
           -2 * A * ((A - 1) + (A + 1) * c),
           A * ((A + 1) + (A - 1) * c - t),
           (A + 1) - (A - 1) * c + t,
           2 * ((A - 1) - (A + 1) * c),
           (A + 1) - (A - 1) * c - t);
}

void biquad_peaking(Biquad *b, float sr, float freq, float q, float gain_db)
{
    double A = pow(10.0, gain_db / 40.0), w = 2.0 * VF_PI * freq / sr;
    double c = cos(w), al = sin(w) / (2.0 * q);
    set(b, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

void biquad_lowpass(Biquad *b, float sr, float freq, float q)
{
    double w = 2.0 * VF_PI * freq / sr, c = cos(w), al = sin(w) / (2.0 * q);
    set(b, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

void biquad_notch(Biquad *b, float sr, float freq, float q)
{
    double w = 2.0 * VF_PI * freq / sr, c = cos(w), al = sin(w) / (2.0 * q);
    set(b, 1.0, -2 * c, 1.0, 1 + al, -2 * c, 1 - al);
}
