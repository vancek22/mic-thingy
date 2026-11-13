/* A/B test: records a few seconds of what the microphone hears (raw and processed) and plays
 * them back to compare - without a call. The memory (two float buffers) exists only while the
 * test is in use. Control thread only; the audio thread only copies frames (VfCapture). */
#ifndef VF_ABTEST_H
#define VF_ABTEST_H

#include <stdatomic.h>
#include "audio/engine.h"
#include "miniaudio.h"

typedef enum { VF_AB_IDLE = 0, VF_AB_RECORDING, VF_AB_READY, VF_AB_PLAYING } VfAbState;
typedef enum { VF_AB_RAW = 0, VF_AB_FILTERED = 1, VF_AB_ALTERNATE = 2 } VfAbWhich;

#define VF_AB_SEGMENT_S 2      /* in alternate mode raw and filtered take turns of this length */

typedef struct VfAbTest {
    VfCapture cap;
    int cap_ready;
    ma_device dev;
    int dev_ready;
    atomic_int state;
    atomic_long ppos;          /* playback position, samples */
    atomic_int which;
    atomic_int match;          /* 1 = scale the filtered signal to the loudness of the raw one */
    float proc_gain;           /* loudness match factor, set when the recording ends */
    long len;                  /* recorded samples */
} VfAbTest;

void vf_ab_init(VfAbTest *a);
/* Starts recording `seconds` (1..30) from a running engine. 0 = ok. */
int  vf_ab_record(VfAbTest *a, Engine *e, int seconds);
/* Call regularly: moves RECORDING -> READY when the capture is full. Returns the state. */
VfAbState vf_ab_update(VfAbTest *a, Engine *e);
/* Plays on `playback_id` (NULL = default device). 0 = ok. */
int  vf_ab_play(VfAbTest *a, ma_context *ctx, const ma_device_id *playback_id, VfAbWhich which);
void vf_ab_stop(VfAbTest *a);                  /* stops playback, keeps the recording */
void vf_ab_free(VfAbTest *a, Engine *e);       /* releases everything */
double vf_ab_seconds(const VfAbTest *a);       /* recorded (or being recorded) length */
double vf_ab_play_seconds(const VfAbTest *a);

/* Playback core without a device (also used by the callback and the tests): fills `out` with
 * n samples from position pos, returns the number of samples that came from the recording. */
long vf_ab_render(const VfAbTest *a, long pos, float *out, long n);

#endif
