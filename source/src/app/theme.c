#include "app/theme.h"

#include <string.h>

#define RGB_(r, g, b) { (r), (g), (b), 255 }

static const VfTheme k_themes[] = {
    /* 0: the original look: graphite + neon green, burgundy for OFF */
    { "graphite", "Graphite green", "Grafit i zieleń", {
        RGB_(30, 34, 43), RGB_(13, 15, 20), RGB_(22, 26, 33), RGB_(72, 81, 99), RGB_(4, 5, 7), RGB_(41, 48, 61),
        RGB_(50, 62, 92), RGB_(24, 29, 42),
        RGB_(57, 255, 20), RGB_(40, 150, 32), RGB_(128, 22, 52), RGB_(196, 60, 96), RGB_(255, 214, 224),
        RGB_(205, 213, 225), RGB_(120, 131, 151), RGB_(236, 241, 248), RGB_(9, 11, 15), RGB_(30, 36, 46),
        RGB_(255, 190, 50), RGB_(255, 84, 110) } },
    /* 1: amber phosphor terminal: warm blacks, amber ON, dark red OFF, pale yellow warning */
    { "amber", "Amber terminal", "Bursztynowy terminal", {
        RGB_(34, 26, 14), RGB_(14, 10, 4), RGB_(26, 19, 9), RGB_(96, 74, 38), RGB_(6, 4, 1), RGB_(58, 44, 22),
        RGB_(84, 62, 28), RGB_(38, 28, 12),
        RGB_(255, 176, 0), RGB_(170, 112, 8), RGB_(130, 28, 24), RGB_(214, 78, 60), RGB_(255, 222, 210),
        RGB_(236, 214, 170), RGB_(148, 124, 78), RGB_(255, 240, 205), RGB_(12, 8, 2), RGB_(44, 33, 15),
        RGB_(255, 244, 150), RGB_(255, 96, 80) } },
    /* 2: classic grey (Windows 98 style): light chassis, navy strips, green ON, red OFF, orange warning */
    { "classic", "Classic gray", "Szary klasyczny", {
        RGB_(204, 204, 204), RGB_(176, 176, 176), RGB_(192, 192, 192), RGB_(255, 255, 255), RGB_(64, 64, 64), RGB_(128, 128, 128),
        RGB_(0, 0, 128), RGB_(16, 132, 208),
        RGB_(0, 128, 0), RGB_(0, 150, 0), RGB_(176, 0, 0), RGB_(220, 40, 40), RGB_(255, 255, 255),
        RGB_(0, 0, 0), RGB_(80, 80, 80), RGB_(255, 255, 255), RGB_(0, 0, 0), RGB_(60, 60, 60),
        RGB_(240, 140, 0), RGB_(200, 0, 0) } },
};

int vf_theme_count(void) { return (int)(sizeof k_themes / sizeof k_themes[0]); }

const VfTheme *vf_theme_get(int index)
{
    return &k_themes[index >= 0 && index < vf_theme_count() ? index : 0];
}

int vf_theme_find(const char *id)
{
    for (int i = 0; i < vf_theme_count(); i++)
        if (id && !strcmp(id, k_themes[i].id)) return i;
    return 0;
}
