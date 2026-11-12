#include "audio/engine.h"

#include <string.h>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#define VF_HAVE_SSE 1
#endif

/* ---- audio thread: no malloc, no locks, no I/O below this line's callback ---- */

void engine_process_block(Engine *e, float *out, const float *in, unsigned frames)
{
#ifdef VF_HAVE_SSE
    if (!e->ftz_set) {                       /* flush denormals: stable CPU load */
        _mm_setcsr(_mm_getcsr() | 0x8040);   /* FTZ | DAZ */
        e->ftz_set = 1;
    }
#endif

    /* First callback: pre-fill the output FIFO with the minimum silence that
     * keeps a constant block size B from ever starving: 480 - gcd(B, 480).
     * (0 for B = 480 or 240-multiples of it, up to 479 otherwise.) If block sizes
     * vary later, a starved callback outputs silence without consuming, which
     * raises the buffer level permanently - a one-off glitch, then stable. */
    if (!e->started) {
        unsigned a = frames, b = VF_FRAME_SIZE;
        while (b) { unsigned t = a % b; a = b; b = t; }
        e->prime_samples = a ? VF_FRAME_SIZE - (int)a : 0;
        e->out_head = 0;
        e->out_count = e->prime_samples;      /* out_fifo is zeroed at init */
        e->started = 1;
    }

    /* 1) capture -> accumulator -> pipeline -> output FIFO */
    for (unsigned i = 0; i < frames; i++) {
        e->in_acc[e->in_count++] = in[i];
        if (e->in_count == VF_FRAME_SIZE) {
            e->in_count = 0;
            pipeline_process(e->pipe, e->in_acc);
            if (e->out_count + VF_FRAME_SIZE <= VF_OUT_FIFO) {
                int tail = e->out_head + e->out_count;
                for (int k = 0; k < VF_FRAME_SIZE; k++)
                    e->out_fifo[(tail + k) % VF_OUT_FIFO] = e->in_acc[k];
                e->out_count += VF_FRAME_SIZE;
            } else {
                atomic_fetch_add_explicit(&e->overruns, 1, memory_order_relaxed);
            }
        }
    }

    /* 2) output FIFO -> playback */
    for (unsigned i = 0; i < frames; i++) {
        if (e->out_count > 0) {
            out[i] = e->out_fifo[e->out_head];
            e->out_head = (e->out_head + 1) % VF_OUT_FIFO;
            e->out_count--;
        } else {
            out[i] = 0.0f;
            atomic_fetch_add_explicit(&e->underruns, 1, memory_order_relaxed);
        }
    }
}

static void data_callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    engine_process_block((Engine *)dev->pUserData, (float *)output, (const float *)input, frames);
}

/* ---- control thread ---- */

void engine_init_offline(Engine *e, Pipeline *pipe)
{
    memset(e, 0, sizeof *e);
    e->pipe = pipe;
    atomic_init(&e->underruns, 0);
    atomic_init(&e->overruns, 0);
}

int engine_init(Engine *e, ma_context *ctx, Pipeline *pipe,
                const ma_device_id *capture_id, const ma_device_id *playback_id, int period_frames)
{
    memset(e, 0, sizeof *e);
    e->pipe = pipe;
    atomic_init(&e->underruns, 0);
    atomic_init(&e->overruns, 0);

    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.sampleRate = VF_SAMPLE_RATE;
    cfg.periodSizeInFrames = (ma_uint32)(period_frames > 0 ? period_frames : VF_DEFAULT_PERIOD);
    cfg.periods = 2;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.capture.pDeviceID = capture_id;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.playback.pDeviceID = playback_id;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.dataCallback = data_callback;
    cfg.pUserData = e;

    if (ma_device_init(ctx, &cfg, &e->dev) != MA_SUCCESS) return -1;
    e->device_ready = 1;
    return 0;
}

int engine_start(Engine *e)
{
    return ma_device_start(&e->dev) == MA_SUCCESS ? 0 : -1;
}

void engine_stop(Engine *e)
{
    if (e->device_ready) ma_device_stop(&e->dev);
}

void engine_uninit(Engine *e)
{
    if (e->device_ready) { ma_device_uninit(&e->dev); e->device_ready = 0; }
}

void engine_latency(const Engine *e, EngineLatency *out)
{
    const ma_device *d = &e->dev;
    out->capture_rate  = d->capture.internalSampleRate;
    out->playback_rate = d->playback.internalSampleRate;
    out->capture_ms  = 1000.0 * d->capture.internalPeriodSizeInFrames *
                       d->capture.internalPeriods / (double)d->capture.internalSampleRate;
    out->playback_ms = 1000.0 * d->playback.internalPeriodSizeInFrames *
                       d->playback.internalPeriods / (double)d->playback.internalSampleRate;
    out->fifo_ms = 1000.0 * e->prime_samples / VF_SAMPLE_RATE;
    out->dsp_ms = 1000.0 * pipeline_delay_samples(e->pipe) / VF_SAMPLE_RATE;
    out->total_ms = out->capture_ms + out->playback_ms + out->fifo_ms + out->dsp_ms;
}
