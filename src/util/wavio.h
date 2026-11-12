/* Whole-file audio I/O for offline tools (not for the audio thread). */
#ifndef VF_WAVIO_H
#define VF_WAVIO_H

/* Decodes WAV/FLAC/MP3, downmixed to mono and resampled to `rate`.
 * Returns a malloc'ed buffer (free with free()) and its length, or NULL. */
float *vf_wav_read_mono(const char *path, int rate, long *out_len);

/* Writes interleaved 32-bit float WAV. 0 = ok. */
int vf_wav_write(const char *path, const float *interleaved, long frames, int channels, int rate);

#endif
