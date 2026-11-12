#include "app/undo.h"

#include <math.h>
#include <string.h>

void vf_snap_capture(const Pipeline *p, VfSnap *s)
{
    memset(s, 0, sizeof *s);
    s->count = p->count;
    for (int i = 0; i < p->count; i++) {
        s->bypass[i] = (char)(atomic_load((atomic_int *)&p->fx[i]->bypass) ? 1 : 0);
        for (int j = 0; j < p->fx[i]->n_params; j++) s->v[i][j] = pipeline_get_param(p, i, j);
    }
}

void vf_snap_apply(Pipeline *p, const VfSnap *s)
{
    for (int i = 0; i < p->count && i < s->count; i++) {
        pipeline_set_bypass(p, i, s->bypass[i]);
        for (int j = 0; j < p->fx[i]->n_params; j++) pipeline_set_param(p, i, j, s->v[i][j]);
    }
}

int vf_snap_equal(const VfSnap *a, const VfSnap *b)
{
    if (a->count != b->count) return 0;
    for (int i = 0; i < a->count; i++) {
        if (a->bypass[i] != b->bypass[i]) return 0;
        for (int j = 0; j < VF_MAX_PARAMS; j++) if (fabsf(a->v[i][j] - b->v[i][j]) > 1e-6f) return 0;
    }
    return 1;
}

void vf_undo_init(VfUndo *u, const Pipeline *p)
{
    memset(u, 0, sizeof *u);
    vf_snap_capture(p, &u->snap[0]);
    u->count = 1;
    u->cur = 0;
}

int vf_undo_commit(VfUndo *u, const Pipeline *p)
{
    VfSnap now;
    vf_snap_capture(p, &now);
    if (vf_snap_equal(&now, &u->snap[u->cur])) return 0;
    u->count = u->cur + 1;                              /* drop the redo tail */
    if (u->count == VF_UNDO_DEPTH) {                    /* full: forget the oldest */
        memmove(&u->snap[0], &u->snap[1], (size_t)(VF_UNDO_DEPTH - 1) * sizeof(VfSnap));
        u->count--;
    }
    u->snap[u->count++] = now;
    u->cur = u->count - 1;
    return 1;
}

int vf_undo_can_undo(const VfUndo *u) { return u->cur > 0; }
int vf_undo_can_redo(const VfUndo *u) { return u->cur + 1 < u->count; }

int vf_undo_undo(VfUndo *u, Pipeline *p)
{
    if (!vf_undo_can_undo(u)) return 0;
    u->cur--;
    vf_snap_apply(p, &u->snap[u->cur]);
    return 1;
}

int vf_undo_redo(VfUndo *u, Pipeline *p)
{
    if (!vf_undo_can_redo(u)) return 0;
    u->cur++;
    vf_snap_apply(p, &u->snap[u->cur]);
    return 1;
}
