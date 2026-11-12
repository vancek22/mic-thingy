/* Look-ahead brickwall limiter (1.5 ms look-ahead = 1.5 ms added latency). */
#ifndef VF_FX_LIMITER_H
#define VF_FX_LIMITER_H

#include "dsp/fx.h"

enum {
    LIM_CEILING_DB = 0,   /* -12..0 (default -1) */
    LIM_RELEASE_MS,       /* 5..1000 (default 60) */
    LIM_LOOKAHEAD_MS      /* 0..1.5 (default 1.5); 0 = instant attack, no delay (low-latency mode) */
};

#define LIMITER_LOOKAHEAD 72   /* max look-ahead, samples @ 48 kHz (1.5 ms) */

void fx_limiter_make(Fx *fx);

#endif
