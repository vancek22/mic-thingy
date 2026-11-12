/* Persistent settings (settings.ini in the user config directory) and presets.
 *
 * settings.ini
 *   [audio]   input / output / monitor = device names (names, not indices: indices change)
 *             mode = headphones | vmic | vmic_monitor, bypass = 0|1
 *   [app]     start_processing = 0|1 (start filtering when the app opens), preset = name
 *   [fx.<effect>]  enabled = 0|1, <param> = value     (e.g. [fx.gate] threshold = 0.5)
 * presets/<name>.ini hold only [fx.*] sections. Built-in presets live in code. */
#ifndef VF_SETTINGS_H
#define VF_SETTINGS_H

#include <stdio.h>
#include "dsp/pipeline.h"
#include "util/ini.h"
#include "util/platform.h"

typedef struct VfSettings {
    char input[256], output[256], monitor[256];
    int  mode;                 /* VfOutMode */
    int  bypass;
    int  start_processing;
    int  background;           /* closing the window keeps mic thingy running (tray) */
    char preset[64];           /* built-in key ("default", "podcast"...) or user preset name */
    char language[8];          /* "en" (default) or "pl" */
} VfSettings;

void vf_settings_defaults(VfSettings *s);
int  vf_settings_path(char *out, size_t n);
/* Loads settings and applies the [fx.*] sections to the pipeline. 0 = ok, -1 = no file (defaults kept). */
int  vf_settings_load(VfSettings *s, Pipeline *p, const char *path);
int  vf_settings_save(const VfSettings *s, const Pipeline *p, const char *path);   /* 0 = ok */

/* [fx.*] sections */
void vf_fx_write_ini(FILE *f, const Pipeline *p);
void vf_fx_apply_ini(Pipeline *p, const VfIni *ini);   /* keys present only; values clamped */
void vf_fx_reset(Pipeline *p);                         /* defaults, all effects enabled */

/* presets */
#define VF_MAX_PRESETS 64
typedef struct VfPresetInfo { char name[64]; int builtin; } VfPresetInfo;

int  vf_presets_list(VfPresetInfo *out, int max);      /* built-ins (name = key) first, then user files */
const char *vf_preset_display_name(const VfPresetInfo *p);   /* built-ins in the UI language */
const char *vf_preset_key(const char *name);           /* any built-in name -> its key; others unchanged */
int  vf_preset_apply(Pipeline *p, const char *name);   /* resets to defaults first; 0 = ok */
int  vf_preset_save(const Pipeline *p, const char *name);     /* user preset; refuses built-in names */
int  vf_preset_delete(const char *name);                       /* user presets only */
int  vf_preset_is_builtin(const char *name);
/* Makes a name safe as a file name (keeps letters incl. UTF-8, digits, space, - _). 0 = non-empty result. */
int  vf_preset_sanitize(const char *in, char *out, size_t n);

#endif
