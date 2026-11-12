/* Fixed chain of effects processed on 480-sample frames. */
#ifndef VF_PIPELINE_H
#define VF_PIPELINE_H

#include "dsp/fx.h"

#define VF_MAX_FX 8

typedef struct Pipeline {
    Fx *fx[VF_MAX_FX];
    int count;
    atomic_int bypass_all;        /* 1 = pass audio through untouched */
    /* Meters, written by the audio thread, read (relaxed) by the UI. */
    _Atomic float peak_in, peak_out, vad;
} Pipeline;

void pipeline_init(Pipeline *p);
int  pipeline_add(Pipeline *p, Fx *fx);             /* before audio starts */
int  pipeline_start(Pipeline *p, int sample_rate);  /* runs fx->init */
void pipeline_stop(Pipeline *p);                    /* runs fx->destroy */

/* Audio thread. buf holds exactly VF_FRAME_SIZE samples, +-1.0 floats. */
void pipeline_process(Pipeline *p, float *buf);

#endif
