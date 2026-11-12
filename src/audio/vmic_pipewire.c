/* PipeWire virtual microphone: an output stream whose node is announced as
 * Audio/Source/Virtual, so applications list it as a capture device.
 *
 * Threads: PipeWire's RT data thread calls on_process(), which only does a
 * lock-free vf_sink_read(). Setup/teardown happen on the control thread. */
#include "audio/vmic.h"

#include <string.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

static struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    VfSink *sink;
    int pw_inited;
} g;

static void on_process(void *userdata)
{
    (void)userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(g.stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    float *dst = (float *)buf->datas[0].data;
    if (dst) {
        uint32_t n = buf->datas[0].maxsize / (uint32_t)sizeof(float);
#if PW_CHECK_VERSION(0, 3, 49)
        if (b->requested > 0 && b->requested < n) n = (uint32_t)b->requested;
#endif
        vf_sink_read(g.sink, dst, (int)n);
        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = (int32_t)sizeof(float);
        buf->datas[0].chunk->size = n * (uint32_t)sizeof(float);
    }
    pw_stream_queue_buffer(g.stream, b);
}

static const struct pw_stream_events k_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

int vf_vmic_supported(void) { return 1; }

int vf_vmic_start(VfSink *sink)
{
    memset(&g, 0, sizeof g);
    g.sink = sink;
    pw_init(NULL, NULL);
    g.pw_inited = 1;

    g.loop = pw_thread_loop_new("mic-thingy-vmic", NULL);
    if (!g.loop) goto fail;

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CLASS, "Audio/Source/Virtual",
        PW_KEY_NODE_NAME, VF_VMIC_NODE_NAME,
        PW_KEY_NODE_DESCRIPTION, VF_VMIC_DESCRIPTION,
        PW_KEY_NODE_LATENCY, "480/48000",          /* ask the graph for a 10 ms quantum */
        PW_KEY_NODE_WANT_DRIVER, "true",
        NULL);
    if (!props) goto fail;

    g.stream = pw_stream_new_simple(pw_thread_loop_get_loop(g.loop), VF_VMIC_DESCRIPTION,
                                    props, &k_events, NULL);   /* takes ownership of props */
    if (!g.stream) goto fail;

    uint8_t pod_buf[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
    struct spa_audio_info_raw info;
    memset(&info, 0, sizeof info);
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000;
    info.channels = 1;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &info);

    /* No AUTOCONNECT: the node just sits in the graph as a source until an
     * application (Discord, OBS, pw-record) links to it. */
    if (pw_stream_connect(g.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          (enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                          params, 1) < 0)
        goto fail;
    if (pw_thread_loop_start(g.loop) < 0) goto fail;
    return 0;

fail:
    vf_vmic_stop();
    return -1;
}

void vf_vmic_stop(void)
{
    if (g.loop) pw_thread_loop_stop(g.loop);
    if (g.stream) pw_stream_destroy(g.stream);
    if (g.loop) pw_thread_loop_destroy(g.loop);
    if (g.pw_inited) pw_deinit();
    memset(&g, 0, sizeof g);
}
