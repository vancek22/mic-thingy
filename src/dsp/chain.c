#include "dsp/chain.h"

#include "dsp/fx_agc.h"
#include "dsp/fx_breath.h"
#include "dsp/fx_compressor.h"
#include "dsp/fx_deesser.h"
#include "dsp/fx_dereverb.h"
#include "dsp/fx_eq3.h"
#include "dsp/fx_gate.h"
#include "dsp/fx_guard.h"
#include "dsp/fx_highpass.h"
#include "dsp/fx_hum.h"
#include "dsp/fx_limiter.h"
#include "dsp/fx_rnnoise.h"
#include "dsp/fx_voice.h"
#include "dsp/fx_warmth.h"

int chain_build(Chain *c, Pipeline *p)
{
    fx_highpass_make(&c->highpass);
    fx_hum_make(&c->hum);
    fx_rnnoise_make(&c->rnnoise);
    fx_dereverb_make(&c->dereverb);
    fx_gate_make(&c->gate);
    fx_breath_make(&c->breath);
    fx_guard_make(&c->guard);
    fx_agc_make(&c->agc);
    fx_compressor_make(&c->comp);
    fx_deesser_make(&c->deesser);
    fx_eq3_make(&c->eq);
    fx_warmth_make(&c->warmth);
    fx_voice_make(&c->voice);
    fx_limiter_make(&c->limiter);
    Fx *order[] = { &c->highpass, &c->hum, &c->rnnoise, &c->dereverb, &c->gate, &c->breath, &c->guard,
                    &c->agc, &c->comp, &c->deesser, &c->eq, &c->warmth, &c->voice, &c->limiter };
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++)
        if (pipeline_add(p, order[i]) != 0) return -1;
    pipeline_set_mix_point(p, pipeline_find_fx(p, "limiter"));     /* mix bus + mute come right before it */
    return 0;
}
