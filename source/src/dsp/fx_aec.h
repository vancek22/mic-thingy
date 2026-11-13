/* Acoustic echo canceller (experimental): removes what the speakers play (the "reference",
 * captured by the system-audio loopback) from the microphone signal with a 2048-tap NLMS
 * adaptive filter (43 ms). The reference is delayed by a manual `delay` first (render + air +
 * capture latency are not known to the program). Adaptation freezes during double talk
 * (Geigel detector), so your own voice is not learned as echo.
 * Needs the reference: Fx.state is attached to a VfSink by fx_aec_attach(); without it (no
 * loopback, other OS) the effect passes the signal through. Zero algorithmic delay. */
#ifndef VF_FX_AEC_H
#define VF_FX_AEC_H

#include "audio/sink.h"
#include "dsp/fx.h"

enum {
    AEC_DELAY_MS = 0,   /* 0..200 bulk delay of the reference (default 40) */
    AEC_STRENGTH        /* 0.05..1 NLMS step size (default 0.4) */
};

#define AEC_TAPS 2048

void fx_aec_make(Fx *fx);
/* Reference source (consumer side is the audio thread). Call after init, before audio starts. */
void fx_aec_attach(Fx *fx, VfSink *ref);

#endif
