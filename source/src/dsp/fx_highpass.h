#ifndef VF_FX_HIGHPASS_H
#define VF_FX_HIGHPASS_H

#include "dsp/fx.h"

enum { HP_FREQ = 0 };        /* cutoff, 20..300 Hz (default 80). 4th-order Butterworth. */

void fx_highpass_make(Fx *fx);

#endif
