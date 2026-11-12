/* Look-ahead brickwall limiter (1.5 ms look-ahead = 1.5 ms added latency). */
#ifndef VF_FX_LIMITER_H
#define VF_FX_LIMITER_H

#include "dsp/fx.h"

enum {
    LIM_CEILING_DB = 0,   /* -12..0 (default -1) */
    LIM_RELEASE_MS        /* 5..1000 (default 60) */
};

#define LIMITER_LOOKAHEAD 72   /* samples @ 48 kHz */

void fx_limiter_make(Fx *fx);

#endif
