/* voicefilter-file: run the production effect chain over an audio file.
 *
 *   voicefilter-file -i in.wav -o out.wav [-c "set gate threshold 0.4"]... [--bypass]
 *                    [--debug dbg.wav] [--csv frames.csv]
 *
 * --debug writes a 4-channel WAV: input, output, VAD probability, gate gain
 * (VAD/gate are held per 10 ms frame), handy for `ffmpeg showwavespic`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        "usage: voicefilter-file -i IN -o OUT [-c CMD]... [--bypass] [--debug DBG.wav] [--csv F.csv]\n"
        "  CMD is any console command, e.g. -c \"set comp ratio 4\" -c \"bypass eq on\"\n");
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *dbg_path = NULL, *csv_path = NULL;
    const char *cmds[MAX_CMDS];
    int ncmd = 0, bypass = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) in_path = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc && ncmd < MAX_CMDS) cmds[ncmd++] = argv[++i];
        else if (!strcmp(argv[i], "--debug") && i + 1 < argc) dbg_path = argv[++i];
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--bypass")) bypass = 1;
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

    float frame[VF_FRAME_SIZE];
    for (long f = 0; f < nframes; f++) {
        const long off = f * VF_FRAME_SIZE;
        for (int i = 0; i < VF_FRAME_SIZE; i++)
            frame[i] = off + i < len ? in[off + i] : 0.0f;
        if (dbg) for (int i = 0; i < VF_FRAME_SIZE; i++) dbg[(off + i) * 4 + 0] = frame[i];

        pipeline_process(&pipe, frame);
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
