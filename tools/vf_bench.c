/* vf-bench: CPU cost of the effect chain per 10 ms frame, per configuration.
 *   vf-bench [--frames N] [--repeat R]
 * Runs pipeline_process() on a speech-like test signal (no devices) and prints microseconds per
 * frame (best of R runs) and the share of one core in real time. Each optional effect is measured
 * on top of the default chain; the last rows are "everything on" and the low-latency mode. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/chain.h"
#include "dsp/fx_aec.h"
#include "dsp/fx_rnnoise.h"
#include "dsp/fx_limiter.h"
#include "audio/sink.h"
#include "util/console.h"

static void speechish(float *x, long n)
{
    unsigned s = 12345;
    for (long i = 0; i < n; i++) {
        const double t = (double)i / VF_SAMPLE_RATE, env = 0.5 + 0.5 * sin(2.0 * 3.14159265 * 4.0 * t);
        double v = 0;
        for (int h = 1; h <= 20; h++) v += sin(2.0 * 3.14159265 * 140.0 * h * t) / h;
        s = s * 1103515245u + 12345u;
        x[i] = (float)(0.15 * env * v + 0.01 * (((s >> 16) & 0x7fff) / 32768.0 - 0.5));
    }
}

static double run(Pipeline *p, const float *in, long frames)
{
    float buf[VF_FRAME_SIZE];
    const double t0 = vf_now_s();
    for (long f = 0; f < frames; f++) {
        memcpy(buf, in + (f % 400) * VF_FRAME_SIZE, sizeof buf);
        pipeline_process(p, buf);
    }
    return (vf_now_s() - t0) * 1e6 / (double)frames;
}

int main(int argc, char **argv)
{
    long frames = 3000;
    int repeat = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
    }
    static float in[400 * VF_FRAME_SIZE];
    speechish(in, 400 * VF_FRAME_SIZE);

    static Chain chain;
    static Pipeline p;
    pipeline_init(&p);
    chain_build(&chain, &p);
    pipeline_start(&p, VF_SAMPLE_RATE);
    static VfSink ref;                                   /* a silent loopback for the echo canceller */
    vf_sink_init(&ref, 480);
    fx_aec_attach(&chain.aec, &ref);
    float zeros[VF_FRAME_SIZE] = { 0 };

    printf("%-34s %10s %10s\n", "configuration", "us/frame", "% of core");
    double base = 0;
    const int n = p.count;
    for (int cfg = -3; cfg < n; cfg++) {
        /* cfg -3: default, -2: low-latency, -1: everything on, >= 0: that optional effect added */
        for (int i = 0; i < n; i++) pipeline_set_bypass(&p, i, p.fx[i]->default_bypass);
        pipeline_set_param(&p, pipeline_find_fx(&p, "rnnoise"), RNNOISE_PARAM_LOWLAT, cfg == -2 ? 1.0f : 0.0f);
        pipeline_set_param(&p, pipeline_find_fx(&p, "limiter"), LIM_LOOKAHEAD_MS, cfg == -2 ? 0.0f : 1.5f);
        char name[64] = "default chain";
        if (cfg == -2) snprintf(name, sizeof name, "default + low-latency mode");
        else if (cfg == -1) { for (int i = 0; i < n; i++) pipeline_set_bypass(&p, i, 0); snprintf(name, sizeof name, "everything on"); }
        else if (cfg >= 0) {
            if (!p.fx[cfg]->default_bypass) continue;
            pipeline_set_bypass(&p, cfg, 0);
            snprintf(name, sizeof name, "+ %s", p.fx[cfg]->name);
            if (!strcmp(p.fx[cfg]->name, "voice")) pipeline_set_param(&p, cfg, 0, 4.0f);   /* pitch shifting is the costly mode */
        }
        if (cfg == -1) pipeline_set_param(&p, pipeline_find_fx(&p, "voice"), 0, 4.0f);
        for (int w = 0; w < 100; w++) { float b[VF_FRAME_SIZE]; vf_sink_write(&ref, zeros, VF_FRAME_SIZE); memcpy(b, in, sizeof b); pipeline_process(&p, b); }   /* warm up, apply params */
        double best = 1e30;
        for (int r = 0; r < repeat; r++) {
            const double us = run(&p, in, frames);
            if (us < best) best = us;
        }
        if (cfg == -3) base = best;
        printf("%-34s %10.1f %9.2f%%%s\n", name, best, best / 100.0, cfg >= 0 ? "" : "");
        (void)base;
    }
    pipeline_stop(&p);
    vf_sink_free(&ref);
    return 0;
}
