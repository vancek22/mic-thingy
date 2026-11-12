/* Calibration wizard: 5 s of silence, then 8 s of normal speech. The meter stream gives the
 * room's noise floor / voice detector behaviour and the speech level; from those the wizard
 * proposes the voice threshold of the gate and the compressor threshold and make-up gain.
 * Pure logic (no audio, no UI): feed meter frames, ask for the recommendation. */
#ifndef VF_CALIBRATE_H
#define VF_CALIBRATE_H

#include "dsp/pipeline.h"

#define VF_CAL_SILENCE_S 5
#define VF_CAL_SPEECH_S  8

typedef enum { VF_CAL_IDLE = 0, VF_CAL_SILENCE, VF_CAL_SPEECH, VF_CAL_DONE, VF_CAL_FAILED } VfCalState;

typedef struct VfCalResult {
    float noise_db;          /* median input peak during silence */
    float vad_silence;       /* 95th percentile of the voice detector during silence */
    float speech_db;         /* median input peak of the voiced frames */
    float speech_p90_db;
    float voiced_pct;        /* share of the speech phase the detector heard voice */
    /* proposal */
    float gate_threshold;
    float comp_threshold_db;
    float comp_makeup_db;
    float snr_db;            /* speech_db - noise_db */
} VfCalResult;

#define VF_CAL_MAX_FRAMES ((VF_CAL_SILENCE_S + VF_CAL_SPEECH_S + 2) * 100)

typedef struct VfCalibrate {
    VfCalState state;
    int frames;                              /* frames seen in the current phase */
    float sil_in[VF_CAL_SILENCE_S * 100 + 8], sil_vad[VF_CAL_SILENCE_S * 100 + 8];
    int n_sil;
    float sp_in[VF_CAL_SPEECH_S * 100 + 8], sp_vad[VF_CAL_SPEECH_S * 100 + 8];
    int n_sp;
    VfCalResult result;
    char why[96];                            /* reason when FAILED */
} VfCalibrate;

void vf_cal_init(VfCalibrate *c);
void vf_cal_start(VfCalibrate *c);                          /* -> SILENCE */
void vf_cal_frame(VfCalibrate *c, const float *meter_frame);/* advances phases by itself, 100 frames = 1 s */
/* 0..1 progress of the whole wizard, for a progress bar */
float vf_cal_progress(const VfCalibrate *c);
/* Applies the proposal to the pipeline (gate threshold, compressor threshold + makeup). */
void vf_cal_apply(const VfCalibrate *c, Pipeline *p);

#endif
