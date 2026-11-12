#ifndef VF_FX_RNNOISE_H
#define VF_FX_RNNOISE_H

#include "dsp/fx.h"

enum {
    RNNOISE_PARAM_WET = 0,        /* 0..1, dry/wet mix (default 0.95) */
    RNNOISE_PARAM_LOOKAHEAD       /* 1 = upstream (20 ms delay), 0 = patched, 10 ms */
};

#define RNNOISE_DELAY_NO_LOOKAHEAD VF_FRAME_SIZE

/* Algorithmic delay of rnnoise_process_frame() in v0.2: two frames (overlap-add
 * window + one frame of look-ahead via `delayed_X`). Verified by tests/test_dsp.c.
 * The dry path is delayed by the same amount so dry/wet mixing is phase-aligned.
 * Note the VAD returned for a frame describes audio that is output ~10-20 ms later,
 * so the gate effectively gets a small look-ahead. */
#define RNNOISE_DELAY_SAMPLES (2 * VF_FRAME_SIZE)

void fx_rnnoise_make(Fx *fx);     /* fills the Fx descriptor; call fx->init next */

#endif
