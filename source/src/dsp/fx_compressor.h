#ifndef VF_FX_COMPRESSOR_H
#define VF_FX_COMPRESSOR_H

#include "dsp/fx.h"

enum {
    COMP_THRESHOLD_DB = 0,   /* -60..0 (default -20) */
    COMP_RATIO,              /* 1..20 (default 3) */
    COMP_ATTACK_MS,          /* 0.1..200 (default 8) */
    COMP_RELEASE_MS,         /* 10..2000 (default 150) */
    COMP_MAKEUP_DB,          /* 0..24 (default 3) */
    COMP_KNEE_DB             /* 0..24 soft knee width (default 6) */
};

void fx_compressor_make(Fx *fx);

#endif
