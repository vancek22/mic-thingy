#include "audio/devices.h"

#include <ctype.h>
#include <string.h>

static void fill(VfDeviceList *out, const ma_device_info *info, ma_uint32 n)
{
    out->count = 0;
    for (ma_uint32 i = 0; i < n && out->count < VF_MAX_DEVICES; i++) {
        VfDevice *d = &out->dev[out->count++];
        strncpy(d->name, info[i].name, sizeof d->name - 1);
        d->name[sizeof d->name - 1] = '\0';
        d->id = info[i].id;
        d->is_default = info[i].isDefault ? 1 : 0;
    }
}

int vf_devices_enumerate(ma_context *ctx, VfDeviceList *capture, VfDeviceList *playback)
{
    ma_device_info *pb, *cap;
    ma_uint32 npb, ncap;
    if (ma_context_get_devices(ctx, &pb, &npb, &cap, &ncap) != MA_SUCCESS)
        return -1;
    if (capture)  fill(capture, cap, ncap);
    if (playback) fill(playback, pb, npb);
    return 0;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

int vf_devices_find(const VfDeviceList *list, const char *needle)
{
    for (int i = 0; i < list->count; i++)
        if (contains_ci(list->dev[i].name, needle)) return i;
    return -1;
}
