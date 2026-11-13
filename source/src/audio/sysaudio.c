#include "audio/sysaudio.h"

#include <string.h>
#include "dsp/fx.h"

#define SYS_SINK_TARGET 960

static void sys_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)output;
    VfSysAudio *s = (VfSysAudio *)dev->pUserData;
    if (input) {
        vf_sink_write(&s->sink, (const float *)input, (int)frames);
        vf_sink_write(&s->ref, (const float *)input, (int)frames);
    }
}

void vf_sys_init(VfSysAudio *s)
{
    memset(s, 0, sizeof *s);
    atomic_init(&s->running, 0);
    s->ref_ready = vf_sink_init(&s->ref, 480) == 0;      /* allocated early: the echo canceller reads it from the start */
}

int vf_sys_start(VfSysAudio *s, ma_context *ctx, const ma_device_id *playback_id)
{
    if (atomic_load(&s->running)) return 0;
    if (!ctx || ctx->backend != ma_backend_wasapi) return -1;       /* loopback exists on WASAPI only */
    if (!s->sink_ready) {
        if (vf_sink_init(&s->sink, SYS_SINK_TARGET) != 0) return -1;
        s->sink_ready = 1;
    }
    vf_sink_flush(&s->sink);
    vf_sink_flush(&s->ref);
    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.sampleRate = VF_SAMPLE_RATE;
    cfg.capture.pDeviceID = playback_id;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.dataCallback = sys_cb;
    cfg.pUserData = s;
    if (ma_device_init(ctx, &cfg, &s->dev) != MA_SUCCESS) return -1;
    if (ma_device_start(&s->dev) != MA_SUCCESS) { ma_device_uninit(&s->dev); return -1; }
    atomic_store(&s->running, 1);
    return 0;
}

void vf_sys_stop(VfSysAudio *s)
{
    if (!atomic_load(&s->running)) return;
    atomic_store(&s->running, 0);
    ma_device_uninit(&s->dev);
}

void vf_sys_free(VfSysAudio *s)
{
    vf_sys_stop(s);
    if (s->sink_ready) { vf_sink_free(&s->sink); s->sink_ready = 0; }
    if (s->ref_ready) { vf_sink_free(&s->ref); s->ref_ready = 0; }
}

void vf_sys_read(void *ctx, float *dst, int n)
{
    VfSysAudio *s = (VfSysAudio *)ctx;
    if (atomic_load_explicit(&s->running, memory_order_acquire)) vf_sink_read(&s->sink, dst, n);
    else memset(dst, 0, (size_t)n * sizeof *dst);
}
