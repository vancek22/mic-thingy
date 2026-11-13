/* Automatic mute: (a) while the screen is locked, (b) after N minutes without speech.
 * The lock mute ends by itself when the screen is unlocked; the idle mute ends only when the
 * user unmutes (otherwise it would not protect anything). Pure logic: the GUI feeds time,
 * lock state and voice activity. */
#ifndef VF_AUTOMUTE_H
#define VF_AUTOMUTE_H

typedef struct VfAutoMute {
    int    on_lock;            /* setting: mute while locked */
    int    idle_minutes;       /* setting: 0 = off */
    double last_voice;         /* time of the last voice activity */
    int    by_lock, by_idle;   /* currently muting */
    int    user_mute;          /* manual mute */
} VfAutoMute;

void vf_automute_init(VfAutoMute *a, double now);
/* Call a few times per second. Returns the mute state the pipeline should have. */
int  vf_automute_update(VfAutoMute *a, double now, int locked, int voice_active);
/* The user pressed mute / the hotkey: returns the new effective state. An auto mute that is
 * active is cancelled (unmute); otherwise the manual mute toggles. */
int  vf_automute_toggle(VfAutoMute *a, double now);
int  vf_automute_effective(const VfAutoMute *a);

#endif
