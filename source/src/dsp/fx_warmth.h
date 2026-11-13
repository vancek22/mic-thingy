/* Voice warmth: gentle tube-style saturation (small asymmetry = even harmonics) plus a low
 * shelf, one knob. Zero delay; small signals keep their level. */
#ifndef VF_FX_WARMTH_H
#define VF_FX_WARMTH_H

#include "dsp/fx.h"

enum { WARMTH_AMOUNT = 0 };   /* 0..1 (default 0.3); 0 = bit-exact pass-through */

void fx_warmth_make(Fx *fx);

#endif
