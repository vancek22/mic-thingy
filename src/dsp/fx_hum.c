#include "dsp/fx_hum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/biquad.h"
#include "dsp/dsp_util.h"

#define MAX_NOTCH 6
#define NOTCH_Q   30.0f

static const FxParamInfo k_params[] = {
    { "freq",      "Hz", 40.0f, 70.0f, 50.0f, 0 },
    { "harmonics", "",   1.0f,  6.0f,  4.0f,  0 },
    { "auto",      "",   0.0f,  1.0f,  0.0f,  1 },
};

typedef struct {
    float sr, freq;
    int harmonics, autodetect;
    Biquad bq[MAX_NOTCH];
    double gs1[4], gs2[4], gcoef[4];   /* running Goertzel at 50, 60, 100, 120 Hz over 100 ms windows */
    int gn;
} HumState;

/* New coefficients, state kept (no click when the notches move or are added). */
static void design(HumState *s)
{
    for (int k = 0; k < MAX_NOTCH; k++) {
        const float f = s->freq * (float)(k + 1);
        const float z1 = s->bq[k].z1, z2 = s->bq[k].z2;
        if (k < s->harmonics && f < s->sr * 0.45f) biquad_notch(&s->bq[k], s->sr, f, NOTCH_Q);
        else biquad_reset(&s->bq[k]);
        s->bq[k].z1 = z1; s->bq[k].z2 = z2;
    }
}

static int hum_init(Fx *fx, int sr)
{
    HumState *s = (HumState *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->sr = (float)sr;
    s->freq = k_params[HUM_FREQ].def;
    s->harmonics = (int)k_params[HUM_HARMONICS].def;
    static const double hz[4] = { 50.0, 60.0, 100.0, 120.0 };
    for (int k = 0; k < 4; k++) s->gcoef[k] = 2.0 * cos(2.0 * 3.14159265358979 * hz[k] / sr);
    for (int k = 0; k < MAX_NOTCH; k++) biquad_reset(&s->bq[k]);
    design(s);
    fx->state = s;
    return 0;
}

static void hum_process(Fx *fx, float *buf, int n, FxFrameCtx *ctx)
{
    (void)ctx;
    HumState *s = (HumState *)fx->state;
    for (int i = 0; i < n; i++) {
        float x = buf[i];
        if (s->autodetect) {                           /* measured on the incoming signal */
            for (int k = 0; k < 4; k++) {
                const double s0 = x + s->gcoef[k] * s->gs1[k] - s->gs2[k];
                s->gs2[k] = s->gs1[k]; s->gs1[k] = s0;
            }
            if (++s->gn == 4800) {                     /* every 100 ms: 10 Hz resolution */
                double p[4];
                for (int k = 0; k < 4; k++) {
                    p[k] = s->gs1[k] * s->gs1[k] + s->gs2[k] * s->gs2[k] - s->gcoef[k] * s->gs1[k] * s->gs2[k];
                    s->gs1[k] = s->gs2[k] = 0.0;
                }
                s->gn = 0;
                const double e50 = p[0] + p[2], e60 = p[1] + p[3];
                const float want = e60 > 3.0 * e50 ? 60.0f : e50 > 3.0 * e60 ? 50.0f : s->freq;
                if (want != s->freq) { s->freq = want; design(s); }
            }
        }
        for (int k = 0; k < s->harmonics; k++) x = biquad_tick(&s->bq[k], x);
        buf[i] = x;
    }
}

static void hum_set_param(Fx *fx, int id, float v)
{
    HumState *s = (HumState *)fx->state;
    v = vf_clampf(v, k_params[id].min, k_params[id].max);
    if (id == HUM_FREQ) { s->freq = v; design(s); }
    else if (id == HUM_HARMONICS) { s->harmonics = (int)(v + 0.5f); design(s); }
    else if (id == HUM_AUTO) s->autodetect = v >= 0.5f;
}

static void hum_destroy(Fx *fx) { free(fx->state); fx->state = NULL; }

void fx_hum_make(Fx *fx)
{
    memset(fx, 0, sizeof *fx);
    fx->name = "hum";
    fx->init = hum_init;
    fx->process = hum_process;
    fx->set_param = hum_set_param;
    fx->destroy = hum_destroy;
    fx->params = k_params;
    fx->n_params = (int)(sizeof k_params / sizeof k_params[0]);
    fx->default_bypass = 1;
    fx->page = 1;
    atomic_init(&fx->latency, 0);
    atomic_init(&fx->bypass, 1);
}
