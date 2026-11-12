/* Automatic gain control: slowly moves the speech level towards a target. Only adapts while
 * the voice is active (RNNoise VAD, or a level threshold when no VAD is available), so pauses
 * and noise are never boosted. Place before the compressor. Zero delay. */
#ifndef VF_FX_AGC_H
#define VF_FX_AGC_H

#include "dsp/fx.h"

enum {
    AGC_TARGET_DB = 0,     /* -40..-6 dBFS RMS of the voice (default -20) */
    AGC_MAX_GAIN_DB,       /* 0..30 (default 12) */
    AGC_SPEED_MS           /* 200..10000 time constant (default 1500) */
};

void fx_agc_make(Fx *fx);

#endif
