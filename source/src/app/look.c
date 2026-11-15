#include "app/look.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- colours */

unsigned vf_rgb_parse(const char *s, int *ok)
{
    if (ok) *ok = 0;
    if (!s) return 0;
    if (*s == '#') s++;
    unsigned v = 0;
    for (int i = 0; i < 6; i++) {
        const int c = (unsigned char)s[i];
        if (!isxdigit(c)) return 0;
        v = v << 4 | (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    if (s[6] != '\0' && !isspace((unsigned char)s[6])) return 0;
    if (ok) *ok = 1;
    return v;
}

void vf_rgb_format(unsigned rgb, char out[8]) { snprintf(out, 8, "#%06x", rgb & 0xFFFFFFu); }

static VfRgba rgba_from(unsigned rgb) { return (VfRgba){ (unsigned char)(rgb >> 16), (unsigned char)(rgb >> 8), (unsigned char)rgb, 255 }; }
static unsigned rgb_of(VfRgba c) { return (unsigned)c.r << 16 | (unsigned)c.g << 8 | c.b; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* hue rotation in HSV (degrees); grey stays grey */
static VfRgba rotate_hue(VfRgba c, int deg)
{
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    if (d < 1e-6f) return c;
    float h;
    if (mx == r) h = fmodf((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h = h * 60.0f + (float)deg;
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    const float s = d / mx, v = mx;
    const float cc = v * s, x = cc * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - cc;
    float rr, gg, bb;
    const int sector = (int)(h / 60.0f);
    switch (sector) {
    case 0: rr = cc; gg = x; bb = 0; break;
    case 1: rr = x; gg = cc; bb = 0; break;
    case 2: rr = 0; gg = cc; bb = x; break;
    case 3: rr = 0; gg = x; bb = cc; break;
    case 4: rr = x; gg = 0; bb = cc; break;
    default: rr = cc; gg = 0; bb = x; break;
    }
    return (VfRgba){ (unsigned char)(clampf(rr + m, 0, 1) * 255.0f + 0.5f), (unsigned char)(clampf(gg + m, 0, 1) * 255.0f + 0.5f),
                     (unsigned char)(clampf(bb + m, 0, 1) * 255.0f + 0.5f), c.a };
}

static int is_state_colour(int tc)
{
    return tc == TC_ACCENT || tc == TC_ON_BTN || tc == TC_OFF || tc == TC_OFF_HI || tc == TC_OFF_TEXT || tc == TC_WARN || tc == TC_ERROR;
}

void vf_look_colors(const VfLook *l, const char *theme_id, VfRgba out[TC_COUNT], int with_hue)
{
    if (theme_id && !strcmp(theme_id, "custom")) {
        for (int i = 0; i < TC_COUNT; i++) out[i] = rgba_from(l->custom[i]);
    } else {
        const VfTheme *t = vf_theme_get(vf_theme_find(theme_id));
        for (int i = 0; i < TC_COUNT; i++) out[i] = t->c[i];
    }
    if (with_hue && l->hue != 0)
        for (int i = 0; i < TC_COUNT; i++)
            if (!is_state_colour(i)) out[i] = rotate_hue(out[i], l->hue);     /* the state colours keep their meaning */
}

void vf_look_make_custom(VfLook *l, const char *theme_id)
{
    VfRgba c[TC_COUNT];
    vf_look_colors(l, theme_id, c, 0);
    for (int i = 0; i < TC_COUNT; i++) l->custom[i] = rgb_of(c[i]);
}

/* ---------------------------------------------------------------- defaults, parsing */

void vf_look_defaults(VfLook *l)
{
    memset(l, 0, sizeof *l);
    const VfTheme *t = vf_theme_get(0);
    for (int i = 0; i < TC_COUNT; i++) l->custom[i] = rgb_of(t->c[i]);
    l->hue = 0;
    l->opacity = 100;
    l->topmost = 0;
    l->grid = l->scanlines = l->glow = 1;
    l->start_tab = 0;
    l->meter_warn_db = -18.0f;
    l->meter_clip_db = -6.0f;
    l->meter_hold_s = 1.5f;
    l->meter_fall = 30.0f;
    l->main_rgb = 0x395cbf;                                      /* hue 222, intensity 0.67 = the graphite chassis */
    l->main_bright = 1.0f;
    snprintf(l->preset, sizeof l->preset, "graphite");
}

static int truthy(const char *v) { return v && (atoi(v) != 0 || !strcmp(v, "true") || !strcmp(v, "on")); }

static int id_ok(const char *v)
{
    const size_t n = strlen(v);
    if (n >= 24) return 0;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)v[i]) || isdigit((unsigned char)v[i]) || v[i] == '_' || v[i] == '-')) return 0;
    return 1;
}

int vf_look_apply(VfLook *l, const char *key, const char *value)
{
    if (!key || !value) return 0;
    if (!strncmp(key, "custom_", 7)) {
        for (int i = 0; i < TC_COUNT; i++)
            if (!strcmp(key + 7, vf_theme_color_name(i))) {
                int ok;
                const unsigned v = vf_rgb_parse(value, &ok);
                if (ok) l->custom[i] = v;
                return ok;
            }
        return 0;
    }
    if (!strcmp(key, "hue")) { l->hue = clampi(atoi(value), -180, 180); return 1; }
    if (!strcmp(key, "opacity")) { l->opacity = clampi(atoi(value), 40, 100); return 1; }
    if (!strcmp(key, "topmost")) { l->topmost = truthy(value); return 1; }
    if (!strcmp(key, "grid")) { l->grid = truthy(value); return 1; }
    if (!strcmp(key, "scanlines")) { l->scanlines = truthy(value); return 1; }
    if (!strcmp(key, "glow")) { l->glow = truthy(value); return 1; }
    if (!strcmp(key, "start_tab")) {                            /* up to v1.0.4.0: 0..3 effect / tool tabs, 4 settings, 5 look */
        const int v = atoi(value);
        l->start_tab = clampi(v == 3 ? 0 : v == 4 ? 1 : v == 5 ? 2 : v, 0, VF_LOOK_TABS - 1);
        return 1;
    }
    if (!strcmp(key, "font_ui") || !strcmp(key, "font_lcd")) {
        if (!id_ok(value)) return 0;
        snprintf(key[5] == 'u' ? l->font_ui : l->font_lcd, 24, "%s", value);
        return 1;
    }
    if (!strcmp(key, "meter_warn")) { l->meter_warn_db = clampf((float)atof(value), -40.0f, -6.0f); return 1; }
    if (!strcmp(key, "meter_clip")) { l->meter_clip_db = clampf((float)atof(value), -20.0f, 0.0f); return 1; }
    if (!strcmp(key, "meter_hold")) { l->meter_hold_s = clampf((float)atof(value), 0.0f, 10.0f); return 1; }
    if (!strcmp(key, "meter_fall")) { l->meter_fall = clampf((float)atof(value), 5.0f, 120.0f); return 1; }
    if (!strcmp(key, "main")) {
        int ok;
        const unsigned v = vf_rgb_parse(value, &ok);
        if (ok) l->main_rgb = v;
        return ok;
    }
    if (!strcmp(key, "main_bright")) { l->main_bright = clampf((float)atof(value), 0.6f, 1.8f); return 1; }
    if (!strcmp(key, "preset")) {
        if (*value && !id_ok(value)) return 0;
        snprintf(l->preset, sizeof l->preset, "%s", value);
        return 1;
    }
    if (!strcmp(key, "layout")) {
        char clean[VF_LOOK_LAYOUT_MAX];
        size_t n = 0;
        for (const char *p = value; *p && n + 1 < sizeof clean; p++)
            if (islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == ',' || *p == ':') clean[n++] = *p;
        clean[n] = '\0';
        snprintf(l->layout, sizeof l->layout, "%s", clean);
        return 1;
    }
    if (!strcmp(key, "hidden")) {
        char clean[VF_LOOK_HIDDEN_MAX];
        size_t n = 0;
        for (const char *p = value; *p && n + 1 < sizeof clean; p++)
            if (islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == ',') clean[n++] = *p;
        clean[n] = '\0';
        snprintf(l->hidden, sizeof l->hidden, "%s", clean);
        return 1;
    }
    return 0;
}

/* every key apply() knows, in file order */
static const char *const k_keys[] = {
    "hue", "opacity", "topmost", "grid", "scanlines", "glow", "start_tab", "font_ui", "font_lcd",
    "meter_warn", "meter_clip", "meter_hold", "meter_fall", "hidden", "main", "main_bright", "preset", "layout" };
#define N_KEYS ((int)(sizeof k_keys / sizeof k_keys[0]))

static void fix_meters(VfLook *l)
{
    if (l->meter_clip_db <= l->meter_warn_db) l->meter_clip_db = fminf(0.0f, l->meter_warn_db + 3.0f);   /* red above amber */
}

void vf_look_load(VfLook *l, const VfIni *ini)
{
    for (int i = 0; i < N_KEYS; i++) {
        const char *v = vf_ini_get(ini, "look", k_keys[i]);
        if (v) vf_look_apply(l, k_keys[i], v);
    }
    char key[64];
    for (int i = 0; i < TC_COUNT; i++) {
        snprintf(key, sizeof key, "custom_%s", vf_theme_color_name(i));
        const char *v = vf_ini_get(ini, "look", key);
        if (v) vf_look_apply(l, key, v);
    }
    fix_meters(l);
}

static int format_values(const VfLook *l, char *out, size_t n)
{
    int used = snprintf(out, n, "hue = %d\nopacity = %d\ntopmost = %d\ngrid = %d\nscanlines = %d\nglow = %d\nstart_tab = %d\n"
                        "font_ui = %s\nfont_lcd = %s\nmeter_warn = %.1f\nmeter_clip = %.1f\nmeter_hold = %.1f\nmeter_fall = %.1f\nhidden = %s\n",
                        l->hue, l->opacity, l->topmost, l->grid, l->scanlines, l->glow, l->start_tab, l->font_ui, l->font_lcd,
                        (double)l->meter_warn_db, (double)l->meter_clip_db, (double)l->meter_hold_s, (double)l->meter_fall, l->hidden);
    if (used < 0 || (size_t)used >= n) return -1;
    {
        char mh[8];
        vf_rgb_format(l->main_rgb, mh);
        const int k = snprintf(out + used, n - (size_t)used, "main = %s\nmain_bright = %.2f\npreset = %s\nlayout = %s\n",
                               mh, (double)l->main_bright, l->preset, l->layout);
        if (k < 0 || (size_t)k >= n - (size_t)used) return -1;
        used += k;
    }
    for (int i = 0; i < TC_COUNT; i++) {
        char hex[8];
        vf_rgb_format(l->custom[i], hex);
        const int k = snprintf(out + used, n - (size_t)used, "custom_%s = %s\n", vf_theme_color_name(i), hex);
        if (k < 0 || (size_t)k >= n - (size_t)used) return -1;
        used += k;
    }
    return used;
}

void vf_look_write(FILE *f, const VfLook *l)
{
    char buf[4096];
    fputs("\n[look]\n", f);
    if (format_values(l, buf, sizeof buf) > 0) fputs(buf, f);
}

int vf_look_to_text(const VfLook *l, const char *theme_id, char *out, size_t n)
{
    int used = snprintf(out, n, "; mic thingy look\ntheme = %s\n", theme_id && *theme_id ? theme_id : "graphite");
    if (used < 0 || (size_t)used >= n) return 0;
    const int k = format_values(l, out + used, n - (size_t)used);
    return k < 0 ? 0 : used + k;
}

int vf_look_from_text(VfLook *l, char *theme_id, size_t theme_n, const char *text)
{
    int known = 0;
    VfLook next = *l;
    char tid[24];
    snprintf(tid, sizeof tid, "%s", theme_id);
    char line[300];
    while (text && *text) {
        size_t n = 0;
        while (text[n] && text[n] != '\n' && text[n] != '\r') n++;
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), text);
        text += n;
        while (*text == '\n' || *text == '\r') text++;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = '\0';
        while (isspace((unsigned char)*v)) v++;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);) *--e = '\0';
        for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(k, "theme")) {
            if (!strcmp(v, "custom") || !strcmp(v, vf_theme_get(vf_theme_find(v))->id)) { snprintf(tid, sizeof tid, "%s", v); known++; }
        } else if (vf_look_apply(&next, k, v)) known++;
    }
    if (known) {
        fix_meters(&next);
        *l = next;
        snprintf(theme_id, theme_n, "%s", tid);
    }
    return known;
}

/* ---------------------------------------------------------------- hidden effect panels */

static int find_tok(const char *list, const char *name, const char **start, size_t *len)
{
    const size_t nl = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        const size_t tl = e ? (size_t)(e - p) : strlen(p);
        if (tl == nl && !strncmp(p, name, nl)) { if (start) *start = p; if (len) *len = tl; return 1; }
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int vf_look_hidden(const VfLook *l, const char *fx) { return fx && *fx && find_tok(l->hidden, fx, NULL, NULL); }

void vf_look_set_hidden(VfLook *l, const char *fx, int hidden)
{
    if (!fx || !*fx || strchr(fx, ',')) return;
    const char *s;
    size_t len;
    const int has = find_tok(l->hidden, fx, &s, &len);
    if (hidden && !has) {
        const size_t cur = strlen(l->hidden);
        if (cur + strlen(fx) + 2 > sizeof l->hidden) return;
        snprintf(l->hidden + cur, sizeof l->hidden - cur, "%s%s", cur ? "," : "", fx);
    } else if (!hidden && has) {
        size_t off = (size_t)(s - l->hidden), end = off + len;
        if (l->hidden[end] == ',') end++;
        else if (off > 0) off--;                              /* last item: take the comma before it */
        memmove(l->hidden + off, l->hidden + end, strlen(l->hidden + end) + 1);
    }
}

/* ---------------------------------------------------------------- main colour, accent, presets */

unsigned vf_hsv_rgb(float h, float s, float v)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    s = clampf(s, 0, 1); v = clampf(v, 0, 1);
    const float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    switch ((int)(h / 60.0f)) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    return (unsigned)((r + m) * 255.0f + 0.5f) << 16 | (unsigned)((g + m) * 255.0f + 0.5f) << 8 | (unsigned)((b + m) * 255.0f + 0.5f);
}

void vf_rgb_hsv(unsigned rgb, float *h, float *s, float *v)
{
    const float r = (rgb >> 16 & 255) / 255.0f, g = (rgb >> 8 & 255) / 255.0f, b = (rgb & 255) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float hh = 0;
    if (d > 1e-6f) {
        if (mx == r) hh = fmodf((g - b) / d, 6.0f);
        else if (mx == g) hh = (b - r) / d + 2.0f;
        else hh = (r - g) / d + 4.0f;
        hh *= 60.0f;
        if (hh < 0) hh += 360.0f;
    }
    *h = hh;
    *s = mx > 1e-6f ? d / mx : 0.0f;
    *v = mx;
}

/* chassis colours as (saturation factor, value) of the main colour, measured on the graphite theme
 * (main = hue 222, intensity 0.67 reproduces it within a few RGB steps); text and display ignore the brightness */
static const struct { int tc; float sf, v; int lit; } k_chassis[] = {
    { TC_BG_TOP, 0.45f, 0.169f, 1 }, { TC_BG_BOTTOM, 0.52f, 0.078f, 1 }, { TC_PANEL, 0.49f, 0.129f, 1 },
    { TC_BEVEL_HI, 0.40f, 0.388f, 1 }, { TC_BEVEL_LO, 0.64f, 0.027f, 0 }, { TC_EDGE, 0.49f, 0.239f, 1 },
    { TC_SLATE_A, 0.69f, 0.361f, 1 }, { TC_SLATE_B, 0.64f, 0.165f, 1 }, { TC_TEXT, 0.13f, 0.882f, 0 },
    { TC_TEXT_DIM, 0.31f, 0.592f, 0 }, { TC_TITLE, 0.07f, 0.973f, 0 }, { TC_LCD_BG, 0.60f, 0.059f, 0 },
    { TC_SEG_OFF, 0.52f, 0.180f, 1 } };

void vf_look_set_main(VfLook *l, unsigned main_rgb, float bright)
{
    float h, s, v;
    vf_rgb_hsv(main_rgb, &h, &s, &v);
    bright = clampf(bright, 0.6f, 1.8f);
    for (size_t i = 0; i < sizeof k_chassis / sizeof k_chassis[0]; i++)
        l->custom[k_chassis[i].tc] = vf_hsv_rgb(h, s * k_chassis[i].sf, k_chassis[i].lit ? clampf(k_chassis[i].v * bright, 0, 0.75f) : k_chassis[i].v);
    l->main_rgb = main_rgb & 0xFFFFFFu;
    l->main_bright = bright;
    l->hue = 0;                                                  /* the main colour replaces the old hue shift */
}

void vf_look_set_accent(VfLook *l, unsigned rgb)
{
    l->custom[TC_ACCENT] = rgb & 0xFFFFFFu;
    l->custom[TC_ON_BTN] = ((rgb >> 16 & 255) * 6 / 10) << 16 | ((rgb >> 8 & 255) * 6 / 10) << 8 | ((rgb & 255) * 6 / 10);
}

static const VfLookPreset k_presets[] = {
    { "graphite", "Graphite", "Grafit", "graphite", 0x395cbf, 0x39ff14, 1.0f },
    { "amber", "Amber terminal", "Bursztyn", "amber", 0x6b4a1c, 0xffb000, 1.0f },
    { "classic", "Classic gray", "Szary klasyk", "classic", 0x808080, 0x008000, 1.0f },
    { "midnight", "Midnight", "Północ", NULL, 0x17338f, 0x00e5ff, 0.9f },
    { "ocean", "Ocean", "Ocean", NULL, 0x1d5f73, 0x2fe3c8, 1.0f },
    { "forest", "Forest", "Las", NULL, 0x2f6b3a, 0xb6ff3c, 1.0f },
    { "synthwave", "Synthwave", "Synthwave", NULL, 0x4a2a80, 0xff40c8, 1.0f },
    { "sakura", "Sakura", "Sakura", NULL, 0x6b3a55, 0xffb7d5, 1.1f },
    { "copper", "Copper", "Miedź", NULL, 0x6b4a2a, 0x3fd0c0, 1.0f },
    { "crimson", "Crimson", "Karmazyn", NULL, 0x6b1f2e, 0xdcf0ff, 0.9f },
    { "mono", "Mono", "Mono", NULL, 0x808080, 0xe6e6e6, 1.0f },
    { "lilac", "Lilac", "Bez", NULL, 0x5a4a8a, 0x5cffb0, 1.3f },
};

int vf_look_preset_count(void) { return (int)(sizeof k_presets / sizeof k_presets[0]); }
const VfLookPreset *vf_look_preset(int i) { return &k_presets[i >= 0 && i < vf_look_preset_count() ? i : 0]; }

void vf_look_use_preset(VfLook *l, char *theme_id, size_t theme_n, int i)
{
    const VfLookPreset *p = vf_look_preset(i);
    l->hue = 0;
    if (p->theme) {
        snprintf(theme_id, theme_n, "%s", p->theme);
        vf_look_make_custom(l, p->theme);                        /* editing from here starts at this theme */
        l->main_rgb = p->main;
        l->main_bright = p->bright;
    } else {
        vf_look_make_custom(l, "graphite");                      /* state colours (OFF, warning, error) of the default */
        vf_look_set_main(l, p->main, p->bright);
        vf_look_set_accent(l, p->accent);
        snprintf(theme_id, theme_n, "custom");
    }
    snprintf(l->preset, sizeof l->preset, "%s", p->id);
}

/* ---------------------------------------------------------------- panel layout */

static int name_index(const char *const *names, int n, const char *s, size_t len)
{
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == len && !strncmp(names[i], s, len)) return i;
    return -1;
}

int vf_look_layout(const VfLook *l, const char *const *names, const int *def_page, int n, int *order, int *page)
{
    if (n > 64) n = 64;
    int used[64] = { 0 }, k = 0;
    for (int i = 0; i < n; i++) page[i] = def_page[i];
    for (const char *p = l->layout; *p;) {
        const char *e = strchr(p, ',');
        const size_t tl = e ? (size_t)(e - p) : strlen(p);
        const char *colon = memchr(p, ':', tl);
        const int idx = name_index(names, n, p, colon ? (size_t)(colon - p) : tl);
        if (idx >= 0 && !used[idx]) {
            used[idx] = 1;
            order[k++] = idx;
            if (colon) page[idx] = clampi(atoi(colon + 1), 0, VF_LOOK_PAGES - 1);
        }
        if (!e) break;
        p = e + 1;
    }
    for (int i = 0; i < n; i++)
        if (!used[i]) order[k++] = i;                            /* effects not in the layout keep chain order */
    return n;
}

int vf_look_move(VfLook *l, const char *const *names, const int *def_page, int n, int fx, int to_page, int before_fx)
{
    if (n > 64 || fx < 0 || fx >= n || to_page < 0 || to_page >= VF_LOOK_PAGES || before_fx >= n || before_fx == fx) return -1;
    int order[64], page[64], k = 0, rest[64];
    vf_look_layout(l, names, def_page, n, order, page);
    for (int i = 0; i < n; i++)
        if (order[i] != fx) rest[k++] = order[i];
    int at = k;                                                  /* insert position in rest[] */
    if (before_fx >= 0) {
        for (int i = 0; i < k; i++) if (rest[i] == before_fx) { at = i; break; }
    } else {
        for (int i = 0; i < k; i++) if (page[rest[i]] == to_page) at = i + 1;
    }
    page[fx] = to_page;
    int final_[64], m = 0;
    for (int i = 0; i <= k; i++) {
        if (i == at) final_[m++] = fx;
        if (i < k) final_[m++] = rest[i];
    }
    char buf[VF_LOOK_LAYOUT_MAX];
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < m; i++) {
        const int w = snprintf(buf + used, sizeof buf - used, "%s%s:%d", used ? "," : "", names[final_[i]], page[final_[i]]);
        if (w < 0 || (size_t)w >= sizeof buf - used) return -1;
        used += (size_t)w;
    }
    snprintf(l->layout, sizeof l->layout, "%s", buf);
    return 0;
}

int vf_look_flatten_layout(VfLook *l, const char *const *names, const int *def_page, int n)
{
    if (!strchr(l->layout, ':')) return 0;
    int has_page = 0;
    for (const char *p = strchr(l->layout, ':'); p; p = strchr(p + 1, ':'))
        if (p[1] != '0') has_page = 1;
    if (!has_page) return 0;
    /* the stored order with the page of every entry, read with the built-in pages of the old per-tab layout */
    (void)def_page;
    int order[64], page[64], last[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) last[i] = VF_LOOK_PAGES - 1;    /* panels the old layout did not list (tools) go last */
    vf_look_layout(l, names, last, n, order, page);
    char buf[VF_LOOK_LAYOUT_MAX];
    size_t used = 0;
    buf[0] = '\0';
    for (int pg = 0; pg < VF_LOOK_PAGES; pg++)                   /* stable: by old page, then by the stored order */
        for (int k = 0; k < n; k++) {
            if (page[order[k]] != pg) continue;
            const int w = snprintf(buf + used, sizeof buf - used, "%s%s:0", used ? "," : "", names[order[k]]);
            if (w < 0 || (size_t)w >= sizeof buf - used) return 0;
            used += (size_t)w;
        }
    snprintf(l->layout, sizeof l->layout, "%s", buf);
    return 1;
}
