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
} Engine;

typedef struct EngineLatency {
    double capture_ms, playback_ms;   /* device buffers reported by miniaudio */
    double fifo_ms;                   /* re-chunking FIFO pre-fill */
    double dsp_ms;                    /* algorithmic delay of the effect chain */
    double total_ms;
} EngineLatency;

int  engine_init(Engine *e, ma_context *ctx, Pipeline *pipe, const EngineConfig *cfg);   /* 0 = ok */
int  engine_add_tap(Engine *e, VfSink *sink);       /* before engine_start */
int  engine_start(Engine *e);
void engine_stop(Engine *e);
void engine_uninit(Engine *e);
void engine_latency(const Engine *e, EngineLatency *out);   /* estimate, playback path */

/* The body of the audio callback, exposed so tests can drive it with
 * arbitrary block sizes without a device. `out` may be NULL (capture-only).
 * Audio thread only. */
void engine_process_block(Engine *e, float *out, const float *in, unsigned frames);
/* Prepares an Engine for engine_process_block() without opening a device. */
void engine_init_offline(Engine *e, Pipeline *pipe);

#endif
