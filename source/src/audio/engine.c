#include "audio/engine.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#define VF_HAVE_SSE 1
#endif

/* ---- audio thread: no malloc, no locks, no I/O ---- */

int vf_capture_init(VfCapture *c, long len)
{
    memset(c, 0, sizeof *c);
    c->raw = (float *)calloc((size_t)len, sizeof(float));
    c->proc = (float *)calloc((size_t)len, sizeof(float));
    if (!c->raw || !c->proc) { vf_capture_free(c); return -1; }
    c->len = len;
    atomic_init(&c->pos, 0);
    atomic_init(&c->active, 0);
    return 0;
}

void vf_capture_free(VfCapture *c)
{
    free(c->raw); free(c->proc);
    memset(c, 0, sizeof *c);
}

void vf_capture_start(VfCapture *c)
{
    atomic_store(&c->pos, 0);
    atomic_store(&c->active, 1);
}

static void capture_push(VfCapture *c, const float *raw, const float *proc, int n)
{
    if (!atomic_load_explicit(&c->active, memory_order_acquire)) return;
    const long pos = atomic_load_explicit(&c->pos, memory_order_relaxed);
    if (pos + n > c->len) { atomic_store(&c->active, 0); return; }
    memcpy(c->raw + pos, raw, (size_t)n * sizeof(float));
    memcpy(c->proc + pos, proc, (size_t)n * sizeof(float));
    atomic_store_explicit(&c->pos, pos + n, memory_order_release);
}

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
     * (0 for B = 480, up to 479 otherwise.) If block sizes vary later, a starved
     * callback outputs silence without consuming, which raises the buffer level
     * permanently - a one-off glitch, then stable. */
    if (!e->started) {
        if (out) {
            unsigned a = frames, b = VF_FRAME_SIZE;
            while (b) { unsigned t = a % b; a = b; b = t; }
            e->prime_samples = a ? VF_FRAME_SIZE - (int)a : 0;
        }
        e->out_head = 0;
        e->out_count = e->prime_samples;      /* out_fifo is zeroed at init */
        e->started = 1;
    }

    /* diagnostics: callback block sizes (relaxed atomics, single writer) */
    atomic_store_explicit(&e->blk_last, frames, memory_order_relaxed);
    if (frames < atomic_load_explicit(&e->blk_min, memory_order_relaxed))
        atomic_store_explicit(&e->blk_min, frames, memory_order_relaxed);
    if (frames > atomic_load_explicit(&e->blk_max, memory_order_relaxed))
        atomic_store_explicit(&e->blk_max, frames, memory_order_relaxed);

    /* 1) capture -> accumulator -> pipeline -> taps + output FIFO */
    for (unsigned i = 0; i < frames; i++) {
        e->in_acc[e->in_count++] = in[i];
        if (e->in_count == VF_FRAME_SIZE) {
            e->in_count = 0;
            float raw[VF_FRAME_SIZE];
            memcpy(raw, e->in_acc, sizeof raw);
            if (e->pre_tap && atomic_load_explicit(&e->pre_tap_on, memory_order_relaxed))
                vf_sink_write(e->pre_tap, raw, VF_FRAME_SIZE);
            pipeline_process(e->pipe, e->in_acc);
            VfCapture *cap = atomic_load_explicit(&e->capture, memory_order_acquire);
            if (cap) capture_push(cap, raw, e->in_acc, VF_FRAME_SIZE);
            VfRing *rr = atomic_load_explicit(&e->rec_ring, memory_order_acquire);
            if (rr) {
                float st[2 * VF_FRAME_SIZE];
                for (int k = 0; k < VF_FRAME_SIZE; k++) { st[2 * k] = raw[k]; st[2 * k + 1] = e->in_acc[k]; }
                if (vf_ring_write(rr, st, 2 * VF_FRAME_SIZE) == 0) atomic_fetch_add_explicit(&e->rec_dropped, 1, memory_order_relaxed);
            }
            for (int t = 0; t < e->ntaps; t++)
                vf_sink_write(e->taps[t], e->in_acc, VF_FRAME_SIZE);
            if (!out) continue;
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
    if (!out) return;
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
    /* FIFO fill left over = extra delay the re-chunking adds right now */
    if (e->out_count < atomic_load_explicit(&e->fifo_min, memory_order_relaxed))
        atomic_store_explicit(&e->fifo_min, e->out_count, memory_order_relaxed);
    if (e->out_count > atomic_load_explicit(&e->fifo_max, memory_order_relaxed))
        atomic_store_explicit(&e->fifo_max, e->out_count, memory_order_relaxed);
}

static void data_callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    Engine *e = (Engine *)dev->pUserData;
    if (!e->file_src) {
        engine_process_block(e, (float *)output, (const float *)input, frames);
        return;
    }
    /* file source: feed looped file samples in place of the microphone */
    float *out = (float *)output;
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        if (n > VF_SCRATCH) n = VF_SCRATCH;
        for (ma_uint32 i = 0; i < n; i++) {
            e->scratch[i] = e->file_src[e->file_pos];
            if (++e->file_pos >= e->file_len) e->file_pos = 0;
        }
        engine_process_block(e, out ? out + done : NULL, e->scratch, n);
        done += n;
    }
}

/* ---- control thread ---- */

void engine_init_offline(Engine *e, Pipeline *pipe)
{
    memset(e, 0, sizeof *e);
    e->pipe = pipe;
    atomic_init(&e->underruns, 0);
    atomic_init(&e->overruns, 0);
    atomic_init(&e->pre_tap_on, 0);
    atomic_init(&e->capture, NULL);
    atomic_init(&e->rec_ring, NULL);
    atomic_init(&e->rec_dropped, 0);
    atomic_init(&e->blk_min, UINT_MAX);
    atomic_init(&e->blk_max, 0);
    atomic_init(&e->blk_last, 0);
    atomic_init(&e->fifo_min, INT_MAX);
    atomic_init(&e->fifo_max, 0);
}

void engine_device_config(ma_device_config *cfg, ma_device_type type, const EngineConfig *c)
{
    *cfg = ma_device_config_init(type);
    cfg->sampleRate = VF_SAMPLE_RATE;
    cfg->periodSizeInFrames = (ma_uint32)(c->period_frames > 0 ? c->period_frames : VF_DEFAULT_PERIOD);
    cfg->periods = 2;
    cfg->performanceProfile = ma_performance_profile_low_latency;
    cfg->capture.pDeviceID = c->capture_id;
    cfg->capture.format = ma_format_f32;
    cfg->capture.channels = 1;
    cfg->playback.pDeviceID = c->playback_id;
    cfg->playback.format = ma_format_f32;
    cfg->playback.channels = 1;
    /* experimental knobs, off by default */
    /* variable-size callbacks: the engine re-chunks anyway, and miniaudio then skips its
     * fixed-size intermediary buffers (~5 ms, L1 measurement in CLAUDE.md) */
    cfg->noFixedSizedCallback = c->fixed_callback ? MA_FALSE : MA_TRUE;
    cfg->noPreSilencedOutputBuffer = MA_TRUE;   /* the callback writes every output sample */
    if (c->pro_audio) cfg->wasapi.usage = ma_wasapi_usage_pro_audio;
    if (c->periods > 0) cfg->periods = (ma_uint32)c->periods;
}

int engine_init(Engine *e, ma_context *ctx, Pipeline *pipe, const EngineConfig *c)
{
    engine_init_offline(e, pipe);
    e->file_src = c->file_src;
    e->file_len = c->file_len;
    if (c->file_src && (c->file_len <= 0 || c->no_playback)) return -1;

    ma_device_type type = c->file_src ? ma_device_type_playback
                        : c->no_playback ? ma_device_type_capture : ma_device_type_duplex;
    ma_device_config cfg;
    engine_device_config(&cfg, type, c);
    cfg.dataCallback = data_callback;
    cfg.pUserData = e;

    if (ma_device_init(ctx, &cfg, &e->dev) != MA_SUCCESS) return -1;
    e->device_ready = 1;
    /* the mixer rate the device really runs at (WASAPI shared: GetMixFormat = first native
     * format). miniaudio asks Windows to convert, so internalSampleRate always reads 48000. */
    ma_device_info info;
    const int wasapi = ctx && ctx->backend == ma_backend_wasapi;   /* elsewhere format 0 is not the mixer */
    if (wasapi && type != ma_device_type_playback && ma_device_get_info(&e->dev, ma_device_type_capture, &info) == MA_SUCCESS &&
        info.nativeDataFormatCount > 0)
        e->cap_mix_rate = info.nativeDataFormats[0].sampleRate;
    if (wasapi && type != ma_device_type_capture && ma_device_get_info(&e->dev, ma_device_type_playback, &info) == MA_SUCCESS &&
        info.nativeDataFormatCount > 0)
        e->play_mix_rate = info.nativeDataFormats[0].sampleRate;
    return 0;
}

int engine_add_tap(Engine *e, VfSink *sink)
{
    if (e->ntaps >= VF_MAX_TAPS) return -1;
    e->taps[e->ntaps++] = sink;
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

/* Estimate of the delay the engine and the device buffers add (UI thread; the
 * device fields are fixed after ma_device_init, the counters are relaxed atomics).
 * Capture: a sample waits for one capture period. Playback: in WASAPI shared mode
 * the stream keeps its buffer (GetBufferSize) nearly full - internalPeriods x
 * internalPeriodSizeInFrames is NOT that buffer under IAudioClient3 (see miniaudio).
 * miniaudio's fixed-size callback adds a playback intermediary of up to one
 * callback; the capture intermediary overlaps with the capture period. */
void engine_latency(const Engine *e, EngineLatency *out)
{
    const ma_device *d = &e->dev;
    memset(out, 0, sizeof *out);
    const int has_cap = e->device_ready && (d->type == ma_device_type_capture || d->type == ma_device_type_duplex);
    const int has_play = e->device_ready && (d->type == ma_device_type_playback || d->type == ma_device_type_duplex);
    if (has_cap) {
        out->cap_period = d->capture.internalPeriodSizeInFrames;
        out->cap_periods = d->capture.internalPeriods;
        out->cap_buffer = out->cap_period * out->cap_periods;
        out->cap_rate = d->capture.internalSampleRate;
        out->cap_interm = d->capture.intermediaryBufferCap;
    }
    if (has_play) {
        out->play_period = d->playback.internalPeriodSizeInFrames;
        out->play_periods = d->playback.internalPeriods;
        out->play_buffer = out->play_period * out->play_periods;
        out->play_rate = d->playback.internalSampleRate;
        out->play_interm = d->playback.intermediaryBufferCap;
    }
#ifdef MA_SUPPORT_WASAPI
    if (e->device_ready && d->pContext && d->pContext->backend == ma_backend_wasapi) {
        out->wasapi = 1;
        if (has_cap && d->wasapi.actualBufferSizeInFramesCapture) out->cap_buffer = d->wasapi.actualBufferSizeInFramesCapture;
        if (has_play && d->wasapi.actualBufferSizeInFramesPlayback) out->play_buffer = d->wasapi.actualBufferSizeInFramesPlayback;
    }
#endif
    out->blk_min = atomic_load_explicit(&e->blk_min, memory_order_relaxed);
    out->blk_max = atomic_load_explicit(&e->blk_max, memory_order_relaxed);
    out->blk_last = atomic_load_explicit(&e->blk_last, memory_order_relaxed);
    if (out->blk_min == UINT_MAX) out->blk_min = 0;
    out->fifo_min = atomic_load_explicit(&e->fifo_min, memory_order_relaxed);
    out->fifo_max = atomic_load_explicit(&e->fifo_max, memory_order_relaxed);
    if (out->fifo_min == INT_MAX) out->fifo_min = 0;
    out->prime = e->prime_samples;
    out->cap_mix_rate = has_cap ? e->cap_mix_rate : 0;
    out->play_mix_rate = has_play ? e->play_mix_rate : 0;

    if (out->cap_rate) out->capture_ms = 1000.0 * out->cap_period / out->cap_rate;
    if (out->play_rate) {
        out->playback_ms = 1000.0 * out->play_buffer / out->play_rate;
        out->interm_ms = 1000.0 * out->play_interm / out->play_rate;
    }
    /* before the first callbacks the FIFO stats are empty: fall back to the pre-fill */
    const int fifo = out->blk_max ? out->fifo_max : out->prime;
    out->fifo_ms = has_play ? 1000.0 * fifo / VF_SAMPLE_RATE : 0.0;
    out->dsp_ms = 1000.0 * pipeline_delay_samples(e->pipe) / VF_SAMPLE_RATE;
    out->total_ms = out->capture_ms + out->playback_ms + out->interm_ms + out->fifo_ms + out->dsp_ms;
}

void engine_latency_report(const Engine *e, char *buf, int size)
{
    EngineLatency l;
    engine_latency(e, &l);
    const size_t n = (size_t)(size > 0 ? size : 0);
    snprintf(buf, n,
             "  capture : period %u x %u, buffer %u frames, mixer %u Hz -> ~%.1f ms%s\n"
             "  playback: period %u x %u, buffer %u frames, mixer %u Hz -> ~%.1f ms%s\n"
             "  miniaudio fixed-size callback buffers: capture %u, playback %u frames -> ~%.1f ms\n"
             "  callback blocks %u..%u (last %u) frames, FIFO fill %d..%d (pre-fill %d) -> ~%.1f ms\n"
             "  DSP %.1f ms  =>  total ~%.1f ms%s\n",
             l.cap_period, l.cap_periods, l.cap_buffer, l.cap_mix_rate, l.capture_ms,
             l.cap_mix_rate && l.cap_mix_rate != VF_SAMPLE_RATE ? "  (not 48 kHz: Windows resamples)" : "",
             l.play_period, l.play_periods, l.play_buffer, l.play_mix_rate, l.playback_ms,
             l.play_mix_rate && l.play_mix_rate != VF_SAMPLE_RATE ? "  (not 48 kHz: Windows resamples)" : "",
             l.cap_interm, l.play_interm, l.interm_ms,
             l.blk_min, l.blk_max, l.blk_last, l.fifo_min, l.fifo_max, l.prime, l.fifo_ms,
             l.dsp_ms, l.total_ms, l.wasapi ? "  (buffers from WASAPI GetBufferSize)" : "");
}
