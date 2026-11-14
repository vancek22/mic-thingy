/* mic-thingy-file: run the production effect chain over an audio file.
 *
 *   mic-thingy-file -i in.wav -o out.wav [-c "set gate threshold 0.4"]... [--bypass]
 *                    [--debug dbg.wav] [--csv frames.csv]
 *
 * --calibrate runs the calibration wizard (app/calibrate.c) over the file - the first 5 s must be the quiet room,
 * then up to 15 s of speech - and prints what it measured and proposes (the same code the GUI uses).
 * --debug writes a 4-channel WAV: input, output, VAD probability, gate gain
 * (VAD/gate are held per 10 ms frame), handy for `ffmpeg showwavespic`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/calibrate.h"
#include "dsp/chain.h"
#include "util/cmdline.h"
#include "util/wavio.h"

#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#define MAX_CMDS 32

static void usage(void)
{
    fprintf(stderr,
        "usage: mic-thingy-file -i IN -o OUT [-c CMD]... [--bypass] [--debug DBG.wav] [--csv F.csv] [--calibrate]\n"
        "  CMD is any console command, e.g. -c \"set comp ratio 4\" -c \"bypass eq on\"\n");
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *dbg_path = NULL, *csv_path = NULL;
    const char *cmds[MAX_CMDS];
    int ncmd = 0, bypass = 0, calibrate = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc && ncmd < MAX_CMDS) cmds[ncmd++] = argv[++i];
        else if (!strcmp(argv[i], "--debug") && i + 1 < argc) dbg_path = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--bypass")) bypass = 1;
        else if (!strcmp(argv[i], "--calibrate")) calibrate = 1;
        else { usage(); return 2; }
    }
    if (!in_path || !out_path) { usage(); return 2; }

#if defined(__SSE__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);   /* FTZ|DAZ, same as the audio callback */
#endif

    long len = 0;
    float *in = vf_wav_read_mono(in_path, VF_SAMPLE_RATE, &len);
    if (!in) { fprintf(stderr, "cannot read %s\n", in_path); return 1; }

    static Chain chain;
    static Pipeline pipe;
    pipeline_init(&pipe);
    if (chain_build(&chain, &pipe) != 0 || pipeline_start(&pipe, VF_SAMPLE_RATE) != 0) {
        fprintf(stderr, "pipeline init failed\n");
        return 1;
    }
    for (int c = 0; c < ncmd; c++) {
        fprintf(stderr, "> %s\n", cmds[c]);
        vf_cmd_execute(&pipe, cmds[c], stderr);
    }
    if (bypass) atomic_store(&pipe.bypass_all, 1);

    const long nframes = (len + VF_FRAME_SIZE - 1) / VF_FRAME_SIZE;
    const long total = nframes * VF_FRAME_SIZE;
    float *out = (float *)calloc((size_t)total, sizeof *out);
    float *dbg = dbg_path ? (float *)calloc((size_t)total * 4, sizeof *dbg) : NULL;
    FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (!out || (dbg_path && !dbg)) { fprintf(stderr, "out of memory\n"); return 1; }
    if (csv) fprintf(csv, "frame,time_s,peak_in,peak_out,vad,gate_gain,comp_gr_db\n");

    float frame[VF_FRAME_SIZE], raw[VF_FRAME_SIZE];
    static VfCalibrate cal;
    if (calibrate) vf_cal_start(&cal);
    for (long f = 0; f < nframes; f++) {
        const long off = f * VF_FRAME_SIZE;
        for (int i = 0; i < VF_FRAME_SIZE; i++)
            frame[i] = off + i < len ? in[off + i] : 0.0f;
        if (dbg) for (int i = 0; i < VF_FRAME_SIZE; i++) dbg[(off + i) * 4 + 0] = frame[i];

        memcpy(raw, frame, sizeof raw);
        pipeline_process(&pipe, frame);
        if (calibrate) {
            float m[VF_METER_FIELDS] = { 0 };
            m[VF_M_PEAK_IN] = atomic_load(&pipe.peak_in);
            m[VF_M_VAD] = atomic_load(&pipe.vad);
            vf_cal_frame(&cal, m);
            vf_cal_audio(&cal, raw, VF_FRAME_SIZE);
        }
        memcpy(out + off, frame, sizeof frame);

        const float vad = atomic_load(&pipe.vad), gate = atomic_load(&pipe.gate_gain);
        if (dbg)
            for (int i = 0; i < VF_FRAME_SIZE; i++) {
                dbg[(off + i) * 4 + 1] = frame[i];
                dbg[(off + i) * 4 + 2] = vad;
                dbg[(off + i) * 4 + 3] = gate;
            }
        if (csv)
            fprintf(csv, "%ld,%.3f,%.5f,%.5f,%.3f,%.4f,%.2f\n", f,
                    (double)off / VF_SAMPLE_RATE, (double)atomic_load(&pipe.peak_in),
                    (double)atomic_load(&pipe.peak_out), (double)vad, (double)gate,
                    (double)atomic_load(&pipe.comp_gr_db));
    }

    int rc = 0;
    if (calibrate) {
        const VfCalResult *c = &cal.result;
        if (cal.state != VF_CAL_DONE) printf("calibration: not finished (state %d) %s\n", cal.state, cal.why);
        else {
            printf("measured: noise %.1f dB, speech %.1f dB (p90 %.1f), SNR %.1f dB, voiced %.0f%%, VAD in silence %.2f\n",
                   (double)c->noise_db, (double)c->speech_db, (double)c->speech_p90_db, (double)c->snr_db,
                   (double)c->voiced_pct, (double)c->vad_silence);
            printf("audio: %s pitch %.0f Hz, hum %d Hz (%.1f dB), rumble %.1f dB, balance low/mid/high %.1f/%.1f/%.1f dB"
                   " (targets %.1f/%.1f/%.1f), sibilance %.1f dB (level %.1f dBFS), transients %.1f dB\n",
                   c->has_audio ? "yes" : "NO", (double)c->f0_hz, c->hum_hz, (double)c->hum_db, (double)c->rumble_db,
                   (double)c->low_db, (double)c->mid_db, (double)c->high_db, (double)VF_CAL_T_LOW, (double)VF_CAL_T_MID,
                   (double)VF_CAL_T_HIGH, (double)c->sib_db, (double)c->sib_level_db, (double)c->transient_db);
            printf("proposal: highpass %.0f Hz, hum %s, rnnoise wet %.2f, gate %.2f, comp %.1f dB + %.1f dB, "
                   "eq %+.1f/%+.1f/%+.1f dB, deesser %s (threshold %.1f dB)\n",
                   (double)c->hp_hz, c->hum_hz ? "on" : "off", (double)c->rn_wet, (double)c->gate_threshold,
                   (double)c->comp_threshold_db, (double)c->comp_makeup_db, (double)c->eq_low_db, (double)c->eq_mid_db,
                   (double)c->eq_high_db, c->deess ? "on" : "off", (double)c->deess_thr_db);
        }
    }
    if (vf_wav_write(out_path, out, len, 1, VF_SAMPLE_RATE) != 0) {
        fprintf(stderr, "cannot write %s\n", out_path); rc = 1;
    }
    if (dbg && vf_wav_write(dbg_path, dbg, len, 4, VF_SAMPLE_RATE) != 0) {
        fprintf(stderr, "cannot write %s\n", dbg_path); rc = 1;
    }
    if (csv) fclose(csv);
    pipeline_stop(&pipe);
    free(in); free(out); free(dbg);
    return rc;
}
