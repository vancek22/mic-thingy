/* Native virtual microphone.
 *  Linux (PipeWire): a pw_stream with media.class Audio/Source/Virtual named
 *  "VoiceFilter Microphone" - visible as a microphone in Discord/OBS.
 *  Windows: not available natively; the CLI writes to VB-Cable's "CABLE Input"
 *  instead and apps select "CABLE Output" as their microphone. */
#ifndef VF_VMIC_H
#define VF_VMIC_H

#include "audio/sink.h"

#define VF_VMIC_NODE_NAME   "voicefilter_mic"
#define VF_VMIC_DESCRIPTION "VoiceFilter Microphone"

int  vf_vmic_supported(void);           /* 1 if this build can create a virtual source */
int  vf_vmic_start(VfSink *sink);       /* 0 = ok; the stream pulls audio from sink */
void vf_vmic_stop(void);

#endif
