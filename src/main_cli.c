/* VoiceFilter console front-end.
 *
 *   voicefilter-cli [--in N] [--out N] [--vmic] [--monitor N] [--period N]
 *                   [--in-file f.wav] [--seconds S] [-c "cmd"]...
 *
 * Output modes:
 *   headphones          mic -> chain -> chosen playback device
 *   virtual mic         Windows: mic -> chain -> "CABLE Input" (apps pick "CABLE Output")
 *                       Linux:   mic -> chain -> PipeWire source "VoiceFilter Microphone"
 *   virtual mic+monitor as above, plus the processed voice on headphones */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/devices.h"
#include "audio/engine.h"
#include "audio/monitor.h"
#include "audio/sink.h"
#include "audio/vmic.h"
#include "dsp/chain.h"
#include "dsp/pipeline.h"
#include "util/cmdline.h"
#include "util/console.h"
#include "util/wavio.h"

#define MAX_CMDS 16
#define SINK_TARGET 960      /* 20 ms cushion between independent audio clocks */

enum { MODE_ASK = 0, MODE_HEADPHONES, MODE_VMIC, MODE_VMIC_MONITOR };

static void print_list(const char *title, const VfDeviceList *l)
{
    printf("%s\n", title);
    for (int i = 0; i < l->count; i++)
        printf("  [%d] %s%s\n", i, l->dev[i].name, l->dev[i].is_default ? "  (default)" : "");
}

/* Returns index, -1 for "system default" (if allowed), or -2 on bad input. */
static int choose(const char *what, const VfDeviceList *l, int allow_default)
{
    char line[64];
    printf("%s - number%s: ", what, allow_default ? " (Enter = default)" : "");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return -2;
    if (line[0] == '\n') return allow_default ? -1 : -2;
    int v = atoi(line);
    return (v >= 0 && v < l->count) ? v : -2;
}

static int ask_number(const char *prompt, int lo, int hi, int def)
{
    char line[32];
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin) || line[0] == '\n') return def;
    int v = atoi(line);
    return (v >= lo && v <= hi) ? v : def;
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

#define contains_ci vf_contains_ci

static void usage(void)
{
    printf("usage: voicefilter-cli [--in N] [--out N] [--vmic] [--monitor N] [--period N]\n"
           "                       [--in-file f.wav] [--seconds S] [-c \"cmd\"]...\n"
           "  --vmic        output to the virtual microphone (Windows: VB-Cable, Linux: PipeWire)\n"
           "  --monitor N   also play the processed voice on playback device N\n"
           "  --in-file F   use a sound file (looped) instead of the microphone (testing)\n"
           "  --seconds S   quit automatically after S seconds\n"
           "  -c CMD        run a console command at start, e.g. -c \"set comp ratio 4\"\n");
}

int main(int argc, char **argv)
{
    int pre_in = -1, pre_out = -1, pre_mon = -1, period = 0, mode = MODE_ASK, ncmd = 0;
    double seconds = 0;
    const char *in_file = NULL, *cmds[MAX_CMDS];
    for (int i = 1; i < argc; i++) {
        const int more = i + 1 < argc;
        if (!strcmp(argv[i], "--in") && more) pre_in = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && more) { pre_out = atoi(argv[++i]); if (!mode) mode = MODE_HEADPHONES; }
        else if (!strcmp(argv[i], "--vmic")) mode = (mode == MODE_VMIC_MONITOR) ? mode : MODE_VMIC;
        else if (!strcmp(argv[i], "--monitor") && more) { pre_mon = atoi(argv[++i]); mode = MODE_VMIC_MONITOR; }
        else if (!strcmp(argv[i], "--period") && more) period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-file") && more) in_file = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c") && more && ncmd < MAX_CMDS) cmds[ncmd++] = argv[++i];
        else { usage(); return 2; }
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
    const int interactive = (mode == MODE_ASK);
    if (interactive || pre_in < 0) {
        print_list("Inputs (microphones):", &caps);
        printf("\n");
        print_list("Outputs (playback):", &plays);
        printf("\n");
    }

    /* ---- input ---- */
    float *file_buf = NULL;
    long file_len = 0;
    int ci = -1;
    if (in_file) {
        file_buf = vf_wav_read_mono(in_file, VF_SAMPLE_RATE, &file_len);
        if (!file_buf || file_len < VF_FRAME_SIZE) { fprintf(stderr, "Cannot read %s\n", in_file); return 1; }
        printf("Input: file %s (%.1f s, looped)\n", in_file, (double)file_len / VF_SAMPLE_RATE);
    } else {
        ci = pre_in >= 0 ? (pre_in < caps.count ? pre_in : -2) : choose("Input (microphone)", &caps, 1);
        if (ci == -2) { fprintf(stderr, "Invalid input device.\n"); return 1; }
    }

    /* ---- output mode ---- */
    const int vmic_native = vf_vmic_supported();
    if (mode == MODE_ASK) {
        printf("\nOutput:\n  [1] headphones/speakers only\n"
               "  [2] virtual microphone (%s)\n  [3] virtual microphone + listen on headphones\n",
               vmic_native ? "PipeWire source \"" VF_VMIC_DESCRIPTION "\"" : "VB-Cable: apps select \"CABLE Output\"");
        mode = ask_number("Choice (Enter = 2): ", 1, 3, 2);
    }

    int pi = -1, mi = -1;
    if (mode == MODE_HEADPHONES) {
        pi = pre_out >= 0 ? (pre_out < plays.count ? pre_out : -2) : choose("Output", &plays, 1);
        if (pi == -2) { fprintf(stderr, "Invalid output device.\n"); return 1; }
    } else if (!vmic_native) {
        pi = vf_devices_find(&plays, "CABLE Input");
        if (pi < 0) {
            fprintf(stderr, "\"CABLE Input\" not found. Install VB-Cable (vb-audio.com/Cable) or use --out N.\n");
            return 1;
        }
    }
    if (mode == MODE_VMIC_MONITOR) {
        mi = pre_mon >= 0 ? (pre_mon < plays.count ? pre_mon : -2) : choose("Monitor (headphones)", &plays, 0);
        if (mi < 0) { fprintf(stderr, "Invalid monitor device.\n"); return 1; }
        if (mi == pi) { fprintf(stderr, "Monitor must be a different device than the main output.\n"); return 1; }
    }

    /* feedback-loop guard: never read the cable / our own source while writing to it */
    if (mode != MODE_HEADPHONES && ci >= 0 &&
        (contains_ci(caps.dev[ci].name, "CABLE Output") || contains_ci(caps.dev[ci].name, "VoiceFilter"))) {
        fprintf(stderr, "Input \"%s\" is the virtual microphone itself - that would loop the output back"
                        " into the input. Choose your real microphone.\n", caps.dev[ci].name);
        return 1;
    }
    if (mode != MODE_HEADPHONES && ci < 0 && !in_file) {
        for (int i = 0; i < caps.count; i++)
            if (caps.dev[i].is_default && (contains_ci(caps.dev[i].name, "CABLE Output") ||
                                           contains_ci(caps.dev[i].name, "VoiceFilter"))) {
                fprintf(stderr, "The system default microphone is \"%s\" (the virtual mic). Pick your real"
                                " microphone explicitly (--in N).\n", caps.dev[i].name);
                return 1;
            }
    }

    /* ---- pipeline ---- */
    static Chain chain;
    static Pipeline pipe;
    pipeline_init(&pipe);
    if (chain_build(&chain, &pipe) != 0 || pipeline_start(&pipe, VF_SAMPLE_RATE) != 0) {
        fprintf(stderr, "Pipeline init failed.\n");
        return 1;
    }
    for (int c = 0; c < ncmd; c++) vf_cmd_execute(&pipe, cmds[c], stdout);

    /* ---- engine + taps ---- */
    static Engine eng;
    static VfSink vmic_sink, mon_sink;
    static VfMonitor mon;
    int use_pw = (mode != MODE_HEADPHONES) && vmic_native;

    EngineConfig ec;
    memset(&ec, 0, sizeof ec);
    ec.capture_id = ci >= 0 ? &caps.dev[ci].id : NULL;
    ec.playback_id = pi >= 0 ? &plays.dev[pi].id : NULL;
    ec.no_playback = use_pw;              /* Linux vmic: the PipeWire stream is the output */
    ec.period_frames = period;
    ec.file_src = file_buf;
    ec.file_len = file_len;
    if (use_pw && file_buf) { fprintf(stderr, "--in-file with the PipeWire source is not supported.\n"); return 1; }

    int ok = engine_init(&eng, &ctx, &pipe, &ec) == 0;
    if (ok && use_pw) {
        ok = vf_sink_init(&vmic_sink, SINK_TARGET) == 0 && engine_add_tap(&eng, &vmic_sink) == 0 &&
             vf_vmic_start(&vmic_sink) == 0;
        if (!ok) fprintf(stderr, "Cannot create the PipeWire source (is PipeWire running?).\n");
    }
    if (ok && mi >= 0) {
        ok = vf_sink_init(&mon_sink, SINK_TARGET) == 0 && engine_add_tap(&eng, &mon_sink) == 0 &&
             vf_monitor_init(&mon, &ctx, &plays.dev[mi].id, &mon_sink) == 0 && vf_monitor_start(&mon) == 0;
        if (!ok) fprintf(stderr, "Cannot open the monitor device.\n");
    }
    if (!ok || engine_start(&eng) != 0) {
        fprintf(stderr, "Cannot open/start audio devices.\n");
        if (use_pw) vf_vmic_stop();
        vf_monitor_uninit(&mon);
        engine_uninit(&eng);
        pipeline_stop(&pipe);
        ma_context_uninit(&ctx);
        return 1;
    }

    vf_sleep_ms(150);                        /* let the first callbacks run (FIFO pre-fill) */
    EngineLatency lat;
    engine_latency(&eng, &lat);
    printf("\nRunning @ %d Hz.\n", VF_SAMPLE_RATE);
    if (in_file) printf("  input : file\n");
    else         printf("  input : %s\n", ci >= 0 ? caps.dev[ci].name : "(system default)");
    if (use_pw) {
        printf("  output: PipeWire source \"%s\" - select it as the microphone in Discord/OBS\n",
               VF_VMIC_DESCRIPTION);
        printf("  latency estimate: capture %.1f + DSP %.1f + source buffer %.1f ms (+ PipeWire quantum)\n",
               lat.capture_ms, lat.dsp_ms, 1000.0 * SINK_TARGET / VF_SAMPLE_RATE);
    } else {
        printf("  output: %s%s\n", pi >= 0 ? plays.dev[pi].name : "(system default)",
               mode != MODE_HEADPHONES ? "  -> in Discord/OBS select \"CABLE Output\" as the microphone" : "");
        printf("  latency estimate: capture %.1f + playback %.1f + FIFO %.1f + DSP %.1f ms = ~%.1f ms%s\n",
               lat.capture_ms, lat.playback_ms, lat.fifo_ms, lat.dsp_ms, lat.total_ms,
               mode != MODE_HEADPHONES ? " (+ VB-Cable and the app's own buffer)" : "");
    }
    if (mi >= 0)
        printf("  monitor: %s (+%.1f ms cushion, %.1f ms device buffer)\n", plays.dev[mi].name,
               1000.0 * SINK_TARGET / VF_SAMPLE_RATE, vf_monitor_buffer_ms(&mon));
    if (lat.total_ms > 20.0) printf("  (above the 20 ms target - see CLAUDE.md, latency notes)\n");
    printf("Keys:  b = bypass all   1..%d = toggle effect   : = command   h = help   q = quit\n\n",
           pipe.count);

    int running = 1, ticks = 0;
    char cmd[128];
    int cmd_len = 0, editing = 0;
    while (running) {
        int k;
        while ((k = vf_console_getkey()) != -1) {
            if (editing) {                         /* line editor for ':' commands */
                if (k == '\r' || k == '\n') {
                    cmd[cmd_len] = '\0';
                    printf("\n");
                    if (vf_cmd_execute(&pipe, cmd, stdout)) running = 0;
                    editing = 0;
                } else if (k == 27) {
                    editing = 0;
                    printf("\n");
                } else if ((k == 8 || k == 127) && cmd_len > 0) {
                    cmd_len--;
                    printf("\b \b");
                } else if (k >= 32 && k < 127 && cmd_len < (int)sizeof cmd - 1) {
                    cmd[cmd_len++] = (char)k;
                    putchar(k);
                }
                fflush(stdout);
                continue;
            }
            if (k == 'q' || k == 'Q' || k == 27) running = 0;
            else if (k == 'b' || k == 'B')
                atomic_store(&pipe.bypass_all, !atomic_load(&pipe.bypass_all));
            else if (k == 'h' || k == 'H') { printf("\n"); vf_cmd_help(stdout); }
            else if (k == ':' || k == ';') {
                editing = 1; cmd_len = 0;
                printf("\n> ");
                fflush(stdout);
            } else if (k >= '1' && k <= '0' + pipe.count) {
                int fi = k - '1';
                pipeline_set_bypass(&pipe, fi, !atomic_load(&pipe.fx[fi]->bypass));
                printf("\n%s: %s\n", pipe.fx[fi]->name,
                       atomic_load(&pipe.fx[fi]->bypass) ? "BYPASS" : "on");
            }
        }
        if (!editing) {
            float pin = atomic_load_explicit(&pipe.peak_in, memory_order_relaxed);
            float pout = atomic_load_explicit(&pipe.peak_out, memory_order_relaxed);
            float vad = atomic_load_explicit(&pipe.vad, memory_order_relaxed);
            float gate = atomic_load_explicit(&pipe.gate_gain, memory_order_relaxed);
            float gr = atomic_load_explicit(&pipe.comp_gr_db, memory_order_relaxed);
            unsigned xr = atomic_load(&eng.underruns);
            if (use_pw) xr += atomic_load(&vmic_sink.underruns);
            if (mi >= 0) xr += atomic_load(&mon_sink.underruns);
            char m_in[24], m_out[24];
            meter(m_in, sizeof m_in, to_db(pin));
            meter(m_out, sizeof m_out, to_db(pout));
            printf("\r%s | IN %s | OUT %s | VAD %3d%% | gate %3d%% | GR %5.1f dB | xr %u   ",
                   atomic_load(&pipe.bypass_all) ? "BYPASS" : "FILTER", m_in, m_out,
                   (int)(vad * 100.0f + 0.5f), (int)(gate * 100.0f + 0.5f), (double)gr, xr);
            fflush(stdout);
        }
        vf_sleep_ms(50);
        if (seconds > 0 && ++ticks * 0.05 >= seconds) running = 0;
    }

    printf("\n");
    engine_stop(&eng);
    if (use_pw) vf_vmic_stop();
    vf_monitor_uninit(&mon);
    engine_uninit(&eng);
    pipeline_stop(&pipe);
    if (use_pw) {
        printf("vmic sink: underruns %u, overruns %u, drift fixes -%u/+%u samples\n",
               atomic_load(&vmic_sink.underruns), atomic_load(&vmic_sink.overruns),
               atomic_load(&vmic_sink.dropped), atomic_load(&vmic_sink.repeated));
        vf_sink_free(&vmic_sink);
    }
    if (mi >= 0) {
        printf("monitor sink: underruns %u, overruns %u, drift fixes -%u/+%u samples\n",
               atomic_load(&mon_sink.underruns), atomic_load(&mon_sink.overruns),
               atomic_load(&mon_sink.dropped), atomic_load(&mon_sink.repeated));
        vf_sink_free(&mon_sink);
    }
    printf("engine: underruns %u, overruns %u\n", atomic_load(&eng.underruns), atomic_load(&eng.overruns));
    ma_context_uninit(&ctx);
    free(file_buf);
    vf_console_restore();
    return 0;
}
