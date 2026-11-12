/* System audio capture for the mix bus (music, game sound): WASAPI loopback of a playback
 * device, converted to 48 kHz mono and handed to the audio thread through a VfSink (the two
 * clocks drift). Windows only; elsewhere vf_sys_start() returns -1.
 *
 * Caution: a loopback of the device that also plays the other people in a call sends
 * their voices back to them. The GUI says so; per-application capture is future work. */
#ifndef VF_SYSAUDIO_H
#define VF_SYSAUDIO_H

#include "miniaudio.h"
#include "audio/sink.h"

typedef struct VfSysAudio {
    ma_device dev;
    VfSink sink;
    int sink_ready;
    atomic_int running;
} VfSysAudio;

void vf_sys_init(VfSysAudio *s);
/* playback_id: the device whose output to capture (NULL = default). 0 = ok. */
int  vf_sys_start(VfSysAudio *s, ma_context *ctx, const ma_device_id *playback_id);
void vf_sys_stop(VfSysAudio *s);
void vf_sys_free(VfSysAudio *s);

/* MixSource.read callback (audio thread). */
void vf_sys_read(void *ctx, float *dst, int n);

#endif
