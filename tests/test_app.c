/* Tests for the application-level pieces: mute, mix bus, soundboard, capture/recorder taps,
 * sink flush. No devices. Exit code != 0 on failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/abtest.h"
#include "app/appwatch.h"
#include "app/automute.h"
#include "app/calibrate.h"
#include "app/hotkeys.h"
#include "app/howl.h"
#include "app/recorder.h"
#include "app/settings.h"
#include "app/soundboard.h"
#include "app/session.h"
#include "app/stats.h"
#include "app/theme.h"
#include "app/undo.h"
#include "util/console.h"
#include "util/platform.h"
#include "audio/engine.h"
#include "audio/sink.h"
#include "audio/vmic_shm.h"
#include "dsp/chain.h"
#include "dsp/fx_limiter.h"
#include "dsp/spectrum.h"

#define SR VF_SAMPLE_RATE
#define PI 3.14159265358979323846

static int g_fail = 0;
static void set_cfg_dir(void)
{
#ifdef _WIN32
    _putenv("VF_CONFIG_DIR=vf_test_cfg_app");
#else
    setenv("VF_CONFIG_DIR", "vf_test_cfg_app", 1);
#endif
}
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static Chain g_chain;
static Pipeline g_pipe;

static void pipe_setup(void)
{
    pipeline_init(&g_pipe);
    chain_build(&g_chain, &g_pipe);
}

static void fill_sine(float *b, long off, int n, double freq, double amp)
{
    for (int i = 0; i < n; i++) b[i] = (float)(amp * sin(2.0 * PI * freq * (double)(off + i) / SR));
}

/* constant-amplitude test source for the mix bus */
typedef struct { double freq, amp; long pos; } ToneSrc;
static void tone_read(void *ctx, float *dst, int n)
{
    ToneSrc *t = (ToneSrc *)ctx;
    fill_sine(dst, t->pos, n, t->freq, t->amp);
    t->pos += n;
}

static void test_mute_and_bus(void)
{
    printf("mute and mix bus\n");
    pipe_setup();
    static ToneSrc tone = { 1000.0, 0.1, 0 };
    static MixSource src;
    src.read = tone_read; src.ctx = &tone;
    pipeline_add_mix(&g_pipe, &src);
    pipeline_start(&g_pipe, SR);

    /* chain bypassed: out = voice + bus exactly, then a safety clip */
    atomic_store(&g_pipe.bypass_all, 1);
    float b[VF_FRAME_SIZE], v[VF_FRAME_SIZE], s[VF_FRAME_SIZE];
    fill_sine(v, 0, VF_FRAME_SIZE, 300.0, 0.2);
    tone.pos = 0;
    memcpy(b, v, sizeof b);
    pipeline_process(&g_pipe, b);
    fill_sine(s, 0, VF_FRAME_SIZE, 1000.0, 0.1);
    float maxerr = 0;
    for (int i = 0; i < VF_FRAME_SIZE; i++) { const float e = fabsf(b[i] - (v[i] + s[i])); if (e > maxerr) maxerr = e; }
    CHECK(maxerr < 1e-6f, "bypassed chain: voice + bus mismatch %g", (double)maxerr);
    CHECK(fabsf(atomic_load(&src.level) - 0.1f) < 0.01f, "bus meter %g", (double)atomic_load(&src.level));

    /* mute: voice goes silent within the 10 ms ramp, the bus keeps playing, no click */
    pipeline_set_mute(&g_pipe, 1);
    float prev = 0, maxjump = 0;
    long t = VF_FRAME_SIZE;
    for (int f = 0; f < 4; f++, t += VF_FRAME_SIZE) {
        fill_sine(v, t, VF_FRAME_SIZE, 300.0, 0.2);
        memcpy(b, v, sizeof b);
        pipeline_process(&g_pipe, b);
        fill_sine(s, t, VF_FRAME_SIZE, 1000.0, 0.1);
        if (f >= 1) {
            float e = 0;
            for (int i = 0; i < VF_FRAME_SIZE; i++) e = fmaxf(e, fabsf(b[i] - s[i]));
            CHECK(e < 1e-5f, "frame %d: voice not muted (residual %g)", f, (double)e);
        }
        for (int i = 0; i < VF_FRAME_SIZE; i++) { maxjump = fmaxf(maxjump, fabsf(b[i] - prev)); prev = b[i]; }
    }
    CHECK(maxjump < 0.12f, "click while muting: sample jump %g", (double)maxjump);
    pipeline_set_mute(&g_pipe, 0);
    for (int f = 0; f < 3; f++, t += VF_FRAME_SIZE) { fill_sine(v, t, VF_FRAME_SIZE, 300.0, 0.2); memcpy(b, v, sizeof b); pipeline_process(&g_pipe, b); }
    float e = 0;
    fill_sine(v, t - VF_FRAME_SIZE, VF_FRAME_SIZE, 300.0, 0.2);
    fill_sine(s, t - VF_FRAME_SIZE, VF_FRAME_SIZE, 1000.0, 0.1);
    for (int i = 0; i < VF_FRAME_SIZE; i++) e = fmaxf(e, fabsf(b[i] - (v[i] + s[i])));
    CHECK(e < 1e-5f, "voice not back after unmute (%g)", (double)e);

    /* ducking: while the voice is active the bus is lowered to `duck` */
    atomic_store(&src.duck, 0.25f);
    for (int f = 0; f < 12; f++, t += VF_FRAME_SIZE) { fill_sine(b, t, VF_FRAME_SIZE, 300.0, 0.3); pipeline_process(&g_pipe, b); }
    CHECK(fabsf(atomic_load(&src.level) - 0.025f) < 0.004f, "ducked bus level %g, want 0.025", (double)atomic_load(&src.level));
    memset(b, 0, sizeof b);
    for (int f = 0; f < 30; f++, t += VF_FRAME_SIZE) { memset(b, 0, sizeof b); pipeline_process(&g_pipe, b); }
    CHECK(fabsf(atomic_load(&src.level) - 0.1f) < 0.01f, "bus did not recover after the voice stopped: %g", (double)atomic_load(&src.level));

    /* chain active: a loud bus still ends up below the limiter ceiling */
    atomic_store(&g_pipe.bypass_all, 0);
    atomic_store(&src.duck, 1.0f);
    tone.amp = 2.0;
    float peak = 0;
    for (int f = 0; f < 60; f++, t += VF_FRAME_SIZE) {
        fill_sine(b, t, VF_FRAME_SIZE, 300.0, 0.3);
        pipeline_process(&g_pipe, b);
        if (f > 5) for (int i = 0; i < VF_FRAME_SIZE; i++) peak = fmaxf(peak, fabsf(b[i]));
    }
    CHECK(peak <= 0.8914f + 1e-4f && peak > 0.5f, "loud bus: peak %g (limiter ceiling 0.8913)", (double)peak);
    pipeline_stop(&g_pipe);
}

static void test_soundboard(void)
{
    printf("soundboard\n");
    static VfSoundboard bd;
    vf_board_init(&bd);
    static float x[SR];
    fill_sine(x, 0, SR, 440.0, 0.5);
    CHECK(vf_board_load_samples(&bd, 2, x, SR, "beep") == 0, "load");
    CHECK(!vf_board_playing(&bd, 2), "idle after load");
    float out[VF_FRAME_SIZE];
    vf_board_read(&bd, out, VF_FRAME_SIZE);
    float pk = 0;
    for (int i = 0; i < VF_FRAME_SIZE; i++) pk = fmaxf(pk, fabsf(out[i]));
    CHECK(pk == 0.0f, "idle clip is audible");

    vf_board_play(&bd, 2);
    CHECK(vf_board_playing(&bd, 2), "playing");
    float maxerr = 0;
    long n = 0;
    while (vf_board_playing(&bd, 2) && n < SR * 2) {
        vf_board_read(&bd, out, VF_FRAME_SIZE);
        for (int i = 0; i < VF_FRAME_SIZE && n + i < SR; i++) maxerr = fmaxf(maxerr, fabsf(out[i] - x[n + i]));
        n += VF_FRAME_SIZE;
    }
    CHECK(maxerr < 1.0f / 16384.0f, "clip differs from the source by %g", (double)maxerr);
    CHECK(n >= SR && n < SR + 2 * VF_FRAME_SIZE && !vf_board_playing(&bd, 2), "clip length %ld", n);
    vf_board_read(&bd, out, VF_FRAME_SIZE);
    pk = 0; for (int i = 0; i < VF_FRAME_SIZE; i++) pk = fmaxf(pk, fabsf(out[i]));
    CHECK(pk == 0.0f, "audio after the end of the clip");

    /* retrigger restarts from the beginning; two clips are summed */
    CHECK(vf_board_load_samples(&bd, 5, x, SR, "beep2") == 0, "load second");
    vf_board_play(&bd, 2);
    vf_board_read(&bd, out, VF_FRAME_SIZE);
    vf_board_play(&bd, 2);
    vf_board_play(&bd, 5);
    vf_board_read(&bd, out, VF_FRAME_SIZE);
    CHECK(fabsf(out[100] - 2.0f * x[100]) < 1e-3f, "retrigger / sum: %g vs %g", (double)out[100], (double)(2.0f * x[100]));
    vf_board_stop_all(&bd);
    CHECK(!vf_board_playing(&bd, 2) && !vf_board_playing(&bd, 5), "stop all");

    /* limits */
    static float big[SR * 16];
    memset(big, 0, sizeof big);
    CHECK(vf_board_load_samples(&bd, 0, big, SR * 16, "long") == -2, "16 s clip must be refused");
    long loaded = vf_board_total_samples(&bd);
    int refused = 0;
    for (int s = 0; s < VF_BOARD_SLOTS; s++) {
        const int r = vf_board_load_samples(&bd, s, big, SR * 14, "fill");
        if (r == -3) refused++;
    }
    printf("  budget: %ld samples used, %d of %d 14 s clips refused\n", vf_board_total_samples(&bd), refused, VF_BOARD_SLOTS);
    CHECK(refused >= 1 && vf_board_total_samples(&bd) <= VF_BOARD_MAX_SAMPLES && loaded > 0, "memory budget not enforced");
    vf_board_clear(&bd, 2);
    CHECK(bd.clip[2].pcm == NULL && bd.clip[2].len == 0, "clear");
    vf_board_free(&bd);
    CHECK(vf_board_total_samples(&bd) == 0, "free");
    CHECK(vf_board_load(&bd, 0, "does-not-exist.wav") == -1, "missing file");
}

static void test_capture_and_taps(void)
{
    printf("capture, recorder and pre-tap\n");
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    static Engine e;
    engine_init_offline(&e, &g_pipe);
    static VfCapture cap;
    CHECK(vf_capture_init(&cap, SR / 2) == 0, "capture init");
    atomic_store(&e.capture, &cap);
    static VfSink pre;
    vf_sink_init(&pre, 960);
    e.pre_tap = &pre;
    atomic_store(&e.pre_tap_on, 1);
    static VfRing rec;
    vf_ring_init(&rec, 1 << 16);
    atomic_store(&e.rec_ring, &rec);

    static float in[SR], out[SR];
    fill_sine(in, 0, SR, 300.0, 0.2);
    vf_capture_start(&cap);
    long pos = 0;
    while (pos < SR) {                       /* odd block size on purpose */
        engine_process_block(&e, out + pos, in + pos, pos + 441 <= SR ? 441 : (unsigned)(SR - pos));
        pos += 441;
    }
    CHECK(!atomic_load(&cap.active), "capture should stop itself when full");
    CHECK(atomic_load(&cap.pos) == (SR / 2) / VF_FRAME_SIZE * VF_FRAME_SIZE, "captured %ld samples", atomic_load(&cap.pos));
    float maxerr = 0;
    for (long i = 0; i < atomic_load(&cap.pos); i++) maxerr = fmaxf(maxerr, fabsf(cap.raw[i] - in[i]));
    CHECK(maxerr == 0.0f, "captured raw differs from the input by %g", (double)maxerr);
    double ep = 0;
    for (long i = SR / 4; i < SR / 2 - 480; i++) ep += (double)cap.proc[i] * cap.proc[i];
    CHECK(ep > 1.0, "processed capture is empty");

    CHECK(vf_ring_available(&pre.ring) > 0 && vf_ring_available(&pre.ring) % VF_FRAME_SIZE == 0, "pre tap holds %u samples", vf_ring_available(&pre.ring));
    float first[VF_FRAME_SIZE];
    vf_ring_read(&pre.ring, first, VF_FRAME_SIZE);
    CHECK(memcmp(first, in, sizeof first) == 0, "pre tap is not the raw input");

    unsigned avail = vf_ring_available(&rec);
    CHECK(avail > 0 && avail % (2 * VF_FRAME_SIZE) == 0, "recorder ring holds %u", avail);
    float pair[2 * VF_FRAME_SIZE];
    vf_ring_read(&rec, pair, 2 * VF_FRAME_SIZE);
    CHECK(pair[0] == in[0] && pair[2] == in[1], "recorder interleaving (raw, processed)");

    /* the recorder drops frames (and counts them) instead of blocking when nobody reads */
    for (int i = 0; i < 200; i++) engine_process_block(&e, out, in, VF_FRAME_SIZE);
    CHECK(atomic_load(&e.rec_dropped) > 0, "overflow not counted");
    atomic_store(&e.rec_ring, NULL);
    atomic_store(&e.capture, NULL);
    vf_capture_free(&cap);
    vf_sink_free(&pre);
    vf_ring_free(&rec);
    pipeline_stop(&g_pipe);
}

static void test_sink_flush(void)
{
    printf("sink flush\n");
    static VfSink s;
    vf_sink_init(&s, 480);
    float x[2000], y[480];
    for (int i = 0; i < 2000; i++) x[i] = 0.5f;
    vf_sink_write(&s, x, 2000);
    vf_sink_read(&s, y, 480);                               /* primes and plays */
    CHECK(y[0] == 0.5f, "primed read");
    vf_sink_flush(&s);
    vf_sink_read(&s, y, 480);
    CHECK(y[0] == 0.0f && vf_ring_available(&s.ring) == 0, "flush must discard the backlog (left %u)", vf_ring_available(&s.ring));
    vf_sink_write(&s, x, 2000);
    vf_sink_read(&s, y, 480);
    CHECK(y[0] == 0.5f, "plays again after the new cushion built up");
    vf_sink_free(&s);
}

/* ---- tests appended: hotkeys, stats, calibration, undo, automute, appwatch, theme, settings ---- */

static void test_hotkeys(void)
{
    printf("hotkeys\n");
    VfHotkey hk;
    char txt[64];
    CHECK(vf_hotkey_parse("ctrl+alt+m", &hk) == 0 && hk.n == 3, "parse ctrl+alt+m (%d keys)", hk.n);
    vf_hotkey_format(&hk, txt, sizeof txt);
    CHECK(!strcmp(txt, "ctrl+alt+m"), "format '%s'", txt);
    CHECK(vf_hotkey_parse("Shift + F13", &hk) == 0 && hk.n == 2, "case/space tolerant");
    vf_hotkey_format(&hk, txt, sizeof txt);
    CHECK(!strcmp(txt, "shift+f13"), "format '%s'", txt);
    CHECK(vf_hotkey_parse("m+ctrl", &hk) == 0, "any order");
    vf_hotkey_format(&hk, txt, sizeof txt);
    CHECK(!strcmp(txt, "ctrl+m"), "modifiers first: '%s'", txt);
    CHECK(vf_hotkey_parse("", &hk) == 0 && hk.n == 0, "empty = unassigned");
    CHECK(vf_hotkey_parse("none", &hk) == 0 && hk.n == 0, "none = unassigned");
    vf_hotkey_format(&hk, txt, sizeof txt);
    CHECK(!strcmp(txt, "none"), "unassigned formats as none");
    CHECK(vf_hotkey_parse("ctrl+nonsense", &hk) != 0 && hk.n == 0, "unknown key rejected");
    CHECK(vf_hotkey_parse("ctrl+alt+shift+win+m", &hk) != 0, "more than %d keys rejected", VF_HK_MAX);
    CHECK(vf_hotkey_parse("f24", &hk) == 0 && vf_hotkey_parse("f25", &hk) != 0, "F1..F24 only");
    int was = 0;
    VfHotkey none;
    vf_hotkey_parse("none", &none);
    CHECK(!vf_hotkey_down(&none) && !vf_hotkey_pressed(&none, &was), "unassigned never fires");
    for (int vk = 0x70; vk <= 0x87; vk++) {              /* every function key round-trips */
        VfHotkey k; k.n = 1; k.vk[0] = vk;
        vf_hotkey_format(&k, txt, sizeof txt);
        VfHotkey b;
        CHECK(vf_hotkey_parse(txt, &b) == 0 && b.n == 1 && b.vk[0] == vk, "round trip of %s", txt);
    }
}

static void feed_frames(VfStats *s, int seconds, float pin, float pout, float vad, float lim)
{
    for (int sec = 0; sec < seconds; sec++) {
        for (int f = 0; f < 100; f++) {
            float m[VF_METER_FIELDS] = { 0 };
            m[VF_M_PEAK_IN] = pin; m[VF_M_PEAK_OUT] = pout; m[VF_M_VAD] = vad; m[VF_M_LIM] = lim; m[VF_M_GATE] = 1.0f;
            vf_stats_frame(s, m);
        }
        vf_stats_tick(s, 0);
    }
}

static void test_stats(void)
{
    printf("statistics and quality verdict\n");
    static VfStats st;
    VfQualityInfo q;
    vf_stats_init(&st);
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_NO_DATA, "empty stats: %d", q.verdict);
    feed_frames(&st, 10, 0.003f, 0.0001f, 0.05f, 1.0f);                 /* quiet room, no speech */
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_NO_SPEECH && q.noise_cut_db > 25.0f, "no speech: %d, noise cut %.1f dB", q.verdict, (double)q.noise_cut_db);

    vf_stats_init(&st);
    feed_frames(&st, 10, 0.01f, 0.01f, 0.95f, 1.0f);                    /* speech at -40 dBFS */
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_TOO_QUIET && fabsf(q.speech_db + 40.0f) < 0.5f, "too quiet: %d (%.1f dB)", q.verdict, (double)q.speech_db);

    vf_stats_init(&st);
    feed_frames(&st, 10, 0.2f, 0.2f, 0.95f, 1.0f);                      /* healthy */
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_OK && q.limiter_pct < 1.0f, "healthy: %d", q.verdict);

    vf_stats_init(&st);
    feed_frames(&st, 10, 0.995f, 0.89f, 0.95f, 1.0f);                   /* peaks at the converter limit */
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_TOO_LOUD, "too loud: %d", q.verdict);

    vf_stats_init(&st);
    feed_frames(&st, 10, 0.5f, 0.89f, 0.95f, 0.5f);                     /* limiter at -6 dB all the time */
    vf_stats_quality(&st, 10, &q);
    CHECK(q.verdict == VF_Q_LIMITER_BUSY && q.limiter_pct > 90.0f, "limiter busy: %d (%.0f%%)", q.verdict, (double)q.limiter_pct);

    /* dropout history: minute buckets, oldest first */
    vf_stats_init(&st);
    for (int sec = 0; sec < 180; sec++) {
        float m[VF_METER_FIELDS] = { 0 };
        vf_stats_frame(&st, m);
        vf_stats_tick(&st, (unsigned)(sec < 60 ? 0 : sec < 120 ? (sec - 59) : 61 + (sec - 119) * 3));
    }
    unsigned h[3];
    CHECK(vf_stats_xrun_history(&st, 3, h) == 3, "history length");
    printf("  dropouts per minute: %u %u %u\n", h[0], h[1], h[2]);
    CHECK(h[0] == 0 && h[1] == 60 && h[2] == 181, "minute buckets %u/%u/%u", h[0], h[1], h[2]);
    /* ring wrap: more than an hour of data stays consistent */
    for (int sec = 0; sec < 4000; sec++) vf_stats_tick(&st, 1000u + (unsigned)sec);
    CHECK(st.count == VF_STATS_SECONDS, "count capped at %d", st.count);
    unsigned h60[60];
    CHECK(vf_stats_xrun_history(&st, 60, h60) == 60, "60 minute history after wrap");
}

static void cal_feed(VfCalibrate *c, int seconds, float pin, float vad_lo, float vad_hi)
{
    for (int f = 0; f < seconds * 100; f++) {
        float m[VF_METER_FIELDS] = { 0 };
        m[VF_M_PEAK_IN] = pin;
        m[VF_M_VAD] = (f % 10 < 5) ? vad_hi : vad_lo;
        vf_cal_frame(c, m);
    }
}

static void test_calibration(void)
{
    printf("calibration wizard\n");
    static VfCalibrate c;
    vf_cal_start(&c);
    CHECK(c.state == VF_CAL_SILENCE, "starts with the silence step");
    cal_feed(&c, VF_CAL_SILENCE_S, 0.004f, 0.02f, 0.10f);              /* room at -48 dBFS, detector calm */
    CHECK(c.state == VF_CAL_SPEECH, "moves on to speech after %d s", VF_CAL_SILENCE_S);
    cal_feed(&c, VF_CAL_SPEECH_S, 0.12f, 0.9f, 0.95f);                 /* speech peaks ~ -18 dBFS */
    CHECK(c.state == VF_CAL_DONE, "done (state %d, %s)", c.state, c.why);
    printf("  noise %.1f dB, speech %.1f dB, SNR %.1f dB -> gate %.2f, comp %.1f dB, makeup %.1f dB\n",
           (double)c.result.noise_db, (double)c.result.speech_db, (double)c.result.snr_db,
           (double)c.result.gate_threshold, (double)c.result.comp_threshold_db, (double)c.result.comp_makeup_db);
    CHECK(fabsf(c.result.noise_db + 48.0f) < 1.0f && fabsf(c.result.speech_db + 18.4f) < 1.0f, "levels measured");
    CHECK(c.result.gate_threshold >= 0.30f && c.result.gate_threshold <= 0.40f, "calm room -> gate %.2f", (double)c.result.gate_threshold);
    CHECK(fabsf(c.result.comp_threshold_db - (c.result.speech_db - 6.0f)) < 0.01f, "comp threshold");
    CHECK(c.result.comp_makeup_db > 6.0f && c.result.comp_makeup_db < 10.0f, "make-up for speech at -18 dBFS (%.1f dB)", (double)c.result.comp_makeup_db);
    CHECK(vf_cal_progress(&c) == 1.0f, "progress at the end");

    pipe_setup();
    pipeline_start(&g_pipe, SR);
    vf_cal_apply(&c, &g_pipe);
    CHECK(fabsf(pipeline_get_param(&g_pipe, pipeline_find_fx(&g_pipe, "gate"), 0) - c.result.gate_threshold) < 1e-5f, "gate applied");
    CHECK(fabsf(pipeline_get_param(&g_pipe, pipeline_find_fx(&g_pipe, "comp"), 4) - c.result.comp_makeup_db) < 1e-5f, "makeup applied");
    pipeline_stop(&g_pipe);

    vf_cal_start(&c);                                                   /* noisy room, quiet speaker */
    cal_feed(&c, VF_CAL_SILENCE_S, 0.03f, 0.40f, 0.60f);
    cal_feed(&c, VF_CAL_SPEECH_S, 0.02f, 0.9f, 0.95f);
    CHECK(c.state == VF_CAL_DONE && c.result.gate_threshold > 0.6f, "noisy room -> stricter gate %.2f", (double)c.result.gate_threshold);
    CHECK(c.result.comp_makeup_db > 10.0f, "quiet speaker -> more make-up (%.1f)", (double)c.result.comp_makeup_db);

    vf_cal_start(&c);                                                   /* loud speaker: no make-up needed */
    cal_feed(&c, VF_CAL_SILENCE_S, 0.004f, 0.02f, 0.10f);
    cal_feed(&c, VF_CAL_SPEECH_S, 0.5f, 0.9f, 0.95f);
    CHECK(c.state == VF_CAL_DONE && c.result.comp_makeup_db == 0.0f, "loud speaker: make-up %.1f", (double)c.result.comp_makeup_db);

    vf_cal_start(&c);                                                   /* nobody speaks */
    cal_feed(&c, VF_CAL_SILENCE_S, 0.004f, 0.02f, 0.10f);
    cal_feed(&c, VF_CAL_SPEECH_S, 0.004f, 0.02f, 0.10f);
    CHECK(c.state == VF_CAL_FAILED && c.why[0], "no speech must fail (state %d)", c.state);
    Pipeline *none = &g_pipe;
    (void)none;
}

static void test_undo(void)
{
    printf("undo / redo\n");
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    static VfUndo u;
    vf_undo_init(&u, &g_pipe);
    const int comp = pipeline_find_fx(&g_pipe, "comp");
    CHECK(!vf_undo_can_undo(&u) && !vf_undo_can_redo(&u), "fresh stack");
    CHECK(vf_undo_commit(&u, &g_pipe) == 0, "no change, nothing recorded");
    pipeline_set_param(&g_pipe, comp, 1, 5.0f);
    CHECK(vf_undo_commit(&u, &g_pipe) == 1 && vf_undo_can_undo(&u), "first edit recorded");
    pipeline_set_param(&g_pipe, comp, 1, 8.0f);
    pipeline_set_bypass(&g_pipe, pipeline_find_fx(&g_pipe, "eq"), 1);
    vf_undo_commit(&u, &g_pipe);
    CHECK(vf_undo_undo(&u, &g_pipe) == 1 && pipeline_get_param(&g_pipe, comp, 1) == 5.0f &&
          atomic_load(&g_pipe.fx[pipeline_find_fx(&g_pipe, "eq")]->bypass) == 0, "undo restores value and on/off");
    CHECK(vf_undo_undo(&u, &g_pipe) == 1 && pipeline_get_param(&g_pipe, comp, 1) == 3.0f, "undo to the start");
    CHECK(vf_undo_undo(&u, &g_pipe) == 0, "nothing left to undo");
    CHECK(vf_undo_redo(&u, &g_pipe) == 1 && vf_undo_redo(&u, &g_pipe) == 1 && pipeline_get_param(&g_pipe, comp, 1) == 8.0f, "redo");
    CHECK(vf_undo_redo(&u, &g_pipe) == 0, "nothing to redo");
    vf_undo_undo(&u, &g_pipe);
    pipeline_set_param(&g_pipe, comp, 1, 2.0f);
    vf_undo_commit(&u, &g_pipe);
    CHECK(!vf_undo_can_redo(&u), "a new edit drops the redo tail");
    for (int i = 0; i < 100; i++) { pipeline_set_param(&g_pipe, comp, 1, 1.0f + (float)(i % 17)); vf_undo_commit(&u, &g_pipe); }
    CHECK(u.count <= VF_UNDO_DEPTH, "depth capped (%d)", u.count);
    int steps = 0;
    while (vf_undo_undo(&u, &g_pipe)) steps++;
    CHECK(steps == u.count - 1 && steps >= VF_UNDO_DEPTH - 2, "undid %d steps", steps);

    VfSnap a, b;                                                        /* snapshots (compare with a preset) */
    vf_snap_capture(&g_pipe, &a);
    pipeline_set_param(&g_pipe, comp, 0, -40.0f);
    vf_snap_capture(&g_pipe, &b);
    CHECK(!vf_snap_equal(&a, &b), "snapshots differ");
    vf_snap_apply(&g_pipe, &a);
    vf_snap_capture(&g_pipe, &b);
    CHECK(vf_snap_equal(&a, &b), "snapshot restored");
    pipeline_stop(&g_pipe);
}

static void test_automute(void)
{
    printf("automatic mute\n");
    VfAutoMute a;
    vf_automute_init(&a, 0.0);
    a.on_lock = 1; a.idle_minutes = 2;
    CHECK(!vf_automute_update(&a, 10.0, 0, 1), "speaking, unlocked: not muted");
    CHECK(vf_automute_update(&a, 11.0, 1, 0) && a.by_lock, "locked screen mutes");
    CHECK(!vf_automute_update(&a, 12.0, 0, 0), "unlocking ends the lock mute by itself");
    CHECK(!vf_automute_update(&a, 100.0, 0, 0), "90 s of silence: not yet");
    CHECK(vf_automute_update(&a, 10.0 + 121.0, 0, 0) && a.by_idle, "idle mute after 2 minutes without speech");
    CHECK(vf_automute_update(&a, 400.0, 0, 1), "speech does not undo the idle mute");
    CHECK(!vf_automute_toggle(&a, 401.0) && !a.by_idle, "the user's unmute cancels it");
    CHECK(!vf_automute_update(&a, 402.0, 0, 0), "and it does not come straight back");
    CHECK(vf_automute_toggle(&a, 403.0) && a.user_mute, "manual mute");
    CHECK(vf_automute_update(&a, 900.0, 0, 0) && a.user_mute, "manual mute persists");
    CHECK(!vf_automute_toggle(&a, 901.0), "manual unmute");
    a.idle_minutes = 0;
    CHECK(!vf_automute_update(&a, 99999.0, 0, 0), "idle mute off: never");
    a.on_lock = 0;
    CHECK(!vf_automute_update(&a, 100000.0, 1, 0), "lock mute off: ignores the lock state");
}

static void test_appwatch(void)
{
    printf("per-application presets\n");
    VfAppRule rules[3];
    memset(rules, 0, sizeof rules);
    snprintf(rules[0].exe, sizeof rules[0].exe, "Game.EXE");   snprintf(rules[0].preset, sizeof rules[0].preset, "noisy");
    snprintf(rules[1].exe, sizeof rules[1].exe, "obs64.exe");  snprintf(rules[1].preset, sizeof rules[1].preset, "podcast");
    snprintf(rules[2].exe, sizeof rules[2].exe, "chrome.exe"); snprintf(rules[2].preset, sizeof rules[2].preset, "natural");
    char run[4][VF_APP_NAME] = { "explorer.exe", "OBS64.exe", "chrome.exe", "svchost.exe" };
    CHECK(vf_appwatch_match(rules, 3, run, 4) == 1, "first matching rule by list order, case-insensitive");
    snprintf(run[0], VF_APP_NAME, "game.exe");
    CHECK(vf_appwatch_match(rules, 3, run, 4) == 0, "higher priority rule wins");
    CHECK(vf_appwatch_match(rules, 3, run, 0) == -1, "nothing running");
    char names[512][VF_APP_NAME];
    const int n = vf_appwatch_list(names, 512);
    printf("  %d processes listed on this system\n", n);
    CHECK(n > 5, "process list is empty");
    int dup = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (!strcmp(names[i], names[j])) dup++;
    CHECK(dup == 0, "%d duplicate names", dup);
}

static int rgb_dist(VfRgba a, VfRgba b)
{
    const int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return (int)sqrt((double)(dr * dr + dg * dg + db * db));
}
static int luma(VfRgba c) { return (299 * c.r + 587 * c.g + 114 * c.b) / 1000; }

static void test_themes(void)
{
    printf("themes\n");
    CHECK(vf_theme_count() >= 3, "%d themes", vf_theme_count());
    CHECK(vf_theme_find("nope") == 0 && vf_theme_find("amber") == 1 && vf_theme_find("graphite") == 0, "lookup by id");
    for (int i = 0; i < vf_theme_count(); i++) {
        const VfTheme *t = vf_theme_get(i);
        const int on = rgb_dist(t->c[TC_ACCENT], t->c[TC_OFF]), warn = rgb_dist(t->c[TC_ACCENT], t->c[TC_WARN]),
                  off_warn = rgb_dist(t->c[TC_OFF], t->c[TC_WARN]);
        printf("  %-9s on/off %3d, on/warn %3d, off/warn %3d, text contrast %3d\n", t->id, on, warn, off_warn,
               abs(luma(t->c[TC_TEXT]) - luma(t->c[TC_PANEL])));
        CHECK(on > 90 && off_warn > 90, "%s: ON and OFF colours too similar (%d / %d)", t->id, on, off_warn);
        CHECK(warn > 60 || rgb_dist(t->c[TC_ACCENT], t->c[TC_WARN]) > 60, "%s: warning colour too close to ON (%d)", t->id, warn);
        CHECK(abs(luma(t->c[TC_TEXT]) - luma(t->c[TC_PANEL])) > 100, "%s: text not readable on panels", t->id);
        CHECK(abs(luma(t->c[TC_ACCENT]) - luma(t->c[TC_LCD_BG])) > 60, "%s: values not readable on the LCD", t->id);
        CHECK(abs(luma(t->c[TC_OFF_TEXT]) - luma(t->c[TC_OFF])) > 100, "%s: OFF button text not readable", t->id);
        CHECK(t->name_en[0] && t->name_pl[0], "%s: names", t->id);
    }
}

static void test_settings_ext(void)
{
    printf("settings: new fields, profiles, export/import\n");
    VfSettings s, r;
    vf_settings_defaults(&s);
    CHECK(!strcmp(s.hk_mute, "ctrl+alt+m") && !strcmp(s.theme, "graphite") && s.win_x == -32000, "defaults");
    s.monitor_gain_db = -9.5f; s.monitor_raw = 1;
    snprintf(s.theme, sizeof s.theme, "amber");
    s.compact = 1; s.overlay = 1; s.overlay_corner = 3; s.win_x = 120; s.win_y = -40; s.cwin_x = 7; s.cwin_y = 8;
    s.per_mic = 1; s.auto_mute_lock = 1; s.auto_mute_idle_min = 15;
    s.board_gain_db = 2.5f; s.sys_gain_db = -12.0f; s.sys_duck_db = -18.0f; s.sys_enabled = 1;
    snprintf(s.sys_device, sizeof s.sys_device, "Speakers (Realtek)");
    snprintf(s.board_path[2], sizeof s.board_path[2], "C:/clips/airhorn.wav");
    snprintf(s.hk_mute, sizeof s.hk_mute, "ctrl+shift+f9");
    snprintf(s.hk_ptt, sizeof s.hk_ptt, "f13"); s.ptt_enabled = 1;
    snprintf(s.hk_board[0], sizeof s.hk_board[0], "ctrl+1");
    s.apps_enabled = 1; s.n_apps = 2;
    snprintf(s.apps[0].exe, sizeof s.apps[0].exe, "game.exe"); snprintf(s.apps[0].preset, sizeof s.apps[0].preset, "noisy");
    snprintf(s.apps[1].exe, sizeof s.apps[1].exe, "obs64.exe"); snprintf(s.apps[1].preset, sizeof s.apps[1].preset, "My preset");
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    char path[VF_PATH_MAX];
    CHECK(vf_settings_path(path, sizeof path) == 0 && vf_settings_save(&s, &g_pipe, path) == 0, "save");
    vf_settings_defaults(&r);
    CHECK(vf_settings_load(&r, &g_pipe, path) == 0, "load");
    CHECK(r.monitor_gain_db == -9.5f && r.monitor_raw == 1 && !strcmp(r.theme, "amber") && r.compact == 1 && r.overlay == 1 &&
          r.overlay_corner == 3 && r.win_x == 120 && r.win_y == -40 && r.cwin_x == 7 && r.cwin_y == 8, "look and window");
    CHECK(r.per_mic == 1 && r.auto_mute_lock == 1 && r.auto_mute_idle_min == 15, "behaviour");
    CHECK(r.board_gain_db == 2.5f && r.sys_gain_db == -12.0f && r.sys_duck_db == -18.0f && r.sys_enabled == 1 &&
          !strcmp(r.sys_device, "Speakers (Realtek)") && !strcmp(r.board_path[2], "C:/clips/airhorn.wav") && r.board_path[0][0] == '\0', "mix");
    CHECK(!strcmp(r.hk_mute, "ctrl+shift+f9") && !strcmp(r.hk_ptt, "f13") && r.ptt_enabled == 1 && !strcmp(r.hk_board[0], "ctrl+1") &&
          !strcmp(r.hk_board[1], "none"), "hotkeys");
    CHECK(r.apps_enabled == 1 && r.n_apps == 2 && !strcmp(r.apps[1].exe, "obs64.exe") && !strcmp(r.apps[1].preset, "My preset"), "app rules");
    CHECK(r.language[0] != '\0', "old keys still there");

    /* an old settings file (before these fields existed) loads with defaults */
    FILE *f = vf_fopen(path, "wb");
    if (f) { fputs("[audio]\nmode = vmic\n[app]\nlanguage = pl\n", f); fclose(f); }
    vf_settings_defaults(&r);
    CHECK(vf_settings_load(&r, &g_pipe, path) == 0 && r.mode == VF_OUT_VMIC && !strcmp(r.theme, "graphite") && !strcmp(r.hk_mute, "ctrl+alt+m") &&
          r.monitor_gain_db == -6.0f, "old file keeps defaults for new keys");
    f = vf_fopen(path, "wb");
    if (f) { fputs("[app]\nauto_mute_idle_min = -5\ntheme = \n", f); fclose(f); }
    vf_settings_defaults(&r);
    vf_settings_load(&r, &g_pipe, path);
    CHECK(r.auto_mute_idle_min == 0 && !strcmp(r.theme, "graphite"), "garbage values are sanitised");

    /* export -> import round trip */
    char text[8192];
    const int comp = pipeline_find_fx(&g_pipe, "comp"), voice = pipeline_find_fx(&g_pipe, "voice");
    pipeline_set_param(&g_pipe, comp, 1, 7.5f);
    pipeline_set_bypass(&g_pipe, voice, 0);
    pipeline_set_param(&g_pipe, voice, 0, 5.0f);
    const int len = vf_preset_export_text(&g_pipe, "Test voice", text, sizeof text);
    printf("  exported preset: %d bytes\n", len);
    CHECK(len > 200 && len < 4000 && strstr(text, "[fx.comp]") && strstr(text, "ratio = 7.5") && strstr(text, "Test voice"), "export text");
    CHECK(vf_preset_export_text(&g_pipe, "x", text, 50) == -1, "too small buffer reported");
    vf_fx_reset(&g_pipe);
    CHECK(pipeline_get_param(&g_pipe, comp, 1) == 3.0f && atomic_load(&g_pipe.fx[voice]->bypass) == 1, "reset");
    vf_preset_export_text(&g_pipe, "x", text, 1);                  /* must not write beyond 1 byte */
    pipeline_set_param(&g_pipe, comp, 1, 7.5f); pipeline_set_bypass(&g_pipe, voice, 0); pipeline_set_param(&g_pipe, voice, 0, 5.0f);
    vf_preset_export_text(&g_pipe, "Test voice", text, sizeof text);
    vf_fx_reset(&g_pipe);
    const int known = vf_preset_import_text(&g_pipe, text);
    CHECK(known > 20, "imported %d keys", known);
    CHECK(pipeline_get_param(&g_pipe, comp, 1) == 7.5f && atomic_load(&g_pipe.fx[voice]->bypass) == 0 && pipeline_get_param(&g_pipe, voice, 0) == 5.0f,
          "import restores the exported state");
    CHECK(vf_preset_import_text(&g_pipe, "hello world, not a preset") == -1 && pipeline_get_param(&g_pipe, comp, 1) == 7.5f, "garbage rejected, nothing changed");
    CHECK(vf_preset_import_text(&g_pipe, "[fx.comp]\nratio = 9999\n") == 1 && pipeline_get_param(&g_pipe, comp, 1) == 20.0f, "values are clamped");
    CHECK(vf_preset_import_text(&g_pipe, NULL) == -1, "NULL text");

    /* per-microphone profiles */
    char mic[96];
    snprintf(mic, sizeof mic, "Mikrofon (Test: USB/Audio) %ld", (long)time(NULL));     /* new name every run */
    CHECK(!vf_profile_exists(mic), "no profile yet");
    pipeline_set_param(&g_pipe, comp, 1, 4.0f);
    CHECK(vf_profile_save(&g_pipe, mic) == 0 && vf_profile_exists(mic), "save profile (odd characters in the name)");
    pipeline_set_param(&g_pipe, comp, 1, 9.0f);
    CHECK(vf_profile_load(&g_pipe, mic) == 0 && pipeline_get_param(&g_pipe, comp, 1) == 4.0f, "load profile");
    CHECK(vf_profile_load(&g_pipe, "Some other mic") == -1, "missing profile reported");
    CHECK(vf_profile_save(&g_pipe, "") != 0, "empty name refused");
    CHECK(vf_profile_delete(mic) == 0 && !vf_profile_exists(mic) && vf_profile_delete(mic) != 0, "delete profile");
    pipeline_stop(&g_pipe);
}

static void test_recorder(void)
{
    printf("recorder (file)\n");
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    static Engine e;
    engine_init_offline(&e, &g_pipe);
    static VfRecorder rec;
    vf_rec_init(&rec);
    char dir[VF_PATH_MAX - 64], path[VF_PATH_MAX];
    vf_config_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/rec_test.wav", dir);
    char shown[1024];
    CHECK(vf_rec_start(&rec, &e, path, 5, shown) == 0 && vf_rec_active(&rec), "start");
    static float in[SR], out[SR];
    fill_sine(in, 0, SR, 300.0, 0.2);
    for (int sec = 0; sec < 2; sec++) {
        for (long pos = 0; pos < SR; pos += 480) engine_process_block(&e, out + pos, in + pos, 480);
        vf_sleep_ms(50);
    }
    CHECK(vf_rec_stop(&rec) == 0, "stop reports a complete file");
    CHECK(atomic_load(&e.rec_ring) == NULL, "engine detached");
    printf("  recorded %.2f s, %u frames dropped\n", vf_rec_seconds(&rec), atomic_load(&e.rec_dropped));
    FILE *f = vf_fopen(path, "rb");
    CHECK(f != NULL, "file exists");
    if (f) {
        unsigned char h[44];
        CHECK(fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4), "header");
        const unsigned long data = (unsigned long)h[40] | ((unsigned long)h[41] << 8) | ((unsigned long)h[42] << 16) | ((unsigned long)h[43] << 24);
        const unsigned long frames = data / 4;
        CHECK(h[22] == 2 && h[24] == (SR & 255) && data > 0, "stereo 48 kHz");
        CHECK(frames >= (unsigned long)(SR * 2) - 2 * 480 && frames <= (unsigned long)SR * 2, "%lu frames in the file", frames);
        short s0[2];
        fseek(f, 44 + 4 * 100, SEEK_SET);                                  /* sample 100: left channel = raw input */
        CHECK(fread(s0, sizeof(short), 2, f) == 2 && abs(s0[0] - (int)(in[100] * 32767.0f)) <= 1, "left channel is the raw input (%d vs %d)", s0[0], (int)(in[100] * 32767.0f));
        fclose(f);
    }
    vf_remove(path);
    vf_rec_free(&rec);
    CHECK(vf_rec_stop(&rec) == -1, "stopping twice is harmless");
    /* a path that cannot be written */
    CHECK(vf_rec_start(&rec, &e, "Z:/definitely/not/there/x.wav", 5, NULL) != 0 && !vf_rec_active(&rec), "unwritable path refused");
    /* automatic file name goes to the documents folder */
    pipeline_stop(&g_pipe);
}

static void test_spectrum_and_scope(void)
{
    printf("spectrum analyser and scope ring\n");
    static VfSpectrum sp;
    vf_spec_init(&sp);
    static float a[VF_SPEC_N], b[VF_SPEC_N];
    fill_sine(a, 0, VF_SPEC_N, 1000.0, 1.0);                 /* full scale at 1 kHz */
    fill_sine(b, 0, VF_SPEC_N, 5000.0, 0.1);                 /* -20 dBFS at 5 kHz */
    vf_spec_push(&sp, a, b, VF_SPEC_N);
    vf_spec_update(&sp, 0.0f);
    int pk_in = 0, pk_out = 0;
    for (int k = 1; k < VF_SPEC_BINS; k++) {
        if (sp.in_db[k] > sp.in_db[pk_in]) pk_in = k;
        if (sp.out_db[k] > sp.out_db[pk_out]) pk_out = k;
    }
    const double f_in = pk_in * (double)SR / VF_SPEC_N, f_out = pk_out * (double)SR / VF_SPEC_N;
    printf("  peaks: %.0f Hz at %.1f dBFS, %.0f Hz at %.1f dBFS\n", f_in, (double)sp.in_db[pk_in], f_out, (double)sp.out_db[pk_out]);
    CHECK(fabs(f_in - 1000.0) < 50.0 && fabs(sp.in_db[pk_in]) < 1.5, "input line");
    CHECK(fabs(f_out - 5000.0) < 50.0 && fabs(sp.out_db[pk_out] + 20.0) < 1.5, "output line");
    CHECK(sp.in_db[300] < -60.0f, "far from the line the floor is low (%.1f)", (double)sp.in_db[300]);
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    vf_spec_push(&sp, a, b, VF_SPEC_N);
    vf_spec_update(&sp, 0.5f);
    CHECK(sp.in_db[21] < -3.0f && sp.in_db[21] > -80.0f, "release is smoothed, not instant (%.1f)", (double)sp.in_db[21]);
    for (int i = 0; i < 40; i++) vf_spec_update(&sp, 0.5f);
    CHECK(sp.in_db[21] <= -100.0f, "and reaches the floor");
    vf_spec_push(&sp, a, b, 5000);                           /* oversize push must not overflow */

    /* the pipeline feeds the scope ring with (input, output) frames */
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    static VfRing ring;
    vf_ring_init(&ring, 8192);
    float f[VF_FRAME_SIZE];
    fill_sine(f, 0, VF_FRAME_SIZE, 300.0, 0.2);
    pipeline_process(&g_pipe, f);
    CHECK(vf_ring_available(&ring) == 0, "scope off: nothing written");
    atomic_store(&g_pipe.scope_ring, &ring);
    fill_sine(f, 0, VF_FRAME_SIZE, 300.0, 0.2);
    float in0[VF_FRAME_SIZE];
    memcpy(in0, f, sizeof f);
    pipeline_process(&g_pipe, f);
    float st[2 * VF_FRAME_SIZE];
    CHECK(vf_ring_available(&ring) == 2 * VF_FRAME_SIZE && vf_ring_read(&ring, st, 2 * VF_FRAME_SIZE) == 2 * VF_FRAME_SIZE, "one frame written");
    CHECK(memcmp(st, in0, sizeof in0) == 0 && memcmp(st + VF_FRAME_SIZE, f, sizeof f) == 0, "scope holds raw input then output");
    atomic_store(&g_pipe.scope_ring, NULL);
    vf_ring_free(&ring);
    pipeline_stop(&g_pipe);
}

static void test_abtest(void)
{
    printf("A/B test\n");
    pipe_setup();
    pipeline_start(&g_pipe, SR);
    static Engine e;
    engine_init_offline(&e, &g_pipe);
    static VfAbTest ab;
    vf_ab_init(&ab);
    CHECK(vf_ab_update(&ab, &e) == VF_AB_IDLE, "idle");
    CHECK(vf_ab_play(&ab, NULL, NULL, VF_AB_RAW) != 0, "cannot play before recording");
    CHECK(vf_ab_record(&ab, &e, 3) == 0 && atomic_load(&ab.state) == VF_AB_RECORDING, "recording");
    static float in[SR * 4], out[SR * 4];
    fill_sine(in, 0, SR * 4, 300.0, 0.05);                   /* quiet voice: the filter chain changes the level */
    for (long pos = 0; pos < SR * 4; pos += 480) engine_process_block(&e, out + pos, in + pos, 480);
    CHECK(vf_ab_update(&ab, &e) == VF_AB_READY, "ready after %.1f s (state %d)", vf_ab_seconds(&ab), atomic_load(&ab.state));
    CHECK(fabs(vf_ab_seconds(&ab) - 3.0) < 0.02, "length %.2f s", vf_ab_seconds(&ab));
    CHECK(atomic_load(&e.capture) == NULL, "engine detached");
    printf("  loudness match factor %.2f\n", (double)ab.proc_gain);

    static float r1[SR], r2[SR];
    atomic_store(&ab.which, VF_AB_RAW);
    vf_ab_render(&ab, SR, r1, SR);
    CHECK(memcmp(r1, in + SR, 4800 * sizeof(float)) == 0, "raw is the recorded input");
    atomic_store(&ab.which, VF_AB_FILTERED);
    atomic_store(&ab.match, 0);
    vf_ab_render(&ab, SR, r2, SR);
    CHECK(memcmp(r2, ab.cap.proc + SR, 4800 * sizeof(float)) == 0, "filtered is the processed signal");
    atomic_store(&ab.match, 1);
    vf_ab_render(&ab, SR, r2, SR);
    double e1 = 0, e2 = 0;
    for (int i = 0; i < SR; i++) { e1 += (double)r1[i] * r1[i]; e2 += (double)r2[i] * r2[i]; }
    CHECK(fabs(10 * log10(e2 / e1)) < 1.0, "loudness matched within 1 dB (%.2f dB)", 10 * log10(e2 / e1));
    atomic_store(&ab.which, VF_AB_ALTERNATE);
    vf_ab_render(&ab, 0, r1, SR);                             /* first 2 s are raw */
    CHECK(memcmp(r1, in, 4800 * sizeof(float)) == 0, "alternate starts with raw");
    vf_ab_render(&ab, (long)VF_AB_SEGMENT_S * SR, r1, SR);    /* then filtered */
    CHECK(memcmp(r1, in + (long)VF_AB_SEGMENT_S * SR, 4800 * sizeof(float)) != 0, "then the filtered signal");
    const long got = vf_ab_render(&ab, 3 * SR - 100, r1, 500);
    CHECK(got == 100 && r1[100] == 0.0f && r1[499] == 0.0f, "past the end: silence (%ld real samples)", got);

    CHECK(vf_ab_record(&ab, &e, 2) == 0, "a second recording replaces the first");
    atomic_store(&ab.cap.active, 0);                          /* too short: ignored, back to idle */
    CHECK(vf_ab_update(&ab, &e) == VF_AB_IDLE, "empty recording discarded");
    vf_ab_free(&ab, &e);
    CHECK(!ab.cap_ready && ab.cap.raw == NULL, "memory released");
    pipeline_stop(&g_pipe);
}

static void test_vmic_shm(void)
{
    printf("virtual microphone shared ring (driver protocol)\n");
    const unsigned cap = 4096;
    VfVmicShared *s = (VfVmicShared *)malloc(vf_vmic_shm_size(cap));
    vf_vmic_shm_init(s, cap, SR);
    CHECK(vf_vmic_shm_valid(s) && s->capacity == cap && s->rate == (unsigned)SR, "header");
    CHECK(!vf_vmic_shm_valid(NULL), "NULL is not valid");
    s->magic = 0;
    CHECK(!vf_vmic_shm_valid(s), "bad magic rejected");
    s->magic = VF_VMIC_MAGIC;

    /* the driver plays silence (and counts an underrun) while the application writes nothing */
    float out[480];
    for (int i = 0; i < 480; i++) out[i] = 1.0f;
    vf_vmic_shm_consume(s, out, 480);
    CHECK(out[0] == 0.0f && out[479] == 0.0f && atomic_load(&s->underruns) == 1, "underrun gives silence");

    /* data comes out in order, across the wrap point, with odd block sizes */
    static float a[480], b[480];
    unsigned nw = 0, nr = 0;
    for (int round = 0; round < 400; round++) {
        const unsigned wn = 100 + (unsigned)(round * 37) % 380;
        for (unsigned i = 0; i < wn; i++) a[i] = (float)((nw + i) % 1000);
        if (vf_vmic_shm_write(s, a, wn) == wn) { nw += wn; }
        const unsigned rn = 100 + (unsigned)(round * 53) % 380;
        if (vf_vmic_shm_level(s) >= rn) {
            vf_vmic_shm_consume(s, b, rn);
            for (unsigned i = 0; i < rn; i++) {
                if (b[i] != (float)((nr + i) % 1000)) { CHECK(0, "sample %u out of order", nr + i); round = 400; break; }
            }
            nr += rn;
        }
    }
    CHECK(nr > 20000 && nw >= nr, "moved %u samples", nr);
    printf("  %u samples passed the ring (several wraps), %u overruns, %u underruns\n", nr, atomic_load(&s->overruns), atomic_load(&s->underruns));

    /* full ring: the block is dropped and counted, nothing is overwritten */
    const unsigned before = vf_vmic_shm_level(s);
    unsigned ok = 1;
    while (ok) ok = vf_vmic_shm_write(s, a, 480);
    CHECK(atomic_load(&s->overruns) >= 1 && vf_vmic_shm_level(s) <= cap, "full ring must drop, level %u", vf_vmic_shm_level(s));
    (void)before;
    CHECK(atomic_load(&s->client_alive) > 0, "heartbeat");
    free(s);
}

static unsigned g_rng_h = 4242;
static float noise(void) { g_rng_h = g_rng_h * 1103515245u + 12345u; return (((g_rng_h >> 16) & 0x7fff) / 16384.0f) - 1.0f; }

static void test_howl(void)
{
    printf("feedback (howl) detector\n");
    static VfSpectrum sp;
    static VfHowl h;
    static float a[VF_SPEC_N], z[VF_SPEC_N];
    memset(z, 0, sizeof z);

    /* a tone that grows by itself: 3 kHz from -40 dBFS, +20 dB per second */
    vf_spec_init(&sp); vf_howl_init(&h);
    float fired = 0; int when = -1;
    for (int u = 0; u < 150 && fired == 0; u++) {
        const double amp = pow(10.0, (-40.0 + 20.0 * (u / 50.0)) / 20.0);
        for (int i = 0; i < VF_SPEC_N; i++) a[i] = (float)(amp * sin(2 * PI * 3000.0 * (double)(u * 960 + i) / SR));
        vf_spec_push(&sp, a, a, VF_SPEC_N);
        vf_spec_update(&sp, 0.0f);
        fired = vf_howl_update(&h, sp.out_db);
        when = u;
    }
    printf("  growing 3 kHz line detected at %.0f Hz after %d updates\n", (double)fired, when + 1);
    CHECK(fabsf(fired - 3000.0f) < 100.0f && when < 100, "growing tone not detected (%.0f Hz, update %d)", (double)fired, when);

    /* a steady loud tone (a musical note) is not feedback */
    vf_spec_init(&sp); vf_howl_init(&h);
    fired = 0;
    for (int u = 0; u < 400; u++) {
        for (int i = 0; i < VF_SPEC_N; i++) a[i] = (float)(0.1 * sin(2 * PI * 440.0 * (double)(u * 960 + i) / SR));
        vf_spec_push(&sp, a, a, VF_SPEC_N);
        vf_spec_update(&sp, 0.0f);
        const float f = vf_howl_update(&h, sp.out_db);
        if (f > 0) fired = f;
    }
    CHECK(fired == 0.0f, "steady tone flagged as feedback (%.0f Hz)", (double)fired);

    /* broadband noise and silence */
    vf_spec_init(&sp); vf_howl_init(&h);
    fired = 0;
    for (int u = 0; u < 300; u++) {
        for (int i = 0; i < VF_SPEC_N; i++) a[i] = 0.2f * noise() * (1.0f + (float)u / 100.0f);
        vf_spec_push(&sp, a, a, VF_SPEC_N);
        vf_spec_update(&sp, 0.0f);
        const float f = vf_howl_update(&h, sp.out_db);
        if (f > 0) fired = f;
    }
    CHECK(fired == 0.0f, "growing noise flagged (%.0f Hz)", (double)fired);
    vf_spec_push(&sp, z, z, VF_SPEC_N);
    vf_spec_update(&sp, 0.0f);
    CHECK(vf_howl_update(&h, sp.out_db) == 0.0f, "silence");
}

static void test_hardening(void)
{
    char out[64];
    const char *dev[] = { "CON", "nul", "Com1", "LPT9", "aux" };
    for (int i = 0; i < 5; i++) {
        CHECK(vf_preset_sanitize(dev[i], out, sizeof out) == 0 && out[0] == '_', "reserved name '%s' -> '%s'", dev[i], out);
    }
    CHECK(vf_preset_sanitize("console", out, sizeof out) == 0 && !strcmp(out, "console"), "ordinary name changed: %s", out);
    CHECK(vf_preset_sanitize("..\\..\\x/y", out, sizeof out) == 0 && !strpbrk(out, "\\/.:"), "path characters survive: %s", out);

    /* an INI with a huge number of keys must stay bounded */
    size_t cap = 600000;
    char *big = (char *)malloc(cap);
    size_t used = 0;
    for (int i = 0; used + 16 < cap; i++) used += (size_t)snprintf(big + used, 16, "k%d=1\n", i);
    big[used] = '\0';
    VfIni ini;
    vf_ini_parse(&ini, big);
    CHECK(ini.count <= 4096, "ini kept %d entries", ini.count);
    vf_ini_free(&ini);
    free(big);
    vf_ini_parse(&ini, "[a]\nn=99999999999999999999\n");
    CHECK(vf_ini_get_int(&ini, "a", "n", 0) > 0, "int overflow wrapped");
    vf_ini_free(&ini);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    set_cfg_dir();
    test_hardening();
    test_mute_and_bus();
    test_soundboard();
    test_capture_and_taps();
    test_sink_flush();
    test_hotkeys();
    test_stats();
    test_calibration();
    test_undo();
    test_automute();
    test_appwatch();
    test_themes();
    test_settings_ext();
    test_recorder();
    test_spectrum_and_scope();
    test_abtest();
    test_vmic_shm();
    test_howl();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
