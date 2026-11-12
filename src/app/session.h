/* VfSession: everything needed to run VoiceFilter - audio context, device lists,
 * the effect chain (kept across restarts, so parameters survive a device change),
 * the engine and the optional virtual-mic / monitor outputs.
 * Shared by the console and the GUI front-ends. Control thread only. */
#ifndef VF_SESSION_H
#define VF_SESSION_H

#include "audio/devices.h"
#include "audio/engine.h"
#include "audio/monitor.h"
#include "audio/sink.h"
#include "dsp/chain.h"
#include "dsp/pipeline.h"
#include "util/ringbuf.h"

typedef enum { VF_OUT_HEADPHONES = 0, VF_OUT_VMIC = 1, VF_OUT_VMIC_MONITOR = 2 } VfOutMode;

#define VF_SINK_TARGET 960      /* 20 ms cushion between independent audio clocks */

typedef struct VfSessionConfig {
    int capture;           /* index into caps, -1 = system default */
    int playback;          /* index into plays (headphones mode), -1 = default */
    int monitor;           /* index into plays (VMIC_MONITOR mode) */
    VfOutMode mode;
    int period;            /* engine period request, 0 = default */
    const float *file_src; /* optional file instead of the microphone */
    long file_len;
} VfSessionConfig;

typedef struct VfSession {
    ma_context ctx;
    int ctx_ready;
    VfDeviceList caps, plays;
    Chain chain;
    Pipeline pipe;
    VfRing meters;                    /* audio -> UI meter stream */
    Engine eng;
    VfSink vmic_sink, mon_sink;
    VfMonitor mon;
    int running, use_pw, has_mon, vmic_native;
    VfSessionConfig cfg;              /* config of the running session */
    int resolved_playback;            /* actual playback index used (CABLE Input in vmic mode) */
    char error[256];
} VfSession;

int  vf_session_init(VfSession *s, int with_meters);   /* 0 = ok, else s->error */
void vf_session_free(VfSession *s);
int  vf_session_refresh_devices(VfSession *s);         /* only while stopped */
int  vf_session_start(VfSession *s, const VfSessionConfig *cfg);   /* 0 = ok, else s->error */
void vf_session_stop(VfSession *s);

/* First capture device that is not a virtual cable / our own source, or -1. */
int  vf_session_default_mic(const VfSession *s);
/* Underruns summed over engine, virtual mic and monitor. */
unsigned vf_session_xruns(const VfSession *s);
/* Human readable one-line latency estimate (after start). */
void vf_session_latency_text(const VfSession *s, char *buf, int size);
/* Name of the virtual-mic device apps should select. */
const char *vf_session_vmic_name(const VfSession *s);

#endif
