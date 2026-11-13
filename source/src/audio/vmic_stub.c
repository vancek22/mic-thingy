/* Builds without PipeWire (Windows, or Linux with -DVF_PIPEWIRE=OFF). */
#include "audio/vmic.h"

int  vf_vmic_supported(void) { return 0; }
int  vf_vmic_start(VfSink *sink) { (void)sink; return -1; }
void vf_vmic_stop(void) {}
