#include "app/automute.h"

#include <string.h>

void vf_automute_init(VfAutoMute *a, double now)
{
    memset(a, 0, sizeof *a);
    a->last_voice = now;
}

int vf_automute_effective(const VfAutoMute *a)
{
    return a->user_mute || a->by_lock || a->by_idle;
}

int vf_automute_update(VfAutoMute *a, double now, int locked, int voice_active)
{
    a->by_lock = a->on_lock && locked;
    if (voice_active && !a->by_idle) a->last_voice = now;
    if (a->idle_minutes > 0 && !a->user_mute && now - a->last_voice > a->idle_minutes * 60.0) a->by_idle = 1;
    if (a->idle_minutes <= 0) a->by_idle = 0;
    return vf_automute_effective(a);
}

int vf_automute_toggle(VfAutoMute *a, double now)
{
    if (a->by_idle || a->by_lock) {              /* cancel the automatic mute */
        a->by_idle = 0;
        a->last_voice = now;
        if (a->by_lock) a->user_mute = 0;       /* (it comes back by itself if the screen is still locked) */
        a->user_mute = 0;
    } else {
        a->user_mute = !a->user_mute;
        a->last_voice = now;
    }
    return vf_automute_effective(a);
}
