/* The mic thingy icon, drawn procedurally at any size: a retro bevelled monitor
 * frame with a navy screen showing a green sine wave and a square wave.
 * Pure C, no dependencies - used by the GUI (window icon), the tray icon and
 * tools/make_icon.c (which writes assets/mic-thingy.ico for the .exe). */
#ifndef VF_ICON_ART_H
#define VF_ICON_ART_H

/* Writes size*size RGBA8 pixels (straight alpha). active = 0 draws grey waves
 * (tray icon while audio is stopped). */
void vf_icon_render(unsigned char *rgba, int size, int active);
/* Same art with the GUI theme's accent (band, LED, rim, meter) and ON-button colour (band body), 0xRRGGBB. */
void vf_icon_render_accent(unsigned char *rgba, int size, int active, unsigned accent_rgb, unsigned button_rgb);

#endif
