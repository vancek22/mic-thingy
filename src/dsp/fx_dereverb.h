/* Room-tail reducer (reverb): a downward expander that follows the recent speech level.
 * When a word ends the room keeps ringing, decaying exponentially; once the level has fallen
 * `reach` dB below the speech, the expander makes the decay `strength` times steeper.
 * Zero delay. (A spectral dereverberator would need a frame of delay - see the plan.) */
#ifndef VF_FX_DEREVERB_H
#define VF_FX_DEREVERB_H

#include "dsp/fx.h"

enum {
    DEREV_STRENGTH = 0,   /* 1..4 expander ratio (default 2) */
    DEREV_DEPTH_DB,       /* 0..30 maximum reduction (default 15) */
    DEREV_REACH_DB        /* 10..40 start this far below the speech level (default 12) */
};

void fx_dereverb_make(Fx *fx);

#endif
