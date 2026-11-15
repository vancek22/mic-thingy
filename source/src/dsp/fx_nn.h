/* "nn": the project's own small neural denoiser (tools/nn). 960-sample Vorbis-windowed frames, hop 480,
 * per-bin gains from a two-layer GRU, overlap-add -> 480 samples (10 ms) delay, like RNNoise. It also writes
 * the voice probability into the frame context, so the gate and breath effects keep working when RNNoise is off. */
#ifndef VF_FX_NN_H
#define VF_FX_NN_H

#include "dsp/fx.h"
#include "dsp/nn.h"

enum {
    NN_PARAM_WET = 0,      /* 0..1 dry/wet */
    NN_PARAM_FLOOR,        /* dB, lowest gain applied to any bin */
    NN_PARAM_MODEL,        /* 0 = nsnet (gains only), 1 = erbdf (ERB-band gains + deep filtering, dsp/erbdf.h) */
};

#define NN_DELAY_SAMPLES VF_FRAME_SIZE

void fx_nn_make(Fx *fx);
/* Test hook: the same effect with other weights (NULL = built-in). Call before init. */
void fx_nn_use_weights(Fx *fx, const VfNnWeights *w);

#endif
