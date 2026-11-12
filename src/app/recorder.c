#include "app/recorder.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsp/fx.h"
#include "util/console.h"
#include "util/platform.h"

#define REC_RING (1 << 18)        /* floats: ~2.7 s of stereo frames of slack for slow disks */
#define CHUNK    (2 * VF_FRAME_SIZE)

void vf_rec_init(VfRecorder *r)
{
    memset(r, 0, sizeof *r);
    atomic_init(&r->run, 0);
    atomic_init(&r->failed, 0);
    atomic_init(&r->frames, 0);
}

static void rec_thread(void *arg)
{
    VfRecorder *r = (VfRecorder *)arg;
    float buf[CHUNK * 8];
    for (;;) {
        const unsigned avail = vf_ring_available(&r->ring);
        if (avail >= CHUNK) {
            unsigned want = avail < sizeof buf / sizeof buf[0] ? avail : (unsigned)(sizeof buf / sizeof buf[0]);
            want -= want % 2;
            const unsigned got = vf_ring_read(&r->ring, buf, want);
            const long fr = (long)(got / 2);
            if (vf_wavw_write_f32(&r->wav, buf, fr) != 0) { atomic_store(&r->failed, 1); atomic_store(&r->run, 0); }
            atomic_fetch_add(&r->frames, fr);
            if (atomic_load(&r->frames) >= r->max_frames) atomic_store(&r->run, 0);
        } else if (!atomic_load(&r->run)) {
            break;                                           /* stopped and drained */
        } else {
            vf_sleep_ms(20);
        }
    }
}

int vf_rec_start(VfRecorder *r, Engine *e, const char *path, int max_seconds, char *path_out)
{
    if (atomic_load(&r->run)) return -1;
    if (!r->ring_ready) {
        if (vf_ring_init(&r->ring, REC_RING) != 0) return -1;
        r->ring_ready = 1;
    }
    vf_ring_read(&r->ring, NULL, vf_ring_available(&r->ring));       /* drop stale data */
    if (path && path[0]) snprintf(r->path, sizeof r->path, "%s", path);
    else {
        char dir[VF_PATH_MAX - 128];
        vf_documents_dir(dir, sizeof dir);
        time_t t = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
        snprintf(r->path, sizeof r->path, "%s\\mic-thingy-%04d%02d%02d-%02d%02d%02d.wav", dir,
#else
        localtime_r(&t, &tmv);
        snprintf(r->path, sizeof r->path, "%s/mic-thingy-%04d%02d%02d-%02d%02d%02d.wav", dir,
#endif
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }
    if (path_out) snprintf(path_out, 1024, "%s", r->path);
    if (vf_wavw_open(&r->wav, r->path, 2, VF_SAMPLE_RATE) != 0) return -1;
    r->max_frames = (long)(max_seconds > 0 ? max_seconds : 3600) * VF_SAMPLE_RATE;
    atomic_store(&r->frames, 0);
    atomic_store(&r->failed, 0);
    atomic_store(&r->run, 1);
    r->eng = e;
    if (vf_thread_start(&r->thread, rec_thread, r) != 0) {
        atomic_store(&r->run, 0);
        vf_wavw_close(&r->wav);
        return -1;
    }
    if (e) {
        atomic_store(&e->rec_dropped, 0);
        atomic_store(&e->rec_ring, &r->ring);          /* the audio thread starts feeding the ring */
    }
    return 0;
}

int vf_rec_stop(VfRecorder *r)
{
    if (!r->thread.started && !atomic_load(&r->run)) return -1;
    if (r->eng) atomic_store(&r->eng->rec_ring, NULL);   /* no more producers... */
    vf_sleep_ms(30);                                      /* ...a callback may still be inside the old pointer */
    atomic_store(&r->run, 0);
    vf_thread_join(&r->thread);                           /* drains what is left */
    const int ok = vf_wavw_close(&r->wav) == 0 && !atomic_load(&r->failed);
    return ok ? 0 : -1;
}

int vf_rec_active(const VfRecorder *r) { return atomic_load((atomic_int *)&r->run); }

double vf_rec_seconds(const VfRecorder *r)
{
    return (double)atomic_load((atomic_long *)&r->frames) / VF_SAMPLE_RATE;
}

void vf_rec_free(VfRecorder *r)
{
    if (r->thread.started || atomic_load(&r->run)) vf_rec_stop(r);
    if (r->ring_ready) { vf_ring_free(&r->ring); r->ring_ready = 0; }
}
