#include "app/icon_art.h"

#include <math.h>

/* The mic thingy logo, after the hand-drawn sketch, in the GUI's palette:
 * a Win2000-style bevelled frame (neon-green accent band + silver band top/left,
 * graphite edges) around a dark LCD screen with scanlines and a faint grid,
 * showing a tilted handheld microphone (slate body, slots, glowing status LED,
 * mesh grille, cable) next to a small LED level meter like the GUI's.
 * "Off" variant (tray while stopped): accent band and LED turn burgundy, the
 * meter goes dark. Everything is analytic on a 32x32 design grid and
 * supersampled, so it stays clean from 16 px up to 1024 px. */

typedef struct { float r, g, b, a; } Col;

static Col rgb(int r, int g, int b) { return (Col){ (float)r, (float)g, (float)b, 1.0f }; }
static Col mix(Col a, Col b, float t) { return (Col){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1 }; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* GUI palette (see the C_* colours in main_gui.c) */
#define INK        rgb(5, 6, 9)
#define GRAPHITE   rgb(22, 26, 33)
#define SLATE_LO   rgb(30, 34, 43)
#define SILVER     rgb(150, 162, 186)
#define NEON       rgb(57, 255, 20)
#define GREEN_BTN  rgb(40, 168, 32)
#define BURGUNDY   rgb(128, 22, 52)
#define BURG_HI    rgb(196, 60, 96)
#define LCD        rgb(9, 11, 15)
#define LCD_EDGE   rgb(41, 48, 61)
#define AMBER      rgb(255, 190, 50)

/* microphone geometry (design units) */
#define MIC_CX 19.4f
#define MIC_CY 17.8f
#define MIC_ANG 0.7156f        /* 41 degrees: grille end points to the lower right */
#define MIC_L 9.6f             /* half length */
#define MIC_W 3.0f             /* half width */
#define MIC_R 2.4f             /* corner radius */
#define OUTLINE 0.5f

static float rounded_box(float u, float v, float bx, float by, float r)
{
    const float qx = fabsf(u) - bx + r, qy = fabsf(v) - by + r;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

static float bezier_dist(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float best = 1e9f, lx = ax, ly = ay;
    for (int i = 1; i <= 40; i++) {
        const float t = i / 40.0f, it = 1 - t;
        const float x = it * it * ax + 2 * it * t * bx + t * t * cx;
        const float y = it * it * ay + 2 * it * t * by + t * t * cy;
        const float dx = x - lx, dy = y - ly;
        const float h = clampf(((px - lx) * dx + (py - ly) * dy) / (dx * dx + dy * dy + 1e-9f), 0, 1);
        const float ex = px - lx - h * dx, ey = py - ly - h * dy;
        const float d = sqrtf(ex * ex + ey * ey);
        if (d < best) best = d;
        lx = x; ly = y;
    }
    return best;
}

/* LED level meter in the lower-left corner of the screen (like the GUI meters) */
static int meter(float x, float y, int active, int detail, Col *out)
{
    if (!detail) return 0;
    static const float heights[4] = { 2.4f, 3.9f, 5.4f, 4.4f };
    const float x0 = 7.9f, col_w = 1.25f, gap = 0.45f, base = 29.0f, seg = 0.62f, sgap = 0.26f;
    for (int c = 0; c < 4; c++) {
        const float cx = x0 + c * (col_w + gap);
        if (x < cx || x >= cx + col_w || y > base || y < base - 6.0f) continue;
        const float from_bottom = base - y;
        const float pos = fmodf(from_bottom, seg + sgap);
        if (pos > seg) return 0;                                   /* gap between segments */
        const int idx = (int)(from_bottom / (seg + sgap));
        const float top = (idx + 1) * (seg + sgap);
        const int lit = active && top <= heights[c] + 0.3f;
        Col on = idx >= 5 ? AMBER : NEON;
        *out = lit ? on : mix(LCD, on, 0.14f);
        return 1;
    }
    return 0;
}

static Col mic(float x, float y, int active, int detail, int *hit)
{
    const float c = cosf(MIC_ANG), s = sinf(MIC_ANG);
    const float u = (x - MIC_CX) * c + (y - MIC_CY) * s;    /* along the mic, + = grille end */
    const float v = -(x - MIC_CX) * s + (y - MIC_CY) * c;   /* across, - = lit side */
    *hit = 1;

    /* cable: from the upper side of the body out through the right edge */
    const float sx = MIC_CX - 0.30f * MIC_L * c + MIC_W * s, sy = MIC_CY - 0.30f * MIC_L * s - MIC_W * c;
    const float cable = bezier_dist(x, y, sx, sy, 24.8f, 8.4f, 32.5f, 8.0f);

    const float d = rounded_box(u, v, MIC_L, MIC_W, MIC_R);
    if (d > 0) {
        if (cable < 0.62f) return cable < 0.25f && y < 12 ? rgb(46, 52, 66) : INK;   /* cable with a sheen */
        *hit = 0;                                           /* screen (with LED glow) shows through */
        return LCD;
    }
    /* outline: ink at large sizes; at icon sizes a neon (or burgundy) rim keeps the mic readable on the dark screen */
    if (d > -(detail ? OUTLINE : 0.9f)) return detail ? INK : (active ? NEON : BURG_HI);

    const float split = 0.10f * MIC_L;                      /* body | grille boundary */
    if (fabsf(u - split) < 0.42f)                           /* accent ring between body and grille */
        return fabsf(u - split) < 0.17f ? (active ? NEON : BURG_HI) : INK;
    if (u > split) {
        /* grille: dark graphite mesh, lit rim */
        Col base = rgb(46, 52, 66);
        if (v < -MIC_W + 1.25f) base = rgb(92, 104, 126);
        else if (v > MIC_W - 1.0f) base = rgb(28, 32, 41);
        const float gu = (u - split) / 0.9f, gv = (v + 10.0f) / 0.9f;
        const float fu = gu - floorf(gu) - 0.5f, fv = gv - floorf(gv) - 0.5f;
        if (fu * fu + fv * fv < 0.31f * 0.31f) return INK;
        return base;
    }
    for (int k = 0; k < 3; k++) {                           /* three slots */
        const float us = -0.78f * MIC_L + k * 0.20f * MIC_L;
        if (fabsf(u - us) < 0.24f && fabsf(v) < MIC_W * 0.50f) return INK;
    }
    const float lu = -0.20f * MIC_L;                        /* status LED */
    const float ld = sqrtf((u - lu) * (u - lu) + v * v);
    if (ld < 0.66f) {
        Col led = active ? NEON : BURG_HI;
        return ld < 0.25f ? mix(led, rgb(240, 255, 230), 0.6f) : led;   /* hot centre */
    }
    if (ld < 0.98f) return INK;
    /* body shading across the width: lit edge -> slate -> shadow */
    const float t = clampf((v + MIC_W) / (2 * MIC_W), 0, 1);
    if (t < 0.18f) return mix(SILVER, rgb(84, 95, 116), t / 0.18f);
    Col body = mix(rgb(66, 74, 92), SLATE_LO, (t - 0.18f) / 0.82f);
    return detail ? body : mix(body, SILVER, 0.35f);                 /* lighter at icon sizes */
}

static Col screen(float x, float y, int active, int detail)
{
    Col c = LCD;
    if (detail) {
        /* faint blueprint grid + scanlines, as in the GUI background / LCD windows */
        const float gx = fmodf(x - 6.2f, 2.0f), gy = fmodf(y - 6.2f, 2.0f);
        if (gx < 0.08f || gy < 0.08f) c = mix(c, rgb(140, 160, 210), 0.07f);
        if (fmodf(y, 0.5f) < 0.25f) c = mix(c, rgb(0, 0, 0), 0.35f);
    }
    /* green glow from the LED */
    const float cs = cosf(MIC_ANG), sn = sinf(MIC_ANG);
    const float lx = MIC_CX - 0.20f * MIC_L * cs, ly = MIC_CY - 0.20f * MIC_L * sn;
    const float dl = sqrtf((x - lx) * (x - lx) + (y - ly) * (y - ly));
    if (dl < 5.5f) c = mix(c, active ? NEON : BURGUNDY, 0.16f * (1.0f - dl / 5.5f) * (1.0f - dl / 5.5f));
    return c;
}

static Col sample(float x, float y, int active, int detail)
{
    if (x < 0 || y < 0 || x >= 32 || y >= 32) return (Col){ 0, 0, 0, 0 };
    /* outer ink line; right/bottom = graphite shadow side of the bevel */
    if (x < 0.8f || y < 0.8f || x >= 31.2f || y >= 31.2f) return INK;
    if (x >= 29.9f || y >= 29.9f) return (x >= 29.9f && y < 2.0f) || (y >= 29.9f && x < 2.0f) ? INK : GRAPHITE;
    /* top/left bands: neon-green accent (burgundy when off), ink, silver, ink */
    if (x < 2.6f || y < 2.6f) {
        Col band = active ? GREEN_BTN : BURGUNDY;
        const float edge = fminf(x, y);
        if (edge < 1.15f) band = mix(band, active ? NEON : BURG_HI, 0.55f);   /* lit top edge */
        return band;
    }
    if (x < 3.4f || y < 3.4f) return INK;
    if (x < 5.0f || y < 5.0f) return fminf(x, y) < 3.8f ? rgb(196, 206, 224) : SILVER;
    if (x < 5.6f || y < 5.6f) return INK;
    /* LCD screen with an inset bevel: dark top/left, edge colour bottom/right */
    if (x < 6.2f || y < 6.2f) return rgb(2, 3, 4);
    if (x >= 29.3f || y >= 29.3f) return LCD_EDGE;
    int hit;
    Col m = mic(x, y, active, detail, &hit);
    if (hit) return m;
    Col led;
    if (meter(x, y, active, detail, &led)) return led;
    return screen(x, y, active, detail);
}

void vf_icon_render(unsigned char *rgba, int size, int active)
{
    const int ss = size >= 256 ? 2 : size >= 64 ? 3 : 4;
    const int detail = size >= 40;            /* grid, scanlines and meter only where they read */
    const float scale = 32.0f / (float)size;
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    const float x = (px + (sx + 0.5f) / ss) * scale, y = (py + (sy + 0.5f) / ss) * scale;
                    const Col c = sample(x, y, active, detail);
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
