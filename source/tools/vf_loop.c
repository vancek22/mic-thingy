/* vf-loop: records a REAL acoustic echo path. Plays a far-end WAV to a playback device (headphones laid on the
 * microphone) and records, at the same time, the microphone and the WASAPI loopback of that playback device - the
 * same reference the echo canceller gets in the app.
 *
 *   vf-loop --list
 *   vf-loop --in "EPOS" --out "Logitech" --far far.wav --mic mic.wav --ref ref.wav [--gain 0.3] [--tail 2]
 *
 * mic.wav / ref.wav are 48 kHz mono float, equal length. The measured output->microphone delay and coupling are
 * printed (mic = coupling * ref delayed by `delay`, found by cross-correlation on the loudest second).
 * The far-end is faded in over 1 s and scaled by --gain (default 0.25) so the headphones start quiet.
 * Not real-time code. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "audio/devices.h"
#include "util/console.h"
#include "util/wavio.h"

#define SR 48000

typedef struct {
    float *far, *mic, *ref;
    long far_len, rec_len;
    atomic_long play_pos, mic_pos, ref_pos;
    float gain;
} Loop;

static void play_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    Loop *l = (Loop *)dev->pUserData;
    float *o = (float *)output;
    long p = atomic_load_explicit(&l->play_pos, memory_order_relaxed);
    for (ma_uint32 i = 0; i < frames; i++) {
        long k = p + (long)i;
        float v = 0.0f;
        if (k < l->far_len) {
            float fade = k < SR ? (float)k / (float)SR : 1.0f;
            v = l->far[k] * l->gain * fade;
        }
        v += 1e-4f * (float)((rand() & 0xFFFF) - 32768) / 32768.0f;   /* dither: keeps WASAPI loopback delivering packets */
        o[i] = v > 0.95f ? 0.95f : (v < -0.95f ? -0.95f : v);
    }
    atomic_store_explicit(&l->play_pos, p + frames, memory_order_relaxed);
}

static void cap_to(float *buf, long len, atomic_long *pos, const float *in, ma_uint32 frames)
{
    long p = atomic_load_explicit(pos, memory_order_relaxed);
    for (ma_uint32 i = 0; i < frames && p + (long)i < len; i++) buf[p + i] = in[i];
    atomic_store_explicit(pos, p + frames, memory_order_relaxed);
}
static void mic_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)output;
    Loop *l = (Loop *)dev->pUserData;
    if (input) cap_to(l->mic, l->rec_len, &l->mic_pos, (const float *)input, frames);
}
static void ref_cb(ma_device *dev, void *output, const void *input, ma_uint32 frames)
{
    (void)output;
    Loop *l = (Loop *)dev->pUserData;
    if (input) cap_to(l->ref, l->rec_len, &l->ref_pos, (const float *)input, frames);
}

static double rms_db(const float *x, long n)
{
    double s = 0;
    for (long i = 0; i < n; i++) s += (double)x[i] * x[i];
    return 10.0 * log10(s / (n > 0 ? n : 1) + 1e-20);
}

/* lag (samples) with the highest normalised correlation of y[i + lag] against x[i] */
static long find_lag(const float *x, const float *y, long n, double *gain, double *corr)
{
    const long win = SR, maxlag = SR * 3 / 2;
    long best_start = 0;
    double best_e = -1;
    for (long s = SR; s + win + maxlag < n; s += SR / 4) {
        double e = 0;
        for (long i = 0; i < win; i += 4) e += (double)x[s + i] * x[s + i];
        if (e > best_e) { best_e = e; best_start = s; }
    }
    *gain = 0; *corr = 0;
    if (best_e <= 0) return 0;
    double ex = 0;
    for (long i = 0; i < win; i++) ex += (double)x[best_start + i] * x[best_start + i];
    long best = 0;
    double bc = 0, bg = 0;
    for (long lag = 0; lag <= maxlag; lag++) {
        double xy = 0, yy = 0;
        for (long i = 0; i < win; i += 2) {
            const double a = x[best_start + i], b = y[best_start + i + lag];
            xy += a * b; yy += b * b;
        }
        xy *= 2; yy *= 2;
        const double c = xy / sqrt(ex * yy + 1e-30);
        if (c > bc) { bc = c; best = lag; bg = xy / (ex + 1e-30); }
    }
    *gain = bg; *corr = bc;
    return best;
}

int main(int argc, char **argv)
{
    const char *in_name = NULL, *out_name = NULL, *far_p = NULL, *mic_p = "mic.wav", *ref_p = "ref.wav";
    float gain = 0.25f;
    double tail = 2.0;
    int list = 0, analyze = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--analyze")) analyze = 1;
        else if (!strcmp(argv[i], "--in") && i + 1 < argc) in_name = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_name = argv[++i];
        else if (!strcmp(argv[i], "--far") && i + 1 < argc) far_p = argv[++i];
        else if (!strcmp(argv[i], "--mic") && i + 1 < argc) mic_p = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref_p = argv[++i];
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
        else { fprintf(stderr, "usage: vf-loop --list | --in MIC --out PLAYBACK --far far.wav [--mic m.wav] [--ref r.wav] [--gain G] [--tail S]\n"); return 2; }
    }
    if (analyze) {                          /* no devices: measure the path of two existing recordings */
        long nm, nr;
        float *m = vf_wav_read_mono(mic_p, SR, &nm), *r = vf_wav_read_mono(ref_p, SR, &nr);
        if (!m || !r) { fprintf(stderr, "cannot read %s / %s\n", mic_p, ref_p); return 1; }
        long n = nm < nr ? nm : nr;
        double g, cc;
        long lag = find_lag(r, m, n, &g, &cc);
        {   /* envelope check over the whole recording: 10 ms frame levels (dB), lag -1 s .. +3 s */
            const int F = 480;
            long nf = n / F;
            double *em = (double *)malloc((size_t)nf * sizeof *em), *er = (double *)malloc((size_t)nf * sizeof *er);
            for (long f = 0; f < nf; f++) { em[f] = rms_db(m + f * F, F); er[f] = rms_db(r + f * F, F); }
            double bc = -2; long bl = 0;
            for (long lag = -100; lag <= 300; lag++) {
                double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0; long k = 0;
                for (long f = 0; f < nf; f++) {
                    const long g2 = f + lag;
                    if (g2 < 0 || g2 >= nf) continue;
                    const double x = er[f] < -80 ? -80 : er[f], y = em[g2] < -80 ? -80 : em[g2];
                    sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; k++;
                }
                if (k < 200) continue;
                const double c = (sxy - sx * sy / k) / sqrt((sxx - sx * sx / k) * (syy - sy * sy / k) + 1e-9);
                if (c > bc) { bc = c; bl = lag; }
            }
            printf("envelope: best lag %ld ms, correlation %.2f (> 0.5 = the microphone hears the playback)\n", bl * 10, bc);
            free(em); free(er);
        }
        printf("rms: mic %.1f dBFS, ref %.1f dBFS\n", rms_db(m, n), rms_db(r, n));
        printf("path: delay %ld samples = %.1f ms, coupling %.1f dB (gain %.4f), correlation %.2f\n",
               lag, 1000.0 * lag / SR, 20 * log10(fabs(g) + 1e-9), g, cc);
        return 0;
    }
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return 1;
    static VfDeviceList cap, pb;
    vf_devices_enumerate(&ctx, &cap, &pb);
    if (list) {
        printf("capture:\n");
        for (int i = 0; i < cap.count; i++) printf("  [%d] %s%s\n", i, cap.dev[i].name, cap.dev[i].is_default ? "  (default)" : "");
        printf("playback:\n");
        for (int i = 0; i < pb.count; i++) printf("  [%d] %s%s\n", i, pb.dev[i].name, pb.dev[i].is_default ? "  (default)" : "");
        ma_context_uninit(&ctx);
        return 0;
    }
    if (!in_name || !out_name || !far_p) { fprintf(stderr, "need --in, --out and --far (or --list)\n"); return 2; }
    if (ctx.backend != ma_backend_wasapi) { fprintf(stderr, "WASAPI loopback needed (Windows)\n"); return 1; }
    int ci = vf_devices_find(&cap, in_name), pi = vf_devices_find(&pb, out_name);
    if (ci < 0) { fprintf(stderr, "capture device '%s' not found\n", in_name); return 1; }
    if (pi < 0) { fprintf(stderr, "playback device '%s' not found\n", out_name); return 1; }
    if (gain > 1.0f) gain = 1.0f;

    Loop l;
    memset(&l, 0, sizeof l);
    l.gain = gain;
    l.far = vf_wav_read_mono(far_p, SR, &l.far_len);
    if (!l.far) { fprintf(stderr, "cannot read %s\n", far_p); return 1; }
    l.rec_len = l.far_len + (long)((tail + 2.0) * SR);
    l.mic = (float *)calloc((size_t)l.rec_len, sizeof(float));
    l.ref = (float *)calloc((size_t)l.rec_len, sizeof(float));
    if (!l.mic || !l.ref) return 1;
    atomic_init(&l.play_pos, 0); atomic_init(&l.mic_pos, 0); atomic_init(&l.ref_pos, 0);

    ma_device dm, dr, dp;
    ma_device_config c = ma_device_config_init(ma_device_type_capture);
    c.sampleRate = SR; c.capture.format = ma_format_f32; c.capture.channels = 1;
    c.capture.pDeviceID = &cap.dev[ci].id; c.dataCallback = mic_cb; c.pUserData = &l;
    if (ma_device_init(&ctx, &c, &dm) != MA_SUCCESS) { fprintf(stderr, "cannot open microphone %s\n", cap.dev[ci].name); return 1; }
    c = ma_device_config_init(ma_device_type_loopback);
    c.sampleRate = SR; c.capture.format = ma_format_f32; c.capture.channels = 1;
    c.capture.pDeviceID = &pb.dev[pi].id; c.dataCallback = ref_cb; c.pUserData = &l;
    if (ma_device_init(&ctx, &c, &dr) != MA_SUCCESS) { fprintf(stderr, "cannot open loopback of %s\n", pb.dev[pi].name); return 1; }
    c = ma_device_config_init(ma_device_type_playback);
    c.sampleRate = SR; c.playback.format = ma_format_f32; c.playback.channels = 1;
    c.playback.pDeviceID = &pb.dev[pi].id; c.dataCallback = play_cb; c.pUserData = &l;
    if (ma_device_init(&ctx, &c, &dp) != MA_SUCCESS) { fprintf(stderr, "cannot open playback %s\n", pb.dev[pi].name); return 1; }

    printf("mic: %s\nplayback + loopback: %s\nfar-end: %.1f s, gain %.2f\n", cap.dev[ci].name, pb.dev[pi].name,
           (double)l.far_len / SR, (double)gain);
    ma_device_start(&dm);
    ma_device_start(&dr);
    vf_sleep_ms(500);                       /* captures run first, so the start of the far-end is recorded */
    ma_device_start(&dp);
    while (atomic_load(&l.play_pos) < l.far_len + (long)(tail * SR)) vf_sleep_ms(20);
    ma_device_uninit(&dp);
    ma_device_uninit(&dr);
    ma_device_uninit(&dm);
    ma_context_uninit(&ctx);

    long mp = atomic_load(&l.mic_pos), rp = atomic_load(&l.ref_pos);
    long n = mp < rp ? mp : rp;
    if (n > l.rec_len) n = l.rec_len;
    double pk_m = 0, pk_r = 0;
    for (long i = 0; i < n; i++) {
        if (fabs(l.mic[i]) > pk_m) pk_m = fabs(l.mic[i]);
        if (fabs(l.ref[i]) > pk_r) pk_r = fabs(l.ref[i]);
    }
    printf("recorded: mic %.2f s, loopback %.2f s (a big difference means the loopback skipped silence)\n", (double)mp / SR, (double)rp / SR);
    printf("peak: mic %.1f dBFS, loopback %.1f dBFS%s\n", 20 * log10(pk_m + 1e-9), 20 * log10(pk_r + 1e-9),
           pk_m > 0.95 ? "  ** MIC CLIPS - lower --gain **" : "");
    printf("rms: mic %.1f dBFS, loopback %.1f dBFS\n", rms_db(l.mic, n), rms_db(l.ref, n));
    double g, cc;
    long lag = find_lag(l.ref, l.mic, n, &g, &cc);
    printf("path: delay %ld samples = %.1f ms, coupling %.1f dB (gain %.4f), correlation %.2f\n",
           lag, 1000.0 * lag / SR, 20 * log10(fabs(g) + 1e-9), g, cc);
    int rc = vf_wav_write(mic_p, l.mic, n, 1, SR) | vf_wav_write(ref_p, l.ref, n, 1, SR);
    printf("%s -> %s, %s\n", rc ? "WRITE FAILED" : "wrote", mic_p, ref_p);
    return rc;
}
