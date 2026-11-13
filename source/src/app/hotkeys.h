/* Global hotkeys by polling the keyboard state (no hook, no window needed, so they work while
 * the window is hidden in the tray). Combination text: "ctrl+alt+m", "f9", "shift+f13".
 * Windows: GetAsyncKeyState; elsewhere keys always read as up (Wayland offers no global keys).
 * Polling alone does not swallow the key; vf_hotkeys_grab() does that for modifier combinations. */
#ifndef VF_HOTKEYS_H
#define VF_HOTKEYS_H

#include <stddef.h>

#define VF_HK_MAX 4

typedef struct VfHotkey {
    int vk[VF_HK_MAX];       /* virtual-key codes (Windows VK_*), stable ids on other systems */
    int n;                   /* 0 = unassigned */
} VfHotkey;

/* "" or "none" = unassigned. 0 = ok, -1 = unknown key / too many keys. */
int  vf_hotkey_parse(const char *text, VfHotkey *hk);
void vf_hotkey_format(const VfHotkey *hk, char *buf, size_t n);     /* canonical text, e.g. "ctrl+alt+m" */

/* True while every key of the combination is held (false when unassigned). */
int  vf_hotkey_down(const VfHotkey *hk);
/* Rising edge: true once per press. `was` is the caller's per-hotkey state. */
int  vf_hotkey_pressed(const VfHotkey *hk, int *was);

/* Windows: swallow these combinations (RegisterHotKey on a helper thread) so the focused application no longer
 * receives them. Polling above keeps working. Replaces the previous set; only combinations with a modifier and
 * exactly one other key are grabbed; one that another program already owns is left alone. No-op elsewhere. */
void vf_hotkeys_grab(const VfHotkey *hks, int n);

/* The first key currently held (for "press a key to assign"), as a VK code; 0 if none. */
int  vf_hotkey_scan(void);
void vf_hotkey_from_text_keys(const int *vks, int n, VfHotkey *hk);   /* tests / capture UI */

#endif
