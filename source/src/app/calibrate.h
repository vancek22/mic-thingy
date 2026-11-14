/* Calibration wizard ("auto setup"): 5 s of silence, then 15 s of reading a text aloud.
 * Two feeds: the meter stream (input peak + voice detector, 100 frames/s) and the raw microphone samples
 * (vf_cal_audio, from the scope ring). From the silence it measures the noise floor, mains hum and how
 * impulsive the noise is; from the reading the speech level, pitch, spectral balance and sibilance.
 * The proposal sets: high-pass, hum filter, gate, denoiser strength, compressor, EQ, de-esser.
 * Pure logic (no audio device, no UI); allocation-free, the struct is large (keep it static). */
#ifndef VF_CALIBRATE_H
#define VF_CALIBRATE_H

#include "dsp/pipeline.h"
#include "dsp/fft960.h"

#define VF_CAL_SILENCE_S 5
#define VF_CAL_SPEECH_S  15

typedef enum { VF_CAL_IDLE = 0, VF_CAL_SILENCE, VF_CAL_SPEECH, VF_CAL_DONE, VF_CAL_FAILED } VfCalState;

/* Speech spectrum targets (dB, per-bin mean power relative to 300-1000 Hz), median of 80 LibriSpeech speakers
 * (tools/nn/ltas.py): 100-250 Hz, 2-4 kHz, 5-8 kHz. */
#define VF_CAL_T_LOW   1.2f
#define VF_CAL_T_MID  -17.6f
#define VF_CAL_T_HIGH -24.6f

typedef struct VfCalResult {
    float noise_db;          /* median input peak during silence */
    float vad_silence;       /* 95th percentile of the voice detector during silence */
    float speech_db;         /* median input peak of the voiced frames */
    float speech_p90_db;
    float voiced_pct;        /* share of the speech phase the detector heard voice */
    float snr_db;            /* speech_db - noise_db */
    /* audio analysis (valid when has_audio) */
    int   has_audio;
    float f0_hz;             /* median pitch of voiced frames, 0 = unknown */
    int   hum_hz;            /* 0, 50 or 60 */
    float hum_db;            /* mains harmonics above the neighbouring noise */
    float rumble_db;         /* noise below 80 Hz relative to the voice there */
    float low_db, mid_db, high_db;   /* measured speech balance, same definition as VF_CAL_T_* */
    float sib_db;            /* 95th percentile of the 5-10 kHz level minus the median 300-3000 Hz level (voiced frames) */
    float sib_level_db;      /* 90th percentile of the sibilance-band peak level, dBFS */
    float transient_db;      /* noise frame energy p99 over median: high = clicks, typing */
    /* proposal */
    float gate_threshold;
    float comp_threshold_db;
    float comp_makeup_db;
    float hp_hz;
    float rn_wet;
    float eq_low_db, eq_mid_db, eq_high_db;
    int   deess;             /* 1 = enable the de-esser */
    float deess_thr_db;
} VfCalResult;

typedef struct VfCalibrate {
    VfCalState state;
    int frames;                              /* meter frames seen in the current phase */
    float sil_in[VF_CAL_SILENCE_S * 100 + 8], sil_vad[VF_CAL_SILENCE_S * 100 + 8];
    int n_sil;
    float sp_in[VF_CAL_SPEECH_S * 100 + 8], sp_vad[VF_CAL_SPEECH_S * 100 + 8];
    int n_sp;
    float last_vad;
    /* audio analysis */
    VfFft fft;
    float win[VF_FFT_N], hist[VF_FFT_N], re[VF_FFT_N], im[VF_FFT_N];
    int hist_fill, audio_frames;
    double noise_psd[VF_FFT_N / 2 + 1], speech_psd[VF_FFT_N / 2 + 1];
    int n_noise_fr, n_speech_fr;
    double gs1[12], gs2[12], gcoef[12];       /* Goertzel over the whole silence phase */
    long gn;
    float nfe[VF_CAL_SILENCE_S * 100 + 8];   /* noise frame energies */
    int n_nfe;
    float f0s[VF_CAL_SPEECH_S * 100 + 8], sib[VF_CAL_SPEECH_S * 100 + 8], sibl[VF_CAL_SPEECH_S * 100 + 8];
    float vlo[VF_CAL_SPEECH_S * 100 + 8];     /* 300-3000 Hz level of the same frames */
    int n_f0, n_sib;
    VfCalResult result;
    char why[96];                            /* reason when FAILED */
} VfCalibrate;

void vf_cal_init(VfCalibrate *c);
void vf_cal_start(VfCalibrate *c);                          /* -> SILENCE */
void vf_cal_frame(VfCalibrate *c, const float *meter_frame);/* advances phases by itself, 100 frames = 1 s */
/* Raw microphone samples (any block size). Optional: without them only gate + compressor are proposed. */
void vf_cal_audio(VfCalibrate *c, const float *in, int n);
/* 0..1 progress of the whole wizard, for a progress bar */
float vf_cal_progress(const VfCalibrate *c);
/* Applies the proposal to the pipeline (and switches the hum filter / de-esser on when they are needed). */
void vf_cal_apply(const VfCalibrate *c, Pipeline *p);

#endif
