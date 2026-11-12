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
#include "app/appwatch.h"
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

    /* own-voice monitor */
    float monitor_gain_db;
    int   monitor_raw;
    /* look and window */
    char  theme[16];           /* theme id, see app/theme.h */
    int   compact;             /* small always-on-top meter window */
    int   overlay;             /* in-game microphone indicator (Windows) */
    int   overlay_corner;      /* 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right */
    int   win_x, win_y, cwin_x, cwin_y;   /* window positions (normal / compact), -32000 = not stored */
    /* behaviour */
    int   howl_guard;          /* lower the own-voice monitor when feedback is detected */
    int   per_mic;             /* separate effect settings for every microphone */
    int   auto_mute_lock;      /* mute while the screen is locked */
    int   auto_mute_idle_min;  /* mute after this many minutes without speech, 0 = off */
    /* mix bus */
    float board_gain_db, sys_gain_db, sys_duck_db;
    int   sys_enabled;
    char  sys_device[256];
    char  board_path[8][260];  /* soundboard clips by path */
    /* hotkeys (text, see app/hotkeys.h) */
    char  hk_mute[48], hk_ptt[48], hk_bypass[48], hk_board[8][48];
    int   ptt_enabled;         /* push-to-talk: muted unless the PTT key is held */
    /* per-application presets */
    int   apps_enabled;
    VfAppRule apps[VF_MAX_APP_RULES];
    int   n_apps;
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

/* Text form of the [fx.*] sections (export, clipboard). Returns the length, -1 if buf is too small. */
int  vf_fx_to_text(const Pipeline *p, char *buf, size_t n);
int  vf_preset_export_text(const Pipeline *p, const char *name, char *buf, size_t n);
/* Applies a preset given as text (an exported preset or a preset file's contents). Returns the number
 * of recognised keys, or -1 if the text holds none (then nothing is changed). */
int  vf_preset_import_text(Pipeline *p, const char *text);

/* Per-microphone effect settings (profiles/<microphone>.ini in the config folder). */
int  vf_profile_exists(const char *device);
int  vf_profile_save(const Pipeline *p, const char *device);   /* 0 = ok */
int  vf_profile_load(Pipeline *p, const char *device);         /* 0 = applied, -1 = no profile */
int  vf_profile_delete(const char *device);                    /* 0 = deleted */

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
