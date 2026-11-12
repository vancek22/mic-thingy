/* Undo / redo of effect settings (all parameters + on/off of every effect), and plain snapshots
 * (used by "compare with the saved preset"). Control thread only. */
#ifndef VF_UNDO_H
#define VF_UNDO_H

#include "dsp/pipeline.h"

#define VF_UNDO_DEPTH 64

typedef struct VfSnap {
    float v[VF_MAX_FX][VF_MAX_PARAMS];
    char  bypass[VF_MAX_FX];
    int   count;                 /* effects in the pipeline when taken */
} VfSnap;

void vf_snap_capture(const Pipeline *p, VfSnap *s);
void vf_snap_apply(Pipeline *p, const VfSnap *s);
int  vf_snap_equal(const VfSnap *a, const VfSnap *b);

typedef struct VfUndo {
    VfSnap snap[VF_UNDO_DEPTH];
    int count;                   /* valid snapshots */
    int cur;                     /* index of the current state */
} VfUndo;

void vf_undo_init(VfUndo *u, const Pipeline *p);       /* the current state becomes the first entry */
/* Call when an edit is finished (slider released, value typed, button): records the state if it
 * differs from the current entry and drops the redo tail. Returns 1 if something was recorded. */
int  vf_undo_commit(VfUndo *u, const Pipeline *p);
int  vf_undo_can_undo(const VfUndo *u);
int  vf_undo_can_redo(const VfUndo *u);
int  vf_undo_undo(VfUndo *u, Pipeline *p);              /* 1 = applied */
int  vf_undo_redo(VfUndo *u, Pipeline *p);

#endif
