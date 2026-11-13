/* Noise gate driven by the RNNoise voice probability (ctx->vad_prob).
 * If RNNoise did not run for this frame (bypassed), the gate stays open. */
#ifndef VF_FX_GATE_H
#define VF_FX_GATE_H

#include "dsp/fx.h"

enum {
    GATE_THRESHOLD = 0,   /* VAD probability that opens the gate, 0..1 (default 0.5) */
    GATE_HOLD_MS,         /* stay open this long after voice stops (default 150) */
    GATE_ATTACK_MS,       /* opening time (default 3) */
    GATE_RELEASE_MS,      /* closing time (default 100) */
    GATE_FLOOR_DB         /* gain while closed (default -50) */
};

void fx_gate_make(Fx *fx);

#endif
