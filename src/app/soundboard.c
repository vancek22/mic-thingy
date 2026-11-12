#include "app/soundboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/fx.h"
#include "util/console.h"
#include "util/wavio.h"

void vf_board_init(VfSoundboard *b)
{
    memset(b, 0, sizeof *b);
    for (int i = 0; i < VF_BOARD_SLOTS; i++) {
        atomic_init(&b->clip[i].enabled, 0);
        atomic_init(&b->clip[i].busy, 0);
        atomic_init(&b->clip[i].pos, -1);
    }
}

void vf_board_free(VfSoundboard *b)
{
    for (int i = 0; i < VF_BOARD_SLOTS; i++) vf_board_clear(b, i);
}

long vf_board_total_samples(const VfSoundboard *b)
{
    long n = 0;
    for (int i = 0; i < VF_BOARD_SLOTS; i++) n += b->clip[i].len;
    return n;
}

/* Takes the clip away from the audio thread: after this it is safe to free/replace pcm. */
static void quiesce(VfClip *c)
{
    atomic_store(&c->enabled, 0);
    atomic_store(&c->pos, -1);
    for (int i = 0; i < 2000 && atomic_load(&c->busy); i++) vf_sleep_ms(1);   /* a callback lasts ~10 ms */
}

void vf_board_clear(VfSoundboard *b, int slot)
{
    if (slot < 0 || slot >= VF_BOARD_SLOTS) return;
    VfClip *c = &b->clip[slot];
    quiesce(c);
    free(c->pcm);
    c->pcm = NULL;
    c->len = 0;
    c->name[0] = c->path[0] = '\0';
}

int vf_board_load_samples(VfSoundboard *b, int slot, const float *x, long n, const char *name)
{
    if (slot < 0 || slot >= VF_BOARD_SLOTS || !x || n <= 0) return -1;
    if (n > (long)VF_BOARD_MAX_SECONDS * VF_SAMPLE_RATE) return -2;
    if (vf_board_total_samples(b) - b->clip[slot].len + n > VF_BOARD_MAX_SAMPLES) return -3;
    short *pcm = (short *)malloc((size_t)n * sizeof *pcm);
    if (!pcm) return -3;
    for (long i = 0; i < n; i++) {
        float v = x[i] * 32767.0f;
        pcm[i] = (short)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
    }
    VfClip *c = &b->clip[slot];
    quiesce(c);
    free(c->pcm);
    c->pcm = pcm;
    c->len = n;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "clip");
    c->path[0] = '\0';
    atomic_store(&c->enabled, 1);
    return 0;
}

int vf_board_load(VfSoundboard *b, int slot, const char *path)
{
    if (slot < 0 || slot >= VF_BOARD_SLOTS || !path) return -1;
    long n = 0;
    float *x = vf_wav_read_mono(path, VF_SAMPLE_RATE, &n);
    if (!x) return -1;
    const char *base = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char name[48];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    const int r = vf_board_load_samples(b, slot, x, n, name);
    free(x);
    if (r == 0) snprintf(b->clip[slot].path, sizeof b->clip[slot].path, "%s", path);
    return r;
}

void vf_board_play(VfSoundboard *b, int slot)
{
    if (slot >= 0 && slot < VF_BOARD_SLOTS && atomic_load(&b->clip[slot].enabled))
        atomic_store(&b->clip[slot].pos, 0);
}

void vf_board_stop(VfSoundboard *b, int slot)
{
    if (slot >= 0 && slot < VF_BOARD_SLOTS) atomic_store(&b->clip[slot].pos, -1);
}

void vf_board_stop_all(VfSoundboard *b)
{
    for (int i = 0; i < VF_BOARD_SLOTS; i++) vf_board_stop(b, i);
}

int vf_board_playing(const VfSoundboard *b, int slot)
{
    return slot >= 0 && slot < VF_BOARD_SLOTS && atomic_load(&((VfSoundboard *)b)->clip[slot].pos) >= 0;
}

void vf_board_read(void *ctx, float *dst, int n)
{
    VfSoundboard *b = (VfSoundboard *)ctx;
    memset(dst, 0, (size_t)n * sizeof *dst);
    for (int s = 0; s < VF_BOARD_SLOTS; s++) {
        VfClip *c = &b->clip[s];
        if (atomic_load_explicit(&c->pos, memory_order_relaxed) < 0) continue;
        atomic_store(&c->busy, 1);                       /* seq_cst: see quiesce() */
        if (atomic_load(&c->enabled)) {
            long pos = atomic_load_explicit(&c->pos, memory_order_relaxed);
            if (pos >= 0) {
                int m = n;
                if (pos + m > c->len) m = (int)(c->len - pos);
                for (int i = 0; i < m; i++) dst[i] += (float)c->pcm[pos + i] * (1.0f / 32768.0f);
                pos += m;
                atomic_store_explicit(&c->pos, pos >= c->len ? -1 : pos, memory_order_relaxed);
            }
        }
        atomic_store(&c->busy, 0);
    }
}
