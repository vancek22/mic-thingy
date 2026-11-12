#include "app/session.h"

#include <stdio.h>
#include <string.h>
#include "audio/vmic.h"

#define METER_RING 4096      /* floats: ~680 ms of meter frames */

static int is_virtual_input(const char *name)
{
    return vf_contains_ci(name, "CABLE Output") || vf_contains_ci(name, "mic thingy") ||
           vf_contains_ci(name, "VoiceFilter");          /* source name of older builds */
}

int vf_session_init(VfSession *s, int with_meters)
{
    memset(s, 0, sizeof *s);
    s->vmic_native = vf_vmic_supported();
    if (ma_context_init(NULL, 0, NULL, &s->ctx) != MA_SUCCESS) {
        snprintf(s->error, sizeof s->error, "Cannot initialise the audio backend.");
        return -1;
    }
    s->ctx_ready = 1;
    if (vf_session_refresh_devices(s) != 0) return -1;
    pipeline_init(&s->pipe);
    if (chain_build(&s->chain, &s->pipe) != 0 || pipeline_start(&s->pipe, VF_SAMPLE_RATE) != 0) {
        snprintf(s->error, sizeof s->error, "Effect chain initialisation failed.");
        return -1;
    }
    if (with_meters) {
        if (vf_ring_init(&s->meters, METER_RING) != 0) return -1;
        s->pipe.meter_ring = &s->meters;
    }
    return 0;
}

void vf_session_free(VfSession *s)
{
    vf_session_stop(s);
    pipeline_stop(&s->pipe);
    if (s->pipe.meter_ring) { vf_ring_free(&s->meters); s->pipe.meter_ring = NULL; }
    if (s->ctx_ready) { ma_context_uninit(&s->ctx); s->ctx_ready = 0; }
}

int vf_session_refresh_devices(VfSession *s)
{
    if (s->running) return -1;
    if (vf_devices_enumerate(&s->ctx, &s->caps, &s->plays) != 0) {
        snprintf(s->error, sizeof s->error, "Device enumeration failed.");
        return -1;
    }
    return 0;
}

int vf_session_default_mic(const VfSession *s)
{
    for (int i = 0; i < s->caps.count; i++)
        if (s->caps.dev[i].is_default && !is_virtual_input(s->caps.dev[i].name)) return i;
    for (int i = 0; i < s->caps.count; i++)
        if (!is_virtual_input(s->caps.dev[i].name)) return i;
    return -1;
}

const char *vf_session_vmic_name(const VfSession *s)
{
    return s->vmic_native ? VF_VMIC_DESCRIPTION : "CABLE Output";
}

int vf_session_start(VfSession *s, const VfSessionConfig *c)
{
    if (s->running) vf_session_stop(s);
    s->error[0] = '\0';
    s->cfg = *c;
    const int vm = c->mode != VF_OUT_HEADPHONES;

    if (c->capture >= s->caps.count || c->playback >= s->plays.count || c->monitor >= s->plays.count) {
        snprintf(s->error, sizeof s->error, "Invalid device index.");
        return -1;
    }
    /* feedback-loop guard */
    if (vm && !c->file_src) {
        int ci = c->capture;
        if (ci < 0)
            for (int i = 0; i < s->caps.count; i++) if (s->caps.dev[i].is_default) ci = i;
        if (ci >= 0 && is_virtual_input(s->caps.dev[ci].name)) {
            snprintf(s->error, sizeof s->error,
                     "Input \"%s\" is the virtual microphone itself (feedback loop). Choose your real microphone.",
                     s->caps.dev[ci].name);
            return -1;
        }
    }

    int pi = c->playback;
    s->use_pw = vm && s->vmic_native;
    if (vm && !s->vmic_native) {
        pi = vf_devices_find(&s->plays, "CABLE Input");
        if (pi < 0) {
            snprintf(s->error, sizeof s->error,
                     "\"CABLE Input\" not found - install VB-Cable (vb-audio.com/Cable).");
            return -1;
        }
    }
    if (s->use_pw && c->file_src) {
        snprintf(s->error, sizeof s->error, "A file source cannot feed the PipeWire microphone.");
        return -1;
    }
    s->has_mon = c->mode == VF_OUT_VMIC_MONITOR;
    if (s->has_mon && (c->monitor < 0 || (!s->use_pw && c->monitor == pi))) {
        snprintf(s->error, sizeof s->error, "Choose a monitor device different from the virtual microphone.");
        return -1;
    }
    s->resolved_playback = s->use_pw ? -1 : pi;

    EngineConfig ec;
    memset(&ec, 0, sizeof ec);
    ec.capture_id = c->capture >= 0 ? &s->caps.dev[c->capture].id : NULL;
    ec.playback_id = pi >= 0 ? &s->plays.dev[pi].id : NULL;
    ec.no_playback = s->use_pw;
    ec.period_frames = c->period;
    ec.periods = c->periods;
    ec.fixed_callback = c->fixed_callback;
    ec.pro_audio = c->pro_audio;
    ec.file_src = c->file_src;
    ec.file_len = c->file_len;

    int sink_v = 0, sink_m = 0, mon_ok = 0, pw_ok = 0;
    if (engine_init(&s->eng, &s->ctx, &s->pipe, &ec) != 0) {
        snprintf(s->error, sizeof s->error, "Cannot open the audio devices (in use or unplugged?).");
        goto fail;
    }
    if (s->use_pw) {
        sink_v = vf_sink_init(&s->vmic_sink, VF_SINK_TARGET) == 0;
        if (!sink_v || engine_add_tap(&s->eng, &s->vmic_sink) != 0 || vf_vmic_start(&s->vmic_sink) != 0) {
            snprintf(s->error, sizeof s->error, "Cannot create the PipeWire microphone (is PipeWire running?).");
            goto fail;
        }
        pw_ok = 1;
    }
    if (s->has_mon) {
        sink_m = vf_sink_init(&s->mon_sink, VF_SINK_TARGET) == 0;
        if (!sink_m || engine_add_tap(&s->eng, &s->mon_sink) != 0 ||
            vf_monitor_init(&s->mon, &s->ctx, &s->plays.dev[c->monitor].id, &s->mon_sink) != 0 ||
            vf_monitor_start(&s->mon) != 0) {
            snprintf(s->error, sizeof s->error, "Cannot open the monitor device.");
            goto fail;
        }
        mon_ok = 1;
    }
    if (engine_start(&s->eng) != 0) {
        snprintf(s->error, sizeof s->error, "Cannot start the audio stream.");
        goto fail;
    }
    s->running = 1;
    return 0;

fail:
    if (pw_ok) vf_vmic_stop();
    if (mon_ok || s->mon.ready) vf_monitor_uninit(&s->mon);
    engine_uninit(&s->eng);
    if (sink_v) vf_sink_free(&s->vmic_sink);
    if (sink_m) vf_sink_free(&s->mon_sink);
    s->use_pw = s->has_mon = 0;
    return -1;
}

void vf_session_stop(VfSession *s)
{
    if (!s->running) return;
    engine_stop(&s->eng);
    if (s->use_pw) vf_vmic_stop();
    if (s->has_mon) vf_monitor_uninit(&s->mon);
    engine_uninit(&s->eng);
    if (s->use_pw) vf_sink_free(&s->vmic_sink);
    if (s->has_mon) vf_sink_free(&s->mon_sink);
    s->running = 0;
}

unsigned vf_session_xruns(const VfSession *s)
{
    if (!s->running) return 0;
    unsigned x = atomic_load(&((VfSession *)s)->eng.underruns);
    if (s->use_pw) x += atomic_load(&((VfSession *)s)->vmic_sink.underruns);
    if (s->has_mon) x += atomic_load(&((VfSession *)s)->mon_sink.underruns);
    return x;
}

void vf_session_latency_text(const VfSession *s, char *buf, int size)
{
    if (!s->running) { snprintf(buf, (size_t)size, "stopped"); return; }
    EngineLatency l;
    engine_latency(&s->eng, &l);
    if (s->use_pw)
        snprintf(buf, (size_t)size, "~%.0f ms (capture %.0f + DSP %.1f + buffer %.0f) + PipeWire",
                 l.capture_ms + l.dsp_ms + 1000.0 * VF_SINK_TARGET / VF_SAMPLE_RATE,
                 l.capture_ms, l.dsp_ms, 1000.0 * VF_SINK_TARGET / VF_SAMPLE_RATE);
    else
        snprintf(buf, (size_t)size, "~%.0f ms (capture %.0f + playback %.0f + FIFO %.0f + DSP %.1f)%s",
                 l.total_ms, l.capture_ms, l.playback_ms + l.interm_ms, l.fifo_ms, l.dsp_ms,
                 s->cfg.mode != VF_OUT_HEADPHONES ? " + VB-Cable" : "");
}
