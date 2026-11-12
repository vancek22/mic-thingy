#include "dsp/pipeline.h"

#include <math.h>

void pipeline_init(Pipeline *p)
{
    p->count = 0;
    atomic_init(&p->bypass_all, 0);
    atomic_init(&p->peak_in, 0.0f);
    atomic_init(&p->peak_out, 0.0f);
    atomic_init(&p->vad, 0.0f);
}

int pipeline_add(Pipeline *p, Fx *fx)
{
    if (p->count >= VF_MAX_FX) return -1;
    p->fx[p->count++] = fx;
    return 0;
}

int pipeline_start(Pipeline *p, int sample_rate)
{
    for (int i = 0; i < p->count; i++) {
        if (p->fx[i]->init(p->fx[i], sample_rate) != 0) {
            while (--i >= 0) p->fx[i]->destroy(p->fx[i]);
            return -1;
        }
    }
    return 0;
}

void pipeline_stop(Pipeline *p)
{
    for (int i = 0; i < p->count; i++)
        if (p->fx[i]->destroy) p->fx[i]->destroy(p->fx[i]);
}

static float peak_of(const float *b, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; i++) { float a = fabsf(b[i]); if (a > m) m = a; }
    return m;
}

void pipeline_process(Pipeline *p, float *buf)
{
    FxFrameCtx ctx = { 0.0f };
    atomic_store_explicit(&p->peak_in, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);

    if (!atomic_load_explicit(&p->bypass_all, memory_order_relaxed)) {
        for (int i = 0; i < p->count; i++) {
            Fx *fx = p->fx[i];
            if (!atomic_load_explicit(&fx->bypass, memory_order_relaxed))
                fx->process(fx, buf, VF_FRAME_SIZE, &ctx);
        }
    }

    atomic_store_explicit(&p->peak_out, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);
    atomic_store_explicit(&p->vad, ctx.vad_prob, memory_order_relaxed);
}
