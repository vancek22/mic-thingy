/* Recorder: writes what the engine hears to a stereo WAV (left = raw microphone, right =
 * processed voice, 16-bit 48 kHz) on its own thread. The audio thread only copies each frame
 * into a lock-free ring (Engine.rec_ring); if the disk cannot keep up, frames are dropped and
 * counted instead of blocking the audio. Records the microphone only - never the other side of
 * a call. */
#ifndef VF_RECORDER_H
#define VF_RECORDER_H

#include <stdatomic.h>
#include "audio/engine.h"
#include "util/ringbuf.h"
#include "util/thread.h"
#include "util/wavio.h"

typedef struct VfRecorder {
    VfRing ring;
    int ring_ready;
    VfThread thread;
    VfWavWriter wav;
    atomic_int run;               /* 1 while recording */
    atomic_int failed;            /* 1 after a write error (stops by itself) */
    atomic_long frames;           /* frames written so far */
    long max_frames;
    char path[1024];
    Engine *eng;
} VfRecorder;

void vf_rec_init(VfRecorder *r);
/* path: target .wav; NULL = <Documents>/mic thingy/mic-thingy-<date>-<time>.wav (written to `path_out`
 * if given, 1024 bytes). max_seconds <= 0: one hour. 0 = ok. */
int  vf_rec_start(VfRecorder *r, Engine *e, const char *path, int max_seconds, char *path_out);
/* Stops, flushes and finalises the file. 0 = the file is complete and valid. */
int  vf_rec_stop(VfRecorder *r);
int  vf_rec_active(const VfRecorder *r);
double vf_rec_seconds(const VfRecorder *r);
void vf_rec_free(VfRecorder *r);

#endif
