/* De-esser: a fast detector on the sibilance band (above `freq`) pulls down only that band
 * when it gets too loud; the rest of the voice is untouched. Zero delay. */
#ifndef VF_FX_DEESSER_H
#define VF_FX_DEESSER_H

#include "dsp/fx.h"

enum {
    DEESS_FREQ = 0,        /* 4000..10000 Hz start of the sibilance band (default 6500) */
    DEESS_THRESHOLD_DB,    /* -50..-5 (default -28) */
    DEESS_RATIO,           /* 2..10 (default 4) */
    DEESS_RANGE_DB         /* 0..20 maximum reduction (default 10) */
};

void fx_deesser_make(Fx *fx);

#endif
