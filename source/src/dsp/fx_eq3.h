/* 3-band EQ: low shelf, peaking mid, high shelf. */
#ifndef VF_FX_EQ3_H
#define VF_FX_EQ3_H

#include "dsp/fx.h"

enum {
    EQ_LOW_DB = 0,    /* -12..12 (default 0) */
    EQ_MID_DB,        /* -12..12 */
    EQ_HIGH_DB,       /* -12..12 */
    EQ_LOW_FREQ,      /* shelf corner, 50..500 Hz (default 120) */
    EQ_MID_FREQ,      /* 200..5000 Hz (default 1500) */
    EQ_MID_Q,         /* 0.3..4 (default 1) */
    EQ_HIGH_FREQ      /* shelf corner, 2000..12000 Hz (default 6000) */
};

void fx_eq3_make(Fx *fx);

#endif
