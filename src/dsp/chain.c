#include "dsp/chain.h"

#include "dsp/fx_compressor.h"
#include "dsp/fx_eq3.h"
#include "dsp/fx_gate.h"
#include "dsp/fx_highpass.h"
#include "dsp/fx_limiter.h"
#include "dsp/fx_rnnoise.h"

int chain_build(Chain *c, Pipeline *p)
{
    fx_highpass_make(&c->highpass);
    fx_rnnoise_make(&c->rnnoise);
    fx_gate_make(&c->gate);
    fx_compressor_make(&c->comp);
    fx_eq3_make(&c->eq);
    fx_limiter_make(&c->limiter);
    Fx *order[] = { &c->highpass, &c->rnnoise, &c->gate, &c->comp, &c->eq, &c->limiter };
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++)
        if (pipeline_add(p, order[i]) != 0) return -1;
    return 0;
}
