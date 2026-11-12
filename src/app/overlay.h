/* In-game microphone indicator (Windows): a small always-on-top, click-through icon in a screen
 * corner - dim while silent, bright green while you speak, burgundy and crossed out when muted.
 * It draws its own layered window (GDI) on its own thread, so it works while the main window is
 * hidden or destroyed. No code is injected into any game; it is visible in windowed and
 * borderless-window modes, not in exclusive full screen. Other systems: no-op. */
#ifndef VF_OVERLAY_H
#define VF_OVERLAY_H

enum { VF_OVL_HIDDEN = 0, VF_OVL_IDLE, VF_OVL_SPEAKING, VF_OVL_MUTED };

int  vf_overlay_start(int corner);        /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right; 0 = ok */
void vf_overlay_set_state(int state);
void vf_overlay_set_corner(int corner);
void vf_overlay_stop(void);
int  vf_overlay_running(void);

#endif
