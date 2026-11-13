/* Whole-file audio I/O for offline tools (not for the audio thread). */
#ifndef VF_WAVIO_H
#define VF_WAVIO_H

/* Decodes WAV/FLAC/MP3, downmixed to mono and resampled to `rate`.
 * Returns a malloc'ed buffer (free with free()) and its length, or NULL. */
float *vf_wav_read_mono(const char *path, int rate, long *out_len);

/* Writes interleaved 32-bit float WAV. 0 = ok. */
int vf_wav_write(const char *path, const float *interleaved, long frames, int channels, int rate);

/* Streaming 16-bit PCM WAV writer (recorder thread). The header is patched on close, so a file
 * is only valid after vf_wavw_close(). Paths are UTF-8. */
#include <stdio.h>
typedef struct VfWavWriter {
    FILE *f;
    long frames;
    int channels, rate;
    int ok;                      /* 0 once a write failed (disk full, ...) */
} VfWavWriter;

int  vf_wavw_open(VfWavWriter *w, const char *path, int channels, int rate);   /* 0 = ok */
int  vf_wavw_write_f32(VfWavWriter *w, const float *interleaved, long frames); /* 0 = ok */
int  vf_wavw_close(VfWavWriter *w);                                            /* 0 = file is complete */

#endif
