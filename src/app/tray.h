/* Notification-area (system tray) icon so mic thingy can keep running with its
 * window hidden. Windows: Shell_NotifyIcon on a private thread with its own hidden
 * message window. Other platforms: not available (vf_tray_start returns -1) - the
 * GUI then minimizes instead of hiding.
 *
 * Also provides single-instance support: a second launch asks the running
 * instance to show its window and exits. */
#ifndef VF_TRAY_H
#define VF_TRAY_H

enum { VF_TRAY_NONE = 0, VF_TRAY_SHOW, VF_TRAY_QUIT, VF_TRAY_TOGGLE_BYPASS, VF_TRAY_TOGGLE_RUN };

int  vf_tray_start(void);                      /* 0 = icon visible */
void vf_tray_stop(void);
int  vf_tray_poll(void);                       /* next pending VF_TRAY_* event (main thread) */
/* State shown in the tooltip and the menu check marks. */
void vf_tray_set_state(int running, int bypass);

/* Single instance. Returns 1 if this is the first instance (keep running),
 * 0 if another one is already running (it has been asked to show itself). */
int  vf_single_instance(void);

#endif
