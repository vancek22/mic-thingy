/* Fixed chain of effects processed on 480-sample frames. */
#ifndef VF_PIPELINE_H
#define VF_PIPELINE_H

#include "dsp/fx.h"

#define VF_MAX_FX 8

typedef struct Pipeline {
    Fx *fx[VF_MAX_FX];
    int count;
    atomic_int bypass_all;        /* 1 = pass audio through untouched */

    /* UI -> audio: target parameter values. The audio thread compares them with
     * `applied` once per frame and calls fx->set_param() only on change. */
    _Atomic float params[VF_MAX_FX][VF_MAX_PARAMS];
    float applied[VF_MAX_FX][VF_MAX_PARAMS];      /* audio thread only */

    /* Meters, written by the audio thread, read (relaxed) by the UI. */
    _Atomic float peak_in, peak_out, vad, gate_gain, comp_gr_db;
    /* Optional per-frame meter stream for the GUI (lock-free SPSC, audio -> UI):
     * VF_METER_FIELDS floats per 10 ms frame, see VfMeterField. NULL = off. */
    struct VfRing *meter_ring;
} Pipeline;

enum VfMeterField { VF_M_PEAK_IN, VF_M_PEAK_OUT, VF_M_RMS_OUT, VF_M_VAD, VF_M_GATE, VF_M_GR, VF_METER_FIELDS };

void pipeline_init(Pipeline *p);
int  pipeline_add(Pipeline *p, Fx *fx);             /* before audio starts */
int  pipeline_start(Pipeline *p, int sample_rate);  /* runs fx->init, applies defaults */
void pipeline_stop(Pipeline *p);                    /* runs fx->destroy */

/* Control thread. Values are clamped to the parameter range. */
int   pipeline_set_param(Pipeline *p, int fx_index, int param_id, float value);
float pipeline_get_param(const Pipeline *p, int fx_index, int param_id);
void  pipeline_set_bypass(Pipeline *p, int fx_index, int bypass);
/* Sum of the algorithmic delays of the active (non-bypassed) effects. */
int   pipeline_delay_samples(const Pipeline *p);

/* Case-insensitive prefix lookups; -1 if absent or ambiguous-free first match. */
int pipeline_find_fx(const Pipeline *p, const char *name);
int pipeline_find_param(const Pipeline *p, int fx_index, const char *name);

/* Audio thread. buf holds exactly VF_FRAME_SIZE samples, +-1.0 floats. */
void pipeline_process(Pipeline *p, float *buf);

#endif
