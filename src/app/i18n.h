/* UI translations. English is the default; Polish is the second language.
 * Strings are UTF-8. tr() is cheap (array lookup) - call it every frame. */
#ifndef VF_I18N_H
#define VF_I18N_H

typedef enum { VF_LANG_EN = 0, VF_LANG_PL = 1, VF_LANG_COUNT } VfLang;

typedef enum {
    S_MICROPHONE, S_OUTPUT, S_MODES, S_SPEAKERS, S_MONITOR, S_VMIC_HINT,
    S_FILTER_ON, S_FILTER_OFF, S_START, S_STOP,
    S_PRESET, S_SAVE_AS, S_DELETE, S_START_WITH_SYSTEM, S_FILTER_ON_LAUNCH, S_RUN_IN_BACKGROUND,
    S_LEVELS, S_INPUT, S_OUTPUT_LEVEL, S_VAD, S_VAD_FMT, S_GATE_OPEN, S_COMPRESSION,
    S_LATENCY, S_LAT_PW_FMT, S_LAT_FMT, S_DROPOUTS_FMT,
    S_TABS_FX,
    S_ENABLED, S_DEFAULTS, S_NO_DEVICES,
    S_ST_RUNNING, S_ST_TESTFILE, S_ST_STOPPED, S_ST_ERROR,
    S_N_NAME_MISSING, S_N_BUILTIN_NAME, S_N_SAVE_FAILED, S_N_SAVED, S_N_DELETED, S_N_LOADED,
    S_N_LOAD_FAILED, S_N_AUTOSTART_ON, S_N_AUTOSTART_OFF, S_N_AUTOSTART_FAIL,
    S_N_BG_TRAY, S_N_BG_MIN, S_N_BG_OFF, S_N_IN_TRAY, S_N_SAVE_SETTINGS_FAIL,
    S_N_DEVICE_LOST, S_N_RETRY_FMT, S_N_RESAMPLE_FMT,
    S_TRAY_SHOW, S_TRAY_PROCESSING, S_TRAY_FILTER, S_TRAY_QUIT,
    S_TRAY_TIP_STOPPED, S_TRAY_TIP_BYPASS, S_TRAY_TIP_ON,
    S_SUBTITLE, S_LANGUAGE,
    S_COUNT
} VfStr;

void        vf_i18n_set(VfLang lang);
VfLang      vf_i18n_get(void);
const char *tr(VfStr id);
const char *vf_lang_code(VfLang lang);             /* "en" / "pl" */
VfLang      vf_lang_from_code(const char *code);   /* unknown -> English */

const char *tr_fx(const char *fx_name);                         /* effect panel title */
const char *tr_param(const char *fx_name, const char *param);   /* slider label */

#endif
