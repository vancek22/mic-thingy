#include "util/wavio.h"

#include <stdlib.h>
#include "miniaudio.h"

float *vf_wav_read_mono(const char *path, int rate, long *out_len)
{
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)rate);
    ma_decoder dec;
    if (ma_decoder_init_file(path, &cfg, &dec) != MA_SUCCESS) return NULL;

    size_t cap = 48000 * 10, len = 0;
    float *buf = (float *)malloc(cap * sizeof *buf);
    while (buf) {
        if (len == cap) {
            float *nb = (float *)realloc(buf, cap * 2 * sizeof *buf);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, buf + len, cap - len, &got);
        len += (size_t)got;
        if (r != MA_SUCCESS || got == 0) break;
    }
    ma_decoder_uninit(&dec);
    if (buf) *out_len = (long)len;
    return buf;
}

int vf_wav_write(const char *path, const float *data, long frames, int channels, int rate)
{
    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32,
                                                   (ma_uint32)channels, (ma_uint32)rate);
    ma_encoder enc;
    if (ma_encoder_init_file(path, &cfg, &enc) != MA_SUCCESS) return -1;
    ma_uint64 written = 0;
    ma_result r = ma_encoder_write_pcm_frames(&enc, data, (ma_uint64)frames, &written);
    ma_encoder_uninit(&enc);
    return (r == MA_SUCCESS && written == (ma_uint64)frames) ? 0 : -1;
}

/* ---- streaming writer ---- */
#include <string.h>
#include "util/platform.h"

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void write_header(VfWavWriter *w, long frames)
{
    unsigned char h[44];
    const unsigned long data = (unsigned long)frames * (unsigned long)w->channels * 2UL;
    memcpy(h, "RIFF", 4); put32(h + 4, 36UL + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, (unsigned)w->channels); put32(h + 24, (unsigned long)w->rate);
    put32(h + 28, (unsigned long)w->rate * (unsigned long)w->channels * 2UL); put16(h + 32, (unsigned)w->channels * 2);
    put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, data);
    if (fwrite(h, 1, sizeof h, w->f) != sizeof h) w->ok = 0;
}

int vf_wavw_open(VfWavWriter *w, const char *path, int channels, int rate)
{
    memset(w, 0, sizeof *w);
    w->channels = channels; w->rate = rate; w->ok = 1;
    w->f = vf_fopen(path, "wb");
    if (!w->f) return -1;
    write_header(w, 0);
    return w->ok ? 0 : -1;
}

int vf_wavw_write_f32(VfWavWriter *w, const float *x, long frames)
{
    if (!w->f || !w->ok) return -1;
    enum { CH = 4096 };
    short tmp[CH];
    long n = frames * w->channels, done = 0;
    while (done < n) {
        const long m = n - done < CH ? n - done : CH;
        for (long i = 0; i < m; i++) {
            float v = x[done + i] * 32767.0f;
            tmp[i] = (short)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        if (fwrite(tmp, sizeof(short), (size_t)m, w->f) != (size_t)m) { w->ok = 0; return -1; }   /* little endian hosts only */
        done += m;
    }
    w->frames += frames;
    return 0;
}

int vf_wavw_close(VfWavWriter *w)
{
    if (!w->f) return -1;
    if (w->ok && fseek(w->f, 0, SEEK_SET) == 0) write_header(w, w->frames); else w->ok = 0;
    if (fclose(w->f) != 0) w->ok = 0;
    w->f = NULL;
    return w->ok ? 0 : -1;
}
