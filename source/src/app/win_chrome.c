#include "app/win_chrome.h"

#ifdef _WIN32
#include <windows.h>
#include "util/platform.h"

/* dwmapi.dll and winmm.dll are not Windows "known DLLs": as ordinary imports they would be searched in the program's
 * own folder first, before main() runs (DLL planting next to a downloaded .exe). Both are loaded from System32 on
 * first use instead (vf_load_system_library). */
typedef void (*AnyFn)(void);
static AnyFn sys_fn(const char *dll, const char *name)
{
    HMODULE m = (HMODULE)vf_load_system_library(dll);       /* a second load only bumps the reference count */
    return m ? (AnyFn)GetProcAddress(m, name) : NULL;
}

static HRESULT dwm_set(HWND h, DWORD attr, const void *v, DWORD n)
{
    static HRESULT (WINAPI *fn)(HWND, DWORD, LPCVOID, DWORD);
    static int tried;
    if (!tried) { tried = 1; fn = (HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD))sys_fn("dwmapi.dll", "DwmSetWindowAttribute"); }
    return fn ? fn(h, attr, v, n) : E_FAIL;
}
#define DwmSetWindowAttribute dwm_set

/* raylib calls timeBeginPeriod(1) / timeEndPeriod(1) (winmm.dll) through the import table. Defining the import
 * pointers here keeps winmm.dll out of the .exe's import table; the calls land in these two functions. */
static UINT WINAPI time_period(UINT ms, int begin)
{
    static UINT (WINAPI *fb)(UINT), (WINAPI *fe)(UINT);
    static int tried;
    if (!tried) {
        tried = 1;
        fb = (UINT (WINAPI *)(UINT))sys_fn("winmm.dll", "timeBeginPeriod");
        fe = (UINT (WINAPI *)(UINT))sys_fn("winmm.dll", "timeEndPeriod");
    }
    UINT (WINAPI *f)(UINT) = begin ? fb : fe;
    return f ? f(ms) : 97;                                   /* TIMERR_NOCANDO */
}
static UINT WINAPI vf_time_begin(UINT ms) { return time_period(ms, 1); }
static UINT WINAPI vf_time_end(UINT ms) { return time_period(ms, 0); }
UINT (WINAPI *__imp_timeBeginPeriod)(UINT) = vf_time_begin;
UINT (WINAPI *__imp_timeEndPeriod)(UINT) = vf_time_end;

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

void vf_trim_memory(void) { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }
#else
void vf_window_style_titlebar(void *hwnd, unsigned caption_rgb, unsigned text_rgb, unsigned border_rgb)
{
    (void)hwnd; (void)caption_rgb; (void)text_rgb; (void)border_rgb;
}
void vf_trim_memory(void) {}
#endif
