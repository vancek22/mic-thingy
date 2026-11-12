/* Tests for the optional effects (hum, guard, warmth, de-esser, AGC, breath/clicks, dereverb,
 * voice). C-generated signals only, no devices. Exit code != 0 on failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/chain.h"
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
    vf_sink_free(&ref);
}

static void test_chain_defaults(void)
{
    printf("optional effects default to off\n");
    static Chain c;
    static Pipeline p;
    pipeline_init(&p);
    chain_build(&c, &p);
    pipeline_start(&p, SR);
    const char *off[] = { "aec", "hum", "dereverb", "breath", "guard", "agc", "deesser", "warmth", "voice" };
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

int main(void)
{
    test_chain_defaults();
    test_aec();
    test_hum();
    test_guard();
    test_warmth();
    test_deesser();
    test_agc();
    test_breath();
    test_dereverb();
    test_voice();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
