/* Tests for the optional effects (hum, guard, warmth, de-esser, AGC, breath/clicks, dereverb,
 * voice). C-generated signals only, no devices. Exit code != 0 on failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/chain.h"
#include "dsp/anc.h"
#include "dsp/fx_aec.h"
#include "dsp/fx_agc.h"
#include "dsp/fx_breath.h"
#include "dsp/fx_deesser.h"
#include "dsp/fx_dereverb.h"
#include "dsp/fx_guard.h"
#include "dsp/fx_hum.h"
#include "dsp/fx_voice.h"
#include "dsp/fx_warmth.h"
#include "app/settings.h"

#define SR VF_SAMPLE_RATE
#define PI 3.14159265358979323846
#define N_MAX (SR * 14)

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static float in_buf[N_MAX], out_buf[N_MAX];

static void start(Fx *fx)
{
    if (fx->init(fx, SR) != 0) { printf("init failed\n"); exit(2); }
    for (int j = 0; j < fx->n_params; j++) fx->set_param(fx, j, fx->params[j].def);
}

/* processes in_buf -> out_buf in 480-sample frames; vad < 0: no VAD information */
static void run(Fx *fx, long n, float vad)
{
    for (long off = 0; off + VF_FRAME_SIZE <= n; off += VF_FRAME_SIZE) {
        FxFrameCtx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.gate_gain = 1.0f;
        if (vad >= 0) { ctx.vad_valid = 1; ctx.vad_prob = vad; }
        memcpy(out_buf + off, in_buf + off, VF_FRAME_SIZE * sizeof(float));
        fx->process(fx, out_buf + off, VF_FRAME_SIZE, &ctx);
    }
}

static void sine(long n, double freq, double amp)
{
    for (long i = 0; i < n; i++) in_buf[i] = (float)(amp * sin(2.0 * PI * freq * (double)i / SR));
}

static double rms_db(const float *x, long from, long to)
{
    double e = 0;
    for (long i = from; i < to; i++) e += (double)x[i] * x[i];
    return 10.0 * log10(e / (double)(to - from) + 1e-20);
}

static unsigned rng = 12345;
static float noise(void) { rng = rng * 1103515245u + 12345u; return (((rng >> 16) & 0x7fff) / 16384.0f) - 1.0f; }

static double peak_freq(const float *x, long from, long to, double lo, double hi)
{
    static double pw[4000];
    int n = 0;
    double best = 0; int bi = 0;
    for (double f = lo; f <= hi && n < 4000; f += 1.0, n++) {
        double c = 0, s = 0;
        for (long i = from; i < to; i++) { c += x[i] * cos(2 * PI * f * (double)i / SR); s += x[i] * sin(2 * PI * f * (double)i / SR); }
        pw[n] = c * c + s * s;
        if (pw[n] > best) { best = pw[n]; bi = n; }
    }
    /* the cross-fading taps amplitude-modulate the output (side bands): use the power centroid
     * of the neighbourhood of the main line as the perceived pitch */
    double sw = 0, sf = 0;
    for (int k = bi - 40; k <= bi + 40; k++) if (k >= 0 && k < n) { sw += pw[k]; sf += pw[k] * (lo + k); }
    return sf / sw;
}

static void test_hum(void)
{
    printf("hum remover\n");
    Fx fx; fx_hum_make(&fx); start(&fx);
    const double fr[] = { 50, 100, 150, 200, 250, 300, 1000 };
    for (int k = 0; k < 7; k++) {
        fx.destroy(&fx); start(&fx);
        sine(SR * 2, fr[k], 0.1);
        run(&fx, SR * 2, -1);
        const double g = rms_db(out_buf, SR, SR * 2) - rms_db(in_buf, SR, SR * 2);
        if (k < 4) CHECK(g < -30, "%.0f Hz should be notched, gain %.1f dB", fr[k], g);
        else CHECK(fabs(g) < 0.5, "%.0f Hz must pass, gain %.2f dB", fr[k], g);
    }
    /* automatic 60 Hz detection while the notches start at 50 Hz */
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, HUM_AUTO, 1.0f);
    for (long i = 0; i < SR * 8; i++)
        in_buf[i] = (float)(0.1 * sin(2 * PI * 60.0 * i / SR) + 0.05 * sin(2 * PI * 120.0 * i / SR) + 0.02 * sin(2 * PI * 1000.0 * i / SR));
    run(&fx, SR * 8, -1);
    /* residual 60/120 Hz after 7 s: compare with the 1 kHz content that must survive */
    float tmp[SR];
    const double hum_in = 0.1, k1_in = 0.02;
    (void)tmp; (void)hum_in; (void)k1_in;
    double e60 = 0, e1k = 0;
    {
        const double w = 2 * PI * 60.0 / SR, w2 = 2 * PI * 1000.0 / SR;
        double a = 0, b = 0, c = 0, d = 0;
        for (long i = SR * 7; i < SR * 8; i++) {
            a += out_buf[i] * cos(w * i); b += out_buf[i] * sin(w * i);
            c += out_buf[i] * cos(w2 * i); d += out_buf[i] * sin(w2 * i);
        }
        e60 = 2.0 * sqrt(a * a + b * b) / SR;
        e1k = 2.0 * sqrt(c * c + d * d) / SR;
    }
    printf("  auto-detect: 60 Hz residual %.4f (was 0.1), 1 kHz kept %.4f (was 0.02)\n", e60, e1k);
    CHECK(e60 < 0.1 * 0.05, "60 Hz not removed after detection: %.4f", e60);
    CHECK(fabs(e1k - 0.02) < 0.002, "1 kHz altered: %.4f", e1k);
    fx.destroy(&fx);
}

static void test_guard(void)
{
    printf("shout guard\n");
    Fx fx; fx_guard_make(&fx); start(&fx);
    sine(SR, 440, 0.05);
    run(&fx, SR, -1);
    CHECK(fabs(rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR)) < 0.1, "quiet signal altered");
    /* 1.0 amplitude shout from silence */
    memset(in_buf, 0, sizeof(float) * SR);
    for (long i = SR / 2; i < SR / 2 + SR / 5; i++) in_buf[i] = (float)sin(2 * PI * 440.0 * (double)i / SR);
    fx.destroy(&fx); start(&fx);
    run(&fx, SR, -1);
    float pk = 0;
    for (long i = SR / 2 + 144; i < SR / 2 + SR / 5; i++) if (fabsf(out_buf[i]) > pk) pk = fabsf(out_buf[i]);   /* after 3 ms */
    printf("  shout peak after 3 ms: %.3f (input 1.0, threshold 0.316, 8:1 -> ~0.37)\n", (double)pk);
    CHECK(pk < 0.45f, "shout not pulled down: %.3f", (double)pk);
    fx.destroy(&fx);
}

static void test_warmth(void)
{
    printf("warmth\n");
    Fx fx; fx_warmth_make(&fx); start(&fx);
    fx.set_param(&fx, WARMTH_AMOUNT, 0.0f);
    sine(SR, 1000, 0.3);
    run(&fx, SR, -1);
    CHECK(memcmp(in_buf, out_buf, sizeof(float) * (SR / VF_FRAME_SIZE * VF_FRAME_SIZE)) == 0, "amount 0 is not bit-exact");
    fx.set_param(&fx, WARMTH_AMOUNT, 0.5f);
    sine(SR, 1000, 0.01);
    run(&fx, SR, -1);
    const double small = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    CHECK(fabs(small) < 0.7, "small-signal level change %.2f dB", small);
    sine(SR, 1000, 0.5);
    run(&fx, SR, -1);
    double c2 = 0, s2 = 0, c1 = 0, s1 = 0;                       /* 2nd harmonic = even-order warmth */
    for (long i = SR / 2; i < SR; i++) {
        c1 += out_buf[i] * cos(2 * PI * 1000.0 * i / SR); s1 += out_buf[i] * sin(2 * PI * 1000.0 * i / SR);
        c2 += out_buf[i] * cos(2 * PI * 2000.0 * i / SR); s2 += out_buf[i] * sin(2 * PI * 2000.0 * i / SR);
    }
    const double h2 = 20 * log10(sqrt(c2 * c2 + s2 * s2) / sqrt(c1 * c1 + s1 * s1) + 1e-12);
    printf("  2nd harmonic at amount 0.5: %.1f dB\n", h2);
    CHECK(h2 > -40 && h2 < -6, "unexpected 2nd harmonic %.1f dB", h2);
    for (long i = 0; i < SR; i++) if (!isfinite(out_buf[i]) || fabsf(out_buf[i]) > 1.0f) { CHECK(0, "bad sample"); break; }
    fx.destroy(&fx);
}

static void test_deesser(void)
{
    printf("de-esser\n");
    Fx fx; fx_deesser_make(&fx); start(&fx);
    sine(SR, 7500, 0.2);
    run(&fx, SR, -1);
    const double g7 = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    sine(SR, 500, 0.2);
    run(&fx, SR, -1);
    const double g5 = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    sine(SR, 7500, 0.005);
    run(&fx, SR, -1);
    const double gq = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    printf("  7.5 kHz loud %.1f dB, 500 Hz %.2f dB, 7.5 kHz quiet %.2f dB\n", g7, g5, gq);
    CHECK(g7 < -3.0, "sibilance not reduced: %.1f dB", g7);
    CHECK(fabs(g5) < 0.3, "low frequency altered: %.2f dB", g5);
    CHECK(fabs(gq) < 0.5, "quiet sibilance altered: %.2f dB", gq);
    fx.destroy(&fx);
}

static void test_agc(void)
{
    printf("AGC\n");
    Fx fx; fx_agc_make(&fx); start(&fx);
    const double amp = pow(10.0, -30.0 / 20.0) * sqrt(2.0);          /* -30 dBFS RMS */
    sine(SR * 14, 300, amp);
    run(&fx, SR * 14, 0.9f);
    const double lvl = rms_db(out_buf, SR * 13, SR * 14);
    printf("  -30 dBFS voice -> %.1f dBFS after 13 s (target -20, max boost 12)\n", lvl);
    CHECK(fabs(lvl + 20.0) < 1.5, "level %.1f dBFS", lvl);
    /* no voice: the gain must not move (pauses are not boosted) */
    sine(SR * 3, 300, amp * 0.1);
    fx.destroy(&fx); start(&fx);
    run(&fx, SR * 3, 0.05f);
    CHECK(fabs(rms_db(out_buf, SR, SR * 3) - rms_db(in_buf, SR, SR * 3)) < 0.05, "gain moved without voice");
    /* loud voice comes down */
    fx.destroy(&fx); start(&fx);
    sine(SR * 14, 300, 0.7);
    run(&fx, SR * 14, 0.9f);
    CHECK(rms_db(out_buf, SR * 13, SR * 14) < -18.0, "loud voice not reduced: %.1f", rms_db(out_buf, SR * 13, SR * 14));
    fx.destroy(&fx);
}

static void test_breath(void)
{
    printf("breaths and clicks\n");
    Fx fx; fx_breath_make(&fx); start(&fx);
    /* establish a speech level (-20 dBFS) with a high VAD, then an uncertain, much quieter sound */
    for (long i = 0; i < SR; i++) in_buf[i] = 0.14f * noise();
    run(&fx, SR, 0.95f);
    CHECK(fabs(rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR)) < 0.5, "speech altered");
    for (long i = 0; i < SR / 2; i++) in_buf[i] = 0.02f * noise();       /* ~ -35 dBFS */
    run(&fx, SR / 2, 0.2f);
    const double gb = rms_db(out_buf, SR / 4, SR / 2) - rms_db(in_buf, SR / 4, SR / 2);
    printf("  breath (VAD 0.2, 15 dB below speech): %.1f dB\n", gb);
    CHECK(gb < -6.0, "breath not reduced: %.1f dB", gb);
    run(&fx, SR / 2, 0.01f);                                              /* silence-like VAD: left to the gate */
    CHECK(fabs(rms_db(out_buf, SR / 4, SR / 2) - rms_db(in_buf, SR / 4, SR / 2)) < 0.5, "VAD ~0 altered");
    /* without VAD information nothing happens */
    fx.destroy(&fx); start(&fx);
    for (long i = 0; i < SR / 2; i++) in_buf[i] = 0.02f * noise();
    run(&fx, SR / 2, -1.0f);
    CHECK(memcmp(in_buf, out_buf, sizeof(float) * (SR / 2 / VF_FRAME_SIZE * VF_FRAME_SIZE)) == 0, "altered without VAD");

    /* keyboard click: 8 ms decaying high-frequency burst in quiet room noise */
    fx.destroy(&fx); start(&fx);
    for (long i = 0; i < SR; i++) in_buf[i] = 0.0005f * noise();
    const long c0 = SR / 2;
    float ein = 0;
    for (int i = 0; i < 384; i++) {
        const float env = expf(-(float)i / 100.0f);
        const float hf = 0.4f * env * noise() * ((i & 1) ? 1.0f : -1.0f);   /* alternating sign = HF heavy */
        in_buf[c0 + i] += hf; ein += hf * hf;
    }
    run(&fx, SR, 0.03f);
    float eout = 0;
    for (int i = 0; i < 384; i++) eout += out_buf[c0 + i] * out_buf[c0 + i];
    const double gc = 10 * log10(eout / ein);
    printf("  click energy change: %.1f dB\n", gc);
    CHECK(gc < -4.0, "click not reduced: %.1f dB", gc);
    CHECK(fabs(rms_db(out_buf, SR / 8, SR / 4) - rms_db(in_buf, SR / 8, SR / 4)) < 0.5, "background before the click altered");
    /* clicks inside speech (high VAD) are left alone */
    fx.destroy(&fx); start(&fx);
    for (long i = 0; i < SR; i++) in_buf[i] = 0.0005f * noise();
    for (int i = 0; i < 384; i++) in_buf[c0 + i] += 0.4f * expf(-(float)i / 100.0f) * noise() * ((i & 1) ? 1.0f : -1.0f);
    run(&fx, SR, 0.95f);
    eout = 0;
    for (int i = 0; i < 384; i++) eout += out_buf[c0 + i] * out_buf[c0 + i];
    CHECK(10 * log10(eout / ein) > -1.0, "click in speech was reduced");
    fx.destroy(&fx);
}

static void test_dereverb(void)
{
    printf("reverb tail reducer\n");
    Fx fx; fx_dereverb_make(&fx); start(&fx);
    /* 300 ms of "speech" noise, then an exponentially decaying tail (RT60 0.4 s = 150 dB/s) */
    const long burst = SR * 3 / 10, tail = SR / 2, n = SR;
    memset(in_buf, 0, sizeof(float) * n);
    for (long i = 0; i < burst; i++) in_buf[SR / 4 + i] = 0.3f * noise();
    for (long i = 0; i < tail; i++) in_buf[SR / 4 + burst + i] = 0.3f * noise() * powf(10.0f, -150.0f * ((float)i / SR) / 20.0f);
    run(&fx, n, -1.0f);
    const long t0 = SR / 4 + burst;
    const double gb = rms_db(out_buf, SR / 4 + burst / 2, t0) - rms_db(in_buf, SR / 4 + burst / 2, t0);
    const double gt = rms_db(out_buf, t0 + SR * 14 / 100, t0 + SR * 16 / 100) - rms_db(in_buf, t0 + SR * 14 / 100, t0 + SR * 16 / 100);
    printf("  speech part %.2f dB, tail level 150 ms after the word %.1f dB\n", gb, gt);
    CHECK(fabs(gb) < 1.0, "speech altered: %.2f dB", gb);
    CHECK(gt < -8.0, "tail not reduced: %.1f dB", gt);
    /* continuous speech-like signal: untouched */
    for (long i = 0; i < n; i++) in_buf[i] = 0.2f * noise();
    fx.destroy(&fx); start(&fx);
    run(&fx, n, -1.0f);
    CHECK(fabs(rms_db(out_buf, SR / 2, n) - rms_db(in_buf, SR / 2, n)) < 0.3, "continuous signal altered");
    fx.destroy(&fx);
}

static void test_voice(void)
{
    printf("voice effects\n");
    Fx fx; fx_voice_make(&fx); start(&fx);
    sine(SR * 2, 440, 0.3);
    run(&fx, SR * 2, -1);
    CHECK(memcmp(in_buf, out_buf, sizeof(float) * (SR * 2 / VF_FRAME_SIZE * VF_FRAME_SIZE)) == 0, "all off is not bit-exact");
    CHECK(atomic_load(&fx.latency) == 0, "latency at pitch 0: %d", atomic_load(&fx.latency));
    const float st[] = { 12.0f, -12.0f, 7.0f };
    for (int k = 0; k < 3; k++) {
        fx.destroy(&fx); start(&fx);
        fx.set_param(&fx, VOICE_PITCH, st[k]);
        CHECK(atomic_load(&fx.latency) == VOICE_PITCH_DELAY, "latency %d", atomic_load(&fx.latency));
        run(&fx, SR * 2, -1);
        const double want = 440.0 * pow(2.0, st[k] / 12.0), got = peak_freq(out_buf, SR, SR + SR / 4, want * 0.85, want * 1.15);
        printf("  %+.0f semitones: 440 Hz -> %.1f Hz (want %.1f)\n", (double)st[k], got, want);
        CHECK(fabs(got - want) < 0.04 * want, "pitch %.1f vs %.1f", got, want);
        const double lvl = rms_db(out_buf, SR, SR * 2) - rms_db(in_buf, SR, SR * 2);
        CHECK(fabs(lvl) < 4.0, "level change %.2f dB", lvl);
    }
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, VOICE_RADIO, 1.0f);
    sine(SR, 100, 0.1); run(&fx, SR, -1);
    const double r100 = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    sine(SR, 1000, 0.1); run(&fx, SR, -1);
    const double r1k = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    sine(SR, 8000, 0.1); run(&fx, SR, -1);
    const double r8k = rms_db(out_buf, SR / 2, SR) - rms_db(in_buf, SR / 2, SR);
    printf("  radio: 100 Hz %.1f dB, 1 kHz %.1f dB, 8 kHz %.1f dB\n", r100, r1k, r8k);
    CHECK(r100 < r1k - 10 && r8k < r1k - 10 && fabs(r1k) < 4, "radio band-limit");
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, VOICE_ROBOT, 1.0f);
    sine(SR, 300, 0.2); run(&fx, SR, -1);
    CHECK(memcmp(in_buf, out_buf, sizeof(float) * 4800) != 0 && rms_db(out_buf, SR / 2, SR) > -40, "robot");
    fx.destroy(&fx);
}

static void test_aec(void)
{
    printf("echo canceller\n");
    enum { N = SR * 18 };
    static float x[N], s[N], d[N], out[N];
    float lp = 0.0f;
    for (long i = 0; i < N; i++) {                                   /* far end: coloured noise with a slow envelope */
        lp = 0.9f * lp + 0.1f * noise();
        x[i] = 1.2f * lp * (0.6f + 0.4f * sinf(2.0f * 3.14159f * 0.7f * (float)i / SR));
    }
    for (long i = 0; i < N; i++) {                                   /* near end: voiced speech between 12 s and 18 s */
        double v = 0;
        for (int h = 1; h <= 12; h++) v += sin(2.0 * PI * 130.0 * h * (double)i / SR) / h;
        s[i] = i >= SR * 12 ? (float)(0.12 * v * (0.5 + 0.5 * sin(2.0 * PI * 3.0 * (double)i / SR))) : 0.0f;
    }
    /* room: three reflections (the sink between "loopback" and the filter adds 480 samples) */
    static const int dly[3] = { 2200, 2600, 3300 };
    static const float gain[3] = { 0.30f, -0.12f, 0.08f };
    for (long i = 0; i < N; i++) {
        float e = 0;
        for (int k = 0; k < 3; k++) if (i >= dly[k]) e += gain[k] * x[i - dly[k]];
        d[i] = e + s[i];
    }

    Fx fx; fx_aec_make(&fx); start(&fx);
    fx.set_param(&fx, AEC_DELAY_MS, 30.0f);                          /* echo arrives 1720..2820 samples after the (480 late) reference */
    static VfSink ref;
    vf_sink_init(&ref, 480);
    fx_aec_attach(&fx, &ref);
    CHECK(atomic_load(&fx.bypass) == 1 && fx.default_bypass == 1, "off by default");
    float zeros[960] = { 0 };
    vf_sink_write(&ref, zeros, 480);                                 /* priming cushion (see the delay note above) */
    for (long off = 0; off + VF_FRAME_SIZE <= N; off += VF_FRAME_SIZE) {
        vf_sink_write(&ref, x + off, VF_FRAME_SIZE);
        FxFrameCtx ctx = { 0 };
        memcpy(out + off, d + off, VF_FRAME_SIZE * sizeof(float));
        fx.process(&fx, out + off, VF_FRAME_SIZE, &ctx);
    }
    /* echo-only part: how much of the echo is gone after the filter had time to adapt? */
    double e_in = 0, e_out = 0;
    for (long i = SR * 8; i < SR * 12; i++) { e_in += (double)d[i] * d[i]; e_out += (double)out[i] * out[i]; }
    const double erle = 10.0 * log10(e_in / (e_out + 1e-20));
    printf("  echo return loss enhancement (8-12 s): %.1f dB\n", erle);
    CHECK(erle > 15.0, "ERLE %.1f dB", erle);
    /* double talk: the near-end voice has to survive */
    double ss = 0, es = 0;
    for (long i = SR * 13; i < SR * 18; i++) { ss += (double)s[i] * s[i]; es += (double)(out[i] - s[i]) * (out[i] - s[i]); }
    const double sdr = 10.0 * log10(ss / (es + 1e-20));
    printf("  near-end voice during double talk: SDR %.1f dB\n", sdr);
    CHECK(sdr > 6.0, "near-end SDR %.1f dB", sdr);
    for (long i = 0; i < N; i++) if (!isfinite(out[i]) || fabsf(out[i]) > 4.0f) { CHECK(0, "unstable output at %ld", i); break; }

    /* no reference (loopback not running): untouched */
    Fx f2; fx_aec_make(&f2); start(&f2);
    memcpy(out, d, 4800 * sizeof(float));
    FxFrameCtx ctx2 = { 0 };
    for (int o = 0; o < 4800; o += 480) f2.process(&f2, out + o, 480, &ctx2);
    CHECK(memcmp(out, d, 4800 * sizeof(float)) == 0, "must pass through without a reference");
    /* reference attached but silent (nothing playing): the microphone is untouched */
    fx_aec_attach(&f2, &ref);
    static float sil[SR];
    memset(sil, 0, sizeof sil);
    vf_sink_flush(&ref);
    for (int o = 0; o < SR / 2; o += 480) vf_sink_write(&ref, sil, 480);
    memcpy(out, s + SR * 14, SR / 4 * sizeof(float));
    for (int o = 0; o + 480 <= SR / 4; o += 480) { vf_sink_write(&ref, sil, 480); f2.process(&f2, out + o, 480, &ctx2); }
    CHECK(memcmp(out + 4800, s + SR * 14 + 4800, 2400 * sizeof(float)) == 0, "silent reference altered the microphone");
    f2.destroy(&f2);
    fx.destroy(&fx);

    /* Headset: the system plays music (reference) and NONE of it reaches the microphone. Voice for 4 s, then only key
     * clicks and room noise. The canceller must leave the microphone alone - v1.1.0.2 added the music to it, 18 dB
     * louder than the microphone itself right after the voice stopped ("oscillation / harmonising" in the pauses). */
    enum { M = SR * 10 };
    for (long i = 0; i < M; i++) {
        const double t = (double)i / SR, env = 0.55 + 0.45 * sin(2.0 * PI * 2.0 * t);
        x[i] = 0.3f * (float)(env * (sin(2.0 * PI * 220.0 * t) + 0.8 * sin(2.0 * PI * 277.2 * t) + 0.7 * sin(2.0 * PI * 329.6 * t)
                                     + 0.4 * sin(2.0 * PI * 440.0 * t) + 0.3 * sin(2.0 * PI * 880.0 * t)) / 3.0) + 0.015f * noise();
        double v = 0;
        if (i < SR * 4) { for (int h = 1; h <= 12; h++) v += sin(2.0 * PI * 130.0 * h * t) / h; v *= 0.12 * (0.5 + 0.5 * sin(2.0 * PI * 3.0 * t)); }
        const long ph = i % (SR / 6);
        const float key = i >= SR * 4 && ph < 200 ? 0.05f * noise() * (float)exp(-(double)ph / 50.0) : 0.0f;
        d[i] = (float)v + key + 0.002f * noise();
    }
    Fx f3; fx_aec_make(&f3); start(&f3);
    fx_aec_attach(&f3, &ref);
    vf_sink_flush(&ref);
    vf_sink_write(&ref, zeros, 480);
    for (long off = 0; off + VF_FRAME_SIZE <= M; off += VF_FRAME_SIZE) {
        vf_sink_write(&ref, x + off, VF_FRAME_SIZE);
        FxFrameCtx ctx = { 0 };
        memcpy(out + off, d + off, VF_FRAME_SIZE * sizeof(float));
        f3.process(&f3, out + off, VF_FRAME_SIZE, &ctx);
    }
    double pm[2] = { 0, 0 }, pa[2] = { 0, 0 };
    for (long i = SR; i < M; i++) {
        const int k = i >= SR * 4;
        const double a = (double)out[i] - d[i];
        pm[k] += (double)d[i] * d[i]; pa[k] += a * a;
    }
    const double add_voice = 10.0 * log10(pa[0] / pm[0] + 1e-20), add_keys = 10.0 * log10(pa[1] / pm[1] + 1e-20);
    printf("  no echo path (headset), music playing: changes the microphone by %.1f dB while speaking, %.1f dB with keys only\n", add_voice, add_keys);
    CHECK(add_keys < -20.0, "system sound leaks into the pauses: %.1f dB relative to the microphone", add_keys);
    CHECK(add_voice < -6.0, "voice altered by %.1f dB without any echo", add_voice);
    f3.destroy(&f3);

    /* Speakers: real echo. The far end turns very quiet (-56 dBFS) for 3 s while the near end talks, then plays again.
     * The echo must be cancelled again at once (a weight decay tried for the headset case lost it for 5+ s). */
    enum { P = SR * 16 };
    float lp2 = 0.0f;
    for (long i = 0; i < P; i++) {
        lp2 = 0.9f * lp2 + 0.1f * noise();
        const int gap = i >= SR * 8 && i < SR * 11;
        x[i] = (gap ? 0.01f : 1.0f) * 1.2f * lp2 * (0.6f + 0.4f * sinf(2.0f * 3.14159f * 0.7f * (float)i / SR));
        double v = 0;
        if (gap) { for (int h = 1; h <= 12; h++) v += sin(2.0 * PI * 130.0 * h * (double)i / SR) / h; v *= 0.12 * (0.5 + 0.5 * sin(2.0 * PI * 3.0 * (double)i / SR)); }
        s[i] = (float)v;
    }
    for (long i = 0; i < P; i++) {
        float e = 0;
        for (int k = 0; k < 3; k++) if (i >= dly[k]) e += gain[k] * x[i - dly[k]];
        d[i] = e + s[i];
    }
    Fx f4; fx_aec_make(&f4); start(&f4);
    f4.set_param(&f4, AEC_DELAY_MS, 30.0f);
    fx_aec_attach(&f4, &ref);
    vf_sink_flush(&ref);
    vf_sink_write(&ref, zeros, 480);
    for (long off = 0; off + VF_FRAME_SIZE <= P; off += VF_FRAME_SIZE) {
        vf_sink_write(&ref, x + off, VF_FRAME_SIZE);
        FxFrameCtx ctx = { 0 };
        memcpy(out + off, d + off, VF_FRAME_SIZE * sizeof(float));
        f4.process(&f4, out + off, VF_FRAME_SIZE, &ctx);
    }
    double ei = 0, eo = 0;
    for (long i = SR * 11 + 4800; i < SR * 12; i++) { const double ech = (double)d[i] - s[i], res = (double)out[i] - s[i]; ei += ech * ech; eo += res * res; }
    const double resume = 10.0 * log10(ei / (eo + 1e-20));
    printf("  speakers, far end quiet for 3 s while talking: echo reduction in the first second after it resumes %.1f dB\n", resume);
    CHECK(resume > 20.0, "echo not cancelled after the far end resumed: %.1f dB", resume);
    f4.destroy(&f4);
    vf_sink_free(&ref);
}

/* ---- ANC skeleton: simulated ear cup ---- */
typedef struct { float buf[16]; } Plant;
static float sec_plant(void *ctx, float y)           /* secondary path S: 3 samples delay 0.6, 5 samples 0.2 */
{
    Plant *p = (Plant *)ctx;
    memmove(p->buf + 1, p->buf, 15 * sizeof(float));
    p->buf[0] = y;
    return 0.6f * p->buf[3] + 0.2f * p->buf[5];
}

static void test_anc(void)
{
    printf("active noise cancellation skeleton (simulated)\n");
    Plant sp = { { 0 } };
    float sec[32];
    vf_anc_identify(sec, 32, sec_plant, &sp, 20000);
    CHECK(fabsf(sec[3] - 0.6f) < 0.02f && fabsf(sec[5] - 0.2f) < 0.02f && fabsf(sec[0]) < 0.02f, "secondary path identified: h3 %.3f h5 %.3f", (double)sec[3], (double)sec[5]);

    static VfAnc anc;
    vf_anc_init(&anc, 128, 0.1f, sec, 32);
    Plant s2 = { { 0 } };
    float xbuf[16] = { 0 };                            /* primary path P: noise reaches the ear 8 samples late */
    double pd = 0, pe = 0;
    float e = 0.0f;
    const long N = SR * 6;
    for (long n = 0; n < N; n++) {
        const float t = (float)n / SR;
        const float x = 0.3f * sinf(2 * 3.14159f * 120 * t) + 0.2f * sinf(2 * 3.14159f * 240 * t) + 0.1f * sinf(2 * 3.14159f * 360 * t) + 0.02f * noise();
        memmove(xbuf + 1, xbuf, 15 * sizeof(float));
        xbuf[0] = x;
        const float d = 0.8f * xbuf[8] + 0.3f * xbuf[10];
        const float y = vf_anc_tick(&anc, x, e, 1);
        e = d + sec_plant(&s2, y);
        if (n >= N - SR) { pd += (double)d * d; pe += (double)e * e; }
    }
    const double red = 10 * log10(pd / (pe + 1e-20));
    printf("  residual at the ear: %.1f dB below the noise (hum + a little broadband)\n", red);
    CHECK(red > 15.0, "FxLMS reduction %.1f dB", red);

    /* fixed inversion: perfect when the paths have no delay, useless at high frequencies when they do */
    VfAnc inv;
    vf_anc_init(&inv, 1, 0.0f, NULL, 0);
    vf_anc_set_invert(&inv, 1, 1.0f);
    double e0 = 0, d0 = 0;
    float dl[16] = { 0 };
    for (long n = 0; n < SR; n++) {
        const float t = (float)n / SR;
        const float lo = sinf(2 * 3.14159f * 100 * t), hi = sinf(2 * 3.14159f * 4000 * t);
        const float x = lo + hi;
        memmove(dl + 1, dl, 15 * sizeof(float));
        dl[0] = x;
        const float y = vf_anc_tick(&inv, x, 0.0f, 0);
        const float noise_at_ear = dl[10];                         /* ear is 10 samples (0.2 ms) later; anti-noise has no delay */
        const float res = noise_at_ear + y;
        (void)hi;
        if (n > 1000) { e0 += (double)res * res; d0 += (double)noise_at_ear * noise_at_ear; }
    }
    printf("  fixed inversion with a 0.2 ms mismatch, 100 Hz + 4 kHz together: %.1f dB\n", 10 * log10(d0 / (e0 + 1e-20)));
    CHECK(10 * log10(d0 / (e0 + 1e-20)) < 3.0, "a broadband 4 kHz component must NOT be cancelled by plain inversion (%.1f dB)", 10 * log10(d0 / (e0 + 1e-20)));
    /* and without any mismatch it is exact */
    vf_anc_init(&inv, 1, 0.0f, NULL, 0);
    vf_anc_set_invert(&inv, 1, 1.0f);
    float worst = 0;
    for (long n = 0; n < 4800; n++) { const float x = 0.5f * sinf(0.3f * (float)n); const float r = x + vf_anc_tick(&inv, x, 0.0f, 0); if (fabsf(r) > worst) worst = fabsf(r); }
    CHECK(worst < 1e-6f, "aligned inversion leaves %g", (double)worst);
}

static void test_chain_defaults(void)
{
    printf("optional effects default to off\n");
    static Chain c;
    static Pipeline p;
    pipeline_init(&p);
    chain_build(&c, &p);
    pipeline_start(&p, SR);
    const char *off[] = { "aec", "hum", "nn", "dereverb", "breath", "guard", "agc", "deesser", "warmth", "voice" };
    const char *on[] = { "highpass", "rnnoise", "gate", "comp", "eq", "limiter" };
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++)
        CHECK(atomic_load(&p.fx[pipeline_find_fx(&p, off[i])]->bypass) == 1, "%s should start off", off[i]);
    for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++)
        CHECK(atomic_load(&p.fx[pipeline_find_fx(&p, on[i])]->bypass) == 0, "%s should start on", on[i]);
    CHECK(pipeline_delay_samples(&p) == 480 + 72, "default delay %d", pipeline_delay_samples(&p));
    pipeline_set_bypass(&p, pipeline_find_fx(&p, "hum"), 0);
    pipeline_set_bypass(&p, pipeline_find_fx(&p, "gate"), 1);
    vf_fx_reset(&p);
    CHECK(atomic_load(&p.fx[pipeline_find_fx(&p, "hum")]->bypass) == 1 && atomic_load(&p.fx[pipeline_find_fx(&p, "gate")]->bypass) == 0,
          "reset restores the default on/off state");
    /* the whole chain with everything enabled stays finite and below the limiter ceiling */
    for (int i = 0; i < p.count; i++) pipeline_set_bypass(&p, i, 0);
    pipeline_set_param(&p, pipeline_find_fx(&p, "voice"), VOICE_PITCH, 3.0f);
    float peak = 0;
    for (long off_ = 0; off_ + VF_FRAME_SIZE <= SR * 3; off_ += VF_FRAME_SIZE) {
        float b[VF_FRAME_SIZE];
        for (int i = 0; i < VF_FRAME_SIZE; i++) b[i] = (float)(0.6 * sin(2 * PI * 250.0 * (double)(off_ + i) / SR)) + 0.05f * noise();
        pipeline_process(&p, b);
        for (int i = 0; i < VF_FRAME_SIZE; i++) {
            if (!isfinite(b[i])) { CHECK(0, "non-finite sample with every effect on"); off_ = SR * 3; break; }
            if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
        }
    }
    CHECK(peak <= 0.8914f + 1e-4f, "peak %.4f above the limiter ceiling", (double)peak);
    pipeline_stop(&p);
}

/* --- own neural denoiser: FFT vs naive DFT, torch parity (golden data from tools/nn/export.py),
 *     unity gains reconstruct the input delayed by 480 samples --- */
#include "dsp/fft960.h"
#include "dsp/fx_nn.h"
#include "nn_golden.h"
#include "dsp/erbdf.h"
#include "erbdf_golden.h"

static float golden_x(long i)
{
    const double d = (double)i;
    return (float)(0.3 * sin(2 * PI * 440 * d / 48000) + 0.1 * sin(2 * PI * 3150 * d / 48000) * ((i % 4800) < 2400)
                   + 0.02 * ((double)(i * 7919 % 1000) / 1000.0 - 0.5));
}

static void test_nn(void)
{
    printf("neural denoiser (fft, torch parity, reconstruction)\n");
    static VfFft f;
    vf_fft_init(&f);
    static float re[VF_FFT_N], im[VF_FFT_N], x[VF_FFT_N];
    for (int i = 0; i < VF_FFT_N; i++) { x[i] = noise(); re[i] = x[i]; im[i] = 0; }
    vf_fft(&f, re, im, 0);
    double maxerr = 0;
    for (int k = 0; k < VF_FFT_N; k += 37) {
        double sr = 0, si = 0;
        for (int n = 0; n < VF_FFT_N; n++) { sr += x[n] * cos(2 * PI * k * n / VF_FFT_N); si -= x[n] * sin(2 * PI * k * n / VF_FFT_N); }
        maxerr = fmax(maxerr, fabs(sr - re[k]) + fabs(si - im[k]));
    }
    CHECK(maxerr < 1e-3, "fft differs from the DFT by %.2g", maxerr);
    vf_fft(&f, re, im, 1);
    maxerr = 0;
    for (int i = 0; i < VF_FFT_N; i++) maxerr = fmax(maxerr, fabs(re[i] / VF_FFT_N - x[i]));
    CHECK(maxerr < 1e-5, "inverse fft error %.2g", maxerr);

    /* features: same window, FFT and log as fx_nn.c, on the golden signal (480 zeros first) */
    static float win[VF_FFT_N], sig_[VF_FRAME_SIZE * (NN_GOLDEN_FRAMES + 1)];
    for (int i = 0; i < VF_FFT_N; i++) { const double a = sin(PI * (i + 0.5) / VF_FFT_N); win[i] = (float)sin(0.5 * PI * a * a); }
    for (int i = 0; i < VF_FRAME_SIZE; i++) sig_[i] = 0;
    for (long i = 0; i < (long)VF_FRAME_SIZE * NN_GOLDEN_FRAMES; i++) sig_[VF_FRAME_SIZE + i] = golden_x(i);
    double ferr = 0, gerr = 0, verr = 0;
    static VfNn nn;
    CHECK(vf_nn_init(&nn, &vf_nn_default) == 0, "weights fit");
    float feat[VF_NN_NB], g[VF_NN_NB];
    for (int t = 0; t < NN_GOLDEN_FRAMES; t++) {
        for (int i = 0; i < VF_FFT_N; i++) { re[i] = sig_[t * VF_FRAME_SIZE + i] * win[i]; im[i] = 0; }
        vf_fft(&f, re, im, 0);
        for (int k = 0; k < NN_GOLDEN_NB; k++) {
            feat[k] = log10f((re[k] * re[k] + im[k] * im[k]) / (float)VF_FRAME_SIZE + 1e-10f);
            ferr = fmax(ferr, fabs(feat[k] - nn_golden_feat[t * NN_GOLDEN_NB + k]));
        }
        const float v = vf_nn_step(&nn, nn_golden_feat + t * NN_GOLDEN_NB, g);
        for (int k = 0; k < NN_GOLDEN_NB; k++) gerr = fmax(gerr, fabs(g[k] - nn_golden_gain[t * NN_GOLDEN_NB + k]));
        verr = fmax(verr, fabs(v - nn_golden_vad[t]));
    }
    CHECK(ferr < 2e-3, "features differ from torch by %.2g", ferr);
    CHECK(gerr < 1e-4 && verr < 1e-4, "inference differs from torch: gains %.2g vad %.2g", gerr, verr);
    printf("  max diff vs torch: features %.2g, gains %.2g, vad %.2g\n", ferr, gerr, verr);

    /* floor 0 dB = every gain 1: output == input delayed by exactly NN_DELAY_SAMPLES */
    static Fx fx;
    fx_nn_make(&fx);
    CHECK(fx.init(&fx, SR) == 0, "nn init");
    fx.set_param(&fx, NN_PARAM_FLOOR, 0.0f);
    static float in[VF_FRAME_SIZE * 40], out[VF_FRAME_SIZE * 40];
    for (int i = 0; i < VF_FRAME_SIZE * 40; i++) in[i] = out[i] = noise() * 0.5f;
    FxFrameCtx ctx = { 0 };
    for (int b = 0; b < 40; b++) fx.process(&fx, out + b * VF_FRAME_SIZE, VF_FRAME_SIZE, &ctx);
    double rerr = 0;
    for (int i = NN_DELAY_SAMPLES; i < VF_FRAME_SIZE * 40; i++) rerr = fmax(rerr, fabs(out[i] - in[i - NN_DELAY_SAMPLES]));
    CHECK(rerr < 1e-5, "unity gains do not reconstruct the delayed input (err %.2g)", rerr);
    CHECK(ctx.vad_valid == 1 && ctx.vad_prob >= 0 && ctx.vad_prob <= 1, "nn writes the voice probability");
    fx.destroy(&fx);
}

static void test_erbdf(void)
{
    printf("erbdf denoiser (torch parity, model switch)\n");
    static VfErbDf net;
    CHECK(vf_erbdf_init(&net, &vf_erbdf_default) == 0, "erbdf weights fit the buffers");
    const int V = vf_erbdf_default.V, NBINS = ERBDF_GOLDEN_BINS;
    static float re[VF_ERB_MAXV], im[VF_ERB_MAXV], yr[VF_ERB_MAXV], yi[VF_ERB_MAXV];
    double worst = 0, vworst = 0, hworst = 0;
    for (int t = 0; t < ERBDF_GOLDEN_FRAMES; t++) {
        const float *X = erbdf_golden_x + (long)t * NBINS * 2, *Y = erbdf_golden_y + (long)t * NBINS * 2;
        for (int k = 0; k < V; k++) { re[k] = X[2 * k]; im[k] = X[2 * k + 1]; }
        float hf;
        const float v = vf_erbdf_step(&net, re, im, 0.0f, yr, yi, &hf);
        double ymax = 1e-12;
        for (int k = 0; k < NBINS; k++) ymax = fmax(ymax, fabs(Y[2 * k]) + fabs(Y[2 * k + 1]));
        for (int k = 0; k < NBINS; k++) {
            const float cr = k < V ? yr[k] : X[2 * k] * hf, ci = k < V ? yi[k] : X[2 * k + 1] * hf;
            worst = fmax(worst, (fabs(cr - Y[2 * k]) + fabs(ci - Y[2 * k + 1])) / ymax);
        }
        vworst = fmax(vworst, fabs(v - erbdf_golden_vad[t]));
        hworst = fmax(hworst, hf);
    }
    CHECK(worst < 2e-4, "erbdf output differs from torch by %.2g (relative to the frame peak)", worst);
    CHECK(vworst < 1e-4, "erbdf voice probability differs from torch by %.2g", vworst);
    CHECK(hworst > 0.0, "erbdf high-band gain is computed");
    printf("  max diff vs torch: spectrum %.2g (relative), vad %.2g\n", worst, vworst);

    /* the effect with model 1: finite, voice probability written, 480 samples delay; switching mid-stream is safe */
    static Fx fx;
    fx_nn_make(&fx);
    CHECK(fx.init(&fx, SR) == 0, "nn init with both nets");
    CHECK(fx.n_params == 3 && fx.params[NN_PARAM_MODEL].toggle == 1 && fx.params[NN_PARAM_MODEL].def == 0.0f,
          "model is a toggle that defaults to nsnet");
    static float buf[VF_FRAME_SIZE * 60];
    for (int i = 0; i < VF_FRAME_SIZE * 60; i++) buf[i] = 0.3f * (float)sin(2 * PI * 300 * i / 48000.0) + noise() * 0.1f;
    FxFrameCtx ctx = { 0 };
    int finite = 1;
    for (int b = 0; b < 60; b++) {
        if (b == 10) fx.set_param(&fx, NN_PARAM_MODEL, 1.0f);
        if (b == 30) fx.set_param(&fx, NN_PARAM_MODEL, 0.0f);
        if (b == 45) fx.set_param(&fx, NN_PARAM_MODEL, 1.0f);
        fx.process(&fx, buf + b * VF_FRAME_SIZE, VF_FRAME_SIZE, &ctx);
        for (int i = 0; i < VF_FRAME_SIZE; i++) if (!isfinite(buf[b * VF_FRAME_SIZE + i]) || fabsf(buf[b * VF_FRAME_SIZE + i]) > 4.0f) finite = 0;
    }
    CHECK(finite, "output stays finite and bounded across model switches");
    CHECK(ctx.vad_valid == 1 && ctx.vad_prob >= 0.0f && ctx.vad_prob <= 1.0f, "erbdf writes the voice probability");
    CHECK(atomic_load(&fx.latency) == NN_DELAY_SAMPLES, "latency is the same for both models");
    fx.destroy(&fx);
}

int main(void)
{
    test_chain_defaults();
    test_aec();
    test_anc();
    test_hum();
    test_guard();
    test_warmth();
    test_deesser();
    test_agc();
    test_breath();
    test_dereverb();
    test_voice();
    test_nn();
    test_erbdf();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
