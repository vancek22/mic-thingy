/* Audio engine: capture -> pipeline -> playback and/or taps.
 *
 * Modes (EngineConfig):
 *   duplex        capture device -> chain -> playback device (headphones or "CABLE Input")
 *   capture-only  capture device -> chain -> taps only (Linux virtual mic via PipeWire)
 *   file source   WAV samples (looped) -> chain -> playback device (tests without a mic)
 * Taps receive every processed 480-sample frame (monitor device, PipeWire stream).
 *
 * miniaudio hands the callback arbitrary block sizes, so samples are
 * re-chunked into exact VF_FRAME_SIZE frames with fixed, preallocated FIFOs. */
#ifndef VF_ENGINE_H
#define VF_ENGINE_H

#include <stdatomic.h>
#include "miniaudio.h"
#include "audio/sink.h"
#include "dsp/pipeline.h"

#define VF_OUT_FIFO       4096   /* samples; must hold several frames */
#define VF_DEFAULT_PERIOD 240    /* frames per device period (request; the OS may round it) */
#define VF_MAX_TAPS       2
#define VF_SCRATCH        1024

typedef struct EngineConfig {
    const ma_device_id *capture_id;    /* NULL = system default */
    const ma_device_id *playback_id;   /* NULL = system default */
    int no_playback;                   /* capture-only: output only to taps */
    int period_frames;                 /* <= 0: VF_DEFAULT_PERIOD. Prefer divisors of 480. */
    const float *file_src;             /* non-NULL: replaces the microphone (looped) */
    long file_len;
    /* Experimental device knobs (L1 A/B tests; all 0 = current defaults). */
    int no_fixed_callback;             /* miniaudio noFixedSizedCallback: no intermediary buffers */
    int pro_audio;                     /* WASAPI: MMCSS "Pro Audio" thread class */
    int periods;                       /* <= 0: 2 (miniaudio uses one value for capture and playback) */
} EngineConfig;

typedef struct Engine {
    ma_device dev;
    Pipeline *pipe;
    int device_ready;
    VfSink *taps[VF_MAX_TAPS];
    int ntaps;

    /* file source (audio thread only once running) */
    const float *file_src;
    long file_len, file_pos;
    float scratch[VF_SCRATCH];

    /* Audio-thread-only state (never touched elsewhere while running). */
    float in_acc[VF_FRAME_SIZE];
    int   in_count;
    float out_fifo[VF_OUT_FIFO];
    int   out_head, out_count;
    int   started;
    int   prime_samples;     /* silence pre-filled on the first callback */
    int   ftz_set;

    atomic_uint underruns;   /* output starved after startup */
    atomic_uint overruns;    /* output FIFO overflowed */

    /* Diagnostics written by the audio thread (relaxed atomics, read by the UI). */
    atomic_uint blk_min, blk_max, blk_last;     /* callback block size, frames */
    atomic_int  fifo_min, fifo_max;             /* output FIFO fill left after each callback */
} Engine;

typedef struct EngineLatency {
    /* expected delay contributions (ms) */
    double capture_ms;                /* one capture period (data waits for a full period) */
    double playback_ms;               /* real playback buffer (WASAPI GetBufferSize when known) */
    double interm_ms;                 /* miniaudio fixed-size callback buffers (capture + playback) */
    double fifo_ms;                   /* re-chunking FIFO: largest fill seen after a callback */
    double dsp_ms;                    /* algorithmic delay of the effect chain */
    double total_ms;
    /* raw facts for diagnostics */
    unsigned cap_period, cap_periods, cap_buffer, cap_rate;     /* frames / count / frames / Hz */
    unsigned play_period, play_periods, play_buffer, play_rate;
    unsigned cap_interm, play_interm;                           /* intermediary capacity, frames */
    unsigned blk_min, blk_max, blk_last;                        /* callback blocks seen, frames */
    int fifo_min, fifo_max, prime;                              /* samples */
    int wasapi;                                                 /* buffer sizes came from WASAPI */
} EngineLatency;

int  engine_init(Engine *e, ma_context *ctx, Pipeline *pipe, const EngineConfig *cfg);   /* 0 = ok */
int  engine_add_tap(Engine *e, VfSink *sink);       /* before engine_start */
int  engine_start(Engine *e);
void engine_stop(Engine *e);
void engine_uninit(Engine *e);
void engine_latency(const Engine *e, EngineLatency *out);   /* estimate, playback path */
/* Multi-line human readable breakdown of engine_latency() (CLI diagnostics). */
void engine_latency_report(const Engine *e, char *buf, int size);

/* Fills a device configuration exactly as engine_init() does; shared with
 * tools/vf_latency.c so measurements use the same settings. */
void engine_device_config(ma_device_config *cfg, ma_device_type type, const EngineConfig *c);

/* The body of the audio callback, exposed so tests can drive it with
 * arbitrary block sizes without a device. `out` may be NULL (capture-only).
 * Audio thread only. */
void engine_process_block(Engine *e, float *out, const float *in, unsigned frames);
/* Prepares an Engine for engine_process_block() without opening a device. */
void engine_init_offline(Engine *e, Pipeline *pipe);

#endif
