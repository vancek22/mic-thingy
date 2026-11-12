/* Feedback ("howl") detector for the own-voice monitor: when the headphones are close to the
 * microphone a narrow line grows by itself. Looks at the spectrum of the monitored signal:
 * a bin that is loud, stands far above its neighbours and keeps growing for about half a second.
 * Pure logic; the GUI lowers the monitor level when it fires. */
#ifndef VF_HOWL_H
#define VF_HOWL_H

#include "dsp/spectrum.h"

typedef struct VfHowl {
    int   cand;                   /* bin currently followed, -1 none */
    int   run;                    /* updates it has been a loud, isolated line */
    float level0;                 /* its level when it started */
    float last;                   /* level at the previous update */
} VfHowl;

void vf_howl_init(VfHowl *h);
/* Call at the spectrum update rate (~50 Hz). Returns the frequency (Hz) of a detected howl, else 0. */
float vf_howl_update(VfHowl *h, const float *db);

#endif
