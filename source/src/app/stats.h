/* Statistics from the meter stream (UI thread): one sample per second for the last hour, the
 * "is my setup OK" verdict (quality meter) and the dropout history. Pure logic, no I/O. */
#ifndef VF_STATS_H
#define VF_STATS_H

#define VF_STATS_SECONDS 3600

typedef struct VfStatSec {
    float in_db;          /* loudest input peak of the second */
    float out_db;         /* loudest output peak */
    float voiced;         /* fraction of frames with the voice detector above 0.5 */
    float voice_in_db;    /* mean input peak of the voiced frames (-120 if none) */
    float noise_cut_db;   /* mean (input peak - output peak) of the unvoiced frames, >= 0 */
    float limiter;        /* fraction of frames in which the limiter reduced by more than 1.5 dB */
    float gr_db;          /* deepest compressor reduction (negative) */
    unsigned xruns;       /* dropouts during this second */
} VfStatSec;

typedef struct VfStats {
    VfStatSec sec[VF_STATS_SECONDS];
    int head;             /* index of the most recent finished second */
    int count;            /* finished seconds stored (<= VF_STATS_SECONDS) */
    unsigned last_xruns;  /* running total seen at the last tick */
    /* accumulators for the second in progress */
    int frames, voiced_frames, unvoiced_frames, lim_frames;
    float peak_in, peak_out, gr;
    double voice_in_sum, cut_sum;
} VfStats;

typedef enum {
    VF_Q_NO_DATA = 0,     /* not enough information yet */
    VF_Q_NO_SPEECH,       /* nobody spoke during the window */
    VF_Q_TOO_QUIET,       /* speech peaks lower than -30 dBFS: raise the microphone level */
    VF_Q_TOO_LOUD,        /* speech peaks reach -1 dBFS or more: lower it, the voice clips */
    VF_Q_LIMITER_BUSY,    /* the limiter works on a large share of the speech */
    VF_Q_OK
} VfQuality;

typedef struct VfQualityInfo {
    VfQuality verdict;
    float speech_db;      /* mean input peak while speaking */
    float noise_cut_db;   /* how much quieter the pauses became */
    float limiter_pct;    /* % of voiced frames with limiter activity */
    float voiced_pct;
} VfQualityInfo;

void vf_stats_init(VfStats *s);
/* One meter frame (fields as in enum VfMeterField); call for every frame drained. */
void vf_stats_frame(VfStats *s, const float *meter_frame);
/* Once per second: closes the current second. xruns_total = running dropout counter of the session. */
void vf_stats_tick(VfStats *s, unsigned xruns_total);
/* Verdict over the last `seconds` (>= 3 stored seconds needed). */
void vf_stats_quality(const VfStats *s, int seconds, VfQualityInfo *out);
/* Dropouts per minute for the last `minutes` minutes, oldest first (out has `minutes` entries). */
int  vf_stats_xrun_history(const VfStats *s, int minutes, unsigned *out);

#endif
