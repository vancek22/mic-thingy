/* The standard mic thingy chain:
 * highpass -> rnnoise -> gate -> comp -> eq -> limiter */
#ifndef VF_CHAIN_H
#define VF_CHAIN_H

#include "dsp/pipeline.h"

typedef struct Chain {
    Fx highpass, rnnoise, gate, comp, eq, limiter;
} Chain;

/* Fills the Fx descriptors and adds them to the pipeline in order. 0 = ok. */
int chain_build(Chain *c, Pipeline *p);

#endif
