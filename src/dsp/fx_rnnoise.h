#ifndef VF_FX_RNNOISE_H
#define VF_FX_RNNOISE_H

#include "dsp/fx.h"

enum { RNNOISE_PARAM_WET = 0 };   /* 0..1, dry/wet mix (default 1) */

void fx_rnnoise_make(Fx *fx);     /* fills the Fx descriptor; call fx->init next */

#endif
