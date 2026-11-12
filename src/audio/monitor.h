/* Monitor: a second playback device (e.g. headphones) that plays the processed
 * voice from a VfSink while the main output goes to the virtual microphone. */
#ifndef VF_MONITOR_H
#define VF_MONITOR_H

#include "miniaudio.h"
#include "audio/sink.h"

typedef struct VfMonitor {
    ma_device dev;
    VfSink *sink;
    int ready;
} VfMonitor;

int  vf_monitor_init(VfMonitor *m, ma_context *ctx, const ma_device_id *id, VfSink *sink);   /* 0 = ok */
int  vf_monitor_start(VfMonitor *m);
void vf_monitor_uninit(VfMonitor *m);
double vf_monitor_buffer_ms(const VfMonitor *m);

#endif
