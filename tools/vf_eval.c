/* vf-eval: objective measurements for VoiceFilter test runs.
 *
 *   vf-eval mix   CLEAN NOISE SNR_DB OUT      mix speech with (looped) noise at an exact SNR
 *   vf-eval eval  CLEAN MIX PROCESSED [NAME]  metrics against a clean reference
 *   vf-eval noref INPUT PROCESSED [NAME]      level statistics when no reference exists
 *
 * SNR in `mix` is measured over speech-active 10 ms frames of CLEAN.
 * All files are read as 48 kHz mono. Not real-time code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/wavio.h"

#define SR 48000
#define FR 480                      /* analysis frame, 10 ms */
#define ACTIVE_DB   (-40.0)         /* frame active if within 40 dB of loudest frame */
#define QUIET_DB    (-60.0)         /* frame quiet if 60 dB below loudest frame */
#define GUARD       10              /* quiet frames this far from speech count as pause */
#define EDGE        5               /* onset/offset window: 5 frames = 50 ms */
#define MAX_LAG     4800            /* search delay up to 100 ms */

static double db10(double x) { return 10.0 * log10(x > 1e-20 ? x : 1e-20); }

static float *load(const char *p, long *n)
{
    float *b = vf_wav_read_mono(p, SR, n);
    if (!b) { fprintf(stderr, "cannot read %s\n", p); exit(1); }
    return b;
}

static double *frame_energy(const float *x, long n, long *nf)
{
    *nf = n / FR;
    double *e = (double *)calloc((size_t)*nf + 1, sizeof *e);
    for (long f = 0; f < *nf; f++) {
        double s = 0;
        for (int i = 0; i < FR; i++) { double v = x[f * FR + i]; s += v * v; }
        e[f] = s / FR;
    }
    return e;
}

/* Lag (samples) at which `y` best matches `x`, i.e. y[i + lag] ~ x[i]. Uses the
 * loudest 1 s of x to keep the search cheap. */
static long find_delay(const float *x, long nx, const float *y, long ny, double *corr_out)
{
    long win = SR, best_start = 0;
    double best_e = -1;
    for (long s = 0; s + win + MAX_LAG < nx && s + win + MAX_LAG < ny; s += SR / 4) {
        double e = 0;
        for (long i = 0; i < win; i += 4) e += (double)x[s + i] * x[s + i];
        if (e > best_e) { best_e = e; best_start = s; }
    }
    if (best_e < 0) { *corr_out = 0; return 0; }
    long best = 0;
    double bc = -1e300, ex = 0;
    for (long i = 0; i < win; i++) ex += (double)x[best_start + i] * x[best_start + i];
    for (long lag = 0; lag <= MAX_LAG; lag++) {
        double c = 0, ey = 0;
        for (long i = 0; i < win; i++) {
            double a = x[best_start + i], b = y[best_start + i + lag];
            c += a * b; ey += b * b;
        }
        c = c / sqrt(ex * ey + 1e-30);
        if (c > bc) { bc = c; best = lag; }
    }
    *corr_out = bc;
    return best;
}

static int cmd_mix(int argc, char **argv)
{
    if (argc != 6) return 2;
    long nc, nn;
    float *c = load(argv[2], &nc), *nz = load(argv[3], &nn);
    double snr = atof(argv[4]);
    long nf;
    double *e = frame_energy(c, nc, &nf), mx = 0, ps = 0;
    long na = 0;
    for (long f = 0; f < nf; f++) if (e[f] > mx) mx = e[f];
    for (long f = 0; f < nf; f++) if (db10(e[f]) > db10(mx) + ACTIVE_DB) { ps += e[f]; na++; }
    ps /= na ? na : 1;
    double pn = 0;
    for (long i = 0; i < nn; i++) pn += (double)nz[i] * nz[i];
    pn /= nn;
    double g = sqrt(ps / (pn * pow(10.0, snr / 10.0)));
    float *m = (float *)malloc((size_t)nc * sizeof *m);
    float peak = 0;
    for (long i = 0; i < nc; i++) {
        m[i] = c[i] + (float)(g * nz[i % nn]);
        if (fabsf(m[i]) > peak) peak = fabsf(m[i]);
    }
    if (peak > 0.99f) {                         /* keep headroom, same scale for both */
        fprintf(stderr, "warning: mix peaks at %.2f, reference will not match levels\n", (double)peak);
    }
    int rc = vf_wav_write(argv[5], m, nc, 1, SR);
    printf("mixed %s @ %.1f dB SNR (noise gain %.4f, peak %.3f)\n", argv[5], snr, g, (double)peak);
    return rc;
}

static int cmd_eval(int argc, char **argv)
{
    /* eval CLEAN MIX PROC [NAME] [REF] [--list]
     * REF = CLEAN processed by the same chain. With REF, SDR and onset/offset losses
     * are measured against what the chain does to noise-free speech (phase-safe);
     * without it, against raw CLEAN (energy-based measures only are meaningful). */
    if (argc < 5) return 2;
    const char *name = argc > 5 ? argv[5] : argv[4];
    int list = 0;
    const char *ref_path = NULL;
    for (int a = 6; a < argc; a++) {
        if (!strcmp(argv[a], "--list")) list = 1;
        else ref_path = argv[a];
    }
    long nc, nm, np, nr = 0;
    float *c = load(argv[2], &nc), *m = load(argv[3], &nm), *p = load(argv[4], &np);
    float *ref = ref_path ? load(ref_path, &nr) : c;
    if (!ref_path) nr = nc;
    long n = nc < nm ? nc : nm;
    if (nr < n) n = nr;

    double xc;
    long d = find_delay(ref, n, p, np, &xc);
    if (n + d > np) n = np - d;

    long nf;
    double *ec = frame_energy(c, n, &nf), mx = 0;
    for (long f = 0; f < nf; f++) if (ec[f] > mx) mx = ec[f];
    char *act = (char *)calloc((size_t)nf, 1), *pause = (char *)calloc((size_t)nf, 1);
    for (long f = 0; f < nf; f++) act[f] = db10(ec[f]) > db10(mx) + ACTIVE_DB;
    for (long f = 0; f < nf; f++) {
        if (db10(ec[f]) > db10(mx) + QUIET_DB) continue;
        int near = 0;
        for (long k = f - GUARD; k <= f + GUARD && !near; k++)
            if (k >= 0 && k < nf && act[k]) near = 1;
        pause[f] = !near;
    }

    /* gains: energy ratio (phase-insensitive) for levels/onsets, least squares for SDR */
    double cp = 0, cc = 0, pp = 0;
    for (long f = 0; f < nf; f++) if (act[f])
        for (int i = 0; i < FR; i++) {
            long k = f * FR + i;
            cp += (double)ref[k] * p[k + d]; cc += (double)ref[k] * ref[k]; pp += (double)p[k + d] * p[k + d];
        }
    double gls = cc > 0 ? cp / cc : 0;
    double g = cc > 0 ? sqrt(pp / cc) : 0;

    double sig = 0, nin = 0, dist = 0, pin = 0, pout = 0, rsig = 0;
    long npause = 0, nact = 0;
    for (long f = 0; f < nf; f++) {
        for (int i = 0; i < FR; i++) {
            long k = f * FR + i;
            double cv = c[k], mv = m[k], pv = p[k + d], rv = ref[k];
            if (act[f]) {
                sig += cv * cv; nin += (mv - cv) * (mv - cv);
                rsig += rv * rv;
                dist += (pv - gls * rv) * (pv - gls * rv);
            }
            if (pause[f]) { pin += mv * mv; pout += pv * pv; }
        }
        nact += act[f]; npause += pause[f];
    }
    /* level change vs the raw input speech (independent of REF) */
    double raw_gain_db = 0;
    {
        double ec = 0, ep = 0;
        for (long f = 0; f < nf; f++) if (act[f])
            for (int i = 0; i < FR; i++) {
                long k = f * FR + i;
                ec += (double)c[k] * c[k]; ep += (double)p[k + d] * p[k + d];
            }
        raw_gain_db = db10(ep) - db10(ec);
    }

    /* onsets/offsets: active run preceded/followed by >= 20 inactive frames */
    double on_sum = 0, on_worst = 0, off_sum = 0, off_worst = 0;
    int non = 0, noff = 0;
    for (long f = 20; f + EDGE < nf; f++) {
        if (act[f] && !act[f - 1]) {
            int quiet = 1;
            for (long k = f - 20; k < f; k++) if (act[k]) quiet = 0;
            if (quiet) {
                double r = 0, t = 0;
                for (long k = f * FR; k < (f + EDGE) * FR; k++) {
                    r += g * g * (double)ref[k] * ref[k]; t += (double)p[k + d] * p[k + d];
                }
                double v = db10(t) - db10(r);
                on_sum += v; non++;
                if (v < on_worst) on_worst = v;
                if (list) printf("  onset  @ %6.2f s: %+6.1f dB\n", (double)f * FR / SR, v);
            }
        }
        if (act[f] && f + 20 < nf && !act[f + 1]) {
            int quiet = 1;
            for (long k = f + 1; k <= f + 20; k++) if (act[k]) quiet = 0;
            if (quiet && f >= EDGE) {
                double r = 0, t = 0;
                for (long k = (f - EDGE + 1) * FR; k < (f + 1) * FR; k++) {
                    r += g * g * (double)ref[k] * ref[k]; t += (double)p[k + d] * p[k + d];
                }
                double v = db10(t) - db10(r);
                off_sum += v; noff++;
                if (v < off_worst) off_worst = v;
                if (list) printf("  offset @ %6.2f s: %+6.1f dB\n", (double)(f + 1) * FR / SR, v);
            }
        }
    }

    float peak = 0;
    long clip = 0, bad = 0;
    for (long i = 0; i < np; i++) {
        if (!isfinite(p[i])) { bad++; continue; }
        float a = fabsf(p[i]);
        if (a > peak) peak = a;
        if (a >= 0.999f) clip++;
    }

    double snr_in = db10(sig) - db10(nin);
    double sdr_out = db10(gls * gls * rsig) - db10(dist);
    double pz_in = npause ? db10(pin / ((double)npause * FR)) : 0;
    double pz_out = npause ? db10(pout / ((double)npause * FR)) : 0;
    printf("%-22s delay %5.1f ms (r=%.2f) | SNR in %5.1f -> SDR%s %5.1f dB | pause %6.1f -> %6.1f dBFS"
           " (-%5.1f) | gain %+5.1f dB | onset %+5.1f (worst %+6.1f) | offset %+5.1f (worst %+6.1f)"
           " | peak %.3f clip %ld nan %ld\n",
           name, 1000.0 * d / SR, xc, snr_in, ref_path ? "/ref" : "/raw", sdr_out, pz_in, pz_out,
           pz_in - pz_out, raw_gain_db,
           non ? on_sum / non : 0.0, on_worst, noff ? off_sum / noff : 0.0, off_worst,
           (double)peak, clip, bad);
    printf("CSV,%s,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%ld,%ld,%ld,%ld,%d,%d\n",
           name, 1000.0 * d / SR, xc, snr_in, sdr_out, pz_in, pz_out, pz_in - pz_out,
           raw_gain_db, non ? on_sum / non : 0.0, on_worst,
           noff ? off_sum / noff : 0.0, off_worst, (double)peak, clip, bad, nact, npause, non, noff);
    return 0;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(double *v, long n, double q) { return v[(long)(q * (n - 1))]; }

static int cmd_noref(int argc, char **argv)
{
    if (argc < 4) return 2;
    const char *name = argc > 4 ? argv[4] : argv[3];
    long ni, np;
    float *x = load(argv[2], &ni), *p = load(argv[3], &np);
    long n = ni < np ? ni : np, nf;
    double *ei = frame_energy(x, n, &nf), *eo = frame_energy(p, n, &nf);
    long silent = 0;
    for (long f = 0; f < nf; f++) {
        ei[f] = db10(ei[f]); eo[f] = db10(eo[f]);
        if (eo[f] < -60.0) silent++;
    }
    qsort(ei, (size_t)nf, sizeof *ei, cmp_d);
    qsort(eo, (size_t)nf, sizeof *eo, cmp_d);
    printf("%-28s frame level p10 %6.1f -> %6.1f dBFS | p50 %6.1f -> %6.1f | p95 %6.1f -> %6.1f |"
           " output below -60 dBFS: %4.1f%% of time\n",
           name, pct(ei, nf, .10), pct(eo, nf, .10), pct(ei, nf, .50), pct(eo, nf, .50),
           pct(ei, nf, .95), pct(eo, nf, .95), 100.0 * silent / (nf ? nf : 1));
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 2;
    if (argc >= 2 && !strcmp(argv[1], "mix"))   rc = cmd_mix(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "eval"))  rc = cmd_eval(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "noref")) rc = cmd_noref(argc, argv);
    if (rc == 2)
        fprintf(stderr, "usage:\n  vf-eval mix CLEAN NOISE SNR_DB OUT\n"
                        "  vf-eval eval CLEAN MIX PROCESSED [NAME] [REF] [--list]\n"
                        "  vf-eval noref INPUT PROCESSED [NAME]\n");
    return rc;
}
