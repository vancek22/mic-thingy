/* Voice effects: pitch shift (two cross-faded delay taps, ~15 ms average delay while the shift
 * is not 0), robot (ring modulation) and radio/telephone (band-limit + saturation). */
#ifndef VF_FX_VOICE_H
#define VF_FX_VOICE_H

#include "dsp/fx.h"

enum {
    VOICE_PITCH = 0,   /* -12..12 semitones (default 0) */
    VOICE_ROBOT,       /* toggle */
    VOICE_RADIO        /* toggle */
};

#define VOICE_PITCH_DELAY 720   /* average delay (samples) while shifting: half the 30 ms window */

void fx_voice_make(Fx *fx);

#endif
