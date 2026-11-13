/* Soundboard: short clips played into the voice (mix bus, right before the limiter).
 * Clips are decoded once (WAV/FLAC/MP3 via miniaudio) to 48 kHz mono 16-bit and kept in
 * memory: at most VF_BOARD_MAX_SECONDS per clip and VF_BOARD_MAX_SAMPLES in total.
 *
 * Threading: the audio thread only calls vf_board_read(); everything else is the control
 * thread. A clip is replaced/removed by first disabling it and waiting until the audio thread
 * is not inside it (busy flag), so there is no lock and no use-after-free. */
#ifndef VF_SOUNDBOARD_H
#define VF_SOUNDBOARD_H

#include <stdatomic.h>

#define VF_BOARD_SLOTS        8
#define VF_BOARD_MAX_SECONDS  15
#define VF_BOARD_MAX_SAMPLES  (4 * 1024 * 1024)   /* 8 MB of int16 over all slots */

typedef struct VfClip {
    short *pcm;                  /* NULL = empty slot */
    long len;
    char name[48];               /* display name (file name without extension) */
    char path[260];              /* where it was loaded from (kept in settings.ini) */
    atomic_int enabled;          /* 1 = the audio thread may read pcm */
    atomic_int busy;             /* 1 while the audio thread is inside this clip */
    atomic_long pos;             /* playback position; < 0 = idle */
} VfClip;

typedef struct VfSoundboard {
    VfClip clip[VF_BOARD_SLOTS];
} VfSoundboard;

void vf_board_init(VfSoundboard *b);
void vf_board_free(VfSoundboard *b);

/* Loads `path` into `slot`. 0 = ok; -1 unreadable; -2 too long; -3 memory budget exceeded. */
int  vf_board_load(VfSoundboard *b, int slot, const char *path);
/* Same from float samples (48 kHz mono), for tests and tools. Same return codes. */
int  vf_board_load_samples(VfSoundboard *b, int slot, const float *samples, long n, const char *name);
void vf_board_clear(VfSoundboard *b, int slot);

void vf_board_play(VfSoundboard *b, int slot);       /* (re)starts from the beginning */
void vf_board_stop(VfSoundboard *b, int slot);
void vf_board_stop_all(VfSoundboard *b);
int  vf_board_playing(const VfSoundboard *b, int slot);
long vf_board_total_samples(const VfSoundboard *b);

/* MixSource.read callback (audio thread): writes the sum of all playing clips. */
void vf_board_read(void *ctx, float *dst, int n);

#endif
