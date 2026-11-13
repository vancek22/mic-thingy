/* The standard mic thingy chain (optional effects are off until enabled):
 * highpass -> aec -> hum -> rnnoise -> dereverb -> gate -> breath -> guard -> agc -> comp -> deesser
 *          -> eq -> warmth -> voice -> limiter */
#ifndef VF_CHAIN_H
#define VF_CHAIN_H

#include "dsp/pipeline.h"

typedef struct Chain {
    Fx highpass, aec, hum, rnnoise, dereverb, gate, breath, guard, agc, comp, deesser, eq, warmth, voice, limiter;
} Chain;

/* Fills the Fx descriptors and adds them to the pipeline in order. 0 = ok. */
int chain_build(Chain *c, Pipeline *p);

#endif
