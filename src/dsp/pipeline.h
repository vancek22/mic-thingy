/* Fixed chain of effects processed on 480-sample frames. */
#ifndef VF_PIPELINE_H
#define VF_PIPELINE_H

#include "dsp/fx.h"

#define VF_MAX_FX  16
#define VF_MAX_MIX 4

/* An extra signal mixed into the voice right before the limiter (soundboard, system audio).
 * read() runs on the audio thread and must fill n samples (silence if it has none). */
typedef struct MixSource {
    void (*read)(void *ctx, float *dst, int n);
    void *ctx;
    _Atomic float gain;      /* linear gain (UI -> audio) */
    _Atomic float duck;      /* linear gain while the voice is active; 1 = no ducking */
    float duck_state;        /* audio thread: smoothed ducking gain */
    _Atomic float level;     /* meter: peak of the last frame after gain (audio -> UI) */
} MixSource;

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
    /* Optional scope stream for the spectrum analyser: per frame VF_FRAME_SIZE input samples followed by
     * VF_FRAME_SIZE output samples. The UI sets/clears it; NULL = off (costs nothing). */
    _Atomic(struct VfRing *) scope_ring;

    /* Mix bus and mute. The bus is added to the voice before effect `mix_before` (the limiter);
     * mute silences only the voice (10 ms ramp), the bus keeps playing. */
    MixSource *mix[VF_MAX_MIX];
    int n_mix;
    int mix_before;               /* effect index, -1 = after the last effect */
    atomic_int mute;
    float mute_gain;              /* audio thread */
} Pipeline;

enum VfMeterField { VF_M_PEAK_IN, VF_M_PEAK_OUT, VF_M_RMS_OUT, VF_M_VAD, VF_M_GATE, VF_M_GR, VF_M_LIM, VF_METER_FIELDS };

void pipeline_init(Pipeline *p);
int  pipeline_add(Pipeline *p, Fx *fx);             /* before audio starts */
int  pipeline_add_mix(Pipeline *p, MixSource *src);   /* before audio starts; gain 1, duck 1 */
void pipeline_set_mix_point(Pipeline *p, int fx_index);
void pipeline_set_mute(Pipeline *p, int mute);
int  pipeline_get_mute(const Pipeline *p);
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
