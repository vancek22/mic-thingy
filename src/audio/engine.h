/* Full-duplex audio engine: capture -> pipeline -> playback.
 *
 * miniaudio hands the callback arbitrary block sizes, so samples are
 * re-chunked into exact VF_FRAME_SIZE frames with fixed, preallocated FIFOs. */
#ifndef VF_ENGINE_H
#define VF_ENGINE_H

#include <stdatomic.h>
#include "miniaudio.h"
#include "dsp/pipeline.h"

#define VF_OUT_FIFO 4096   /* samples; must hold several frames */

typedef struct Engine {
    ma_device dev;
    Pipeline *pipe;
    int device_ready;

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
    unsigned capture_rate, playback_rate;
} EngineLatency;

/* NULL ids select the system default device. 0 = ok. */
int  engine_init(Engine *e, ma_context *ctx, Pipeline *pipe,
                 const ma_device_id *capture_id, const ma_device_id *playback_id);
int  engine_start(Engine *e);
void engine_stop(Engine *e);
void engine_uninit(Engine *e);
void engine_latency(const Engine *e, EngineLatency *out);   /* estimate */

/* The body of the audio callback, exposed so tests can drive it with
 * arbitrary block sizes without a device. Audio thread only. */
void engine_process_block(Engine *e, float *out, const float *in, unsigned frames);
/* Prepares an Engine for engine_process_block() without opening a device. */
void engine_init_offline(Engine *e, Pipeline *pipe);

#endif
