#include "dsp/pipeline.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "dsp/dsp_util.h"

void pipeline_init(Pipeline *p)
{
    memset(p->fx, 0, sizeof p->fx);
    p->count = 0;
    atomic_init(&p->bypass_all, 0);
    atomic_init(&p->peak_in, 0.0f);
    atomic_init(&p->peak_out, 0.0f);
    atomic_init(&p->vad, 0.0f);
    atomic_init(&p->gate_gain, 1.0f);
    atomic_init(&p->comp_gr_db, 0.0f);
    for (int i = 0; i < VF_MAX_FX; i++)
        for (int j = 0; j < VF_MAX_PARAMS; j++) {
            atomic_init(&p->params[i][j], 0.0f);
            p->applied[i][j] = 0.0f;
        }
}

int pipeline_add(Pipeline *p, Fx *fx)
{
    if (p->count >= VF_MAX_FX || fx->n_params > VF_MAX_PARAMS) return -1;
    p->fx[p->count++] = fx;
    return 0;
}

int pipeline_start(Pipeline *p, int sample_rate)
{
    for (int i = 0; i < p->count; i++) {
        Fx *fx = p->fx[i];
        if (fx->init(fx, sample_rate) != 0) {
            while (--i >= 0) p->fx[i]->destroy(p->fx[i]);
            return -1;
        }
        for (int j = 0; j < fx->n_params; j++) {
            float def = fx->params[j].def;
            atomic_store(&p->params[i][j], def);
            p->applied[i][j] = def;
            if (fx->set_param) fx->set_param(fx, j, def);
        }
    }
    return 0;
}

void pipeline_stop(Pipeline *p)
{
    for (int i = 0; i < p->count; i++)
        if (p->fx[i]->destroy) p->fx[i]->destroy(p->fx[i]);
}

int pipeline_set_param(Pipeline *p, int fi, int id, float value)
{
    if (fi < 0 || fi >= p->count || id < 0 || id >= p->fx[fi]->n_params || isnan(value))
        return -1;
    const FxParamInfo *pi = &p->fx[fi]->params[id];
    atomic_store_explicit(&p->params[fi][id], vf_clampf(value, pi->min, pi->max),
                          memory_order_relaxed);
    return 0;
}

float pipeline_get_param(const Pipeline *p, int fi, int id)
{
    return atomic_load_explicit(&((Pipeline *)p)->params[fi][id], memory_order_relaxed);
}

void pipeline_set_bypass(Pipeline *p, int fi, int bypass)
{
    if (fi >= 0 && fi < p->count) atomic_store(&p->fx[fi]->bypass, bypass ? 1 : 0);
}

int pipeline_delay_samples(const Pipeline *p)
{
    if (atomic_load(&((Pipeline *)p)->bypass_all)) return 0;
    int d = 0;
    for (int i = 0; i < p->count; i++)
        if (!atomic_load(&p->fx[i]->bypass)) d += atomic_load(&p->fx[i]->latency);
    return d;
}

static int prefix_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

static int equal_ci(const char *a, const char *b)
{
    return prefix_ci(a, b) && strlen(a) == strlen(b);
}

int pipeline_find_fx(const Pipeline *p, const char *name)
{
    for (int i = 0; i < p->count; i++)
        if (equal_ci(p->fx[i]->name, name)) return i;
    for (int i = 0; i < p->count; i++)
        if (prefix_ci(p->fx[i]->name, name)) return i;
    return -1;
}

int pipeline_find_param(const Pipeline *p, int fi, const char *name)
{
    if (fi < 0 || fi >= p->count) return -1;
    const Fx *fx = p->fx[fi];
    for (int j = 0; j < fx->n_params; j++)
        if (equal_ci(fx->params[j].name, name)) return j;
    for (int j = 0; j < fx->n_params; j++)
        if (prefix_ci(fx->params[j].name, name)) return j;
    return -1;
}

static float peak_of(const float *b, int n)
{
    float m = 0.0f;
    for (int i = 0; i < n; i++) { float a = fabsf(b[i]); if (a > m) m = a; }
    return m;
}

void pipeline_process(Pipeline *p, float *buf)
{
    FxFrameCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gate_gain = 1.0f;

    /* Pull changed parameters from the UI. */
    for (int i = 0; i < p->count; i++) {
        Fx *fx = p->fx[i];
        for (int j = 0; j < fx->n_params; j++) {
            const float v = atomic_load_explicit(&p->params[i][j], memory_order_relaxed);
            if (v != p->applied[i][j]) {
                p->applied[i][j] = v;
                if (fx->set_param) fx->set_param(fx, j, v);
            }
        }
    }

    atomic_store_explicit(&p->peak_in, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);

    if (!atomic_load_explicit(&p->bypass_all, memory_order_relaxed)) {
        for (int i = 0; i < p->count; i++) {
            Fx *fx = p->fx[i];
            if (!atomic_load_explicit(&fx->bypass, memory_order_relaxed))
                fx->process(fx, buf, VF_FRAME_SIZE, &ctx);
        }
    }

    atomic_store_explicit(&p->peak_out, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);
    atomic_store_explicit(&p->vad, ctx.vad_valid ? ctx.vad_prob : 0.0f, memory_order_relaxed);
    atomic_store_explicit(&p->gate_gain, ctx.gate_gain, memory_order_relaxed);
    atomic_store_explicit(&p->comp_gr_db, ctx.comp_gr_db, memory_order_relaxed);
}
