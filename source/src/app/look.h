/* GUI customisation ("Look" tab): everything that changes how the window looks or behaves visually, as plain data
 * with load / save / text export (pure logic, tested). Colours: a user-editable "custom" theme of the same 20 colours as
 * app/theme.h plus a chassis hue shift that never touches the state colours (ON, OFF, warning, error stay clearly
 * different from each other). Stored in settings.ini section [look]; the theme id itself ("graphite", "amber",
 * "classic" or "custom") stays in [app] theme. The same key = value lines are the clipboard format. */
#ifndef VF_LOOK_H
#define VF_LOOK_H

#include <stddef.h>
#include <stdio.h>
#include "app/theme.h"
#include "util/ini.h"

#define VF_LOOK_HIDDEN_MAX 192
#define VF_LOOK_TABS 6                     /* basic, extra, effects, tools, settings, look */
#define VF_LOOK_LAYOUT_MAX 512
#define VF_LOOK_PAGES 3                    /* effect pages that hold panels (basic, extra, effects) */

typedef struct VfLook {
    unsigned custom[TC_COUNT];             /* 0xRRGGBB of the "custom" theme */
    int   hue;                             /* chassis hue shift in degrees, -180..180 (0 = as designed) */
    int   opacity;                         /* window opacity in percent, 40..100 */
    int   topmost;                         /* main window always on top */
    int   grid, scanlines, glow;           /* decoration: background grid, LCD scanlines, glowing text and LEDs */
    int   start_tab;                       /* tab shown at start, 0..VF_LOOK_TABS-1 */
    char  font_ui[24], font_lcd[24];       /* font family ids ("" = default), resolved by the GUI */
    float meter_warn_db, meter_clip_db;    /* level meters: amber from, red from */
    float meter_hold_s, meter_fall;        /* peak hold time (s), fall speed (dB per s) */
    char  hidden[VF_LOOK_HIDDEN_MAX];      /* comma separated effect names whose panels are not shown */
    unsigned main_rgb;                     /* main (chassis) colour picked on the Look tab: its hue + intensity are used */
    float main_bright;                     /* chassis brightness, 0.6..1.8 (1 = like the built-in dark themes) */
    char  preset[24];                      /* id of the last applied look preset, "" after a manual change */
    char  layout[VF_LOOK_LAYOUT_MAX];      /* effect panel order and page, "name:page,name:page,..." (dragged in the GUI) */
} VfLook;

/* Ready-made looks. theme != NULL: one of the built-in themes; else main + accent generate the custom theme. */
typedef struct VfLookPreset { const char *id, *name_en, *name_pl, *theme; unsigned main, accent; float bright; } VfLookPreset;
int                 vf_look_preset_count(void);
const VfLookPreset *vf_look_preset(int i);
void vf_look_use_preset(VfLook *l, char *theme_id, size_t theme_n, int i);

/* Main colour: regenerates the chassis colours (backgrounds, panels, title strips, bevels, text, display) of the custom
 * theme from one colour's hue and intensity; accent and state colours stay. Accent: accent + ON-button colour.
 * Both expect the custom theme to be the active one (the GUI copies the theme on screen into it first). */
void vf_look_set_main(VfLook *l, unsigned main_rgb, float bright);
void vf_look_set_accent(VfLook *l, unsigned accent_rgb);
unsigned vf_hsv_rgb(float h_deg, float s, float v);            /* 0..360, 0..1, 0..1 -> 0xRRGGBB */
void     vf_rgb_hsv(unsigned rgb, float *h_deg, float *s, float *v);

/* Panel layout. names[i] / def_page[i]: the effects in chain order and their built-in page (n <= 64).
 * vf_look_layout fills order[] (effect indices in display order, all n) and page[] (page of effect i); returns n.
 * vf_look_move puts effect fx on page to_page right before effect before_fx (-1 = after the last effect of that page)
 * and rewrites l->layout; 0 = ok. */
int  vf_look_layout(const VfLook *l, const char *const *names, const int *def_page, int n, int *order, int *page);
int  vf_look_move(VfLook *l, const char *const *names, const int *def_page, int n, int fx, int to_page, int before_fx);

void vf_look_defaults(VfLook *l);
/* One key = value; clamps and validates. 1 = key known (and value usable), 0 = ignored. */
int  vf_look_apply(VfLook *l, const char *key, const char *value);
void vf_look_load(VfLook *l, const VfIni *ini);                 /* [look] section */
void vf_look_write(FILE *f, const VfLook *l);                   /* writes "[look]" and its keys */
/* Clipboard / share format: "theme = id" + the same keys. to_text returns the length (0 = buffer too small);
 * from_text applies every recognised line and returns how many it recognised. */
int  vf_look_to_text(const VfLook *l, const char *theme_id, char *out, size_t n);
int  vf_look_from_text(VfLook *l, char *theme_id, size_t theme_n, const char *text);

/* Effective colours of a theme id ("graphite" .. "custom"), with or without the hue shift. */
void vf_look_colors(const VfLook *l, const char *theme_id, VfRgba out[TC_COUNT], int with_hue);
/* Copies the current colours of theme_id into the custom theme (so editing starts from what is on screen). */
void vf_look_make_custom(VfLook *l, const char *theme_id);

int  vf_look_hidden(const VfLook *l, const char *fx);
void vf_look_set_hidden(VfLook *l, const char *fx, int hidden);

unsigned vf_rgb_parse(const char *s, int *ok);                  /* "#1e222b" / "1E222B" -> 0x1e222b */
void     vf_rgb_format(unsigned rgb, char out[8]);              /* "#1e222b" */

#endif
