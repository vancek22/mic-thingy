#include "audio/monitor.h"

#include <string.h>
#include "dsp/fx.h"

static void monitor_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    VfMonitor *m = (VfMonitor *)dev->pUserData;
    float *out = (float *)output;
    vf_sink_read(atomic_load_explicit(&m->sink, memory_order_acquire), out, (int)frames);   /* lock-free, no allocation */
    const float g = atomic_load_explicit(&m->gain, memory_order_relaxed);
    if (g != 1.0f) for (ma_uint32 i = 0; i < frames; i++) out[i] *= g;
}

int vf_monitor_init(VfMonitor *m, ma_context *ctx, const ma_device_id *id, VfSink *sink)
{
    memset(m, 0, sizeof *m);
    atomic_init(&m->sink, sink);
    atomic_init(&m->gain, 1.0f);
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.sampleRate = VF_SAMPLE_RATE;
    cfg.periodSizeInFrames = VF_FRAME_SIZE / 2;
    cfg.periods = 2;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.noFixedSizedCallback = MA_TRUE;     /* the sink serves any block size: no extra buffer */
    cfg.playback.pDeviceID = id;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.dataCallback = monitor_cb;
    cfg.pUserData = m;
    if (ma_device_init(ctx, &cfg, &m->dev) != MA_SUCCESS) return -1;
    m->ready = 1;
    return 0;
}

int vf_monitor_start(VfMonitor *m)
{
    return ma_device_start(&m->dev) == MA_SUCCESS ? 0 : -1;
}

void vf_monitor_uninit(VfMonitor *m)
{
    if (m->ready) { ma_device_uninit(&m->dev); m->ready = 0; }
}

double vf_monitor_buffer_ms(const VfMonitor *m)
{
    return 1000.0 * m->dev.playback.internalPeriodSizeInFrames * m->dev.playback.internalPeriods /
           (double)m->dev.playback.internalSampleRate;
}
