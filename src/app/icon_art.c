#include "app/icon_art.h"

#include <math.h>

/* The mic thingy logo, after the hand-drawn sketch: a retro bevelled frame (green
 * and white bands top/left, black edges) around a navy screen with a tilted
 * handheld microphone - slotted body, mesh grille, status LED and a cable leaving
 * to the right. Everything is computed analytically on a 32x32 design grid and
 * supersampled, so it stays clean from 16 px up to 1024 px. */

typedef struct { float r, g, b, a; } Col;

static Col rgb(int r, int g, int b) { return (Col){ (float)r, (float)g, (float)b, 1.0f }; }

#define FRAME_GREEN_ON   rgb(28, 112, 22)
#define FRAME_GREEN_OFF  rgb(84, 92, 104)
#define BLACK            rgb(6, 6, 10)
#define WHITE            rgb(246, 246, 246)
#define NAVY             rgb(20, 16, 104)
#define NAVY_DEEP        rgb(12, 10, 72)

/* microphone geometry */
#define MIC_CX 18.6f
#define MIC_CY 18.9f
#define MIC_ANG 0.7156f        /* 41 degrees: grille end points to the lower right */
#define MIC_L 10.2f            /* half length */
#define MIC_W 3.1f             /* half width */
#define MIC_R 2.4f             /* corner radius */
#define OUTLINE 0.55f

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static float rounded_box(float u, float v, float bx, float by, float r)
{
    const float qx = fabsf(u) - bx + r, qy = fabsf(v) - by + r;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

/* distance from p to a quadratic bezier, by sampling */
static float bezier_dist(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float best = 1e9f, lx = ax, ly = ay;
    for (int i = 1; i <= 40; i++) {
        const float t = i / 40.0f, it = 1 - t;
        const float x = it * it * ax + 2 * it * t * bx + t * t * cx;
        const float y = it * it * ay + 2 * it * t * by + t * t * cy;
        /* distance to segment (lx,ly)-(x,y) */
        const float dx = x - lx, dy = y - ly;
        const float h = clampf(((px - lx) * dx + (py - ly) * dy) / (dx * dx + dy * dy + 1e-9f), 0, 1);
        const float ex = px - lx - h * dx, ey = py - ly - h * dy;
        const float d = sqrtf(ex * ex + ey * ey);
        if (d < best) best = d;
        lx = x; ly = y;
    }
    return best;
}

static Col mic(float x, float y, int active, int *hit)
{
    const float c = cosf(MIC_ANG), s = sinf(MIC_ANG);
    const float u = (x - MIC_CX) * c + (y - MIC_CY) * s;    /* along the mic, + = grille end */
    const float v = -(x - MIC_CX) * s + (y - MIC_CY) * c;   /* across */
    *hit = 1;

    /* cable: from the upper side of the body out through the right edge */
    const float sx = MIC_CX - 0.30f * MIC_L * c + MIC_W * s, sy = MIC_CY - 0.30f * MIC_L * s - MIC_W * c;
    const float cable = bezier_dist(x, y, sx, sy, 24.5f, 8.6f, 32.5f, 8.2f);

    const float d = rounded_box(u, v, MIC_L, MIC_W, MIC_R);
    if (d > 0) {
        if (cable < 0.62f) return BLACK;
        *hit = 0;
        return NAVY;
    }
    if (d > -OUTLINE) return BLACK;                         /* outline */

    const float split = 0.10f * MIC_L;                      /* body | grille boundary */
    if (fabsf(u - split) < 0.42f) return BLACK;
    if (u > split) {
        /* mesh grille: dark metal with a grid of holes, lighter rim highlight */
        Col base = rgb(78, 84, 98);
        if (v < -MIC_W + 1.3f) base = rgb(120, 128, 146);
        /* holes: 0.92-unit grid aligned with the mic, hole radius 32% of a cell */
        const float gu = (u - split) / 0.92f, gv = (v + 10.0f) / 0.92f;
        const float fu = gu - floorf(gu) - 0.5f, fv = gv - floorf(gv) - 0.5f;
        if (fu * fu + fv * fv < 0.32f * 0.32f) return rgb(14, 14, 20);
        return base;
    }
    /* body: slate with a highlight stripe along the lit side */
    for (int k = 0; k < 3; k++) {                           /* three slots */
        const float us = -0.78f * MIC_L + k * 0.20f * MIC_L;
        if (fabsf(u - us) < 0.26f && fabsf(v) < MIC_W * 0.52f) return BLACK;
    }
    const float lu = -0.20f * MIC_L, lv = 0.0f;              /* status LED */
    const float ld = sqrtf((u - lu) * (u - lu) + (v - lv) * (v - lv));
    if (ld < 0.62f) return active ? rgb(80, 255, 40) : rgb(120, 124, 136);
    if (ld < 0.95f) return BLACK;
    if (v < -MIC_W + 1.25f) return rgb(150, 160, 180);
    if (v > MIC_W - 1.0f) return rgb(40, 44, 54);
    return rgb(62, 68, 82);
}

static Col sample(float x, float y, int active)
{
    if (x < 0 || y < 0 || x >= 32 || y >= 32) return (Col){ 0, 0, 0, 0 };
    /* right / bottom: plain black edge; top / left: green band, black, white band, black */
    if (x >= 30.6f || y >= 30.6f || x < 0.9f || y < 0.9f) return BLACK;
    if (x < 2.6f || y < 2.6f) return active ? FRAME_GREEN_ON : FRAME_GREEN_OFF;
    if (x < 3.5f || y < 3.5f) return BLACK;
    if (x < 5.2f || y < 5.2f) return WHITE;
    if (x < 6.2f || y < 6.2f) return BLACK;
    int hit;
    Col m = mic(x, y, active, &hit);
    if (hit) return m;
    /* screen: navy with a soft vignette towards the corners */
    const float dx = (x - 18.4f) / 12.2f, dy = (y - 18.4f) / 12.2f;
    const float t = clampf((dx * dx + dy * dy - 0.5f) * 0.8f, 0, 1);
    return (Col){ NAVY.r + (NAVY_DEEP.r - NAVY.r) * t, NAVY.g + (NAVY_DEEP.g - NAVY.g) * t,
                  NAVY.b + (NAVY_DEEP.b - NAVY.b) * t, 1 };
}

void vf_icon_render(unsigned char *rgba, int size, int active)
{
    const int ss = size >= 256 ? 2 : size >= 64 ? 3 : 4;
    const float scale = 32.0f / (float)size;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    const float x = (px + (sx + 0.5f) / ss) * scale, y = (py + (sy + 0.5f) / ss) * scale;
                    const Col c = sample(x, y, active);
                    r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
                }
            unsigned char *o = rgba + 4 * (py * size + px);
            const float n = (float)(ss * ss);
            o[3] = (unsigned char)(a / n * 255.0f + 0.5f);
            o[0] = a > 0 ? (unsigned char)(r / a + 0.5f) : 0;
            o[1] = a > 0 ? (unsigned char)(g / a + 0.5f) : 0;
            o[2] = a > 0 ? (unsigned char)(b / a + 0.5f) : 0;
        }
}
