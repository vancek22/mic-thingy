/* Native window decoration tweaks (kept apart from raylib.h, which clashes with windows.h).
 * Windows 11: colours the title bar / border via DWM so there is no white bar above the UI.
 * Windows 10: falls back to the dark title bar. Elsewhere: no-op. */
#ifndef VF_WIN_CHROME_H
#define VF_WIN_CHROME_H

/* hwnd = GetWindowHandle() from raylib. Colours are 0xRRGGBB. */
void vf_window_style_titlebar(void *hwnd, unsigned caption_rgb, unsigned text_rgb, unsigned border_rgb);

#endif
