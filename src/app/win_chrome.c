#include "app/win_chrome.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

static COLORREF bgr(unsigned rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

void vf_window_style_titlebar(void *hwnd, unsigned caption_rgb, unsigned text_rgb, unsigned border_rgb)
{
    HWND h = (HWND)hwnd;
    if (!h) return;
    BOOL dark = TRUE;
    /* dark title bar (Windows 10 20H1+ uses 20, older builds 19) */
    if (FAILED(DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark)))
        DwmSetWindowAttribute(h, 19, &dark, sizeof dark);
    /* exact colours (Windows 11 22000+; ignored on Windows 10) */
    COLORREF cap = bgr(caption_rgb), txt = bgr(text_rgb), brd = bgr(border_rgb);
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &cap, sizeof cap);
    DwmSetWindowAttribute(h, DWMWA_TEXT_COLOR, &txt, sizeof txt);
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &brd, sizeof brd);
}
#else
void vf_window_style_titlebar(void *hwnd, unsigned caption_rgb, unsigned text_rgb, unsigned border_rgb)
{
    (void)hwnd; (void)caption_rgb; (void)text_rgb; (void)border_rgb;
}
#endif
