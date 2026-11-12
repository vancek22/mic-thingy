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
