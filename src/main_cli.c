/* VoiceFilter console front-end (stage 1: mic -> RNNoise -> headphones). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/devices.h"
#include "audio/engine.h"
#include "dsp/fx_rnnoise.h"
#include "dsp/pipeline.h"
#include "util/console.h"

static void print_list(const char *title, const VfDeviceList *l)
{
    printf("%s\n", title);
    for (int i = 0; i < l->count; i++)
        printf("  [%d] %s%s\n", i, l->dev[i].name, l->dev[i].is_default ? "  (default)" : "");
}

/* Returns index, or -1 for "system default". */
static int choose(const char *what, const VfDeviceList *l, int preset)
{
    if (preset >= 0) return preset < l->count ? preset : -1;
    char line[64];
    printf("%s - number (Enter = default): ", what);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) || line[0] == '\n') return -1;
    int v = atoi(line);
    return (v >= 0 && v < l->count) ? v : -1;
}

static float to_db(float x) { return x > 1e-5f ? 20.0f * log10f(x) : -100.0f; }

static void meter(char *dst, size_t n, float db)   /* -60..0 dB over 20 cells */
{
    int cells = (int)((db + 60.0f) / 3.0f);
    if (cells < 0) cells = 0;
    if (cells > 20) cells = 20;
    size_t i = 0;
    for (; i < (size_t)cells && i + 1 < n; i++) dst[i] = '#';
    for (; i < 20 && i + 1 < n; i++) dst[i] = '.';
    dst[i < n ? i : n - 1] = '\0';
}

int main(int argc, char **argv)
{
    int pre_in = -1, pre_out = -1;
    for (int i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--in"))  pre_in  = atoi(argv[i + 1]);
        if (!strcmp(argv[i], "--out")) pre_out = atoi(argv[i + 1]);
    }

    vf_console_init();

    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
        fprintf(stderr, "Cannot initialise audio backend.\n");
        return 1;
    }
    printf("VoiceFilter - backend: %s\n\n", ma_get_backend_name(ctx.backend));

    static VfDeviceList caps, plays;
    if (vf_devices_enumerate(&ctx, &caps, &plays) != 0) {
        fprintf(stderr, "Device enumeration failed.\n");
        ma_context_uninit(&ctx);
        return 1;
    }
    print_list("Inputs (microphones):", &caps);
    printf("\n");
    print_list("Outputs (playback):", &plays);
    printf("\nUse HEADPHONES - speakers will feed back into the microphone.\n\n");

    int ci = choose("Input", &caps, pre_in);
    int pi = choose("Output", &plays, pre_out);

    Fx rn;
    fx_rnnoise_make(&rn);
    Pipeline pipe;
    pipeline_init(&pipe);
    pipeline_add(&pipe, &rn);
    if (pipeline_start(&pipe, VF_SAMPLE_RATE) != 0) {
        fprintf(stderr, "Pipeline init failed.\n");
        ma_context_uninit(&ctx);
        return 1;
    }

    static Engine eng;
    if (engine_init(&eng, &ctx, &pipe,
                    ci >= 0 ? &caps.dev[ci].id : NULL,
                    pi >= 0 ? &plays.dev[pi].id : NULL) != 0 ||
        engine_start(&eng) != 0) {
        fprintf(stderr, "Cannot open/start audio devices.\n");
        engine_uninit(&eng);
        pipeline_stop(&pipe);
        ma_context_uninit(&ctx);
        return 1;
    }

    EngineLatency lat;
    engine_latency(&eng, &lat);
    printf("\nRunning @ %d Hz. Estimated latency: capture buf %.1f ms + playback buf %.1f ms"
           " + frame %.1f ms = ~%.1f ms%s\n",
           VF_SAMPLE_RATE, lat.capture_ms, lat.playback_ms, lat.frame_ms, lat.total_ms,
           lat.total_ms > 20.0 ? "  (above the 20 ms target)" : "");
    if (lat.capture_rate != VF_SAMPLE_RATE || lat.playback_rate != VF_SAMPLE_RATE)
        printf("Note: device runs at %u/%u Hz, resampling adds some delay.\n",
               lat.capture_rate, lat.playback_rate);
    printf("Keys:  b = bypass on/off   q = quit\n\n");

    int running = 1;
    while (running) {
        int k;
        while ((k = vf_console_getkey()) != -1) {
            if (k == 'q' || k == 'Q' || k == 27) running = 0;
            if (k == 'b' || k == 'B')
                atomic_store(&pipe.bypass_all, !atomic_load(&pipe.bypass_all));
        }
        float pin = atomic_load_explicit(&pipe.peak_in, memory_order_relaxed);
        float pout = atomic_load_explicit(&pipe.peak_out, memory_order_relaxed);
        float vad = atomic_load_explicit(&pipe.vad, memory_order_relaxed);
        char mi[24], mo[24];
        meter(mi, sizeof mi, to_db(pin));
        meter(mo, sizeof mo, to_db(pout));
        printf("\r%s | IN %s | OUT %s | VAD %3d%% | underruns %u   ",
               atomic_load(&pipe.bypass_all) ? "BYPASS" : "FILTER", mi, mo,
               (int)(vad * 100.0f + 0.5f), atomic_load(&eng.underruns));
        fflush(stdout);
        vf_sleep_ms(50);
    }

    printf("\n");
    engine_stop(&eng);
    engine_uninit(&eng);
    pipeline_stop(&pipe);
    ma_context_uninit(&ctx);
    vf_console_restore();
    return 0;
}
