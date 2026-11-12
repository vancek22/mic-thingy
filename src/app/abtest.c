#include "app/abtest.h"

#include <math.h>
#include <string.h>
#include "dsp/fx.h"
#include "util/console.h"

void vf_ab_init(VfAbTest *a)
{
    memset(a, 0, sizeof *a);
    atomic_init(&a->state, VF_AB_IDLE);
    atomic_init(&a->ppos, 0);
    atomic_init(&a->which, VF_AB_ALTERNATE);
    atomic_init(&a->match, 1);
    a->proc_gain = 1.0f;
}

int vf_ab_record(VfAbTest *a, Engine *e, int seconds)
{
    if (!e || atomic_load(&a->state) == VF_AB_RECORDING) return -1;
    vf_ab_stop(a);
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;
    if (a->cap_ready) { atomic_store(&e->capture, NULL); vf_sleep_ms(30); vf_capture_free(&a->cap); a->cap_ready = 0; }
    if (vf_capture_init(&a->cap, (long)seconds * VF_SAMPLE_RATE) != 0) return -1;
    a->cap_ready = 1;
    a->len = 0;
    vf_capture_start(&a->cap);
    atomic_store(&e->capture, &a->cap);
    atomic_store(&a->state, VF_AB_RECORDING);
    return 0;
}

static double rms(const float *x, long n)
{
    double e = 0;
    for (long i = 0; i < n; i++) e += (double)x[i] * x[i];
    return n > 0 ? sqrt(e / (double)n) : 0.0;
}

VfAbState vf_ab_update(VfAbTest *a, Engine *e)
{
    if (atomic_load(&a->state) == VF_AB_RECORDING && !atomic_load(&a->cap.active)) {
        if (e) atomic_store(&e->capture, NULL);
        vf_sleep_ms(30);                                   /* a callback may still hold the old pointer */
        a->len = atomic_load(&a->cap.pos);
        if (a->len < VF_FRAME_SIZE) { atomic_store(&a->state, VF_AB_IDLE); return VF_AB_IDLE; }
        const double rr = rms(a->cap.raw, a->len), rp = rms(a->cap.proc, a->len);
        a->proc_gain = rp > 1e-6 ? (float)(rr / rp) : 1.0f;
        if (a->proc_gain > 4.0f) a->proc_gain = 4.0f;
        if (a->proc_gain < 0.25f) a->proc_gain = 0.25f;
        atomic_store(&a->state, VF_AB_READY);
    }
    if (atomic_load(&a->state) == VF_AB_PLAYING && atomic_load(&a->ppos) >= a->len && a->which != VF_AB_ALTERNATE) {
        /* single pass finished: the callback sets READY itself */
    }
    return (VfAbState)atomic_load(&a->state);
}

long vf_ab_render(const VfAbTest *a, long pos, float *out, long n)
{
    const int which = atomic_load((atomic_int *)&a->which);
    const float g = atomic_load((atomic_int *)&a->match) ? a->proc_gain : 1.0f;
    long got = 0;
    for (long i = 0; i < n; i++) {
        const long p = pos + i;
        if (p >= a->len) { out[i] = 0.0f; continue; }
        int use_proc = which == VF_AB_FILTERED;
        if (which == VF_AB_ALTERNATE) use_proc = (p / ((long)VF_AB_SEGMENT_S * VF_SAMPLE_RATE)) & 1;   /* raw first */
        out[i] = use_proc ? a->cap.proc[p] * g : a->cap.raw[p];
        got++;
    }
    return got;
}

static void play_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    VfAbTest *a = (VfAbTest *)dev->pUserData;
    float *out = (float *)output;
    const long pos = atomic_load_explicit(&a->ppos, memory_order_relaxed);
    const long got = vf_ab_render(a, pos, out, (long)frames);
    atomic_store_explicit(&a->ppos, pos + (long)frames, memory_order_relaxed);
    if (got < (long)frames) atomic_store(&a->state, VF_AB_READY);     /* reached the end */
}

int vf_ab_play(VfAbTest *a, ma_context *ctx, const ma_device_id *playback_id, VfAbWhich which)
{
    if (atomic_load(&a->state) != VF_AB_READY && atomic_load(&a->state) != VF_AB_PLAYING) return -1;
    vf_ab_stop(a);
    atomic_store(&a->which, which);
    atomic_store(&a->ppos, 0);
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.sampleRate = VF_SAMPLE_RATE;
    cfg.playback.pDeviceID = playback_id;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.dataCallback = play_cb;
    cfg.pUserData = a;
    if (ma_device_init(ctx, &cfg, &a->dev) != MA_SUCCESS) return -1;
    a->dev_ready = 1;
    atomic_store(&a->state, VF_AB_PLAYING);
    if (ma_device_start(&a->dev) != MA_SUCCESS) { vf_ab_stop(a); return -1; }
    return 0;
}

void vf_ab_stop(VfAbTest *a)
{
    if (a->dev_ready) { ma_device_uninit(&a->dev); a->dev_ready = 0; }
    if (atomic_load(&a->state) == VF_AB_PLAYING) atomic_store(&a->state, VF_AB_READY);
}

void vf_ab_free(VfAbTest *a, Engine *e)
{
    vf_ab_stop(a);
    if (e) atomic_store(&e->capture, NULL);
    if (a->cap_ready) { vf_sleep_ms(30); vf_capture_free(&a->cap); a->cap_ready = 0; }
    atomic_store(&a->state, VF_AB_IDLE);
    a->len = 0;
}

double vf_ab_seconds(const VfAbTest *a)
{
    if (atomic_load((atomic_int *)&a->state) == VF_AB_RECORDING) return (double)atomic_load((atomic_long *)&a->cap.pos) / VF_SAMPLE_RATE;
    return (double)a->len / VF_SAMPLE_RATE;
}

double vf_ab_play_seconds(const VfAbTest *a)
{
    const long p = atomic_load((atomic_long *)&a->ppos);
    return (double)(p < a->len ? p : a->len) / VF_SAMPLE_RATE;
}
