#include "app/icon_art.h"

#include <math.h>

/* Layout on a 32x32 design grid (scaled to the requested size, 4x4 supersampled). */
typedef struct { unsigned char r, g, b, a; } Px;

static const Px WHITE = { 245, 245, 245, 255 }, BLACK = { 8, 8, 12, 255 }, GREY = { 128, 128, 136, 255 },
                NAVY = { 24, 26, 118, 255 }, WAVE_ON = { 70, 214, 64, 255 }, WAVE_OFF = { 150, 154, 170, 255 };

static int in(float x, float y, float x0, float y0, float x1, float y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static float sine_y(float x) { return 12.5f - 3.6f * sinf((x - 8.0f) / 17.0f * 6.2831853f); }

/* colour of one sample point in design units */
static Px sample(float x, float y, int active)
{
    const Px wave = active ? WAVE_ON : WAVE_OFF;
    if (!in(x, y, 0, 0, 32, 32)) return (Px){ 0, 0, 0, 0 };
    if (!in(x, y, 1, 1, 31, 31)) return BLACK;                              /* outer outline */
    if (in(x, y, 6, 6, 27, 27)) {                                           /* screen */
        /* sine wave, constant visual thickness */
        if (x >= 8 && x <= 25) {
            const float d = y - sine_y(x);
            const float slope = -3.6f * cosf((x - 8.0f) / 17.0f * 6.2831853f) * 6.2831853f / 17.0f;
            if (fabsf(d) / sqrtf(1.0f + slope * slope) < 0.85f) return wave;
        }
        /* square wave: high (y=19) on 8..13, low (y=23.5) on 13..21, high again 21..25 */
        const float hi = 19.0f, lo = 23.5f, t = 0.85f;
        if ((x >= 8 && x <= 13.4f && fabsf(y - hi) < t) || (x >= 12.6f && x <= 21.4f && fabsf(y - lo) < t) ||
            (x >= 20.6f && x <= 25 && fabsf(y - hi) < t) ||
            (fabsf(x - 13) < t && y >= hi - t && y <= lo + t) || (fabsf(x - 21) < t && y >= hi - t && y <= lo + t))
            return wave;
        return NAVY;
    }
    if (in(x, y, 5, 5, 28, 28)) return BLACK;                               /* screen bezel */
    if (in(x, y, 28, 1, 31, 31) || in(x, y, 1, 28, 31, 31)) return GREY;    /* shadow side */
    if ((in(x, y, 3, 3, 28, 4) || in(x, y, 3, 3, 4, 28))) return BLACK;     /* inner groove (L) */
    return WHITE;                                                           /* lit bevel */
}

void vf_icon_render(unsigned char *rgba, int size, int active)
{
    const int ss = size >= 64 ? 3 : 4;
    const float scale = 32.0f / (float)size;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    const float x = (px + (sx + 0.5f) / ss) * scale, y = (py + (sy + 0.5f) / ss) * scale;
                    const Px c = sample(x, y, active);
                    const float al = c.a / 255.0f;
                    r += c.r * al; g += c.g * al; b += c.b * al; a += al;
                }
            unsigned char *o = rgba + 4 * (py * size + px);
            const float n = (float)(ss * ss);
            o[3] = (unsigned char)(a / n * 255.0f + 0.5f);
            o[0] = a > 0 ? (unsigned char)(r / a + 0.5f) : 0;   /* un-premultiply */
            o[1] = a > 0 ? (unsigned char)(g / a + 0.5f) : 0;
            o[2] = a > 0 ? (unsigned char)(b / a + 0.5f) : 0;
        }
}
