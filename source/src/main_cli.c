/* mic thingy console front-end.
 *
 *   mic-thingy-cli [--in N] [--out N] [--vmic] [--monitor N] [--period N]
 *                   [--in-file f.wav] [--seconds S] [-c "cmd"]...
 *
 * Output modes:
 *   headphones          mic -> chain -> chosen playback device
 *   virtual mic         Windows: mic -> chain -> "CABLE Input" (apps pick "CABLE Output")
 *                       Linux:   mic -> chain -> PipeWire source "mic thingy microphone"
 *   virtual mic+monitor as above, plus the processed voice on headphones */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/session.h"
#include "app/settings.h"
#include "util/cmdline.h"
#include "util/console.h"
#include "util/wavio.h"

#define MAX_CMDS 16

static void print_list(const char *title, const VfDeviceList *l)
{
    printf("%s\n", title);
    for (int i = 0; i < l->count; i++)
        printf("  [%d] %s%s\n", i, l->dev[i].name, l->dev[i].is_default ? "  (default)" : "");
}

/* Returns index, `def` on Enter, or -2 on bad input. */
static int choose(const char *what, const VfDeviceList *l, int def)
{
    char line[64];
    if (def >= 0) printf("%s - number (Enter = %d): ", what, def);
    else          printf("%s - number: ", what);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return -2;
    if (line[0] == '\n') return def >= 0 ? def : -2;
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

static void usage(void)
{
    printf("usage: mic-thingy-cli [--in N] [--out N] [--vmic] [--monitor N] [--period N]\n"
           "                       [--in-file f.wav] [--seconds S] [-c \"cmd\"]...\n"
           "  --vmic        output to the virtual microphone (Windows: VB-Cable, Linux: PipeWire)\n"
           "  --monitor N   also play the processed voice on playback device N\n"
           "  --in-file F   use a sound file (looped) instead of the microphone (testing)\n"
           "  --seconds S   quit automatically after S seconds\n"
           "  -c CMD        run a console command at start, e.g. -c \"set comp ratio 4\"\n"
           "  --preset NAME apply a preset (built-in: Domyślny, Głośne otoczenie, Podcast, Naturalny)\n"
           "  --no-config   ignore the effect settings saved by the GUI (settings.ini)\n"
           "  --latency-report  print the full buffer breakdown at start and every 5 s\n"
           "  experimental (latency A/B tests): --periods M  --fixed  --pro-audio\n");
}

int main(int argc, char **argv)
{
    vf_harden_process();
    int pre_in = -1, pre_out = -1, pre_mon = -1, period = 0, mode = -1, ncmd = 0, no_config = 0;
    int periods = 0, fixed = 0, pro_audio = 0, lat_report = 0;
    double seconds = 0;
    const char *in_file = NULL, *cmds[MAX_CMDS], *preset = NULL;
    for (int i = 1; i < argc; i++) {
        const int more = i + 1 < argc;
        if (!strcmp(argv[i], "--in") && more) pre_in = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && more) { pre_out = atoi(argv[++i]); if (mode < 0) mode = VF_OUT_HEADPHONES; }
        else if (!strcmp(argv[i], "--vmic")) { if (mode != VF_OUT_VMIC_MONITOR) mode = VF_OUT_VMIC; }
        else if (!strcmp(argv[i], "--monitor") && more) { pre_mon = atoi(argv[++i]); mode = VF_OUT_VMIC_MONITOR; }
        else if (!strcmp(argv[i], "--period") && more) period = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--periods") && more) periods = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fixed")) fixed = 1;
        else if (!strcmp(argv[i], "--pro-audio")) pro_audio = 1;
        else if (!strcmp(argv[i], "--latency-report")) lat_report = 1;
        else if (!strcmp(argv[i], "--in-file") && more) in_file = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && more) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c") && more && ncmd < MAX_CMDS) cmds[ncmd++] = argv[++i];
        else if (!strcmp(argv[i], "--no-config")) no_config = 1;
        else if (!strcmp(argv[i], "--preset") && more) preset = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { usage(); return 2; }
    }

    vf_console_init();
    static VfSession s;
    if (vf_session_init(&s, 0) != 0) { fprintf(stderr, "%s\n", s.error); return 1; }
    printf("mic thingy - backend: %s\n", ma_get_backend_name(s.ctx.backend));
    char cfg_path[VF_PATH_MAX];
    if (!no_config && vf_settings_path(cfg_path, sizeof cfg_path) == 0) {
        VfSettings st;
        vf_settings_defaults(&st);
        if (vf_settings_load(&st, &s.pipe, cfg_path) == 0)
            printf("Effect settings loaded from %s (--no-config to skip)\n", cfg_path);
    }
    if (preset) {
        if (vf_preset_apply(&s.pipe, preset) == 0) printf("Preset \"%s\" applied\n", preset);
        else { fprintf(stderr, "Unknown preset \"%s\"\n", preset); return 1; }
    }
    printf("\n");

    const int interactive = mode < 0;
    if (interactive || (pre_in < 0 && !in_file)) {
        print_list("Inputs (microphones):", &s.caps);
        printf("\n");
        print_list("Outputs (playback):", &s.plays);
        printf("\n");
    }

    VfSessionConfig c;
    memset(&c, 0, sizeof c);
    c.period = period;
    c.periods = periods;
    c.fixed_callback = fixed;
    c.pro_audio = pro_audio;
    c.capture = -1;
    float *file_buf = NULL;
    if (in_file) {
        file_buf = vf_wav_read_mono(in_file, VF_SAMPLE_RATE, &c.file_len);
        if (!file_buf || c.file_len < VF_FRAME_SIZE) { fprintf(stderr, "Cannot read %s\n", in_file); return 1; }
        c.file_src = file_buf;
        printf("Input: file %s (%.1f s, looped)\n", in_file, (double)c.file_len / VF_SAMPLE_RATE);
    } else {
        c.capture = pre_in >= 0 ? (pre_in < s.caps.count ? pre_in : -2)
                                : choose("Input (microphone)", &s.caps, vf_session_default_mic(&s));
        if (c.capture == -2) { fprintf(stderr, "Invalid input device.\n"); return 1; }
    }

    if (interactive) {
        printf("\nOutput:\n  [1] headphones/speakers only\n"
               "  [2] virtual microphone (apps select \"%s\")\n  [3] virtual microphone + listen on headphones\n",
               vf_session_vmic_name(&s));
        mode = ask_number("Choice (Enter = 2): ", 1, 3, 2) - 1;
    }
    c.mode = (VfOutMode)mode;
    c.playback = -1;
    c.monitor = -1;
    if (c.mode == VF_OUT_HEADPHONES) {
        c.playback = pre_out >= 0 ? (pre_out < s.plays.count ? pre_out : -2) : choose("Output", &s.plays, -1);
        if (c.playback == -2) { fprintf(stderr, "Invalid output device.\n"); return 1; }
    }
    if (c.mode == VF_OUT_VMIC_MONITOR) {
        c.monitor = pre_mon >= 0 ? (pre_mon < s.plays.count ? pre_mon : -2) : choose("Monitor (headphones)", &s.plays, -1);
        if (c.monitor < 0) { fprintf(stderr, "Invalid monitor device.\n"); return 1; }
    }

    for (int k = 0; k < ncmd; k++) vf_cmd_execute(&s.pipe, cmds[k], stdout);

    if (vf_session_start(&s, &c) != 0) {
        fprintf(stderr, "%s\n", s.error);
        vf_session_free(&s);
        return 1;
    }

    vf_sleep_ms(150);                        /* let the first callbacks run (FIFO pre-fill) */
    char lat[160];
    vf_session_latency_text(&s, lat, sizeof lat);
    printf("\nRunning @ %d Hz.\n", VF_SAMPLE_RATE);
    printf("  input : %s\n", in_file ? "file" : c.capture >= 0 ? s.caps.dev[c.capture].name : "(system default)");
    if (s.use_pw)
        printf("  output: PipeWire source \"%s\" - select it as the microphone in Discord/OBS\n",
               vf_session_vmic_name(&s));
    else
        printf("  output: %s%s\n",
               s.resolved_playback >= 0 ? s.plays.dev[s.resolved_playback].name : "(system default)",
               c.mode != VF_OUT_HEADPHONES ? "  -> in Discord/OBS select \"CABLE Output\" as the microphone" : "");
    if (s.has_mon) printf("  monitor: %s\n", s.plays.dev[c.monitor].name);
    printf("  latency estimate: %s\n", lat);
    char rep[1024];
    if (lat_report) { engine_latency_report(&s.eng, rep, sizeof rep); printf("%s", rep); }
    printf("Keys:  b = bypass all   1..%d = toggle effect (the first nine; use : bypass NAME for the rest)   : = command   h = help   q = quit\n\n",
           s.pipe.count > 9 ? 9 : s.pipe.count);

    Pipeline *pipe = &s.pipe;
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
                    if (vf_cmd_execute(pipe, cmd, stdout)) running = 0;
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
                atomic_store(&pipe->bypass_all, !atomic_load(&pipe->bypass_all));
            else if (k == 'h' || k == 'H') { printf("\n"); vf_cmd_help(stdout); }
            else if (k == ':' || k == ';') {
                editing = 1; cmd_len = 0;
                printf("\n> ");
                fflush(stdout);
            } else if (k >= '1' && k <= '0' + (pipe->count > 9 ? 9 : pipe->count)) {
                int fi = k - '1';
                pipeline_set_bypass(pipe, fi, !atomic_load(&pipe->fx[fi]->bypass));
                printf("\n%s: %s\n", pipe->fx[fi]->name,
                       atomic_load(&pipe->fx[fi]->bypass) ? "BYPASS" : "on");
            }
        }
        if (!editing) {
            float pin = atomic_load_explicit(&pipe->peak_in, memory_order_relaxed);
            float pout = atomic_load_explicit(&pipe->peak_out, memory_order_relaxed);
            float vad = atomic_load_explicit(&pipe->vad, memory_order_relaxed);
            float gate = atomic_load_explicit(&pipe->gate_gain, memory_order_relaxed);
            float gr = atomic_load_explicit(&pipe->comp_gr_db, memory_order_relaxed);
            char m_in[24], m_out[24];
            meter(m_in, sizeof m_in, to_db(pin));
            meter(m_out, sizeof m_out, to_db(pout));
            printf("\r%s | IN %s | OUT %s | VAD %3d%% | gate %3d%% | GR %5.1f dB | xr %u   ",
                   atomic_load(&pipe->bypass_all) ? "BYPASS" : "FILTER", m_in, m_out,
                   (int)(vad * 100.0f + 0.5f), (int)(gate * 100.0f + 0.5f), (double)gr,
                   vf_session_xruns(&s));
            fflush(stdout);
        }
        vf_sleep_ms(50);
        if (lat_report && !editing && ticks % 100 == 99) {
            engine_latency_report(&s.eng, rep, sizeof rep);
            printf("\n%s", rep);
        }
        if (seconds > 0 && ++ticks * 0.05 >= seconds) running = 0;
    }

    printf("\n");
    if (lat_report) { engine_latency_report(&s.eng, rep, sizeof rep); printf("final:\n%s", rep); }
    if (s.use_pw)
        printf("vmic sink: underruns %u, overruns %u, drift fixes -%u/+%u samples\n",
               atomic_load(&s.vmic_sink.underruns), atomic_load(&s.vmic_sink.overruns),
               atomic_load(&s.vmic_sink.dropped), atomic_load(&s.vmic_sink.repeated));
    if (s.has_mon)
        printf("monitor sink: underruns %u, overruns %u, drift fixes -%u/+%u samples\n",
               atomic_load(&s.mon_sink.underruns), atomic_load(&s.mon_sink.overruns),
               atomic_load(&s.mon_sink.dropped), atomic_load(&s.mon_sink.repeated));
    printf("engine: underruns %u, overruns %u\n", atomic_load(&s.eng.underruns), atomic_load(&s.eng.overruns));
    vf_session_free(&s);
    free(file_buf);
    vf_console_restore();
    return 0;
}
