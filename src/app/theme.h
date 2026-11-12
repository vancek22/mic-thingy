/* GUI colour themes as plain data (the GUI turns them into raylib Colors). Every theme keeps
 * the meaning of the state colours: ON accent, OFF contrast and the warning colour must stay
 * clearly different from each other (checked by the tests). */
#ifndef VF_THEME_H
#define VF_THEME_H

typedef struct VfRgba { unsigned char r, g, b, a; } VfRgba;

enum VfThemeColor {
    TC_BG_TOP, TC_BG_BOTTOM, TC_PANEL, TC_BEVEL_HI, TC_BEVEL_LO, TC_EDGE, TC_SLATE_A, TC_SLATE_B,
    TC_ACCENT,       /* ON / meters / values (was "neon") */
    TC_ON_BTN,       /* filled ON buttons */
    TC_OFF,          /* OFF buttons / title strips of stopped effects */
    TC_OFF_HI, TC_OFF_TEXT, TC_TEXT, TC_TEXT_DIM, TC_TITLE, TC_LCD_BG, TC_SEG_OFF,
    TC_WARN,         /* meter warnings, dropouts */
    TC_ERROR,        /* error text */
    TC_COUNT
};

typedef struct VfTheme {
    const char *id;              /* stored in settings.ini */
    const char *name_en, *name_pl;
    VfRgba c[TC_COUNT];
} VfTheme;

int            vf_theme_count(void);
const VfTheme *vf_theme_get(int index);
int            vf_theme_find(const char *id);      /* index, 0 (default) if unknown */

#endif
