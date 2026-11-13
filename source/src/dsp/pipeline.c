#include "dsp/pipeline.h"

#include <ctype.h>
#include <math.h>
#include <string.h>
#include "dsp/dsp_util.h"
#include "util/ringbuf.h"

void pipeline_init(Pipeline *p)
{
    memset(p->fx, 0, sizeof p->fx);
    p->count = 0;
    p->meter_ring = NULL;
    atomic_init(&p->scope_ring, NULL);
    memset(p->mix, 0, sizeof p->mix);
    p->n_mix = 0;
    p->mix_before = -1;
    p->mute_gain = 1.0f;
    atomic_init(&p->mute, 0);
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

int pipeline_add_mix(Pipeline *p, MixSource *src)
{
    if (p->n_mix >= VF_MAX_MIX || !src || !src->read) return -1;
    atomic_init(&src->gain, 1.0f);
    atomic_init(&src->duck, 1.0f);
    atomic_init(&src->level, 0.0f);
    src->duck_state = 1.0f;
    p->mix[p->n_mix++] = src;
    return 0;
}

void pipeline_set_mix_point(Pipeline *p, int fx_index) { p->mix_before = fx_index; }
void pipeline_set_mute(Pipeline *p, int mute) { atomic_store(&p->mute, mute ? 1 : 0); }
int  pipeline_get_mute(const Pipeline *p) { return atomic_load((atomic_int *)&p->mute); }

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

/* Mute (10 ms ramp, voice only) and then the mix bus, in front of the limiter. */
static void mute_and_mix(Pipeline *p, float *buf, const FxFrameCtx *ctx)
{
    const float target = atomic_load_explicit(&p->mute, memory_order_relaxed) ? 0.0f : 1.0f;
    if (p->mute_gain != 1.0f || target != 1.0f) {
        const float g0 = p->mute_gain;
        for (int i = 0; i < VF_FRAME_SIZE; i++)
            buf[i] *= g0 + (target - g0) * ((float)(i + 1) / (float)VF_FRAME_SIZE);
        p->mute_gain = target;
    }
    const int voice = ctx->vad_valid ? ctx->vad_prob > 0.5f : p->mute_gain > 0.5f && peak_of(buf, VF_FRAME_SIZE) > 0.01f;
    for (int m = 0; m < p->n_mix; m++) {
        MixSource *src = p->mix[m];
        float tmp[VF_FRAME_SIZE];
        src->read(src->ctx, tmp, VF_FRAME_SIZE);
        const float duck = atomic_load_explicit(&src->duck, memory_order_relaxed);
        const float want = voice ? duck : 1.0f;
        const float k = want < src->duck_state ? 0.5f : 0.1f;            /* quick duck, slower recovery (per frame) */
        src->duck_state += (want - src->duck_state) * k;
        const float g = atomic_load_explicit(&src->gain, memory_order_relaxed) * src->duck_state;
        float pk = 0.0f;
        for (int i = 0; i < VF_FRAME_SIZE; i++) {
            const float v = tmp[i] * g;
            buf[i] += v;
            if (fabsf(v) > pk) pk = fabsf(v);
        }
        atomic_store_explicit(&src->level, pk, memory_order_relaxed);
    }
}

void pipeline_process(Pipeline *p, float *buf)
{
    FxFrameCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gate_gain = 1.0f;
    ctx.limiter_gain = 1.0f;

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

    float raw[VF_FRAME_SIZE];
    struct VfRing *scope = atomic_load_explicit(&p->scope_ring, memory_order_acquire);
    if (scope) memcpy(raw, buf, sizeof raw);
    atomic_store_explicit(&p->peak_in, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);

    int mixed = 0;
    if (!atomic_load_explicit(&p->bypass_all, memory_order_relaxed)) {
        for (int i = 0; i < p->count; i++) {
            Fx *fx = p->fx[i];
            if (i == p->mix_before) { mute_and_mix(p, buf, &ctx); mixed = 1; }
            if (!atomic_load_explicit(&fx->bypass, memory_order_relaxed))
                fx->process(fx, buf, VF_FRAME_SIZE, &ctx);
        }
    }
    if (!mixed) {                       /* chain bypassed (or no mix point): same mute and bus, then a hard safety clip */
        mute_and_mix(p, buf, &ctx);
        for (int i = 0; i < VF_FRAME_SIZE; i++) buf[i] = buf[i] > 1.0f ? 1.0f : buf[i] < -1.0f ? -1.0f : buf[i];
    }

    atomic_store_explicit(&p->peak_out, peak_of(buf, VF_FRAME_SIZE), memory_order_relaxed);
    if (scope) {
        float st[2 * VF_FRAME_SIZE];
        memcpy(st, raw, sizeof raw);
        memcpy(st + VF_FRAME_SIZE, buf, sizeof raw);
        vf_ring_write(scope, st, 2 * VF_FRAME_SIZE);       /* dropped if the UI lags */
    }
    atomic_store_explicit(&p->vad, ctx.vad_valid ? ctx.vad_prob : 0.0f, memory_order_relaxed);
    atomic_store_explicit(&p->gate_gain, ctx.gate_gain, memory_order_relaxed);
    atomic_store_explicit(&p->comp_gr_db, ctx.comp_gr_db, memory_order_relaxed);

    if (p->meter_ring) {
        float rms = 0.0f;
        for (int i = 0; i < VF_FRAME_SIZE; i++) rms += buf[i] * buf[i];
        float m[VF_METER_FIELDS];
        m[VF_M_PEAK_IN] = atomic_load_explicit(&p->peak_in, memory_order_relaxed);
        m[VF_M_PEAK_OUT] = atomic_load_explicit(&p->peak_out, memory_order_relaxed);
        m[VF_M_RMS_OUT] = sqrtf(rms / VF_FRAME_SIZE);
        m[VF_M_VAD] = ctx.vad_valid ? ctx.vad_prob : 0.0f;
        m[VF_M_GATE] = ctx.gate_gain;
        m[VF_M_GR] = ctx.comp_gr_db;
        m[VF_M_LIM] = ctx.limiter_gain;
        vf_ring_write(p->meter_ring, m, VF_METER_FIELDS);   /* dropped if the UI lags */
    }
}
