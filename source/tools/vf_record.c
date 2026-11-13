/* vf-record: record a capture device to a WAV file (48 kHz mono float).
 *   vf-record --in "CABLE Output" --seconds 8 out.wav
 * Used by the end-to-end virtual-microphone test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "audio/devices.h"
#include "util/console.h"
#include "util/wavio.h"

typedef struct { float *buf; long len; atomic_long pos; } Rec;

static void cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)output;
    Rec *r = (Rec *)dev->pUserData;
    long p = atomic_load_explicit(&r->pos, memory_order_relaxed);
    const float *in = (const float *)input;
    for (ma_uint32 i = 0; i < frames && p + (long)i < r->len; i++) r->buf[p + i] = in[i];
    atomic_store_explicit(&r->pos, p + frames, memory_order_relaxed);
}

int main(int argc, char **argv)
{
    const char *in_name = "CABLE Output", *out = NULL;
    double seconds = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc) in_name = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else out = argv[i];
    }
    if (!out) { fprintf(stderr, "usage: vf-record [--in NAME] [--seconds S] out.wav\n"); return 2; }

    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 1;
    static VfDeviceList caps;
    vf_devices_enumerate(&ctx, &caps, NULL);
    int ci = vf_devices_find(&caps, in_name);
    if (ci < 0) { fprintf(stderr, "capture device '%s' not found\n", in_name); return 1; }

    Rec r;
    r.len = (long)(seconds * 48000);
    r.buf = (float *)calloc((size_t)r.len, sizeof(float));
    atomic_init(&r.pos, 0);
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.sampleRate = 48000;
    cfg.capture.pDeviceID = &caps.dev[ci].id;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.dataCallback = cb;
    cfg.pUserData = &r;
    ma_device dev;
    if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS || ma_device_start(&dev) != MA_SUCCESS) {
        fprintf(stderr, "cannot open %s\n", caps.dev[ci].name);
        return 1;
    }
    while (atomic_load(&r.pos) < r.len) vf_sleep_ms(20);
    ma_device_uninit(&dev);
    ma_context_uninit(&ctx);
    int rc = vf_wav_write(out, r.buf, r.len, 1, 48000);
    printf("recorded %.1f s from %s -> %s\n", seconds, caps.dev[ci].name, out);
    free(r.buf);
    return rc;
}
