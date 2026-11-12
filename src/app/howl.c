#include "app/howl.h"

#include <string.h>

#define MIN_LEVEL  (-32.0f)      /* dBFS: quiet lines are not a danger */
#define ISOLATION  18.0f         /* dB above the mean of the bins 4..10 away on both sides */
#define NEED_RUN   25            /* updates (~0.5 s at 50 Hz) */
#define NEED_RISE  3.0f          /* dB of growth over that time */

void vf_howl_init(VfHowl *h)
{
    h->cand = -1;
    h->run = 0;
    h->level0 = h->last = -120.0f;
}

float vf_howl_update(VfHowl *h, const float *db)
{
    int best = 1;
    for (int k = 2; k < VF_SPEC_BINS - 1; k++) if (db[k] > db[best]) best = k;
    const float lvl = db[best];

    float side = 0.0f;
    int n = 0;
    for (int d = 4; d <= 10; d++) {
        if (best - d >= 0) { side += db[best - d]; n++; }
        if (best + d < VF_SPEC_BINS) { side += db[best + d]; n++; }
    }
    const int isolated = n > 0 && lvl > MIN_LEVEL && lvl - side / (float)n > ISOLATION;

    if (!isolated) { h->cand = -1; h->run = 0; return 0.0f; }
    if (h->cand < 0 || best < h->cand - 1 || best > h->cand + 1) {      /* a new line: start following it */
        h->cand = best;
        h->run = 1;
        h->level0 = h->last = lvl;
        return 0.0f;
    }
    h->cand = best;
    h->run++;
    h->last = lvl;
    if (h->run >= NEED_RUN && lvl - h->level0 > NEED_RISE) {
        const float hz = (float)best * 48000.0f / (float)VF_SPEC_N;
        h->run = 0;                                                     /* fire once, then look again */
        h->level0 = lvl;
        return hz;
    }
    if (h->run > 4 * NEED_RUN) { h->run = NEED_RUN; h->level0 = lvl; }  /* a steady tone is not feedback */
    return 0.0f;
}
