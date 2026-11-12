/* Non-speech event suppressor, placed after the gate:
 *  - breaths / lip smacks: sounds the voice detector is unsure about (VAD between "silence"
 *    and "speech") and that are clearly quieter than the speech are ducked;
 *  - clicks (keyboard, mouse): short high-frequency transients outside speech are ducked.
 * Needs the RNNoise VAD (ctx->vad_valid); passes audio unchanged without it. Zero delay. */
#ifndef VF_FX_BREATH_H
#define VF_FX_BREATH_H

#include "dsp/fx.h"

enum {
    BREATH_DEPTH_DB = 0,    /* 0..30 reduction of breaths (default 12) */
    BREATH_CLICK_DB,        /* 0..30 reduction of clicks (default 14) */
    BREATH_SENS             /* 0..1 how eagerly events are detected (default 0.5) */
};

void fx_breath_make(Fx *fx);

#endif
