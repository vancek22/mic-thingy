/* vf-synth: detailed synthetic near-end disturbances and echo paths with known event times, and a scorer.
 *
 *   vf-synth gen [--len S] [--seed N] [--keys DB] [--mouse DB] [--breath DB] [--mouth DB] [--bump DB]
 *                [--kb 0..3|-1] [--room 0..1] [--density 0.3..2] [--avoid voice.csv] near.wav events.csv
 *       Event classes (peak dBFS per class, "off" = none): keys (4 keyboard types: clicky, linear, membrane, laptop,
 *       space/enter with stabiliser rattle, key up clicks), mouse (button click + release, scroll wheel ticks, sliding on
 *       the pad), breath (nasal / mouth inhale + exhale, sigh, plosive puff), mouth (lip smack, tongue click, swallow,
 *       throat clear), bump (desk thump, chair creak). --room mixes a small-room reflection network into the result.
 *   vf-synth voicemap speech.wav voice.csv       speech segments (class "voice") for --avoid and for the scorer
 *   vf-synth echo OUT.wav REF.wav [--delay MS] [--gain DB] [--room 0..1] [--nl DRIVE] [--drift PPM] [--lp HZ]
 *       the reference as the microphone would hear it: delay, speaker nonlinearity (tanh), clock drift, band limit,
 *       early reflections (inside the 43 ms the canceller covers) and a diffuse tail (beyond it).
 *   vf-synth silence OUT.wav SECONDS
 *   vf-synth mix OUT.wav BASE.wav [ADD.wav:DB]...     OUT = BASE + sum of ADD scaled by DB
 *   vf-synth score events.csv IN.wav OUT.wav [REF.wav]
 *       per class: level change in the event windows (energy and peak, dB, negative = removed), share of events still
 *       audible in the output; outside events: level change and the correlation of the output with REF (the system
 *       sound in the output); class "voice": level change and onset (first 50 ms) loss. Prints "CSV,..." lines too.
 * 48 kHz mono. Not real-time code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/wavio.h"

#define SR 48000
#define PI 3.14159265358979
#define MAX_EV 8192

static unsigned long long g_rng = 88172645463325252ULL;
static double rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) / 9007199254740992.0; }
static double rr(double a, double b) { return a + (b - a) * rnd(); }
static double gauss(void) { double s = 0; for (int i = 0; i < 6; i++) s += rnd(); return (s - 3.0) * 1.41421; }
static double db2lin(double d) { return pow(10.0, d / 20.0); }

typedef struct { char cls[16]; double t0, t1; } Ev;
static Ev g_ev[MAX_EV];
static int g_nev;
static void add_ev(const char *cls, double t0, double t1)
{
    if (g_nev >= MAX_EV) return;
    snprintf(g_ev[g_nev].cls, sizeof g_ev[0].cls, "%s", cls);
    g_ev[g_nev].t0 = t0; g_ev[g_nev].t1 = t1; g_nev++;
}

/* ------------------------------------------------------------------ sound atoms */
/* decaying noise through a 2-pole resonator */
static void res(float *x, long n, long at, double f, double q, double tau_ms, double amp)
{
    const long len = (long)(tau_ms * 8.0 * SR / 1000.0);
    const double w = 2 * PI * f / SR, r = exp(-w / (2 * q));
    const double a1 = 2 * r * cos(w), a2 = -r * r;
    double y1 = 0, y2 = 0;
    for (long i = 0; i < len && at + i < n; i++) {
        if (at + i < 0) continue;
        const double e = exp(-(double)i / (tau_ms * 1e-3 * SR));
        const double y = (1 - r) * gauss() * e * 4.0 + a1 * y1 + a2 * y2;
        y2 = y1; y1 = y;
        x[at + i] += (float)(amp * y);
    }
}
/* decaying sine with a falling pitch: body of a key bottoming out, a desk thump */
static void thump(float *x, long n, long at, double f, double tau_ms, double amp)
{
    const long len = (long)(tau_ms * 6.0 * SR / 1000.0);
    double ph = 0;
    for (long i = 0; i < len && at + i < n; i++) {
        const double t = (double)i / SR;
        ph += 2 * PI * f * (1.0 + 0.25 * exp(-t / 0.006)) / SR;
        if (at + i >= 0) x[at + i] += (float)(amp * sin(ph) * exp(-t / (tau_ms * 1e-3)));
    }
}
/* band-limited noise burst with a skewed raised-cosine envelope (breath, sliding, swallow) */
static void nburst(float *x, long n, long at, double dur, double lo, double hi, double amp, double skew, double am_hz)
{
    const long len = (long)(dur * SR);
    const double alo = exp(-2 * PI * lo / SR), ahi = exp(-2 * PI * hi / SR);
    double hp = 0, lp = 0, prev = 0;
    for (long i = 0; i < len && at + i < n; i++) {
        const double ph = (double)i / (double)len;
        const double u = ph < skew ? 0.5 * ph / skew : 0.5 + 0.5 * (ph - skew) / (1 - skew);
        const double env = pow(sin(PI * u), 2.0);
        const double g = gauss();
        lp = (1 - ahi) * g + ahi * lp;                    /* low-pass at hi */
        hp = alo * (hp + lp - prev); prev = lp;           /* high-pass at lo */
        const double am = am_hz > 0 ? 0.6 + 0.4 * sin(2 * PI * am_hz * (double)i / SR + 1.0) : 1.0;
        if (at + i >= 0) x[at + i] += (float)(amp * env * am * hp * 3.0);
    }
}

/* ------------------------------------------------------------------ the classes */
static void key_one(float *x, long n, long at, int kb, int big, double amp)
{
    const double f = rr(0.9, 1.15);
    if (big) {                                            /* space bar / enter: heavy body + stabiliser rattle */
        thump(x, n, at, rr(110, 180), 22, amp * 1.3);
        res(x, n, at, 1800 * f, 1.5, 5, amp * 0.8);
        for (int k = 0; k < 4; k++) res(x, n, at + (long)((0.006 + 0.007 * k + rr(0, 0.003)) * SR), 3200 * f, 3.0, 1.5, amp * 0.35);
        res(x, n, at + (long)(rr(0.08, 0.13) * SR), 2400 * f, 2.0, 3, amp * 0.5);
        return;
    }
    switch (kb) {
    case 0:                                               /* clicky switch: click leaf + bottom out + top out */
        res(x, n, at, 4800 * f, 3.0, 1.3, amp);
        thump(x, n, at + 120, rr(250, 380), 12, amp * 0.7);
        res(x, n, at + (long)(rr(0.07, 0.11) * SR), 4200 * f, 3.0, 1.2, amp * 0.55);
        break;
    case 1:                                               /* linear, thocky: low body, dull top */
        thump(x, n, at, rr(120, 220), 16, amp * 1.0);
        res(x, n, at, 1100 * f, 1.2, 6, amp * 0.5);
        res(x, n, at + (long)(rr(0.07, 0.12) * SR), 1500 * f, 1.5, 4, amp * 0.3);
        break;
    case 2:                                               /* membrane: soft, broad */
        res(x, n, at, 2600 * f, 0.9, 4, amp * 0.8);
        thump(x, n, at, rr(300, 450), 6, amp * 0.5);
        res(x, n, at + (long)(rr(0.06, 0.1) * SR), 2200 * f, 1.0, 3, amp * 0.3);
        break;
    default:                                              /* laptop: short, shallow, brighter */
        res(x, n, at, 3600 * f, 1.6, 2.2, amp * 0.9);
        thump(x, n, at, rr(400, 600), 4, amp * 0.35);
        res(x, n, at + (long)(rr(0.05, 0.08) * SR), 3200 * f, 1.8, 1.8, amp * 0.3);
    }
}
static double keys_burst(float *x, long n, double t, int kb, double amp)
{
    const int nk = 4 + (int)rr(0, 14);
    const double speed = rr(0.07, 0.2);                   /* typist */
    double tt = t;
    for (int k = 0; k < nk; k++) {
        const int big = rnd() < 0.1;
        key_one(x, n, (long)(tt * SR), kb, big, amp * db2lin(rr(-4, 3)));
        tt += speed * rr(0.6, 1.5) + (big ? 0.1 : 0.0);
    }
    add_ev("keys", t, tt + 0.2);
    return tt + 0.2;
}
static double mouse_event(float *x, long n, double t, double amp)
{
    const double r = rnd();
    double e = t + 0.25;
    if (r < 0.55) {                                       /* button click + release (single or double) */
        const int dbl = rnd() < 0.3;
        for (int k = 0; k <= dbl; k++) {
            const long at = (long)((t + k * rr(0.12, 0.2)) * SR);
            const double f = rr(2500, 4500);
            res(x, n, at, f, 3.0, 1.2, amp);
            thump(x, n, at, rr(350, 500), 5, amp * 0.4);
            res(x, n, at + (long)(rr(0.06, 0.1) * SR), f * 1.1, 3.0, 1.1, amp * 0.7);
            e = t + 0.3 + k * 0.2;
        }
    } else if (r < 0.8) {                                 /* scroll wheel ticks */
        const int nt = 3 + (int)rr(0, 10);
        double tt = t;
        for (int k = 0; k < nt; k++) { res(x, n, (long)(tt * SR), rr(1200, 1800), 2.5, 1.6, amp * 0.55 * db2lin(rr(-2, 2))); thump(x, n, (long)(tt * SR), 180, 3, amp * 0.15); tt += rr(0.025, 0.06); }
        e = tt + 0.1;
    } else {                                              /* sliding on the pad */
        const double d = rr(0.3, 0.9);
        nburst(x, n, (long)(t * SR), d, 1800, 7000, amp * 0.12, 0.4, rr(6, 14));
        e = t + d + 0.05;
    }
    add_ev("mouse", t, e);
    return e;
}
static double breath_event(float *x, long n, double t, double amp)
{
    const double r = rnd();
    double e;
    if (r < 0.35) {                                       /* nasal inhale + exhale */
        nburst(x, n, (long)(t * SR), rr(0.9, 1.4), 300, 2200, amp * 0.8, 0.45, 0);
        nburst(x, n, (long)((t + 1.5) * SR), rr(1.0, 1.6), 250, 1800, amp * 0.9, 0.35, 0);
        e = t + 3.2;
    } else if (r < 0.65) {                                /* mouth breath: hissier */
        nburst(x, n, (long)(t * SR), rr(0.6, 1.0), 900, 6000, amp * 0.6, 0.5, 0);
        nburst(x, n, (long)((t + 1.1) * SR), rr(0.7, 1.1), 600, 4500, amp * 0.7, 0.3, 0);
        e = t + 2.3;
    } else if (r < 0.85) {                                /* sigh */
        nburst(x, n, (long)(t * SR), rr(1.2, 2.0), 200, 3000, amp * 1.0, 0.2, 0);
        e = t + 2.1;
    } else {                                              /* plosive puff right at the capsule */
        thump(x, n, (long)(t * SR), rr(70, 110), 28, amp * 1.4);
        nburst(x, n, (long)(t * SR), 0.18, 200, 2500, amp * 0.7, 0.15, 0);
        e = t + 0.4;
    }
    add_ev("breath", t, e);
    return e;
}
static double mouth_event(float *x, long n, double t, double amp)
{
    const double r = rnd();
    double e = t + 0.3;
    if (r < 0.35) {                                       /* lip smack */
        long at = (long)(t * SR);
        res(x, n, at, rr(1200, 2500), 2.0, 3, amp);
        res(x, n, at + (long)(rr(0.008, 0.02) * SR), rr(1800, 3200), 2.0, 2, amp * 0.8);
        thump(x, n, at, 700, 8, amp * 0.3);
    } else if (r < 0.6) {                                 /* tongue click */
        res(x, n, (long)(t * SR), rr(1300, 1900), 4.0, 2.2, amp);
        thump(x, n, (long)(t * SR), 600, 10, amp * 0.5);
    } else if (r < 0.85) {                                /* swallow: low gurgle */
        nburst(x, n, (long)(t * SR), rr(0.3, 0.5), 80, 500, amp * 0.8, 0.5, rr(14, 24));
        e = t + 0.6;
    } else {                                              /* throat clear: buzzy, voice-like */
        const long at = (long)(t * SR), len = (long)(0.4 * SR);
        double ph = 0;
        for (long i = 0; i < len && at + i < n; i++) {
            const double u = (double)i / len, env = sin(PI * u) * sin(PI * u);
            ph += 2 * PI * (110 + 40 * u) / SR;
            const double saw = fmod(ph / (2 * PI), 1.0) * 2 - 1;
            x[at + i] += (float)(amp * 0.7 * env * (0.6 * saw + 0.5 * gauss() * 0.3));
        }
        e = t + 0.55;
    }
    add_ev("mouth", t, e);
    return e;
}
static double bump_event(float *x, long n, double t, double amp)
{
    double e;
    if (rnd() < 0.6) {                                    /* desk thump (cup, elbow) */
        thump(x, n, (long)(t * SR), rr(55, 120), 45, amp);
        res(x, n, (long)(t * SR), rr(500, 900), 2.0, 12, amp * 0.25);
        e = t + 0.4;
    } else {                                              /* chair creak: sweeping tone + noise */
        const long at = (long)(t * SR), len = (long)(rr(0.3, 0.6) * SR);
        double ph = 0;
        const double f0 = rr(350, 700), f1 = f0 * rr(0.7, 1.4);
        for (long i = 0; i < len && at + i < n; i++) {
            const double u = (double)i / len, env = sin(PI * u);
            ph += 2 * PI * (f0 + (f1 - f0) * u) / SR;
            x[at + i] += (float)(amp * 0.35 * env * (sin(ph) + 0.4 * sin(2 * ph) + 0.3 * gauss()));
        }
        e = t + (double)len / SR + 0.1;
    }
    add_ev("bump", t, e);
    return e;
}

/* ------------------------------------------------------------------ room */
/* Schroeder-style room: early reflections + 4 damped combs + 2 all-passes. y = dry + wet * room(x) */
static void room_fx(float *x, long n, double wet, double rt60)
{
    if (wet <= 0) return;
    static const double cd[4] = { 0.0297, 0.0371, 0.0411, 0.0437 }, ap[2] = { 0.0050, 0.0017 };
    float *dry = (float *)malloc((size_t)n * sizeof(float)), *acc = (float *)calloc((size_t)n, sizeof(float));
    memcpy(dry, x, (size_t)n * sizeof(float));
    static const double et[5] = { 0.0031, 0.0077, 0.0129, 0.0187, 0.0253 }, eg[5] = { 0.5, 0.38, 0.3, 0.2, 0.15 };
    for (int k = 0; k < 5; k++) { const long d = (long)(et[k] * SR); for (long i = d; i < n; i++) acc[i] += (float)eg[k] * dry[i - d]; }
    for (int c = 0; c < 4; c++) {
        const long d = (long)(cd[c] * SR);
        const double g = pow(10.0, -3.0 * cd[c] / rt60);
        float *buf = (float *)calloc((size_t)d, sizeof(float));
        double lp = 0;
        long p = 0;
        for (long i = 0; i < n; i++) {
            const double y = buf[p];
            lp += 0.55 * (y - lp);                        /* damping: high frequencies die first */
            buf[p] = (float)(dry[i] + g * lp);
            p = (p + 1) % d;
            acc[i] += (float)(0.25 * y);
        }
        free(buf);
    }
    for (int a = 0; a < 2; a++) {
        const long d = (long)(ap[a] * SR);
        float *bx = (float *)calloc((size_t)d, sizeof(float)), *by = (float *)calloc((size_t)d, sizeof(float));
        long p = 0;
        for (long i = 0; i < n; i++) {
            const float in = acc[i], y = -0.6f * in + bx[p] + 0.6f * by[p];
            bx[p] = in; by[p] = y; p = (p + 1) % d;
            acc[i] = y;
        }
        free(bx); free(by);
    }
    for (long i = 0; i < n; i++) x[i] = dry[i] + (float)wet * acc[i];
    free(dry); free(acc);
}

/* ------------------------------------------------------------------ commands */
static double opt_db(const char *v) { return strcmp(v, "off") ? atof(v) : -999; }

static int cmd_gen(int argc, char **argv)
{
    double len = 30, lv[5] = { -26, -24, -34, -32, -28 }, room = 0.25, density = 1.0;
    int seed = 1, kbfix = -1;
    const char *out = NULL, *csv = NULL, *avoid = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--len") && i + 1 < argc) len = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) lv[0] = opt_db(argv[++i]);
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) lv[1] = opt_db(argv[++i]);
        else if (!strcmp(argv[i], "--breath") && i + 1 < argc) lv[2] = opt_db(argv[++i]);
        else if (!strcmp(argv[i], "--mouth") && i + 1 < argc) lv[3] = opt_db(argv[++i]);
        else if (!strcmp(argv[i], "--bump") && i + 1 < argc) lv[4] = opt_db(argv[++i]);
        else if (!strcmp(argv[i], "--kb") && i + 1 < argc) kbfix = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc) room = atof(argv[++i]);
        else if (!strcmp(argv[i], "--density") && i + 1 < argc) density = atof(argv[++i]);
        else if (!strcmp(argv[i], "--avoid") && i + 1 < argc) avoid = argv[++i];
        else if (!out) out = argv[i]; else csv = argv[i];
    }
    if (!out || !csv) { fprintf(stderr, "usage: vf-synth gen [options] near.wav events.csv\n"); return 2; }
    g_rng ^= (unsigned long long)seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) rnd();
    const long n = (long)(len * SR);
    float *x = (float *)calloc((size_t)n, sizeof *x);
    double av0[256], av1[256];
    int nav = 0;
    if (avoid) {
        FILE *f = fopen(avoid, "r");
        char line[256], cls[16];
        double a, b;
        while (f && fgets(line, sizeof line, f) && nav < 256) if (sscanf(line, "%15[^,],%lf,%lf", cls, &a, &b) == 3) { av0[nav] = a; av1[nav] = b; nav++; }
        if (f) fclose(f);
    }
    const double w[5] = { 0.38, 0.2, 0.2, 0.14, 0.08 };
    double wsum = 0;
    for (int c = 0; c < 5; c++) if (lv[c] > -900) wsum += w[c];
    int cnt[5] = { 0 };
    double t = 0.8;
    while (t < len - 3.5) {
        const int ev_before = g_nev;
        double r = rnd() * wsum, acc = 0;
        int c = 0;
        for (; c < 5; c++) { if (lv[c] <= -900) continue; acc += w[c]; if (r <= acc) break; }
        if (c > 4) c = 4;
        const double amp = 0.12;
        double e = t;
        switch (c) {
        case 0: e = keys_burst(x, n, t, kbfix >= 0 ? kbfix : (int)rr(0, 4), amp); break;
        case 1: e = mouse_event(x, n, t, amp); break;
        case 2: e = breath_event(x, n, t, amp * 0.5); break;
        case 3: e = mouth_event(x, n, t, amp); break;
        default: e = bump_event(x, n, t, amp); break;
        }
        /* a sound that touches a voice segment (+ margin) is taken back: erased and forgotten */
        int clash = 0;
        for (int a = 0; a < nav; a++) if (e + 0.3 > av0[a] && t - 0.15 < av1[a]) { clash = 1; break; }
        if (clash) {
            for (long i = (long)(t * SR) - 2400 > 0 ? (long)(t * SR) - 2400 : 0; i < (long)((e + 0.5) * SR) && i < n; i++) x[i] = 0.0f;
            g_nev = ev_before;
            t += 0.35;
            continue;
        }
        cnt[c]++;
        t = e + (1.0 + rnd() * 1.6) / density;
    }
    /* each class scaled so its loudest event reaches the requested peak */
    static const char *names[5] = { "keys", "mouse", "breath", "mouth", "bump" };
    for (int c = 0; c < 5; c++) {
        if (lv[c] <= -900) continue;
        double pk = 0;
        for (int e = 0; e < g_nev; e++) if (!strcmp(g_ev[e].cls, names[c]))
            for (long i = (long)(g_ev[e].t0 * SR); i < (long)(g_ev[e].t1 * SR) && i < n; i++) if (fabs(x[i]) > pk) pk = fabs(x[i]);
        if (pk > 0)
            for (int e = 0; e < g_nev; e++) if (!strcmp(g_ev[e].cls, names[c]))
                for (long i = (long)(g_ev[e].t0 * SR); i < (long)(g_ev[e].t1 * SR) && i < n; i++) x[i] *= (float)(db2lin(lv[c]) / pk);
    }
    room_fx(x, n, room, 0.25);
    FILE *f = fopen(csv, "w");
    if (!f) return 1;
    for (int e = 0; e < g_nev; e++) fprintf(f, "%s,%.3f,%.3f\n", g_ev[e].cls, g_ev[e].t0, g_ev[e].t1);
    fclose(f);
    int rc = vf_wav_write(out, x, n, 1, SR);
    printf("%d events (keys %d, mouse %d, breath %d, mouth %d, bump %d) over %.1f s, room %.2f -> %s, %s\n", g_nev, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], len, room, out, csv);
    free(x);
    return rc;
}

static int cmd_voicemap(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vf-synth voicemap speech.wav voice.csv\n"); return 2; }
    long n;
    float *x = vf_wav_read_mono(argv[0], SR, &n);
    if (!x) { fprintf(stderr, "cannot read %s\n", argv[0]); return 1; }
    const long nf = n / 480;
    double *e = (double *)malloc((size_t)nf * sizeof *e), mx = 0;
    for (long f = 0; f < nf; f++) { double s = 0; for (int i = 0; i < 480; i++) s += (double)x[f * 480 + i] * x[f * 480 + i]; e[f] = s / 480; if (e[f] > mx) mx = e[f]; }
    FILE *o = fopen(argv[1], "w");
    if (!o) return 1;
    long start = -1, last = -1;
    int cnt = 0;
    for (long f = 0; f <= nf; f++) {
        const int act = f < nf && 10 * log10(e[f] / mx + 1e-20) > -38.0;
        if (act) { if (start < 0) start = f; last = f; }
        else if (start >= 0 && f - last > 20) {           /* a pause of 200 ms ends the segment */
            if (last - start >= 15) { fprintf(o, "voice,%.3f,%.3f\n", start * 0.01, (last + 1) * 0.01); cnt++; }
            start = -1;
        }
    }
    fclose(o);
    printf("%d voice segments -> %s\n", cnt, argv[1]);
    free(x); free(e);
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    double delay = 50, gain = -20, room = 0.4, nl = 0, drift = 0, lp = 9000;
    const char *out = NULL, *ref = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = atof(argv[++i]);
        else if (!strcmp(argv[i], "--room") && i + 1 < argc) room = atof(argv[++i]);
        else if (!strcmp(argv[i], "--nl") && i + 1 < argc) nl = atof(argv[++i]);
        else if (!strcmp(argv[i], "--drift") && i + 1 < argc) drift = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lp") && i + 1 < argc) lp = atof(argv[++i]);
        else if (!out) out = argv[i]; else ref = argv[i];
    }
    if (!out || !ref) { fprintf(stderr, "usage: vf-synth echo OUT REF [options]\n"); return 2; }
    long n;
    float *r = vf_wav_read_mono(ref, SR, &n);
    if (!r) { fprintf(stderr, "cannot read %s\n", ref); return 1; }
    float *y = (float *)calloc((size_t)n, sizeof *y);
    const double a = exp(-2 * PI * lp / SR), ah = exp(-2 * PI * 150.0 / SR);
    double lps = 0, hps = 0, prev = 0;
    for (long i = 0; i < n; i++) {
        const double pos = (double)i * (1.0 + drift * 1e-6) - delay * 48.0;       /* clock drift: the echo is read at another rate */
        double v = 0;
        if (pos >= 0 && pos < (double)(n - 1)) { const long k = (long)pos; const double fr = pos - k; v = r[k] * (1 - fr) + r[k + 1] * fr; }
        if (nl > 0) v = tanh(nl * v) / nl;                                         /* driver / amplifier nonlinearity */
        lps = (1 - a) * v + a * lps;                                               /* band limit */
        hps = ah * (hps + lps - prev); prev = lps;                                 /* small drivers: no bass */
        y[i] = (float)(hps * db2lin(gain));
    }
    room_fx(y, n, room, 0.35);                                                      /* reflections, tail beyond the canceller's reach */
    int rc = vf_wav_write(out, y, n, 1, SR);
    printf("echo of %s: delay %.0f ms, gain %.0f dB, room %.2f, nl %.1f, drift %.0f ppm -> %s\n", ref, delay, gain, room, nl, drift, out);
    free(r); free(y);
    return rc;
}

static int cmd_silence(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vf-synth silence OUT SECONDS\n"); return 2; }
    const long n = (long)(atof(argv[1]) * SR);
    float *x = (float *)calloc((size_t)n, sizeof *x);
    int rc = vf_wav_write(argv[0], x, n, 1, SR);
    free(x);
    return rc;
}

static int cmd_mix(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vf-synth mix OUT BASE [ADD:DB]...\n"); return 2; }
    long n;
    float *b = vf_wav_read_mono(argv[1], SR, &n);
    if (!b) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    for (int i = 2; i < argc; i++) {
        char path[1024];
        double db = 0;
        snprintf(path, sizeof path, "%s", argv[i]);
        const int neg = path[0] == '~';                                   /* ~file.wav = subtract it */
        if (neg) memmove(path, path + 1, strlen(path));
        char *c = strrchr(path, ':');
        if (c && c > path + 1 && (c[1] == '-' || c[1] == '+' || (c[1] >= '0' && c[1] <= '9'))) { *c = 0; db = atof(c + 1); }
        long m;
        float *a = vf_wav_read_mono(path, SR, &m);
        if (!a) { fprintf(stderr, "cannot read %s\n", path); return 1; }
        const float g = (float)db2lin(db) * (neg ? -1.0f : 1.0f);
        for (long k = 0; k < n && k < m; k++) b[k] += g * a[k];
        free(a);
    }
    int rc = vf_wav_write(argv[0], b, n, 1, SR);
    free(b);
    return rc;
}

/* ------------------------------------------------------------------ scoring */
static double lvl(const float *x, long a, long b)
{
    double s = 0;
    for (long i = a; i < b; i++) s += (double)x[i] * x[i];
    return 10.0 * log10(s / (double)(b > a ? b - a : 1) + 1e-20);
}
static int cmp_d(const void *a, const void *b) { const double d = *(const double *)a - *(const double *)b; return d < 0 ? -1 : d > 0; }

static int cmd_score(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: vf-synth score events.csv IN OUT [REF]\n"); return 2; }
    long ni, no, nr = 0;
    float *in = vf_wav_read_mono(argv[1], SR, &ni), *out = vf_wav_read_mono(argv[2], SR, &no), *ref = argc > 3 ? vf_wav_read_mono(argv[3], SR, &nr) : NULL;
    if (!in || !out) { fprintf(stderr, "cannot read input/output\n"); return 1; }
    const long n = ni < no ? ni : no;
    char *mark = (char *)calloc((size_t)n, 1);
    Ev ev[MAX_EV];
    int nev = 0;
    FILE *f = fopen(argv[0], "r");
    if (!f) return 1;
    char line[256];
    while (nev < MAX_EV && fgets(line, sizeof line, f)) {
        char cls[16];
        double a, b;
        if (sscanf(line, "%15[^,],%lf,%lf", cls, &a, &b) == 3) { snprintf(ev[nev].cls, sizeof ev[0].cls, "%s", cls); ev[nev].t0 = a; ev[nev].t1 = b; nev++; }
    }
    fclose(f);
    for (int e = 0; e < nev; e++) {
        long a = (long)(ev[e].t0 * SR), b = (long)(ev[e].t1 * SR);
        if (a < 0) a = 0;
        for (long i = a; i < b + SR / 5 && i < n; i++) mark[i] = 1;
    }
    /* output frame levels outside events: the floor that an event has to rise above to be audible */
    double *fl = (double *)malloc((size_t)(n / 480 + 1) * sizeof *fl);
    long nfl = 0;
    double ei = 0, eo = 0, sxy = 0, sxx = 0, syy = 0;
    for (long a = 0; a + 480 <= n; a += 480) {
        int hit = 0;
        for (long i = a; i < a + 480; i++) if (mark[i]) { hit = 1; break; }
        if (hit) continue;
        fl[nfl++] = lvl(out, a, a + 480);
        ei += pow(10.0, lvl(in, a, a + 480) / 10.0);
        eo += pow(10.0, lvl(out, a, a + 480) / 10.0);
        if (ref && a + 480 <= nr) for (long i = a; i < a + 480; i++) { sxy += (double)out[i] * ref[i]; sxx += (double)ref[i] * ref[i]; syy += (double)out[i] * out[i]; }
    }
    double floor_db = -120;
    if (nfl) { qsort(fl, (size_t)nfl, sizeof *fl, cmp_d); floor_db = fl[nfl / 2]; }
    const double aud = floor_db + 8.0 > -70.0 ? floor_db + 8.0 : -70.0;         /* audible: 8 dB over the floor and over -70 dBFS */
    char done[MAX_EV] = { 0 };
    for (int e = 0; e < nev; e++) {
        if (done[e]) continue;
        const char *cls = ev[e].cls;
        double sin_ = 0, sout = 0, pk_att = 0, on_in = 0, on_out = 0;
        long cnt = 0;
        int ne = 0, leak = 0;
        for (int g = e; g < nev; g++) {
            if (strcmp(ev[g].cls, cls)) continue;
            done[g] = 1;
            long a = (long)(ev[g].t0 * SR), b = (long)(ev[g].t1 * SR);
            if (a < 0) a = 0;
            if (b > n) b = n;
            if (b <= a + 480) continue;
            double pi = 0, po = 0, mf = -200;
            for (long i = a; i < b; i++) { sin_ += (double)in[i] * in[i]; sout += (double)out[i] * out[i]; if (fabs(in[i]) > pi) pi = fabs(in[i]); if (fabs(out[i]) > po) po = fabs(out[i]); }
            for (long k = a; k + 480 <= b + SR / 10 && k + 480 <= n; k += 480) { const double l = lvl(out, k, k + 480); if (l > mf) mf = l; }
            pk_att += 20 * log10((po + 1e-9) / (pi + 1e-9));
            cnt += b - a; ne++;
            if (mf > aud) leak++;
            if (!strcmp(cls, "voice") && a + 2400 <= n) { on_in += lvl(in, a, a + 2400); on_out += lvl(out, a, a + 2400); }
        }
        if (!ne) continue;
        const double att = 10 * log10((sout + 1e-20) / (sin_ + 1e-20));
        if (!strcmp(cls, "voice")) {
            printf("%-7s %3d segments: level %+.1f dB, first 50 ms %+.1f dB (onset loss %.1f dB), peak %+.1f dB\n", cls, ne, att,
                   (on_out - on_in) / ne, att - (on_out - on_in) / ne, pk_att / ne);
            printf("CSV,%s,%d,%.2f,%.2f,%.2f\n", cls, ne, att, pk_att / ne, att - (on_out - on_in) / ne);
        } else {
            printf("%-7s %3d events:   energy %+6.1f dB, peak %+6.1f dB, audible in the output: %d of %d\n", cls, ne, att, pk_att / ne, leak, ne);
            printf("CSV,%s,%d,%.2f,%.2f,%.1f\n", cls, ne, att, pk_att / ne, 100.0 * leak / ne);
        }
        (void)cnt;
    }
    if (nfl) {
        const double d = 10 * log10((eo + 1e-20) / (ei + 1e-20));
        printf("outside events (%ld frames): input %.1f dBFS -> output %.1f dBFS (%+.1f dB), median output frame %.1f dBFS\n", nfl,
               10 * log10(ei / nfl + 1e-20), 10 * log10(eo / nfl + 1e-20), d, floor_db);
        double corr = 0;
        if (ref && sxx > 0 && syy > 0) corr = sxy / sqrt(sxx * syy);
        if (ref) printf("output vs reference outside events: correlation at lag 0 = %.3f\n", corr);
        printf("CSV,outside,%ld,%.2f,%.1f,%.4f\n", nfl, d, floor_db, corr);
    }
    free(fl); free(mark);
    return 0;
}

/* per-second RMS levels (dBFS) of several files side by side */
static int cmd_levels(int argc, char **argv)
{
    long n[8];
    float *x[8];
    long mn = 0;
    if (argc > 8) argc = 8;
    for (int i = 0; i < argc; i++) { x[i] = vf_wav_read_mono(argv[i], SR, &n[i]); if (!x[i]) { fprintf(stderr, "cannot read %s\n", argv[i]); return 1; } if (!i || n[i] < mn) mn = n[i]; }
    printf("  s");
    for (int i = 0; i < argc; i++) printf(" %9.9s", argv[i]);
    printf("\n");
    for (long t = 0; (t + 1) * SR <= mn; t++) {
        printf("%3ld", t);
        for (int i = 0; i < argc; i++) printf(" %9.1f", lvl(x[i], t * SR, (t + 1) * SR));
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vf-synth gen|voicemap|echo|silence|mix|score ...\n"); return 2; }
    if (!strcmp(argv[1], "gen")) return cmd_gen(argc - 2, argv + 2);
    if (!strcmp(argv[1], "voicemap")) return cmd_voicemap(argc - 2, argv + 2);
    if (!strcmp(argv[1], "echo")) return cmd_echo(argc - 2, argv + 2);
    if (!strcmp(argv[1], "silence")) return cmd_silence(argc - 2, argv + 2);
    if (!strcmp(argv[1], "mix")) return cmd_mix(argc - 2, argv + 2);
    if (!strcmp(argv[1], "score")) return cmd_score(argc - 2, argv + 2);
    if (!strcmp(argv[1], "levels")) return cmd_levels(argc - 2, argv + 2);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
