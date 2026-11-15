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
} VfLook;

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
