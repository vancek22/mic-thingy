/* mic thingy GUI (raylib + raygui) - early-2000s "green phosphor" look.
 *
 *   mic-thingy [--autostart] [--minimized] [--mode 0|1|2] [--lang en|pl] [--in-file f.wav]
 *               [--screenshot shot.png --frames N] [--verbose]
 *
 * The audio runs on miniaudio's thread; this thread only writes parameters
 * (atomics via pipeline_set_param) and drains the meter ring buffer. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#include "app/abtest.h"
#include "app/appwatch.h"
#include "app/automute.h"
#include "app/calibrate.h"
#include "app/hotkeys.h"
#include "app/howl.h"
#include "app/i18n.h"
#include "app/icon_art.h"
#include "app/overlay.h"
#include "app/recorder.h"
#include "app/session.h"
#include "app/soundboard.h"
#include "app/stats.h"
#include "app/theme.h"
#include "app/undo.h"
#include "dsp/spectrum.h"
#include "app/win_chrome.h"
#include "app/settings.h"
#include "app/tray.h"
#include "util/console.h"
#include "util/platform.h"
#include "util/wavio.h"

#define WIN_W 1180
#define WIN_H 792
#define HEADER_H 48
#define TOP   172          /* y of the meter / effect panels */
#define LIST_CHARS 8192
#define NAME_MAX_CHARS 44

/* ------------------------------------------------------------------ theme */

/* Split-complementary palette around the green accent (hue ~110 deg):
 *   neutral  - cool graphite / slate (hue ~220 deg, low saturation): chassis, panels, title strips
 *   accent   - neon green: LEDs, values, meters, anything switched ON
 *   contrast - burgundy (hue ~345 deg): anything switched OFF, errors
 * Amber stays only as the meter warning colour. */
/* Colours come from the active theme (app/theme.c): g_c[] is filled by apply_theme().
 * Split-complementary idea of the default theme: cool neutrals, ONE accent (ON), burgundy (OFF),
 * amber only as the meter warning. Derived shades are computed with mixc() so every theme works. */
static Color g_c[TC_COUNT];
#define C_BG_TOP     g_c[TC_BG_TOP]
#define C_BG_BOTTOM  g_c[TC_BG_BOTTOM]
#define C_PANEL      g_c[TC_PANEL]
#define C_BEVEL_HI   g_c[TC_BEVEL_HI]
#define C_BEVEL_LO   g_c[TC_BEVEL_LO]
#define C_EDGE       g_c[TC_EDGE]
#define C_SLATE_A    g_c[TC_SLATE_A]
#define C_SLATE_B    g_c[TC_SLATE_B]
#define C_NEON       g_c[TC_ACCENT]
#define C_GREEN_BTN  g_c[TC_ON_BTN]
#define C_BURGUNDY   g_c[TC_OFF]
#define C_BURG_HI    g_c[TC_OFF_HI]
#define C_BURG_TEXT  g_c[TC_OFF_TEXT]
#define C_TEXT       g_c[TC_TEXT]
#define C_TEXT_DIM   g_c[TC_TEXT_DIM]
#define C_TITLE      g_c[TC_TITLE]
#define C_LCD_BG     g_c[TC_LCD_BG]
#define C_SEG_OFF    g_c[TC_SEG_OFF]
#define C_AMBER      g_c[TC_WARN]
#define C_RED        g_c[TC_ERROR]

static Color mixc(Color a, Color b, float t)
{
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}
#define C_DISABLED   mixc(C_TEXT_DIM, C_PANEL, 0.55f)

/* text drawn on a dark strip (header, status bar): light themes would otherwise get dark-on-dark */
static int lumac(Color c) { return (299 * c.r + 587 * c.g + 114 * c.b) / 1000; }
static Color on_dark(Color c) { return lumac(c) > 110 ? c : C_TITLE; }

typedef struct { Font ui, bold, title, lcd, lcd_big; } Fonts;
static Fonts F;

static Font load_font(const char *const *paths, int npaths, int size)
{
    int cps[95 + 224], n = 0;
    for (int c = 32; c < 127; c++) cps[n++] = c;
    for (int c = 160; c < 384; c++) cps[n++] = c;      /* Latin-1 + Latin Extended-A (Polish) */
    for (int i = 0; i < npaths; i++)
        if (FileExists(paths[i])) {
            Font f = LoadFontEx(paths[i], size, cps, n);
            if (f.texture.id != 0) return f;
        }
    return GetFontDefault();
}

static void load_fonts(void)
{
    static const char *ui[] = { "C:/Windows/Fonts/tahoma.ttf", "C:/Windows/Fonts/verdana.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" };
    static const char *bold[] = { "C:/Windows/Fonts/tahomabd.ttf", "C:/Windows/Fonts/verdanab.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf" };
    static const char *mono[] = { "C:/Windows/Fonts/lucon.ttf", "C:/Windows/Fonts/consola.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf" };
#define N(a) (int)(sizeof a / sizeof a[0])
    F.ui = load_font(ui, N(ui), 16);
    F.bold = load_font(bold, N(bold), 14);
    F.title = load_font(bold, N(bold), 28);
    F.lcd = load_font(mono, N(mono), 14);
    F.lcd_big = load_font(mono, N(mono), 20);
#undef N
}

static void unload_fonts(void)
{
    Font *all[] = { &F.ui, &F.bold, &F.title, &F.lcd, &F.lcd_big };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
        if (all[i]->texture.id != GetFontDefault().texture.id) UnloadFont(*all[i]);
}

static int hexc(Color c) { return ColorToInt(c); }

static Color header_top(void) { return mixc(C_SLATE_A, C_BEVEL_HI, 0.25f); }

static void apply_theme(int index)
{
    const VfTheme *th = vf_theme_get(index);
    for (int i = 0; i < TC_COUNT; i++) g_c[i] = (Color){ th->c[i].r, th->c[i].g, th->c[i].b, th->c[i].a };
}


static void apply_style(void)
{
    /* Order matters: GuiLoadStyleDefault() unloads the texture of a previously set
     * custom font, so reset the style first and set our font afterwards. */
    GuiLoadStyleDefault();
    GuiSetFont(F.ui);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, hexc(mixc(C_PANEL, C_BEVEL_HI, 0.85f)));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, hexc(mixc(C_PANEL, C_BEVEL_HI, 0.2f)));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, hexc(C_TEXT));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, hexc(mixc(C_NEON, C_TEXT, 0.3f)));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, hexc(mixc(C_PANEL, C_BEVEL_HI, 0.4f)));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, hexc(C_TITLE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, hexc(mixc(C_NEON, C_TITLE, 0.4f)));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, hexc(C_GREEN_BTN));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, hexc(C_TITLE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, hexc(mixc(C_PANEL, C_BEVEL_HI, 0.3f)));
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, hexc(mixc(C_PANEL, C_BG_TOP, 0.4f)));
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, hexc(C_DISABLED));
    GuiSetStyle(DEFAULT, LINE_COLOR, hexc(mixc(C_PANEL, C_BEVEL_HI, 0.85f)));
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, hexc(C_PANEL));
    GuiSetStyle(SLIDER, BORDER_WIDTH, 1);
    GuiSetStyle(SLIDER, SLIDER_PADDING, 1);
    GuiSetStyle(SLIDER, BASE_COLOR_NORMAL, hexc(C_LCD_BG));
    GuiSetStyle(SLIDER, BASE_COLOR_PRESSED, hexc(mixc(C_NEON, C_LCD_BG, 0.25f)));   /* slider fill = accent */
    GuiSetStyle(SLIDER, BASE_COLOR_FOCUSED, hexc(mixc(C_NEON, C_LCD_BG, 0.25f)));
    GuiSetStyle(SLIDER, TEXT_COLOR_FOCUSED, hexc(mixc(C_NEON, C_TITLE, 0.25f)));   /* bar while hovered */
    GuiSetStyle(SLIDER, TEXT_COLOR_PRESSED, hexc(C_NEON));                          /* bar while dragged */
    GuiSetStyle(CHECKBOX, BASE_COLOR_PRESSED, hexc(C_NEON));                       /* check mark */
    GuiSetStyle(CHECKBOX, BORDER_COLOR_NORMAL, hexc(mixc(C_PANEL, C_TEXT_DIM, 0.8f)));
    GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED, hexc(C_LCD_BG));
    GuiSetStyle(TEXTBOX, TEXT_COLOR_PRESSED, hexc(C_NEON));
    GuiSetStyle(TEXTBOX, BORDER_COLOR_PRESSED, hexc(C_NEON));
    GuiSetStyle(DROPDOWNBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(DROPDOWNBOX, TEXT_PADDING, 8);
}

static void text(Font f, const char *s, float x, float y, Color c)
{
    DrawTextEx(f, s, (Vector2){ x, y }, (float)f.baseSize, 0, c);
}

static void glow_text(Font f, const char *s, float x, float y, Color c)
{
    Color g = c;
    g.a = 50;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            if (dx || dy) DrawTextEx(f, s, (Vector2){ x + 2 * dx, y + 2 * dy }, (float)f.baseSize, 0, g);
    DrawTextEx(f, s, (Vector2){ x, y }, (float)f.baseSize, 0, c);
}

/* Raised 3D frame (Win2000-style bevel) with an optional green title strip. */
static void bevel(Rectangle r, int raised)
{
    Color hi = raised ? C_BEVEL_HI : C_BEVEL_LO, lo = raised ? C_BEVEL_LO : C_BEVEL_HI;
    DrawRectangleRec(r, C_PANEL);
    DrawLineEx((Vector2){ r.x, r.y + 1 }, (Vector2){ r.x + r.width, r.y + 1 }, 2, hi);
    DrawLineEx((Vector2){ r.x + 1, r.y }, (Vector2){ r.x + 1, r.y + r.height }, 2, hi);
    DrawLineEx((Vector2){ r.x, r.y + r.height - 1 }, (Vector2){ r.x + r.width, r.y + r.height - 1 }, 2, lo);
    DrawLineEx((Vector2){ r.x + r.width - 1, r.y }, (Vector2){ r.x + r.width - 1, r.y + r.height }, 2, lo);
    DrawRectangleLinesEx((Rectangle){ r.x + 3, r.y + 3, r.width - 6, r.height - 6 }, 1, C_EDGE);
}

/* led: -1 none, 0 off (burgundy), 1 on (green), 2 enabled but idle (dim green) */
static void panel(Rectangle r, const char *title, int led)
{
    bevel(r, 1);
    Rectangle t = { r.x + 4, r.y + 4, r.width - 8, 24 };
    DrawRectangleGradientH((int)t.x, (int)t.y, (int)t.width, (int)t.height, C_SLATE_A, C_SLATE_B);
    DrawLine((int)t.x, (int)t.y, (int)(t.x + t.width), (int)t.y, Fade(C_TITLE, 0.43f));
    DrawLine((int)t.x, (int)(t.y + t.height), (int)(t.x + t.width), (int)(t.y + t.height), Fade(C_NEON, 0.27f));
    float x = t.x + 8;
    if (led >= 0) {
        Color c = led == 1 ? C_NEON : led == 2 ? mixc(C_NEON, C_LCD_BG, 0.62f) : C_BURG_HI;
        DrawCircle((int)x + 4, (int)(t.y + 12), 4.5f, c);
        if (led == 1) DrawCircle((int)x + 4, (int)(t.y + 12), 8, Fade(C_NEON, 0.16f));
        x += 16;
    }
    text(F.bold, title, x, t.y + 5, C_TITLE);
}

/* On/off switch: green when on, burgundy when off. Returns the new state. */
static bool onoff(Rectangle r, bool on, const char *label_on, const char *label_off)
{
    const int props[] = { BASE_COLOR_NORMAL, BORDER_COLOR_NORMAL, TEXT_COLOR_NORMAL,
                          BASE_COLOR_FOCUSED, BORDER_COLOR_FOCUSED, TEXT_COLOR_FOCUSED };
    int saved[6];
    for (int i = 0; i < 6; i++) saved[i] = GuiGetStyle(TOGGLE, props[i]);
    if (on) {
        GuiSetStyle(TOGGLE, BASE_COLOR_NORMAL, hexc(C_GREEN_BTN));
        GuiSetStyle(TOGGLE, BORDER_COLOR_NORMAL, hexc(mixc(C_NEON, C_TEXT, 0.25f)));
        GuiSetStyle(TOGGLE, TEXT_COLOR_NORMAL, hexc(C_TITLE));
        GuiSetStyle(TOGGLE, BASE_COLOR_FOCUSED, hexc(mixc(C_GREEN_BTN, C_NEON, 0.35f)));
        GuiSetStyle(TOGGLE, BORDER_COLOR_FOCUSED, hexc(mixc(C_NEON, C_TITLE, 0.5f)));
        GuiSetStyle(TOGGLE, TEXT_COLOR_FOCUSED, hexc((Color){ 255, 255, 255, 255 }));
    } else {
        GuiSetStyle(TOGGLE, BASE_COLOR_NORMAL, hexc(C_BURGUNDY));
        GuiSetStyle(TOGGLE, BORDER_COLOR_NORMAL, hexc(C_BURG_HI));
        GuiSetStyle(TOGGLE, TEXT_COLOR_NORMAL, hexc(C_BURG_TEXT));
        GuiSetStyle(TOGGLE, BASE_COLOR_FOCUSED, hexc(mixc(C_BURGUNDY, C_BURG_HI, 0.3f)));
        GuiSetStyle(TOGGLE, BORDER_COLOR_FOCUSED, hexc(mixc(C_BURG_HI, C_BURG_TEXT, 0.4f)));
        GuiSetStyle(TOGGLE, TEXT_COLOR_FOCUSED, hexc((Color){ 255, 255, 255, 255 }));
    }
    /* draw as a non-active toggle so our "normal" colours are used in both states */
    bool pressed = false;
    GuiToggle(r, on ? label_on : label_off, &pressed);
    for (int i = 0; i < 6; i++) GuiSetStyle(TOGGLE, props[i], saved[i]);
    return pressed ? !on : on;
}

/* LCD window: dark inset with faint scanlines. */
static void lcd(Rectangle r)
{
    DrawRectangleRec(r, C_LCD_BG);
    for (int y = (int)r.y + 1; y < (int)(r.y + r.height); y += 2)
        DrawLine((int)r.x, y, (int)(r.x + r.width), y, (Color){ 0, 0, 0, 70 });
    /* inset bevel: dark top-left, light bottom-right */
    DrawLine((int)r.x, (int)r.y, (int)(r.x + r.width), (int)r.y, C_BEVEL_LO);
    DrawLine((int)r.x, (int)r.y, (int)r.x, (int)(r.y + r.height), C_BEVEL_LO);
    DrawLine((int)r.x, (int)(r.y + r.height), (int)(r.x + r.width), (int)(r.y + r.height), C_EDGE);
    DrawLine((int)(r.x + r.width), (int)r.y, (int)(r.x + r.width), (int)(r.y + r.height), C_EDGE);
}

/* Segmented LED meter, -60..0 dBFS: green / amber (> -18) / red (> -6), peak-hold segment. */
static void led_meter(Rectangle r, float db, float hold_db)
{
    lcd(r);
    const int seg_w = 4, gap = 2;
    const int n = (int)((r.width - 4) / (seg_w + gap));
    const float t = (db + 60.0f) / 60.0f, h = (hold_db + 60.0f) / 60.0f;
    const int hold_seg = (int)(h * n);
    for (int i = 0; i < n; i++) {
        const float pos = (float)(i + 1) / n;
        const float seg_db = pos * 60.0f - 60.0f;
        Color on = seg_db > -6 ? C_RED : seg_db > -18 ? C_AMBER : C_NEON;
        Color off = { (unsigned char)(on.r / 7), (unsigned char)(on.g / 7), (unsigned char)(on.b / 7), 255 };
        const int lit = pos <= t || i == hold_seg - 1;
        DrawRectangle((int)r.x + 2 + i * (seg_w + gap), (int)r.y + 3, seg_w, (int)r.height - 6, lit ? on : off);
    }
}

static void led_bar(Rectangle r, float t, Color on)
{
    lcd(r);
    const int seg_w = 4, gap = 2;
    const int n = (int)((r.width - 4) / (seg_w + gap));
    Color off = { (unsigned char)(on.r / 7), (unsigned char)(on.g / 7), (unsigned char)(on.b / 7), 255 };
    for (int i = 0; i < n; i++)
        DrawRectangle((int)r.x + 2 + i * (seg_w + gap), (int)r.y + 3, seg_w, (int)r.height - 6,
                      (float)(i + 1) / n <= t ? on : off);
}

static void background(void)
{
    DrawRectangleGradientV(0, 0, WIN_W, WIN_H, C_BG_TOP, C_BG_BOTTOM);
    for (int x = 0; x < WIN_W; x += 24)                          /* faint blueprint grid */
        DrawLine(x, HEADER_H, x, WIN_H, Fade(C_TEXT, 0.03f));
    for (int y = HEADER_H; y < WIN_H; y += 24)
        DrawLine(0, y, WIN_W, y, Fade(C_TEXT, 0.03f));
    /* header: brushed slate "chrome" strip with a neon underline */
    DrawRectangleGradientV(0, 0, WIN_W, HEADER_H, header_top(), C_SLATE_B);
    DrawLine(0, 1, WIN_W, 1, Fade(C_TITLE, 0.35f));
    DrawRectangle(0, HEADER_H - 2, WIN_W, 2, C_NEON);
    DrawRectangle(0, HEADER_H, WIN_W, 5, Fade(C_NEON, 0.09f));
}

/* ------------------------------------------------------------------ helpers */

static int g_demo;   /* --demo: generic device names, for public screenshots */

static void build_list(char *out, size_t cap, const VfDeviceList *l, const char *demo_label)
{
    out[0] = '\0';
    size_t len = 0;
    if (g_demo) {
        for (int i = 0; i < l->count && len + 64 < cap; i++)
            len += (size_t)snprintf(out + len, cap - len, i ? ";%s %d (USB Audio)" : "%s (USB Audio)", demo_label, i + 1);
        return;
    }
    for (int i = 0; i < l->count && len + 8 < cap; i++) {
        char name[NAME_MAX_CHARS + 8];
        int n = 0, chars = 0;
        for (const unsigned char *p = (const unsigned char *)l->dev[i].name; *p && n < (int)sizeof name - 4; p++) {
            if ((*p & 0xC0) != 0x80 && ++chars > NAME_MAX_CHARS) { name[n++] = '.'; name[n++] = '.'; break; }
            name[n++] = (*p == ';') ? ',' : (char)*p;
        }
        name[n] = '\0';
        len += (size_t)snprintf(out + len, cap - len, "%s%s", i ? ";" : "", name);
    }
    if (l->count == 0) snprintf(out, cap, "%s", tr(S_NO_DEVICES));
}

static float lin_to_db(float x) { return x > 1e-6f ? 20.0f * log10f(x) : -120.0f; }

typedef struct {
    float peak_in, peak_out, hold_in, hold_out, hold_t_in, hold_t_out;
    float vad, gate, gr, rms_out;
} Meters;

static void ui_frame(const float *f);          /* gui_tools.inc: statistics, calibration */

static void drain_meters(VfSession *s, Meters *m, float dt)
{
    float buf[VF_METER_FIELDS * 32];
    float pin = 0, pout = 0, rms = 0, gr = 0;
    int frames = 0;
    float vad = m->vad, gate = m->gate;
    unsigned got;
    while ((got = vf_ring_read(&s->meters, buf, sizeof buf / sizeof buf[0])) > 0) {
        for (unsigned k = 0; k + VF_METER_FIELDS <= got; k += VF_METER_FIELDS) {
            const float *f = buf + k;
            if (f[VF_M_PEAK_IN] > pin) pin = f[VF_M_PEAK_IN];
            if (f[VF_M_PEAK_OUT] > pout) pout = f[VF_M_PEAK_OUT];
            if (f[VF_M_RMS_OUT] > rms) rms = f[VF_M_RMS_OUT];
            if (f[VF_M_GR] < gr) gr = f[VF_M_GR];
            ui_frame(f);
            vad = f[VF_M_VAD];
            gate = f[VF_M_GATE];
            frames++;
        }
    }
    const float fall = 30.0f * dt;
    float din = lin_to_db(pin), dout = lin_to_db(pout);
    m->peak_in = din > m->peak_in ? din : m->peak_in - fall;
    m->peak_out = dout > m->peak_out ? dout : m->peak_out - fall;
    if (din >= m->hold_in) { m->hold_in = din; m->hold_t_in = 1.5f; }
    else if ((m->hold_t_in -= dt) <= 0) m->hold_in -= 20.0f * dt;
    if (dout >= m->hold_out) { m->hold_out = dout; m->hold_t_out = 1.5f; }
    else if ((m->hold_t_out -= dt) <= 0) m->hold_out -= 20.0f * dt;
    if (frames) {
        m->vad = vad;
        m->gate = gate;
        m->gr = gr < m->gr ? gr : m->gr + (gr - m->gr) * fminf(1.0f, 6.0f * dt);
        m->rms_out = rms;
    } else if (!s->running) {
        m->vad = 0; m->gate = 1; m->gr = 0;
    }
}

static int find_name(const VfDeviceList *l, const char *name)
{
    if (!name[0]) return -1;
    for (int i = 0; i < l->count; i++)
        if (!strcmp(l->dev[i].name, name)) return i;
    return -1;
}

/* Cheap fingerprint of everything that is saved, to detect changes. */
static unsigned long long settings_hash(const VfSession *s, const VfSettings *st, int a, int b, int c, int d)
{
    unsigned long long h = 1469598103934665603ULL;
#define MIX(x) (h = (h ^ (unsigned long long)(x)) * 1099511628211ULL)
    MIX(a); MIX(b); MIX(c); MIX(d); MIX(st->start_processing); MIX(st->background);
    MIX(atomic_load(&((VfSession *)s)->pipe.bypass_all));
    for (const char *p = st->preset; *p; p++) MIX(*p);
    for (const char *p = st->language; *p; p++) MIX(*p);
    for (const char *p = st->theme; *p; p++) MIX(*p);
    MIX(st->compact); MIX(st->overlay); MIX(st->overlay_corner); MIX(st->per_mic); MIX(st->auto_mute_lock);
    MIX(st->auto_mute_idle_min); MIX(st->howl_guard); MIX(st->ptt_enabled); MIX(st->apps_enabled); MIX(st->sys_enabled); MIX(st->monitor_raw);
    MIX((long long)(st->monitor_gain_db * 10)); MIX((long long)(st->board_gain_db * 10));
    MIX((long long)(st->sys_gain_db * 10)); MIX((long long)(st->sys_duck_db * 10));
    for (const char *p = st->sys_device; *p; p++) MIX(*p);
    for (const char *p = st->hk_mute; *p; p++) MIX(*p);
    for (const char *p = st->hk_ptt; *p; p++) MIX(*p);
    for (const char *p = st->hk_bypass; *p; p++) MIX(*p);
    for (int i = 0; i < 8; i++) { for (const char *p = st->hk_board[i]; *p; p++) MIX(*p); for (const char *p = st->board_path[i]; *p; p++) MIX(*p); }
    for (int i = 0; i < st->n_apps; i++) { for (const char *p = st->apps[i].exe; *p; p++) MIX(*p); for (const char *p = st->apps[i].preset; *p; p++) MIX(*p); }
    for (int i = 0; i < s->pipe.count; i++) {
        MIX(atomic_load(&s->pipe.fx[i]->bypass));
        for (int j = 0; j < s->pipe.fx[i]->n_params; j++) {
            float v = pipeline_get_param(&s->pipe, i, j);
            unsigned u;
            memcpy(&u, &v, sizeof u);
            MIX(u);
        }
    }
#undef MIX
    return h;
}

static void build_preset_list(char *out, size_t cap, VfPresetInfo *list, int *n)
{
    *n = vf_presets_list(list, VF_MAX_PRESETS);
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < *n && len + 4 < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, "%s%s%s", i ? ";" : "",
                                vf_preset_display_name(&list[i]), list[i].builtin ? "" : " *");
}

static void fill_device_names(VfSettings *st, const VfSession *s, int in, int out, int mon, int mode)
{
    snprintf(st->input, sizeof st->input, "%s", in < s->caps.count ? s->caps.dev[in].name : "");
    snprintf(st->output, sizeof st->output, "%s", out < s->plays.count ? s->plays.dev[out].name : "");
    snprintf(st->monitor, sizeof st->monitor, "%s", mon < s->plays.count ? s->plays.dev[mon].name : "");
    st->mode = mode;
    st->bypass = atomic_load(&((VfSession *)s)->pipe.bypass_all);
}

/* ------------------------------------------------------------------ main */

/* ---- window lifetime: in the tray the window AND its OpenGL context are destroyed (the driver
 * alone maps ~80 MB); showing the window creates them again. All app state lives in main(). ---- */
static unsigned rgbi(Color c) { return ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b; }

static void style_titlebar(void)
{
    vf_window_style_titlebar(GetWindowHandle(), rgbi(header_top()), rgbi(C_TITLE), rgbi(C_SLATE_B));
}

/* Switches the colours of the running GUI (fonts and window stay). */
static void retheme(int index)
{
    apply_theme(index);
    GuiSetFont(GetFontDefault());          /* raygui frees its current font in GuiLoadStyleDefault() */
    apply_style();
    style_titlebar();
}

static int g_win_open;
static VfSettings *g_st;                       /* settings, shared with gui_tools.inc */

static int window_pos_ok(int x, int y)
{
    const int mon = GetCurrentMonitor();
    return x > -200 && y > -20 && x < GetMonitorWidth(mon) - 100 && y < GetMonitorHeight(mon) - 100;
}
static Texture2D logo_tex;

static void window_open(int minimized)
{
    SetConfigFlags(FLAG_VSYNC_HINT);               /* no MSAA: the UI is rectangles + bitmap text */
    InitWindow(WIN_W, WIN_H, "mic thingy");
    SetWindowState(FLAG_WINDOW_ALWAYS_RUN);        /* keep the loop alive while minimized */
    SetExitKey(KEY_NULL);                          /* Esc must not quit */
    SetTargetFPS(60);
    if (minimized) MinimizeWindow();
    /* window icon (Windows also gets it from the .exe resource) and a title bar in the
     * header colour instead of the white system one */
    static unsigned char px[64 * 64 * 4];
    vf_icon_render(px, 64, 1);
    SetWindowIcon((Image){ px, 64, 64, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 });
    style_titlebar();
    /* header logo rendered at its display size (36 px) so it gets the icon-size variant */
    static unsigned char logo_px[36 * 36 * 4];
    vf_icon_render(logo_px, 36, 1);
    logo_tex = LoadTextureFromImage((Image){ logo_px, 36, 36, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 });
    load_fonts();
    apply_style();
    g_win_open = 1;
    if (g_st) {
        const Vector2 wp = { (float)(g_st->compact ? g_st->cwin_x : g_st->win_x), (float)(g_st->compact ? g_st->cwin_y : g_st->win_y) };
        if (wp.x != -32000 && window_pos_ok((int)wp.x, (int)wp.y)) SetWindowPosition((int)wp.x, (int)wp.y);
        if (g_st->compact) { SetWindowSize(300, 150); SetWindowState(FLAG_WINDOW_TOPMOST); }
    }
}

static void remember_window_pos(void);

static void window_close(void)
{
    if (!g_win_open) return;
    remember_window_pos();
    /* raygui keeps a pointer to the active font and frees it in the next GuiLoadStyleDefault():
     * point it back to the built-in font first, or reopening the window frees our fonts twice. */
    GuiSetFont(GetFontDefault());
    UnloadTexture(logo_tex);
    unload_fonts();
    CloseWindow();
    g_win_open = 0;
    vf_trim_memory();                              /* hand the freed pages back to the OS */
}

#include "gui_tools.inc"

int main(int argc, char **argv)
{
    int autostart = 0, mode = -1, frames_to_shot = 0, verbose = 0, minimized = 0, demo = 0, tab_arg = 0;
    const char *in_file = NULL, *shot = NULL, *lang_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autostart")) autostart = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-file") && i + 1 < argc) in_file = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames_to_shot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lang_arg = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--minimized")) minimized = 1;
        else if (!strcmp(argv[i], "--demo")) demo = 1;
        else if (!strcmp(argv[i], "--tab") && i + 1 < argc) tab_arg = atoi(argv[++i]);
    }

    /* One mic thingy at a time: a second launch (e.g. autostart + manual) would
     * open the microphone twice and feed the virtual mic twice. */
    if (!shot && !vf_single_instance()) return 0;     /* the running one shows its window */

    static VfSession s;
    int init_ok = vf_session_init(&s, 1) == 0;

    /* settings.ini: devices by name, mode, language, effect parameters */
    VfSettings st;
    g_st = &st;
    vf_settings_defaults(&st);
    char settings_file[VF_PATH_MAX] = "";
    int have_settings_path = vf_settings_path(settings_file, sizeof settings_file) == 0;
    if (init_ok && have_settings_path) vf_settings_load(&st, &s.pipe, settings_file);
    if (lang_arg) snprintf(st.language, sizeof st.language, "%s", lang_arg);
    vf_i18n_set(vf_lang_from_code(st.language));
    apply_theme(vf_theme_find(st.theme));
    int autostart_on = vf_autostart_get();

    float *file_buf = NULL;
    long file_len = 0;
    if (in_file) file_buf = vf_wav_read_mono(in_file, VF_SAMPLE_RATE, &file_len);

    /* Background mode: closing the window hides it to the tray icon (Windows) or
     * minimizes it (no tray available); audio keeps running until "Quit". */
    const int tray_ok = !shot && vf_tray_start() == 0;
    const int start_hidden = minimized && st.background && tray_ok;

    SetTraceLogLevel(verbose ? LOG_INFO : LOG_WARNING);
    TraceLog(LOG_INFO, "mic thingy: tray %s, background %d, start hidden %d",
             tray_ok ? "ok" : "unavailable", st.background, start_hidden);
    /* Started to the tray: no window, no OpenGL context at all (that is most of the memory). */
    if (!start_hidden) window_open(minimized);
    if (vf_autostart_migrate()) autostart_on = 1;   /* old "VoiceFilter" entry -> this exe */

    /* selections */
    int sel_in = init_ok ? vf_session_default_mic(&s) : 0;
    if (sel_in < 0) sel_in = 0;
    int sel_out = 0, sel_mon = 0;
    for (int i = 0; i < s.plays.count; i++)
        if (s.plays.dev[i].is_default) sel_out = sel_mon = i;
    if (sel_mon < s.plays.count && vf_contains_ci(s.plays.dev[sel_mon].name, "CABLE"))
        for (int i = 0; i < s.plays.count; i++)
            if (!vf_contains_ci(s.plays.dev[i].name, "CABLE")) { sel_mon = i; break; }
    int k;
    if ((k = find_name(&s.caps, st.input)) >= 0) sel_in = k;
    if ((k = find_name(&s.plays, st.output)) >= 0) sel_out = k;
    if ((k = find_name(&s.plays, st.monitor)) >= 0) sel_mon = k;
    int sel_mode = mode >= 0 && mode <= 2 ? mode : (st.mode >= 0 && st.mode <= 2 ? st.mode : VF_OUT_VMIC);
    int sel_lang = (int)vf_i18n_get();
    int edit_in = 0, edit_out = 0, edit_preset = 0, edit_name = 0;
    U.tab = tab_arg >= 0 && tab_arg <= 4 ? tab_arg : 0;     /* 0..2 effect pages, 3 tools, 4 settings (--tab N for screenshots) */
    U.compact = st.compact;
    vf_undo_init(&U.undo, &s.pipe);
    vf_snap_capture(&s.pipe, &U.seen);
    vf_stats_init(&U.stats);
    vf_ab_init(&U.ab);
    vf_rec_init(&U.rec);
    vf_spec_init(&U.spec);
    U.scope_ready = vf_ring_init(&U.scope, 16384) == 0;
    vf_automute_init(&U.am, vf_now_s());
    U.hk_capture = -1;
    U.app_active_rule = -1;
    U.prev_sel_in = -2;
    sync_hotkeys();
    for (int i = 0; i < VF_BOARD_SLOTS; i++)
        if (st.board_path[i][0] && vf_board_load(&s.board, i, st.board_path[i]) != 0) st.board_path[i][0] = '\0';
    static char list_in[LIST_CHARS], list_out[LIST_CHARS], list_presets[LIST_CHARS];
    g_demo = demo;
    build_list(list_in, sizeof list_in, &s.caps, "Microphone");
    build_list(list_out, sizeof list_out, &s.plays, "Speakers");
    static VfPresetInfo presets[VF_MAX_PRESETS];
    int n_presets = 0, sel_preset = 0, shown_preset = 0;
    build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
    for (int i = 0; i < n_presets; i++) if (!strcmp(presets[i].name, st.preset)) sel_preset = shown_preset = i;
    char preset_name[64] = "";
    int ve_fx = -1, ve_param = -1, vad_drag = 0;     /* value being typed / VAD threshold drag */
    char ve_buf[32] = "";

    char status[300] = "", note[200] = "";
    g_note = note; g_note_cap = sizeof note;
    if (!init_ok) snprintf(status, sizeof status, "%s", s.error);

    Meters m = { -120, -120, -120, -120, 0, 0, 0, 1, 0, 0 };
    int want_running = (autostart || st.start_processing) && init_ok;
    int applied_in = -1, applied_out = -1, applied_mon = -1, applied_mode = -1;
    int frame_no = 0;
    unsigned long long saved_hash = settings_hash(&s, &st, sel_in, sel_out, sel_mon, sel_mode);
    double dirty_since = -1, retry_at = 0;
    int quit = 0, hidden = start_hidden, refresh_before_start = 0;
    if (start_hidden) snprintf(note, sizeof note, "%s", tr(S_N_IN_TRAY));

    while (!quit) {
        static double last_t;
        const double now_t = vf_now_s();
        const float dt = last_t > 0 ? (float)(now_t - last_t) : 0.016f;
        last_t = now_t;

        /* ---- window close button: hide to tray in background mode, else quit ---- */
        if (g_win_open && WindowShouldClose()) {
            if (st.background && !shot) {
                if (tray_ok) { window_close(); hidden = 1; }        /* destroys window + GL context */
                else MinimizeWindow();
                edit_in = edit_out = edit_preset = edit_name = 0;
            } else quit = 1;
        }

        /* ---- tray icon menu ---- */
        for (int ev; (ev = vf_tray_poll()) != VF_TRAY_NONE;) {
            if (ev == VF_TRAY_SHOW) {
                if (!g_win_open) window_open(0);
                else { if (IsWindowMinimized()) RestoreWindow(); SetWindowFocused(); }
                hidden = 0;
            } else if (ev == VF_TRAY_QUIT) quit = 1;
            else if (ev == VF_TRAY_TOGGLE_BYPASS) atomic_store(&s.pipe.bypass_all, !atomic_load(&s.pipe.bypass_all));
            else if (ev == VF_TRAY_TOGGLE_RUN) { want_running = !want_running; retry_at = 0; applied_in = -2; }
            else if (ev == VF_TRAY_TOGGLE_MUTE) vf_automute_toggle(&U.am, vf_now_s());
        }
        if (g_win_open) SetTargetFPS(IsWindowMinimized() ? 10 : U.compact ? 20 : IsWindowFocused() ? 60 : s.running ? 20 : 10);   /* M4: draw less when not in use */

        /* ---- (re)start the session when the wanted configuration changes ---- */
        if (init_ok) {
            /* a device that vanished (unplugged, driver reset) stops the stream: restart it */
            if (s.running && ma_device_get_state(&s.eng.dev) != ma_device_state_started) {
                vf_session_stop(&s);
                snprintf(status, sizeof status, "%s", tr(S_N_DEVICE_LOST));
                retry_at = vf_now_s() + 2.0;
                refresh_before_start = 1;
            }
            int cfg_changed = sel_in != applied_in || sel_out != applied_out || sel_mon != applied_mon ||
                              sel_mode != applied_mode;
            if (want_running && (!s.running || cfg_changed) && vf_now_s() >= retry_at) {
                if (refresh_before_start && !s.running) {
                    /* device indices may have changed: re-enumerate and find ours by name */
                    char n_in[256], n_out[256], n_mon[256];
                    snprintf(n_in, sizeof n_in, "%s", sel_in < s.caps.count ? s.caps.dev[sel_in].name : "");
                    snprintf(n_out, sizeof n_out, "%s", sel_out < s.plays.count ? s.plays.dev[sel_out].name : "");
                    snprintf(n_mon, sizeof n_mon, "%s", sel_mon < s.plays.count ? s.plays.dev[sel_mon].name : "");
                    if (vf_session_refresh_devices(&s) == 0) {
                        if ((k = find_name(&s.caps, n_in)) >= 0) sel_in = k;
                        if ((k = find_name(&s.plays, n_out)) >= 0) sel_out = k;
                        if ((k = find_name(&s.plays, n_mon)) >= 0) sel_mon = k;
                        build_list(list_in, sizeof list_in, &s.caps, "Microphone");
                        build_list(list_out, sizeof list_out, &s.plays, "Speakers");
                    }
                }
                VfSessionConfig c;
                memset(&c, 0, sizeof c);
                c.capture = file_buf ? -1 : sel_in;
                c.mode = (VfOutMode)sel_mode;
                c.playback = sel_mode == VF_OUT_HEADPHONES ? sel_out : -1;
                c.monitor = sel_mode == VF_OUT_VMIC_MONITOR ? sel_mon : -1;
                c.file_src = file_buf;
                c.file_len = file_len;
                c.monitor_gain_db = st.monitor_gain_db;
                c.monitor_raw = st.monitor_raw;
                if (vf_session_start(&s, &c) == 0) {
                    status[0] = '\0'; refresh_before_start = 0;
                    EngineLatency l;              /* device mixer not at 48 kHz -> resampler adds delay */
                    engine_latency(&s.eng, &l);
                    const unsigned bad = l.cap_mix_rate && l.cap_mix_rate != VF_SAMPLE_RATE ? l.cap_mix_rate
                                       : l.play_mix_rate && l.play_mix_rate != VF_SAMPLE_RATE ? l.play_mix_rate : 0;
                    if (bad) snprintf(note, sizeof note, tr(S_N_RESAMPLE_FMT), bad);
                }
                else {
                    /* keep trying (e.g. a USB headset that appears a few seconds after login) */
                    snprintf(status, sizeof status, tr(S_N_RETRY_FMT), s.error);
                    retry_at = vf_now_s() + 3.0;
                    refresh_before_start = 1;
                }
                applied_in = sel_in; applied_out = sel_out; applied_mon = sel_mon; applied_mode = sel_mode;
            } else if (!want_running && s.running) {
                vf_session_stop(&s);
            }
            if (!want_running && !s.running && retry_at > 0) { retry_at = 0; status[0] = '\0'; }
            drain_meters(&s, &m, dt);
        }
        vf_tray_set_state(s.running, atomic_load(&s.pipe.bypass_all));
        {
            UiLink L = { &sel_in, &sel_preset, &shown_preset, presets, n_presets, m.vad,
                         edit_in || edit_out || edit_preset || edit_name || ve_fx >= 0 || U.board_edit_on || U.hk_capture >= 0 };
            ui_tick(&s, vf_now_s(), g_win_open, &L);
        }

        /* ---- autosave settings.ini one second after the last change ---- */
        if (init_ok && have_settings_path) {
            unsigned long long h = settings_hash(&s, &st, sel_in, sel_out, sel_mon, sel_mode);
            if (h != saved_hash) {
                if (dirty_since < 0) dirty_since = vf_now_s();
                if (vf_now_s() - dirty_since > 1.0) {
                    fill_device_names(&st, &s, sel_in, sel_out, sel_mon, sel_mode);
                    if (vf_settings_save(&st, &s.pipe, settings_file) != 0)
                        snprintf(status, sizeof status, tr(S_N_SAVE_SETTINGS_FAIL), settings_file);
                    saved_hash = h;
                    dirty_since = -1;
                }
            }
        }

        if (hidden || !g_win_open) {       /* in the tray: no window at all, only tray events, retries, autosave */
            vf_sleep_ms(50);
            continue;
        }

        if (U.compact) {
            BeginDrawing();
            draw_compact(&s, &st, &m);
            EndDrawing();
            if (shot && ++frame_no >= (frames_to_shot > 0 ? frames_to_shot : 60)) { TakeScreenshot(shot); break; }
            continue;
        }

        const Vector2 mouse = GetMousePosition();
        int cursor = MOUSE_CURSOR_DEFAULT;

        BeginDrawing();
        background();
        if (edit_in || edit_out || edit_preset) GuiLock();

        /* ---- header: logo, subtitle, language ---- */
        DrawTexturePro(logo_tex, (Rectangle){ 0, 0, (float)logo_tex.width, (float)logo_tex.height },
                       (Rectangle){ 12, 6, 36, 36 }, (Vector2){ 0, 0 }, 0, WHITE);
        glow_text(F.title, "MIC THINGY", 58, 8, on_dark(C_NEON));
        const float logo_w = MeasureTextEx(F.title, "MIC THINGY", (float)F.title.baseSize, 0).x;
        text(F.lcd, "v1.0", 66 + logo_w, 12, on_dark(C_TEXT_DIM));
        text(F.ui, tr(S_SUBTITLE), 66 + logo_w, 26, on_dark(C_TEXT));
        {   /* blinking "power" LED */
            const int on = s.running && ((int)(vf_now_s() * 2) % 2 || !atomic_load(&s.pipe.bypass_all));
            DrawCircle(WIN_W - 128, 24, 6, on ? C_NEON : C_SEG_OFF);
            if (on) DrawCircle(WIN_W - 128, 24, 12, Fade(C_NEON, 0.18f));
        }
        /* undo / redo / hold-to-compare / mute / compact */
        {
            const int can_undo = vf_undo_can_undo(&U.undo), can_redo = vf_undo_can_redo(&U.undo);
            if (!can_undo) GuiDisable();
            if (GuiButton((Rectangle){ 520, 11, 56, 26 }, tr(S_UNDO)) && can_undo) {
                vf_undo_commit(&U.undo, &s.pipe); vf_undo_undo(&U.undo, &s.pipe); vf_snap_capture(&s.pipe, &U.seen); U.undo_pending = 0;
            }
            GuiEnable();
            if (!can_redo) GuiDisable();
            if (GuiButton((Rectangle){ 580, 11, 56, 26 }, tr(S_REDO)) && can_redo) { vf_undo_redo(&U.undo, &s.pipe); vf_snap_capture(&s.pipe, &U.seen); U.undo_pending = 0; }
            GuiEnable();
            Rectangle cb = { 644, 11, 92, 26 };
            GuiButton(cb, tr(S_COMPARE));
            const int hold = CheckCollisionPointRec(mouse, cb) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !GuiIsLocked();
            if (hold && !U.comparing) { vf_snap_capture(&s.pipe, &U.cmp_snap); vf_preset_apply(&s.pipe, st.preset); U.comparing = 1; }
            else if (!hold && U.comparing) { vf_snap_apply(&s.pipe, &U.cmp_snap); U.comparing = 0; vf_snap_capture(&s.pipe, &U.seen); U.undo_pending = 0; }
            const int muted = vf_automute_effective(&U.am) || U.ptt_block;
            GuiSetFont(F.bold);
            if (onoff((Rectangle){ 748, 9, 150, 30 }, !muted, tr(S_MIC_ON), tr(S_MIC_MUTED)) != !muted) {
                const int now_m = vf_automute_toggle(&U.am, vf_now_s());
                snprintf(note, sizeof note, "%s", tr(now_m ? S_N_MUTED : S_N_UNMUTED));
            }
            GuiSetFont(F.ui);
            if (GuiButton((Rectangle){ 906, 11, 56, 26 }, tr(S_COMPACT_BTN))) apply_window_mode(1);
        }
        int lang = sel_lang;
        GuiToggleGroup((Rectangle){ WIN_W - 104, 12, 42, 24 }, "EN;PL", &lang);
        if (lang != sel_lang) {
            sel_lang = lang;
            vf_i18n_set((VfLang)lang);
            snprintf(st.language, sizeof st.language, "%s", vf_lang_code((VfLang)lang));
            build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
            build_list(list_in, sizeof list_in, &s.caps, "Microphone");
            build_list(list_out, sizeof list_out, &s.plays, "Speakers");
            note[0] = '\0';
        }

        /* ---- row 1: microphone, output mode, start/stop ---- */
        const float r1 = HEADER_H + 12, r2 = r1 + 36, r3 = r2 + 36;
        GuiLabel((Rectangle){ 16, r1, 92, 28 }, tr(S_MICROPHONE));
        Rectangle r_in = { 110, r1, 400, 28 };
        GuiLabel((Rectangle){ 526, r1, 70, 28 }, tr(S_OUTPUT));
        GuiToggleGroup((Rectangle){ 596, r1, 134, 28 }, tr(S_MODES), &sel_mode);
        {   /* audio on/off: green while running, burgundy while stopped */
            const bool now = s.running || (want_running && retry_at > 0);
            if (onoff((Rectangle){ 1012, r1, 152, 28 }, now, tr(S_STOP), tr(S_START)) != now) {
                want_running = !now;
                retry_at = 0;
                if (want_running) applied_in = -2;        /* force (re)start */
            }
        }

        /* ---- row 2: second device / vmic hint, filter on/off ---- */
        Rectangle r_out = { 110, r2, 400, 28 };
        if (sel_mode == VF_OUT_HEADPHONES) GuiLabel((Rectangle){ 16, r2, 92, 28 }, tr(S_SPEAKERS));
        else if (sel_mode == VF_OUT_VMIC_MONITOR) GuiLabel((Rectangle){ 16, r2, 92, 28 }, tr(S_MONITOR));
        if (sel_mode != VF_OUT_HEADPHONES) {
            char vm[200];
            snprintf(vm, sizeof vm, tr(S_VMIC_HINT), vf_session_vmic_name(&s));
            text(F.ui, vm, 526, r2 + 6, C_TEXT_DIM);
        }
        const bool filt = !atomic_load(&s.pipe.bypass_all);
        if (onoff((Rectangle){ 1012, r2, 152, 28 }, filt, tr(S_FILTER_ON), tr(S_FILTER_OFF)) != filt)
            atomic_store(&s.pipe.bypass_all, filt ? 1 : 0);

        /* ---- row 3: presets + app options ---- */
        GuiLabel((Rectangle){ 16, r3, 92, 28 }, tr(S_PRESET));
        Rectangle r_preset = { 110, r3, 200, 28 };
        if (GuiTextBox((Rectangle){ 318, r3, 150, 28 }, preset_name, sizeof preset_name - 1, edit_name)) edit_name = !edit_name;
        if (GuiButton((Rectangle){ 474, r3, 100, 28 }, tr(S_SAVE_AS))) {
            char safe[64];
            if (vf_preset_sanitize(preset_name, safe, sizeof safe) != 0)
                snprintf(note, sizeof note, "%s", tr(S_N_NAME_MISSING));
            else if (vf_preset_is_builtin(safe))
                snprintf(note, sizeof note, tr(S_N_BUILTIN_NAME), safe);
            else if (vf_preset_save(&s.pipe, safe) != 0)
                snprintf(note, sizeof note, "%s", tr(S_N_SAVE_FAILED));
            else {
                build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
                for (int i = 0; i < n_presets; i++) if (!strcmp(presets[i].name, safe)) sel_preset = shown_preset = i;
                snprintf(st.preset, sizeof st.preset, "%s", safe);
                snprintf(note, sizeof note, tr(S_N_SAVED), safe);
                preset_name[0] = '\0';
                edit_name = 0;
            }
        }
        const int user_preset = sel_preset < n_presets && !presets[sel_preset].builtin;
        if (!user_preset) GuiDisable();
        if (GuiButton((Rectangle){ 580, r3, 72, 28 }, tr(S_DELETE)) && user_preset) {
            char gone[64];
            snprintf(gone, sizeof gone, "%s", presets[sel_preset].name);
            if (vf_preset_delete(gone) == 0) {
                build_preset_list(list_presets, sizeof list_presets, presets, &n_presets);
                sel_preset = shown_preset = 0;
                snprintf(st.preset, sizeof st.preset, "%s", presets[0].name);
                snprintf(note, sizeof note, tr(S_N_DELETED), gone);
            }
        }
        GuiEnable();
        bool as = autostart_on != 0;
        GuiCheckBox((Rectangle){ 676, r3 + 5, 18, 18 }, tr(S_START_WITH_SYSTEM), &as);
        if ((int)as != autostart_on) {
            if (vf_autostart_set(as) == 0) {
                autostart_on = as;
                if (as) {
                    /* starting with the system only makes sense if it then filters in the background */
                    st.start_processing = 1;
                    st.background = 1;
                    snprintf(note, sizeof note, "%s", tr(S_N_AUTOSTART_ON));
                } else snprintf(note, sizeof note, "%s", tr(S_N_AUTOSTART_OFF));
            } else snprintf(status, sizeof status, "%s", tr(S_N_AUTOSTART_FAIL));
        }
        bool sp = st.start_processing != 0;
        GuiCheckBox((Rectangle){ 846, r3 + 5, 18, 18 }, tr(S_FILTER_ON_LAUNCH), &sp);
        st.start_processing = sp;
        bool bg = st.background != 0;
        GuiCheckBox((Rectangle){ 1012, r3 + 5, 18, 18 }, tr(S_RUN_IN_BACKGROUND), &bg);
        if ((int)bg != st.background) {
            st.background = bg;
            snprintf(note, sizeof note, "%s", tr(bg ? (tray_ok ? S_N_BG_TRAY : S_N_BG_MIN) : S_N_BG_OFF));
        }

        /* ---- meters ---- */
        Rectangle mb = { 16, TOP, 250, 586 };
        panel(mb, tr(S_LEVELS), -1);
        float y = TOP + 40;
        const float mx = 30, mw = 222;
        const Color val_c = s.running ? C_NEON : C_TEXT_DIM;   /* readouts glow only while running */
        char t[128];
        text(F.ui, tr(S_INPUT), mx, y, C_TEXT);
        snprintf(t, sizeof t, "%6.1f dB", (double)(m.hold_in < -99 ? -99 : m.hold_in));
        text(F.lcd, t, mx + mw - MeasureTextEx(F.lcd, t, 14, 0).x, y + 2, val_c);
        y += 22;
        led_meter((Rectangle){ mx, y, mw, 20 }, m.peak_in, m.hold_in); y += 36;

        text(F.ui, tr(S_OUTPUT_LEVEL), mx, y, C_TEXT);
        snprintf(t, sizeof t, "%6.1f dB", (double)(m.hold_out < -99 ? -99 : m.hold_out));
        text(F.lcd, t, mx + mw - MeasureTextEx(F.lcd, t, 14, 0).x, y + 2, val_c);
        y += 22;
        led_meter((Rectangle){ mx, y, mw, 20 }, m.peak_out, m.hold_out); y += 44;

        /* VAD with the gate threshold marker and a "voice" lamp */
        const int gi = pipeline_find_fx(&s.pipe, "gate");
        float thr = gi >= 0 ? pipeline_get_param(&s.pipe, gi, 0) : 0.5f;
        text(F.ui, tr(S_VAD), mx, y, C_TEXT); y += 22;
        {   /* the amber marker is the gate threshold: drag it (or use the wheel) to change it */
            Rectangle vb = { mx, y, mw - 30, 18 };
            Rectangle hit = { vb.x - 4, vb.y - 6, vb.width + 8, vb.height + 12 };
            const int over = gi >= 0 && !GuiIsLocked() && ve_fx < 0 && CheckCollisionPointRec(mouse, hit);
            if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) vad_drag = 1;
            if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) vad_drag = 0;
            if (vad_drag) {
                thr = (mouse.x - vb.x) / vb.width;
                thr = thr < 0 ? 0 : thr > 1 ? 1 : roundf(thr * 100.0f) / 100.0f;
                pipeline_set_param(&s.pipe, gi, 0, thr);
            } else if (over && GetMouseWheelMove() != 0) {
                thr += GetMouseWheelMove() * 0.01f;
                pipeline_set_param(&s.pipe, gi, 0, thr);
                thr = pipeline_get_param(&s.pipe, gi, 0);
            }
            if (over || vad_drag) cursor = MOUSE_CURSOR_RESIZE_EW;
            const int voice_ = s.running && m.vad >= thr;
            led_bar(vb, m.vad, voice_ ? C_NEON : mixc(C_NEON, C_LCD_BG, 0.45f));
            const float tx = vb.x + vb.width * thr;
            DrawRectangle((int)tx - 1, (int)y - 3, 2, 24, C_AMBER);
            DrawTriangle((Vector2){ tx - 5, y - 8 }, (Vector2){ tx, y - 2 }, (Vector2){ tx + 5, y - 8 }, C_AMBER);
            DrawTriangle((Vector2){ tx, y + 20 }, (Vector2){ tx - 5, y + 26 }, (Vector2){ tx + 5, y + 26 }, C_AMBER);
            if (over || vad_drag) DrawRectangleLinesEx(hit, 1, Fade(C_AMBER, 0.35f));
        }
        const int voice = s.running && m.vad >= thr;
        DrawCircle((int)(mx + mw - 10), (int)y + 9, 8, voice ? C_NEON : C_SEG_OFF);
        if (voice) DrawCircle((int)(mx + mw - 10), (int)y + 9, 14, Fade(C_NEON, 0.2f));
        snprintf(t, sizeof t, tr(S_VAD_FMT), (int)(m.vad * 100 + 0.5f), (int)(thr * 100 + 0.5f));
        text(F.lcd, t, mx, y + 30, C_TEXT_DIM); y += 62;

        text(F.ui, tr(S_GATE_OPEN), mx, y, C_TEXT);
        snprintf(t, sizeof t, "%3d %%", (int)(m.gate * 100 + 0.5f));
        text(F.lcd, t, mx + mw - MeasureTextEx(F.lcd, t, 14, 0).x, y + 2, val_c);
        y += 22;
        led_bar((Rectangle){ mx, y, mw, 18 }, s.running ? m.gate : 0.0f, mixc(C_NEON, C_TEXT, 0.35f)); y += 40;

        text(F.ui, tr(S_COMPRESSION), mx, y, C_TEXT);
        snprintf(t, sizeof t, "%5.1f dB", (double)m.gr);
        text(F.lcd, t, mx + mw - MeasureTextEx(F.lcd, t, 14, 0).x, y + 2, s.running ? C_AMBER : C_TEXT_DIM);
        y += 22;
        led_bar((Rectangle){ mx, y, mw, 18 }, -m.gr / 24.0f, C_AMBER); y += 46;

        /* latency readout in an LCD window */
        text(F.ui, tr(S_LATENCY), mx, y, C_TEXT); y += 22;
        Rectangle lr = { mx, y, mw, 78 };
        lcd(lr);
        if (s.running) {
            EngineLatency l;
            engine_latency(&s.eng, &l);
            const double sink_ms = 1000.0 * VF_SINK_TARGET / VF_SAMPLE_RATE;
            const double total = s.use_pw ? l.capture_ms + l.dsp_ms + sink_ms : l.total_ms;
            snprintf(t, sizeof t, "~%.0f ms%s", total,
                     s.use_pw ? " +PW" : sel_mode != VF_OUT_HEADPHONES ? " +CABLE" : "");
            glow_text(F.lcd_big, t, lr.x + 8, lr.y + 8, C_NEON);
            if (s.use_pw) snprintf(t, sizeof t, tr(S_LAT_PW_FMT), l.capture_ms, l.dsp_ms, sink_ms);
            else snprintf(t, sizeof t, tr(S_LAT_FMT), l.capture_ms, l.playback_ms + l.interm_ms, l.fifo_ms, l.dsp_ms);
            SetTextLineSpacing(4);                    /* raylib: extra pixels between lines */
            DrawTextEx(F.lcd, t, (Vector2){ lr.x + 8, lr.y + 38 }, (float)F.lcd.baseSize, 0, C_TEXT_DIM);
        } else {
            glow_text(F.lcd_big, "-- ms", lr.x + 8, lr.y + 8, C_TEXT_DIM);
        }
        y += 90;
        snprintf(t, sizeof t, tr(S_DROPOUTS_FMT), vf_session_xruns(&s));
        text(F.lcd, t, mx, y, vf_session_xruns(&s) ? C_AMBER : C_TEXT_DIM);

        /* ---- effect panels: 3 columns x 2 rows, built from the parameter tables ---- */
        GuiToggleGroup((Rectangle){ 282, TOP, 120, 24 }, tr(S_TABS_ALL), &U.tab);
        if (U.tab == 3) draw_tools((Rectangle){ 282, TOP + 30, 0, 0 }, &s, &st, sel_mode, sel_out, sel_mon);
        else if (U.tab == 4) draw_settings((Rectangle){ 282, TOP + 30, 0, 0 }, &s, &st, presets, n_presets, &sel_preset, tray_ok);
        int slot = 0;
        for (int i = 0; i < s.pipe.count && U.tab <= 2; i++) {
            Fx *fx = s.pipe.fx[i];
            if (fx->page != U.tab) continue;
            const float pw = 292, ph = 276;
            Rectangle box = { 282 + (slot % 3) * (pw + 6), TOP + 30 + (slot / 3) * (ph + 6), pw, ph };
            slot++;
            bool on = !atomic_load(&fx->bypass);
            panel(box, tr_fx(fx->name), !on ? 0 : (s.running && !atomic_load(&s.pipe.bypass_all)) ? 1 : 2);
            GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
            GuiSetFont(F.bold);
            const bool non = onoff((Rectangle){ box.x + box.width - 128, box.y + 6, 44, 20 }, on, "ON", "OFF");
            GuiSetFont(F.ui);
            GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
            if (non != on) { pipeline_set_bypass(&s.pipe, i, !non); on = non; }
            GuiSetStyle(DEFAULT, TEXT_SIZE, 14);
            GuiSetFont(F.bold);
            if (GuiButton((Rectangle){ box.x + box.width - 80, box.y + 6, 72, 20 }, tr(S_DEFAULTS)))
                for (int j = 0; j < fx->n_params; j++) pipeline_set_param(&s.pipe, i, j, fx->params[j].def);
            GuiSetFont(F.ui);
            GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
            if (!on) GuiDisable();
            float py = box.y + 38;
            for (int j = 0; j < fx->n_params; j++) {
                const FxParamInfo *pi = &fx->params[j];
                float v = pipeline_get_param(&s.pipe, i, j);
                if (pi->toggle) {
                    bool b = v >= 0.5f;
                    GuiCheckBox((Rectangle){ box.x + 14, py + 4, 18, 18 }, tr_param(fx->name, pi->name), &b);
                    if (b != (v >= 0.5f)) pipeline_set_param(&s.pipe, i, j, b ? 1.0f : 0.0f);
                    py += 34;
                    continue;
                }
                text(F.ui, tr_param(fx->name, pi->name), box.x + 14, py, on ? C_TEXT : C_DISABLED);
                char val[48];
                const int ints = pi->max - pi->min >= 50.0f;
                snprintf(val, sizeof val, ints ? "%.0f %s" : "%.2f %s", (double)v, pi->unit);
                const float vw = MeasureTextEx(F.lcd, val, 14, 0).x;
                const float ew = fmaxf(vw, 70);
                Rectangle vr = { box.x + box.width - 14 - ew, py - 3, ew + 2, 21 };
                if (ve_fx == i && ve_param == j) {
                    /* typing an exact value: Enter or click elsewhere applies, Esc cancels */
                    if (IsKeyPressed(KEY_ESCAPE)) ve_fx = -1;
                    else if (GuiTextBox(vr, ve_buf, sizeof ve_buf - 1, true)) {
                        for (char *c = ve_buf; *c; c++) if (*c == ',') *c = '.';   /* "0,5" works too */
                        char *end;
                        const float x = strtof(ve_buf, &end);
                        if (end != ve_buf) pipeline_set_param(&s.pipe, i, j, ints ? roundf(x) : x);
                        ve_fx = -1;
                    }
                } else {
                    const int hot = on && !GuiIsLocked() && ve_fx < 0 && CheckCollisionPointRec(mouse, vr);
                    if (hot) {
                        DrawRectangleRec(vr, Fade(C_NEON, 0.1f));
                        DrawRectangleLinesEx(vr, 1, Fade(C_NEON, 0.47f));
                        cursor = MOUSE_CURSOR_IBEAM;
                        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            ve_fx = i; ve_param = j;
                            snprintf(ve_buf, sizeof ve_buf, ints ? "%.0f" : "%.2f", (double)v);
                        }
                    }
                    text(F.lcd, val, box.x + box.width - 14 - vw, py + 2, on ? C_NEON : C_DISABLED);
                }
                Rectangle sr_ = { box.x + 14, py + 19, box.width - 28, 10 };
                float nv = v;
                GuiSliderBar(sr_, NULL, NULL, &nv, pi->min, pi->max);
                /* mouse wheel over a slider: fine steps (Shift = 10x finer) */
                const float wheel = GetMouseWheelMove();
                if (on && wheel != 0 && !GuiIsLocked() &&
                    CheckCollisionPointRec(mouse, (Rectangle){ sr_.x, sr_.y - 8, sr_.width, sr_.height + 14 })) {
                    float step = (pi->max - pi->min) / 100.0f;
                    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) step /= 10.0f;
                    if (ints && step < 1.0f) step = 1.0f;
                    nv = v + wheel * step;
                }
                if (nv != v) {
                    if (ints) nv = roundf(nv);
                    pipeline_set_param(&s.pipe, i, j, nv);
                }
                py += 34;
            }
            GuiEnable();
        }

        /* ---- status bar: LCD strip ---- */
        char sb[600];
        if (status[0]) snprintf(sb, sizeof sb, tr(S_ST_ERROR), status);
        else if (s.running) {
            char run[160];
            snprintf(run, sizeof run, tr(S_ST_RUNNING), s.use_pw ? "PipeWire" : ma_get_backend_name(s.ctx.backend));
            snprintf(sb, sizeof sb, "%s%s%s%s", run, file_buf ? tr(S_ST_TESTFILE) : "", note[0] ? "   |   " : "", note);
        } else snprintf(sb, sizeof sb, "%s%s%s", tr(S_ST_STOPPED), note[0] ? "   |   " : "", note);
        Rectangle sr = { 0, WIN_H - 26, WIN_W, 26 };
        lcd(sr);
        DrawRectangle(0, WIN_H - 27, WIN_W, 1, C_NEON);
        text(F.lcd, "> ", 10, sr.y + 6, on_dark(C_NEON));
        text(F.lcd, sb, 30, sr.y + 6, status[0] ? on_dark(C_RED) : on_dark(C_TEXT));
        if ((int)(vf_now_s() * 2) % 2) {                       /* blinking block cursor */
            const float cw = MeasureTextEx(F.lcd, sb, 14, 0).x;
            DrawRectangle((int)(34 + cw), (int)sr.y + 6, 8, 14, status[0] ? on_dark(C_RED) : on_dark(C_NEON));
        }

        /* ---- dropdowns last, so their open lists cover the rest ---- */
        GuiUnlock();
        if (sel_mode != VF_OUT_VMIC) {
            int *target = sel_mode == VF_OUT_HEADPHONES ? &sel_out : &sel_mon;
            if (GuiDropdownBox(r_out, list_out, target, edit_out)) edit_out = !edit_out;
        }
        if (s.running && file_buf) GuiDisable();
        if (GuiDropdownBox(r_in, list_in, &sel_in, edit_in)) edit_in = !edit_in;
        GuiEnable();
        if (GuiDropdownBox(r_preset, list_presets, &sel_preset, edit_preset)) edit_preset = !edit_preset;
        if (!edit_preset && sel_preset != shown_preset && sel_preset < n_presets) {
            if (vf_preset_apply(&s.pipe, presets[sel_preset].name) == 0) {
                snprintf(st.preset, sizeof st.preset, "%s", presets[sel_preset].name);
                snprintf(note, sizeof note, tr(S_N_LOADED), vf_preset_display_name(&presets[sel_preset]));
            } else snprintf(status, sizeof status, "%s", tr(S_N_LOAD_FAILED));
            shown_preset = sel_preset;
        }

        ui_handle_drops(&s, &st);
        SetMouseCursor(cursor);
        EndDrawing();

        if (shot && ++frame_no >= (frames_to_shot > 0 ? frames_to_shot : 60)) {
            TakeScreenshot(shot);
            break;
        }
    }

    remember_window_pos();
    /* final save (also catches changes made within the last second) */
    if (init_ok && have_settings_path && !shot) {
        fill_device_names(&st, &s, sel_in, sel_out, sel_mon, sel_mode);
        vf_settings_save(&st, &s.pipe, settings_file);
    }
    if (vf_rec_active(&U.rec)) vf_rec_stop(&U.rec);
    vf_rec_free(&U.rec);
    vf_ab_free(&U.ab, NULL);
    vf_overlay_stop();
    atomic_store(&s.pipe.scope_ring, NULL);
    vf_ring_free(&U.scope);
    vf_tray_stop();
    vf_session_free(&s);
    window_close();
    free(file_buf);
    return 0;
}
