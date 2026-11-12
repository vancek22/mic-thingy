#ifndef VF_DEVICES_H
#define VF_DEVICES_H

#include "miniaudio.h"

#define VF_MAX_DEVICES 64

typedef struct VfDevice {
    char name[256];            /* UTF-8 */
    ma_device_id id;
    int is_default;
} VfDevice;

typedef struct VfDeviceList {
    VfDevice dev[VF_MAX_DEVICES];
    int count;
} VfDeviceList;

/* Fills capture and playback lists (either pointer may be NULL). 0 = ok. */
int vf_devices_enumerate(ma_context *ctx, VfDeviceList *capture, VfDeviceList *playback);

/* Index of the first device whose name contains `needle` (case-insensitive), or -1. */
int vf_devices_find(const VfDeviceList *list, const char *needle);

#endif
