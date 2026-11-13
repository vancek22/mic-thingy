/* Mains hum remover: narrow notches on the fundamental (50/60 Hz) and its harmonics.
 * Zero delay. "auto" picks 50 or 60 Hz from the signal (energy at the two candidates). */
#ifndef VF_FX_HUM_H
#define VF_FX_HUM_H

#include "dsp/fx.h"

enum {
    HUM_FREQ = 0,      /* 40..70 Hz fundamental (default 50) */
    HUM_HARMONICS,     /* 1..6 notches (default 4) */
    HUM_AUTO           /* 1 = choose 50 or 60 Hz automatically */
};

void fx_hum_make(Fx *fx);

#endif
