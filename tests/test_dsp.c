/* Offline DSP tests: no audio devices needed. Exit code != 0 on failure. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/engine.h"
#include "dsp/chain.h"
#include "dsp/fx_rnnoise.h"
#include "dsp/fx_compressor.h"
#include "dsp/fx_eq3.h"
#include "dsp/fx_gate.h"
#include "dsp/fx_highpass.h"
#include "dsp/fx_limiter.h"
#include "util/cmdline.h"

#define SR VF_SAMPLE_RATE
#define PI 3.14159265358979323846

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Runs `seconds` of a sine through fx in 480-sample frames; returns the output
 * RMS (dB) of the last `tail` seconds relative to the input RMS. */
static double sine_gain_db(Fx *fx, double freq, double amp, double seconds, double tail,
                           FxFrameCtx *ctx)
{
    int frames = (int)(seconds * SR / VF_FRAME_SIZE);
    int tail_frames = (int)(tail * SR / VF_FRAME_SIZE);
    float buf[VF_FRAME_SIZE];
    double in_acc = 0, out_acc = 0;
    long t = 0;
    for (int f = 0; f < frames; f++) {
        float in[VF_FRAME_SIZE];
        for (int i = 0; i < VF_FRAME_SIZE; i++, t++)
            buf[i] = in[i] = (float)(amp * sin(2.0 * PI * freq * (double)t / SR));
        fx->process(fx, buf, VF_FRAME_SIZE, ctx);
        if (f >= frames - tail_frames) {
            for (int i = 0; i < VF_FRAME_SIZE; i++) {
                in_acc += (double)in[i] * in[i];
                out_acc += (double)buf[i] * buf[i];
            }
        }
    }
    return 10.0 * log10(out_acc / in_acc);
}

static void start(Fx *fx)
{
    if (fx->init(fx, SR) != 0) { printf("init failed\n"); exit(2); }
    for (int j = 0; j < fx->n_params; j++) fx->set_param(fx, j, fx->params[j].def);
}

static void test_highpass(void)
{
    printf("highpass\n");
    FxFrameCtx ctx = { 0 };
    Fx fx; fx_highpass_make(&fx); start(&fx);
    double g1k = sine_gain_db(&fx, 1000, 0.5, 1.0, 0.5, &ctx);
    CHECK(fabs(g1k) < 0.2, "1 kHz gain %.2f dB", g1k);
    fx.destroy(&fx); start(&fx);
    double g30 = sine_gain_db(&fx, 30, 0.5, 1.0, 0.5, &ctx);
    CHECK(g30 < -25.0, "30 Hz should be attenuated >25 dB, got %.2f", g30);
    fx.destroy(&fx); start(&fx);
    double g80 = sine_gain_db(&fx, 80, 0.5, 1.0, 0.5, &ctx);
    CHECK(fabs(g80 + 3.0) < 0.6, "cutoff should be about -3 dB, got %.2f", g80);
    fx.destroy(&fx);
}

static void test_gate(void)
{
    printf("gate\n");
    Fx fx; fx_gate_make(&fx); start(&fx);
    FxFrameCtx ctx = { 0 };
    ctx.vad_valid = 1; ctx.vad_prob = 0.0f;
    double closed = sine_gain_db(&fx, 1000, 0.5, 1.5, 0.2, &ctx);
    CHECK(closed < -45.0, "closed gate gain %.2f dB (floor -50)", closed);
    ctx.vad_prob = 0.95f;
    double open = sine_gain_db(&fx, 1000, 0.5, 0.5, 0.2, &ctx);
    CHECK(fabs(open) < 0.3, "open gate gain %.2f dB", open);
    /* hold: after voice stops the gate must stay open for ~hold ms */
    ctx.vad_prob = 0.0f;
    float buf[VF_FRAME_SIZE];
    for (int i = 0; i < VF_FRAME_SIZE; i++) buf[i] = 0.5f;
    fx.process(&fx, buf, VF_FRAME_SIZE, &ctx);         /* 10 ms after voice */
    CHECK(buf[VF_FRAME_SIZE - 1] > 0.45f, "gate closed inside hold time (%.3f)", buf[VF_FRAME_SIZE - 1]);
    /* no VAD information (RNNoise bypassed) -> never gate */
    fx.destroy(&fx); start(&fx);
    ctx.vad_valid = 0; ctx.vad_prob = 0.0f;
    double nov = sine_gain_db(&fx, 1000, 0.5, 1.0, 0.2, &ctx);
    CHECK(fabs(nov) < 0.1, "gate without VAD must pass audio, got %.2f dB", nov);
    fx.destroy(&fx);
}

static void test_compressor(void)
{
    printf("compressor\n");
    FxFrameCtx ctx = { 0 };
    Fx fx; fx_compressor_make(&fx); start(&fx);
    fx.set_param(&fx, COMP_MAKEUP_DB, 0.0f);
    /* amp 0.5 = -6.02 dBFS peak; thr -20, ratio 3 -> expected gain reduction ~9.3 dB */
    double g = sine_gain_db(&fx, 1000, 0.5, 2.5, 0.5, &ctx);
    CHECK(fabs(g + 9.3) < 1.5, "expected about -9.3 dB, got %.2f", g);
    CHECK(ctx.comp_gr_db < -7.0 && ctx.comp_gr_db > -11.0, "gr meter %.2f", (double)ctx.comp_gr_db);
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, COMP_MAKEUP_DB, 0.0f);
    double quiet = sine_gain_db(&fx, 1000, 0.01, 2.0, 0.5, &ctx);   /* -40 dBFS: below knee */
    CHECK(fabs(quiet) < 0.3, "below threshold must be untouched, got %.2f", quiet);
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, COMP_RATIO, 1.0f);
    fx.set_param(&fx, COMP_MAKEUP_DB, 0.0f);
    double r1 = sine_gain_db(&fx, 1000, 0.5, 1.5, 0.5, &ctx);
    CHECK(fabs(r1) < 0.3, "ratio 1 must be transparent, got %.2f", r1);
    fx.destroy(&fx);
}

static void test_eq(void)
{
    printf("eq3\n");
    FxFrameCtx ctx = { 0 };
    Fx fx; fx_eq3_make(&fx); start(&fx);
    double flat = sine_gain_db(&fx, 1000, 0.3, 1.0, 0.5, &ctx);
    CHECK(fabs(flat) < 0.05, "flat EQ must be transparent, got %.3f", flat);

    fx.set_param(&fx, EQ_LOW_DB, 6.0f);
    double lo = sine_gain_db(&fx, 40, 0.3, 1.5, 0.5, &ctx);
    CHECK(fabs(lo - 6.0) < 1.0, "low shelf +6 at 40 Hz: %.2f", lo);
    double lo_mid = sine_gain_db(&fx, 1000, 0.3, 1.0, 0.5, &ctx);
    CHECK(fabs(lo_mid) < 0.5, "low shelf leaks into 1 kHz: %.2f", lo_mid);

    fx.set_param(&fx, EQ_LOW_DB, 0.0f);
    fx.set_param(&fx, EQ_MID_DB, 6.0f);
    double mid = sine_gain_db(&fx, 1500, 0.3, 1.0, 0.5, &ctx);
    CHECK(fabs(mid - 6.0) < 0.3, "mid +6 at 1.5 kHz: %.2f", mid);

    fx.set_param(&fx, EQ_MID_DB, 0.0f);
    fx.set_param(&fx, EQ_HIGH_DB, -6.0f);
    double hi = sine_gain_db(&fx, 15000, 0.3, 1.0, 0.5, &ctx);
    CHECK(fabs(hi + 6.0) < 1.0, "high shelf -6 at 15 kHz: %.2f", hi);
    fx.destroy(&fx);
}

static void test_limiter(void)
{
    printf("limiter\n");
    Fx fx; fx_limiter_make(&fx); start(&fx);
    FxFrameCtx ctx = { 0 };
    const float ceil_lin = powf(10.0f, -1.0f / 20.0f);
    float peak = 0.0f;
    long t = 0;
    /* loud sine (+6 dBFS), then a sudden full-scale burst from silence */
    for (int f = 0; f < 300; f++) {
        float b[VF_FRAME_SIZE];
        for (int i = 0; i < VF_FRAME_SIZE; i++, t++) {
            float x = 0.0f;
            if (f < 100) x = (float)(2.0 * sin(2.0 * PI * 440.0 * (double)t / SR));
            else if (f >= 200) x = (t % 100 < 50) ? 4.0f : -4.0f;
            b[i] = x;
        }
        fx.process(&fx, b, VF_FRAME_SIZE, &ctx);
        for (int i = 0; i < VF_FRAME_SIZE; i++) if (fabsf(b[i]) > peak) peak = fabsf(b[i]);
    }
    CHECK(peak <= ceil_lin + 1e-5f, "peak %.5f exceeds ceiling %.5f", (double)peak, (double)ceil_lin);
    CHECK(peak > ceil_lin * 0.9f, "limiter should reach close to the ceiling, peak %.4f", (double)peak);

    fx.destroy(&fx); start(&fx);
    double quiet = sine_gain_db(&fx, 440, 0.1, 1.0, 0.5, &ctx);
    CHECK(fabs(quiet) < 0.1, "signal below ceiling must pass unchanged, got %.3f dB", quiet);
    fx.destroy(&fx);
}

static void test_pipeline(void)
{
    printf("pipeline + commands\n");
    static Chain chain;
    static Pipeline p;
    pipeline_init(&p);
    CHECK(chain_build(&chain, &p) == 0, "chain_build");
    CHECK(pipeline_start(&p, SR) == 0, "pipeline_start");

    /* noisy stress: nothing may produce NaN/Inf, output never above the limiter ceiling */
    srand(1234);
    float worst = 0.0f;
    int bad = 0;
    for (int f = 0; f < 300; f++) {
        float b[VF_FRAME_SIZE];
        for (int i = 0; i < VF_FRAME_SIZE; i++)
            b[i] = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * (f % 50 < 25 ? 0.9f : 0.05f);
        pipeline_process(&p, b);
        for (int i = 0; i < VF_FRAME_SIZE; i++) {
            if (!isfinite(b[i])) bad++;
            if (fabsf(b[i]) > worst) worst = fabsf(b[i]);
        }
    }
    CHECK(bad == 0, "%d non-finite samples", bad);
    CHECK(worst <= 0.8914f, "output %.4f above limiter ceiling", (double)worst);

    FILE *out = tmpfile();
    CHECK(out != NULL, "tmpfile");
    if (out) {
        CHECK(vf_cmd_execute(&p, "set comp thr -30", out) == 0, "set returns");
        int ci = pipeline_find_fx(&p, "comp");
        int ti = pipeline_find_param(&p, ci, "thr");
        CHECK(ci >= 0 && ti == COMP_THRESHOLD_DB, "name lookup (%d, %d)", ci, ti);
        CHECK(fabsf(pipeline_get_param(&p, ci, ti) + 30.0f) < 1e-6f, "param stored");
        vf_cmd_execute(&p, "set comp ratio 999", out);
        CHECK(pipeline_get_param(&p, ci, COMP_RATIO) == 20.0f, "value clamped to max");
        vf_cmd_execute(&p, "set comp ratio abc", out);
        CHECK(pipeline_get_param(&p, ci, COMP_RATIO) == 20.0f, "bad number ignored");
        vf_cmd_execute(&p, "bypass gate on", out);
        CHECK(atomic_load(&chain.gate.bypass) == 1, "gate bypass");
        vf_cmd_execute(&p, "bypass all", out);
        CHECK(atomic_load(&p.bypass_all) == 1, "bypass all toggles on");
        vf_cmd_execute(&p, "reset", out);
        CHECK(pipeline_get_param(&p, ci, COMP_RATIO) == 3.0f, "reset restores default");
        CHECK(vf_cmd_execute(&p, "quit", out) == 1, "quit");
        CHECK(pipeline_set_param(&p, ci, 0, NAN) != 0, "NaN rejected");
        vf_cmd_show(&p, out);
        fclose(out);
    }

    /* the audio thread picks up the new value on the next frame */
    pipeline_set_param(&p, 3, COMP_MAKEUP_DB, 12.0f);
    float b[VF_FRAME_SIZE] = { 0 };
    pipeline_process(&p, b);
    CHECK(p.applied[3][COMP_MAKEUP_DB] == 12.0f, "param applied on audio thread");
    pipeline_stop(&p);
}

/* Speech-like test signal: 140 Hz pulse train (glottal-ish) with a syllable
 * envelope plus a little noise; deterministic. */
static void speechish(float *x, long n)
{
    unsigned s = 12345;
    for (long i = 0; i < n; i++) {
        double t = (double)i / SR;
        double env = 0.5 + 0.5 * sin(2.0 * PI * 4.0 * t);
        double v = 0;
        for (int h = 1; h <= 20; h++) v += sin(2.0 * PI * 140.0 * h * t) / h;
        s = s * 1103515245u + 12345u;
        double nz = ((s >> 16) & 0x7fff) / 32768.0 - 0.5;
        x[i] = (float)(0.15 * env * v + 0.01 * nz);
    }
}

/* Lag (0..maxlag) maximising normalised correlation of y against x. */
static long best_lag(const float *x, const float *y, long n, long maxlag, double *corr)
{
    long best = 0;
    double bc = -2;
    for (long lag = 0; lag <= maxlag; lag++) {
        double c = 0, ex = 0, ey = 0;
        for (long i = SR / 2; i < n - maxlag; i++) {
            c += (double)x[i] * y[i + lag]; ex += (double)x[i] * x[i]; ey += (double)y[i + lag] * y[i + lag];
        }
        c /= sqrt(ex * ey + 1e-30);
        if (c > bc) { bc = c; best = lag; }
    }
    *corr = bc;
    return best;
}

#define LAT_N (SR * 2)

static void run_fx_on(Fx *fx, const float *in, float *out, long n)
{
    FxFrameCtx ctx;
    for (long off = 0; off + VF_FRAME_SIZE <= n; off += VF_FRAME_SIZE) {
        memset(&ctx, 0, sizeof ctx);
        memcpy(out + off, in + off, VF_FRAME_SIZE * sizeof *out);
        fx->process(fx, out + off, VF_FRAME_SIZE, &ctx);
    }
}

static void test_rnnoise_delay(void)
{
    printf("rnnoise delay / dry-wet\n");
    static float in[LAT_N], out[LAT_N];
    speechish(in, LAT_N);
    /* upstream mode (look-ahead on) first */
    Fx fx; fx_rnnoise_make(&fx); start(&fx);
    fx.set_param(&fx, RNNOISE_PARAM_LOOKAHEAD, 1.0f);
    run_fx_on(&fx, in, out, LAT_N);
    double r;
    long lag = best_lag(in, out, LAT_N, 1500, &r);
    printf("  rnnoise delay (look-ahead) = %ld samples (%.1f ms), corr %.2f\n", lag, 1000.0 * lag / SR, r);
    /* +-3: RNNoise's internal DC-blocking high-pass shifts the correlation peak slightly */
    CHECK(labs(lag - RNNOISE_DELAY_SAMPLES) <= 3, "RNNOISE_DELAY_SAMPLES (%d) does not match measured %ld",
          RNNOISE_DELAY_SAMPLES, lag);

    /* wet 0.5 must not comb-filter: the dry path has to be delayed by the same amount,
     * so the output correlates with the input at ONE lag only. */
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, RNNOISE_PARAM_LOOKAHEAD, 1.0f);
    fx.set_param(&fx, RNNOISE_PARAM_WET, 0.5f);
    run_fx_on(&fx, in, out, LAT_N);
    double r0, r1;
    long l = best_lag(in, out, LAT_N, 1500, &r1);
    best_lag(in, out + l, LAT_N - l, 0, &r0);
    CHECK(labs(l - RNNOISE_DELAY_SAMPLES) <= 3, "wet 0.5: main lag %ld", l);
    double c0 = 0, e0 = 0, e1 = 0;                     /* correlation at lag 0 when l != 0 */
    for (long i = SR / 2; i < LAT_N - 1500; i++) {
        c0 += (double)in[i] * out[i]; e0 += (double)in[i] * in[i]; e1 += (double)out[i] * out[i];
    }
    double zc = c0 / sqrt(e0 * e1 + 1e-30);
    CHECK(l == 0 || fabs(zc) < 0.5 * r1, "wet 0.5 has a second (undelayed) path, corr %.2f vs %.2f", zc, r1);

    /* wet 0 = pure (delayed) dry signal, bit-exact apart from the delay */
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, RNNOISE_PARAM_LOOKAHEAD, 1.0f);
    fx.set_param(&fx, RNNOISE_PARAM_WET, 0.0f);
    run_fx_on(&fx, in, out, LAT_N);
    float maxerr = 0;
    for (long i = RNNOISE_DELAY_SAMPLES; i < LAT_N - VF_FRAME_SIZE; i++) {
        float e = fabsf(out[i] - in[i - RNNOISE_DELAY_SAMPLES]);
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 1e-6f, "wet 0 should be the delayed input, max err %g", (double)maxerr);

    /* patched mode without look-ahead: one frame less delay, dry path follows */
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, RNNOISE_PARAM_LOOKAHEAD, 0.0f);
    fx.set_param(&fx, RNNOISE_PARAM_WET, 1.0f);
    CHECK(atomic_load(&fx.latency) == RNNOISE_DELAY_NO_LOOKAHEAD, "latency field %d", atomic_load(&fx.latency));
    run_fx_on(&fx, in, out, LAT_N);
    lag = best_lag(in, out, LAT_N, 1500, &r);
    printf("  rnnoise delay without look-ahead = %ld samples (%.1f ms), corr %.2f\n", lag, 1000.0 * lag / SR, r);
    CHECK(labs(lag - RNNOISE_DELAY_NO_LOOKAHEAD) <= 3, "no-lookahead delay %ld", lag);
    fx.destroy(&fx); start(&fx);
    fx.set_param(&fx, RNNOISE_PARAM_LOOKAHEAD, 0.0f);
    fx.set_param(&fx, RNNOISE_PARAM_WET, 0.0f);
    run_fx_on(&fx, in, out, LAT_N);
    maxerr = 0;
    for (long i = RNNOISE_DELAY_NO_LOOKAHEAD; i < LAT_N - VF_FRAME_SIZE; i++) {
        float e = fabsf(out[i] - in[i - RNNOISE_DELAY_NO_LOOKAHEAD]);
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 1e-6f, "no-lookahead wet 0 should be input delayed by 480, max err %g", (double)maxerr);
    fx.destroy(&fx);
}

static void test_chain_delay(void)
{
    printf("chain delay\n");
    static float in[LAT_N], out[LAT_N];
    static Chain chain;
    static Pipeline p;
    speechish(in, LAT_N);
    pipeline_init(&p);
    chain_build(&chain, &p);
    pipeline_start(&p, SR);
    /* measure pure delay: disable the nonlinear and phase-shifting (IIR) stages */
    FILE *sink = tmpfile();
    vf_cmd_execute(&p, "bypass gate on", sink);
    vf_cmd_execute(&p, "bypass comp on", sink);
    vf_cmd_execute(&p, "bypass highpass on", sink);
    vf_cmd_execute(&p, "bypass eq on", sink);
    if (sink) fclose(sink);
    memcpy(out, in, sizeof in);
    for (long off = 0; off + VF_FRAME_SIZE <= LAT_N; off += VF_FRAME_SIZE)
        pipeline_process(&p, out + off);
    double r;
    long lag = best_lag(in, out, LAT_N, 2000, &r);
    long expect = RNNOISE_DELAY_NO_LOOKAHEAD + LIMITER_LOOKAHEAD;   /* default chain */
    printf("  chain delay = %ld samples (%.1f ms), corr %.2f\n", lag, 1000.0 * lag / SR, r);
    CHECK(labs(lag - expect) <= 3, "chain delay %ld, expected ~%ld", lag, expect);
    CHECK(pipeline_delay_samples(&p) == expect, "pipeline_delay_samples %d", pipeline_delay_samples(&p));
    pipeline_stop(&p);
}

/* The engine's FIFO re-chunking must give the same samples as running the
 * pipeline directly, for any callback block size, with no underruns. */
static void test_engine_blocks(void)
{
    printf("engine re-chunking\n");
    const unsigned sizes[] = { 480, 441, 448, 512, 128, 1024, 0 /* random */ };
    static float in[LAT_N], ref[LAT_N], out[LAT_N];
    speechish(in, LAT_N);

    for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        static Chain c1, c2;
        static Pipeline p1, p2;
        static Engine e;
        pipeline_init(&p1); chain_build(&c1, &p1); pipeline_start(&p1, SR);
        pipeline_init(&p2); chain_build(&c2, &p2); pipeline_start(&p2, SR);

        memcpy(ref, in, sizeof in);
        for (long off = 0; off + VF_FRAME_SIZE <= LAT_N; off += VF_FRAME_SIZE)
            pipeline_process(&p1, ref + off);

        engine_init_offline(&e, &p2);
        unsigned seed = 99;
        long pos = 0;
        while (pos < LAT_N) {
            unsigned b = sizes[si];
            if (b == 0) { seed = seed * 1103515245u + 12345u; b = 32 + (seed >> 16) % 1000; }
            if (pos + (long)b > LAT_N) b = (unsigned)(LAT_N - pos);
            engine_process_block(&e, out + pos, in + pos, b);
            pos += b;
        }
        /* output = ref delayed by a constant (FIFO priming); find it */
        long shift = -1;
        for (long s = 0; s <= VF_FRAME_SIZE * 2 && shift < 0; s++) {
            int ok = 1;
            for (long i = 0; i < 2000 && ok; i++)
                if (out[SR / 2 + i + s] != ref[SR / 2 + i]) ok = 0;
            if (ok) shift = s;
        }
        int mism = 0;
        if (shift >= 0)
            for (long i = SR / 2; i + shift < LAT_N - VF_FRAME_SIZE; i++)
                if (out[i + shift] != ref[i]) mism++;
        unsigned ur = atomic_load(&e.underruns), orun = atomic_load(&e.overruns);
        printf("  block %4u: fifo shift %ld samples, mismatches %d, underruns %u, overruns %u\n",
               sizes[si], shift, mism, ur, orun);
        CHECK(shift >= 0 && mism == 0, "block %u: output is not the delayed pipeline output", sizes[si]);
        /* fixed block sizes: never starve. Random sizes: one-off settling only. */
        unsigned allowed = sizes[si] ? 0 : VF_FRAME_SIZE;
        CHECK(ur <= allowed && orun == 0, "block %u: underruns %u overruns %u", sizes[si], ur, orun);
        if (sizes[si])
            CHECK(shift == e.prime_samples, "block %u: delay %ld != prime %d", sizes[si], shift, e.prime_samples);
        pipeline_stop(&p1);
        pipeline_stop(&p2);
    }
}

int main(void)
{
    test_rnnoise_delay();
    test_chain_delay();
    test_engine_blocks();
    test_highpass();
    test_gate();
    test_compressor();
    test_eq();
    test_limiter();
    test_pipeline();
    printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
