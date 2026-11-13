/* Shout guard: very fast downward compressor in front of the main compressor, so a sudden
 * shout is pulled down within ~1 ms instead of hitting the limiter. Zero delay. */
#ifndef VF_FX_GUARD_H
#define VF_FX_GUARD_H

#include "dsp/fx.h"

enum {
    GUARD_THRESHOLD_DB = 0,   /* -30..0 (default -10) */
    GUARD_RATIO,              /* 2..20 (default 8) */
    GUARD_RELEASE_MS          /* 50..2000 (default 400) */
};

void fx_guard_make(Fx *fx);

#endif
