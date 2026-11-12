/* vf-latency: measures real device round-trip latency through a loopback path.
 *
 *   vf-latency [--out "CABLE Input"] [--in "CABLE Output"] [--period N] [--periods M]
 *              [--exclusive] [--seconds S] [--no-fixed] [--pro-audio]
 *
 * A duplex miniaudio device (same configuration style as the mic thingy engine)
 * plays short tone bursts to --out and records --in. With VB-Cable the signal goes
 * CABLE Input -> CABLE Output, so the measured lag = playback buffering + cable +
 * capture buffering (+ miniaudio's duplex coupling). Analysis runs after the
 * stream stops; the callback only copies samples into preallocated buffers.
 * The device is configured by engine_device_config(), i.e. exactly like the
 * engine; --no-fixed / --pro-audio / --periods are the experimental engine knobs
 * (EngineConfig), so this tool is the A/B rig for buffer settings.
 *
 * WARNING: the bursts are audible to every application listening to --in. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "audio/devices.h"
#include "audio/engine.h"
#include "dsp/fx.h"
#include "dsp/pipeline.h"
#include "util/console.h"

#define SR        VF_SAMPLE_RATE
#define BURST     96            /* 2 ms */
#define SPACING   (SR / 2)      /* one burst every 500 ms */
#define MAX_LAG   (SR / 4)      /* search up to 250 ms */

typedef struct {
    float *play, *rec;
    long len;
    atomic_long pos_out, pos_in;
    unsigned min_block, max_block;
} Rig;

static void cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    Rig *r = (Rig *)dev->pUserData;
    float *out = (float *)output;
    const float *in = (const float *)input;
    long po = atomic_load_explicit(&r->pos_out, memory_order_relaxed);
    long pi = atomic_load_explicit(&r->pos_in, memory_order_relaxed);
    if (out) for (ma_uint32 i = 0; i < frames; i++) out[i] = po + (long)i < r->len ? r->play[po + i] : 0.0f;
    if (in)  for (ma_uint32 i = 0; i < frames && pi + (long)i < r->len; i++) r->rec[pi + i] = in[i];
    if (frames < r->min_block) r->min_block = frames;
    if (frames > r->max_block) r->max_block = frames;
    if (out) atomic_store_explicit(&r->pos_out, po + frames, memory_order_relaxed);
    if (in)  atomic_store_explicit(&r->pos_in, pi + frames, memory_order_relaxed);
}

static int cmp_l(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *out_name = "CABLE Input", *in_name = "CABLE Output";
    int period = VF_DEFAULT_PERIOD, periods = 2, exclusive = 0, no_fixed = 0, pro_audio = 0;
    double seconds = 6.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_name = argv[++i];
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) in_name = argv[++i];
        else if (!strcmp(argv[i], "--period") && i + 1 < argc) period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--periods") && i + 1 < argc) periods = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--exclusive")) exclusive = 1;
        else if (!strcmp(argv[i], "--no-fixed")) no_fixed = 1;
        else if (!strcmp(argv[i], "--pro-audio")) pro_audio = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 1;
    static VfDeviceList caps, plays;
    vf_devices_enumerate(&ctx, &caps, &plays);
    int oi = vf_devices_find(&plays, out_name), ii = vf_devices_find(&caps, in_name);
    if (oi < 0 || ii < 0) {
        fprintf(stderr, "device not found (out '%s': %d, in '%s': %d)\n", out_name, oi, in_name, ii);
        return 1;
    }

    Rig r;
    memset(&r, 0, sizeof r);
    r.len = (long)(seconds * SR);
    r.play = (float *)calloc((size_t)r.len, sizeof(float));
    r.rec = (float *)calloc((size_t)r.len, sizeof(float));
    r.min_block = 1u << 30;
    atomic_init(&r.pos_out, 0);
    atomic_init(&r.pos_in, 0);
    /* Hann-windowed 3 kHz bursts, starting after 0.5 s */
    int nb = 0;
    long starts[64];
    for (long s = SPACING; s + SPACING < r.len && nb < 64; s += SPACING) {
        for (int i = 0; i < BURST; i++) {
            double w = 0.5 - 0.5 * cos(2.0 * 3.14159265358979 * i / (BURST - 1));
            r.play[s + i] = (float)(0.5 * w * sin(2.0 * 3.14159265358979 * 3000.0 * i / SR));
        }
        starts[nb++] = s;
    }

    EngineConfig ec;
    memset(&ec, 0, sizeof ec);
    ec.capture_id = &caps.dev[ii].id;
    ec.playback_id = &plays.dev[oi].id;
    ec.period_frames = period;
    ec.periods = periods;
    ec.no_fixed_callback = no_fixed;
    ec.pro_audio = pro_audio;
    ma_device_config cfg;
    engine_device_config(&cfg, ma_device_type_duplex, &ec);
    if (exclusive) {
        cfg.capture.shareMode = ma_share_mode_exclusive;
        cfg.playback.shareMode = ma_share_mode_exclusive;
    }
    cfg.dataCallback = cb;
    cfg.pUserData = &r;

    ma_device dev;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) {
        fprintf(stderr, "cannot open devices (%s mode)\n", exclusive ? "exclusive" : "shared");
        return 1;
    }
    printf("out: %s\nin:  %s\nmode %s, requested period %d x %d\n", plays.dev[oi].name, caps.dev[ii].name,
           exclusive ? "EXCLUSIVE" : "shared", period, periods);
    printf("internal: playback %u x %u @ %u Hz, capture %u x %u @ %u Hz\n",
           dev.playback.internalPeriodSizeInFrames, dev.playback.internalPeriods, dev.playback.internalSampleRate,
           dev.capture.internalPeriodSizeInFrames, dev.capture.internalPeriods, dev.capture.internalSampleRate);
    unsigned cap_buf = dev.capture.internalPeriodSizeInFrames * dev.capture.internalPeriods;
    unsigned play_buf = dev.playback.internalPeriodSizeInFrames * dev.playback.internalPeriods;
#ifdef MA_SUPPORT_WASAPI
    if (ctx.backend == ma_backend_wasapi) {
        if (dev.wasapi.actualBufferSizeInFramesCapture) cap_buf = dev.wasapi.actualBufferSizeInFramesCapture;
        if (dev.wasapi.actualBufferSizeInFramesPlayback) play_buf = dev.wasapi.actualBufferSizeInFramesPlayback;
    }
#endif
    const unsigned cap_period = dev.capture.internalPeriodSizeInFrames;
    const unsigned play_interm = dev.playback.intermediaryBufferCap, cap_interm = dev.capture.intermediaryBufferCap;
    printf("real buffers: capture %u frames, playback %u frames; miniaudio fixed-size buffers: capture %u, playback %u%s%s\n",
           cap_buf, play_buf, cap_interm, play_interm, no_fixed ? "  [--no-fixed]" : "", pro_audio ? "  [--pro-audio]" : "");
    ma_device_start(&dev);
    while (atomic_load(&r.pos_in) < r.len) vf_sleep_ms(20);
    ma_device_stop(&dev);
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    printf("callback blocks: %u..%u frames\n", r.min_block, r.max_block);

    /* per burst: lag that maximises correlation with the template */
    long lags[64];
    int found = 0;
    for (int b = 0; b < nb; b++) {
        long s = starts[b], best = -1;
        double bc = 0, e_t = 0;
        for (int i = 0; i < BURST; i++) e_t += (double)r.play[s + i] * r.play[s + i];
        for (long lag = 0; lag < MAX_LAG && s + lag + BURST < r.len; lag++) {
            double c = 0, e = 0;
            for (int i = 0; i < BURST; i++) {
                double y = r.rec[s + lag + i];
                c += r.play[s + i] * y; e += y * y;
            }
            c /= sqrt(e_t * e + 1e-20);
            if (c > bc) { bc = c; best = lag; }
        }
        if (best >= 0 && bc > 0.8) lags[found++] = best;
    }
    if (found == 0) {
        printf("no bursts detected - is anything muted, or is the cable at a different rate?\n");
        return 1;
    }
    qsort(lags, (size_t)found, sizeof lags[0], cmp_l);
    printf("detected %d/%d bursts: round trip min %.2f / median %.2f / max %.2f ms\n", found, nb,
           1000.0 * lags[0] / SR, 1000.0 * lags[found / 2] / SR, 1000.0 * lags[found - 1] / SR);
    /* what our side of the loop accounts for; the rest is the cable + the Windows audio engine */
    const double ours = 1000.0 * (cap_period + play_buf + play_interm) / SR;
    const double med = 1000.0 * lags[found / 2] / SR;
    printf("breakdown (median): capture period %.1f + playback buffer %.1f + miniaudio %.1f = %.1f ms ours, "
           "%.1f ms cable + OS (unattributed)\n",
           1000.0 * cap_period / SR, 1000.0 * play_buf / SR, 1000.0 * play_interm / SR, ours, med - ours);
    printf("RESULT,%s,%d,%d,%s%s,%.2f,%.2f,%.2f,%.2f\n", exclusive ? "excl" : "shared", period, periods,
           no_fixed ? "nofixed" : "fixed", pro_audio ? "+pro" : "",
           1000.0 * lags[0] / SR, med, 1000.0 * lags[found - 1] / SR, med - ours);
    free(r.play);
    free(r.rec);
    return 0;
}
