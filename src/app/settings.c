#include "app/settings.h"

#include <stdlib.h>
#include <string.h>
#include "app/i18n.h"
#include "app/session.h"

/* ------------------------------------------------------------ built-in presets */

/* key = stable id stored in settings.ini; en/pl = display names (both accepted as input) */
typedef struct { const char *key, *en, *pl; const char *cmds[12]; } BuiltinPreset;

static const BuiltinPreset k_builtin[] = {
    { "default", "Default", "Domyślny", { NULL } },
    { "noisy", "Noisy room", "Głośne otoczenie",
      { "set highpass freq 100", "set rnnoise wet 1", "set gate threshold 0.6",
        "set gate floor -70", "set gate hold 200", NULL } },
    { "podcast", "Podcast", "Podcast",
      { "set highpass freq 70", "set comp threshold -24", "set comp ratio 3.5",
        "set comp makeup 5", "set eq low 2", "set eq mid -1", "set eq high 3",
        "set limiter ceiling -1.5", NULL } },
    { "natural", "Natural", "Naturalny",
      { "set rnnoise wet 0.85", "set gate threshold 0.4", "set gate floor -25",
        "set comp ratio 2", "set comp makeup 1", NULL } },
    /* ~1.5 ms DSP delay instead of 11.5: RNNoise gains drive a min-phase FIR, limiter without look-ahead */
    { "lowlat", "Low latency", "Niskie opóźnienie",
      { "set rnnoise lowlat 1", "set limiter lookahead 0", NULL } },
};
#define N_BUILTIN ((int)(sizeof k_builtin / sizeof k_builtin[0]))

static int find_builtin(const char *name)
{
    for (int i = 0; i < N_BUILTIN; i++)
        if (!strcmp(k_builtin[i].key, name) || !strcmp(k_builtin[i].en, name) || !strcmp(k_builtin[i].pl, name))
            return i;
    return -1;
}

const char *vf_preset_key(const char *name)
{
    int i = find_builtin(name);
    return i >= 0 ? k_builtin[i].key : name;
}

const char *vf_preset_display_name(const VfPresetInfo *p)
{
    int i = p->builtin ? find_builtin(p->name) : -1;
    if (i < 0) return p->name;
    return vf_i18n_get() == VF_LANG_PL ? k_builtin[i].pl : k_builtin[i].en;
}

/* ------------------------------------------------------------ helpers */

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

static const char *mode_name(int m)
{
    return m == VF_OUT_HEADPHONES ? "headphones" : m == VF_OUT_VMIC_MONITOR ? "vmic_monitor" : "vmic";
}

static int mode_from(const char *s, int def)
{
    if (!s) return def;
    if (!strcmp(s, "headphones")) return VF_OUT_HEADPHONES;
    if (!strcmp(s, "vmic")) return VF_OUT_VMIC;
    if (!strcmp(s, "vmic_monitor")) return VF_OUT_VMIC_MONITOR;
    return def;
}

void vf_settings_defaults(VfSettings *s)
{
    memset(s, 0, sizeof *s);
    s->mode = VF_OUT_VMIC;
    s->start_processing = 0;
    copy(s->preset, sizeof s->preset, "default");
    copy(s->language, sizeof s->language, "en");
    s->monitor_gain_db = -6.0f;
    s->monitor_raw = 0;
    snprintf(s->theme, sizeof s->theme, "graphite");
    s->overlay_corner = 1;
    s->howl_guard = 1;
    s->win_x = s->win_y = s->cwin_x = s->cwin_y = -32000;
    s->board_gain_db = 0.0f; s->sys_gain_db = -6.0f; s->sys_duck_db = -12.0f;
    snprintf(s->hk_mute, sizeof s->hk_mute, "ctrl+alt+m");
    snprintf(s->hk_ptt, sizeof s->hk_ptt, "none");
    snprintf(s->hk_bypass, sizeof s->hk_bypass, "none");
    for (int i = 0; i < 8; i++) snprintf(s->hk_board[i], sizeof s->hk_board[i], "none");
}

int vf_settings_path(char *out, size_t n)
{
    char dir[VF_PATH_MAX];
    if (vf_config_dir(dir, sizeof dir)) return -1;
#ifdef _WIN32
    return (size_t)snprintf(out, n, "%s\\settings.ini", dir) < n ? 0 : -1;
#else
    return (size_t)snprintf(out, n, "%s/settings.ini", dir) < n ? 0 : -1;
#endif
}

static int presets_dir(char *out, size_t n)
{
    char dir[VF_PATH_MAX];
    if (vf_config_dir(dir, sizeof dir)) return -1;
#ifdef _WIN32
    if ((size_t)snprintf(out, n, "%s\\presets", dir) >= n) return -1;
#else
    if ((size_t)snprintf(out, n, "%s/presets", dir) >= n) return -1;
#endif
    return vf_mkdir_p(out);
}

static int preset_path(const char *name, char *out, size_t n)
{
    char dir[VF_PATH_MAX], safe[64];
    if (vf_preset_sanitize(name, safe, sizeof safe) || presets_dir(dir, sizeof dir)) return -1;
#ifdef _WIN32
    return (size_t)snprintf(out, n, "%s\\%s.ini", dir, safe) < n ? 0 : -1;
#else
    return (size_t)snprintf(out, n, "%s/%s.ini", dir, safe) < n ? 0 : -1;
#endif
}

/* ------------------------------------------------------------ [fx.*] */

void vf_fx_write_ini(FILE *f, const Pipeline *p)
{
    for (int i = 0; i < p->count; i++) {
        const Fx *fx = p->fx[i];
        fprintf(f, "\n[fx.%s]\nenabled = %d\n", fx->name, !atomic_load(&((Fx *)fx)->bypass));
        for (int j = 0; j < fx->n_params; j++)
            fprintf(f, "%s = %.6g\n", fx->params[j].name, (double)pipeline_get_param(p, i, j));
    }
}

void vf_fx_apply_ini(Pipeline *p, const VfIni *ini)
{
    char sec[80];
    for (int i = 0; i < p->count; i++) {
        Fx *fx = p->fx[i];
        snprintf(sec, sizeof sec, "fx.%s", fx->name);
        const char *en = vf_ini_get(ini, sec, "enabled");
        if (en) pipeline_set_bypass(p, i, atoi(en) == 0);
        for (int j = 0; j < fx->n_params; j++) {
            const char *v = vf_ini_get(ini, sec, fx->params[j].name);
            if (!v) continue;
            char *end;
            float x = strtof(v, &end);
            if (end != v) pipeline_set_param(p, i, j, x);    /* clamps, rejects NaN */
        }
    }
}

void vf_fx_reset(Pipeline *p)
{
    for (int i = 0; i < p->count; i++) {
        pipeline_set_bypass(p, i, p->fx[i]->default_bypass);
        for (int j = 0; j < p->fx[i]->n_params; j++)
            pipeline_set_param(p, i, j, p->fx[i]->params[j].def);
    }
}

/* ------------------------------------------------------------ settings file */

int vf_settings_load(VfSettings *s, Pipeline *p, const char *path)
{
    VfIni ini;
    if (vf_ini_load(&ini, path) != 0) return -1;
    copy(s->input, sizeof s->input, vf_ini_get(&ini, "audio", "input"));
    copy(s->output, sizeof s->output, vf_ini_get(&ini, "audio", "output"));
    copy(s->monitor, sizeof s->monitor, vf_ini_get(&ini, "audio", "monitor"));
    s->mode = mode_from(vf_ini_get(&ini, "audio", "mode"), s->mode);
    s->bypass = vf_ini_get_int(&ini, "audio", "bypass", 0) != 0;
    s->start_processing = vf_ini_get_int(&ini, "app", "start_processing", s->start_processing) != 0;
    s->background = vf_ini_get_int(&ini, "app", "background", s->background) != 0;
    const char *pr = vf_ini_get(&ini, "app", "preset");
    if (pr) copy(s->preset, sizeof s->preset, vf_preset_key(pr));   /* old files stored Polish names */
    const char *lang = vf_ini_get(&ini, "app", "language");
    if (lang) copy(s->language, sizeof s->language, lang);
    s->monitor_gain_db = (float)vf_ini_get_double(&ini, "audio_extra", "monitor_gain_db", s->monitor_gain_db);
    s->monitor_raw = vf_ini_get_int(&ini, "audio_extra", "monitor_raw", s->monitor_raw) != 0;
    copy(s->theme, sizeof s->theme, vf_ini_get(&ini, "app", "theme"));
    if (!s->theme[0]) snprintf(s->theme, sizeof s->theme, "graphite");
    s->compact = vf_ini_get_int(&ini, "app", "compact", s->compact) != 0;
    s->overlay = vf_ini_get_int(&ini, "app", "overlay", s->overlay) != 0;
    s->overlay_corner = vf_ini_get_int(&ini, "app", "overlay_corner", s->overlay_corner) & 3;
    s->win_x = vf_ini_get_int(&ini, "window", "x", s->win_x);
    s->win_y = vf_ini_get_int(&ini, "window", "y", s->win_y);
    s->cwin_x = vf_ini_get_int(&ini, "window", "compact_x", s->cwin_x);
    s->cwin_y = vf_ini_get_int(&ini, "window", "compact_y", s->cwin_y);
    s->per_mic = vf_ini_get_int(&ini, "app", "per_mic_profiles", s->per_mic) != 0;
    s->howl_guard = vf_ini_get_int(&ini, "app", "howl_guard", s->howl_guard) != 0;
    s->auto_mute_lock = vf_ini_get_int(&ini, "app", "auto_mute_lock", s->auto_mute_lock) != 0;
    s->auto_mute_idle_min = vf_ini_get_int(&ini, "app", "auto_mute_idle_min", s->auto_mute_idle_min);
    if (s->auto_mute_idle_min < 0 || s->auto_mute_idle_min > 600) s->auto_mute_idle_min = 0;
    s->board_gain_db = (float)vf_ini_get_double(&ini, "mix", "board_gain_db", s->board_gain_db);
    s->sys_gain_db = (float)vf_ini_get_double(&ini, "mix", "sys_gain_db", s->sys_gain_db);
    s->sys_duck_db = (float)vf_ini_get_double(&ini, "mix", "sys_duck_db", s->sys_duck_db);
    s->sys_enabled = vf_ini_get_int(&ini, "mix", "sys_enabled", s->sys_enabled) != 0;
    copy(s->sys_device, sizeof s->sys_device, vf_ini_get(&ini, "mix", "sys_device"));
    char key[16];
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof key, "slot%d", i + 1);
        copy(s->board_path[i], sizeof s->board_path[i], vf_ini_get(&ini, "board", key));
        snprintf(key, sizeof key, "board%d", i + 1);
        const char *h = vf_ini_get(&ini, "hotkeys", key);
        if (h) copy(s->hk_board[i], sizeof s->hk_board[i], h);
    }
    {
        const char *h;
        if ((h = vf_ini_get(&ini, "hotkeys", "mute"))) copy(s->hk_mute, sizeof s->hk_mute, h);
        if ((h = vf_ini_get(&ini, "hotkeys", "ptt"))) copy(s->hk_ptt, sizeof s->hk_ptt, h);
        if ((h = vf_ini_get(&ini, "hotkeys", "bypass"))) copy(s->hk_bypass, sizeof s->hk_bypass, h);
    }
    s->ptt_enabled = vf_ini_get_int(&ini, "hotkeys", "ptt_enabled", s->ptt_enabled) != 0;
    s->apps_enabled = vf_ini_get_int(&ini, "apps", "enabled", s->apps_enabled) != 0;
    s->n_apps = 0;
    for (int i = 0; i < VF_MAX_APP_RULES; i++) {
        snprintf(key, sizeof key, "rule%d", i + 1);
        const char *r = vf_ini_get(&ini, "apps", key);
        const char *bar = r ? strchr(r, '|') : NULL;              /* "program.exe|preset" */
        if (!r || !bar || bar == r) continue;
        VfAppRule *ar = &s->apps[s->n_apps];
        snprintf(ar->exe, sizeof ar->exe, "%.*s", (int)(bar - r), r);
        snprintf(ar->preset, sizeof ar->preset, "%s", bar + 1);
        s->n_apps++;
    }
    if (p) {
        vf_fx_apply_ini(p, &ini);
        atomic_store(&p->bypass_all, s->bypass);
    }
    vf_ini_free(&ini);
    return 0;
}

int vf_settings_save(const VfSettings *s, const Pipeline *p, const char *path)
{
    char tmp[VF_PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = vf_fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "; mic thingy settings - written automatically, safe to edit while the app is closed\n"
               "[audio]\ninput = %s\noutput = %s\nmonitor = %s\nmode = %s\nbypass = %d\n"
               "\n[app]\nlanguage = %s\nstart_processing = %d\nbackground = %d\npreset = %s\n",
            s->input, s->output, s->monitor, mode_name(s->mode), s->bypass,
            s->language, s->start_processing, s->background, s->preset);
    fprintf(f, "theme = %s\ncompact = %d\noverlay = %d\noverlay_corner = %d\nper_mic_profiles = %d\nauto_mute_lock = %d\nauto_mute_idle_min = %d\nhowl_guard = %d\n",
            s->theme, s->compact, s->overlay, s->overlay_corner, s->per_mic, s->auto_mute_lock, s->auto_mute_idle_min, s->howl_guard);
    fprintf(f, "\n[audio_extra]\nmonitor_gain_db = %.1f\nmonitor_raw = %d\n", (double)s->monitor_gain_db, s->monitor_raw);
    fprintf(f, "\n[window]\nx = %d\ny = %d\ncompact_x = %d\ncompact_y = %d\n", s->win_x, s->win_y, s->cwin_x, s->cwin_y);
    fprintf(f, "\n[mix]\nboard_gain_db = %.1f\nsys_gain_db = %.1f\nsys_duck_db = %.1f\nsys_enabled = %d\nsys_device = %s\n",
            (double)s->board_gain_db, (double)s->sys_gain_db, (double)s->sys_duck_db, s->sys_enabled, s->sys_device);
    fprintf(f, "\n[board]\n");
    for (int i = 0; i < 8; i++) fprintf(f, "slot%d = %s\n", i + 1, s->board_path[i]);
    fprintf(f, "\n[hotkeys]\nmute = %s\nptt = %s\nptt_enabled = %d\nbypass = %s\n", s->hk_mute, s->hk_ptt, s->ptt_enabled, s->hk_bypass);
    for (int i = 0; i < 8; i++) fprintf(f, "board%d = %s\n", i + 1, s->hk_board[i]);
    fprintf(f, "\n[apps]\nenabled = %d\n", s->apps_enabled);
    for (int i = 0; i < s->n_apps; i++) fprintf(f, "rule%d = %s|%s\n", i + 1, s->apps[i].exe, s->apps[i].preset);
    if (p) vf_fx_write_ini(f, p);
    if (fclose(f) != 0) { vf_remove(tmp); return -1; }
    return vf_replace_file(tmp, path);
}

/* ------------------------------------------------------------ presets */

int vf_preset_sanitize(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
        unsigned char c = *p;
        int ok = c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == ' ' || c == '-' || c == '_';
        out[o++] = ok ? (char)c : '_';
    }
    /* don't end with a UTF-8 sequence cut in half by the length limit */
    if (o > 0) {
        size_t k = o;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0 && (unsigned char)out[k - 1] >= 0xC0) {
            unsigned char lead = (unsigned char)out[k - 1];
            size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (o - (k - 1) < need) o = k - 1;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;          /* no trailing spaces in file names */
    size_t start = 0;
    while (start < o && out[start] == ' ') start++;
    memmove(out, out + start, o - start);
    o -= start;
    out[o] = '\0';
    return o ? 0 : -1;
}

int vf_preset_is_builtin(const char *name)
{
    return find_builtin(name) >= 0;
}

typedef struct { VfPresetInfo *out; int max, n; } ListCtx;

static void on_file(const char *name, void *user)
{
    ListCtx *c = (ListCtx *)user;
    if (c->n >= c->max || vf_preset_is_builtin(name)) return;
    copy(c->out[c->n].name, sizeof c->out[c->n].name, name);
    c->out[c->n].builtin = 0;
    c->n++;
}

static int cmp_preset(const void *a, const void *b)
{
    return strcmp(((const VfPresetInfo *)a)->name, ((const VfPresetInfo *)b)->name);
}

int vf_presets_list(VfPresetInfo *out, int max)
{
    int n = 0;
    for (int i = 0; i < N_BUILTIN && n < max; i++) {
        copy(out[n].name, sizeof out[n].name, k_builtin[i].key);
        out[n++].builtin = 1;
    }
    char dir[VF_PATH_MAX];
    if (presets_dir(dir, sizeof dir) == 0) {
        ListCtx c = { out + n, max - n, 0 };
        vf_list_files(dir, ".ini", on_file, &c);
        qsort(out + n, (size_t)c.n, sizeof *out, cmp_preset);
        n += c.n;
    }
    return n;
}

int vf_preset_apply(Pipeline *p, const char *name)
{
    const int i = find_builtin(name);
    if (i >= 0) {
        vf_fx_reset(p);
        for (int k = 0; k_builtin[i].cmds[k]; k++) {
            /* built-in presets are "set <fx> <param> <value>" lines */
            char fxn[32], pn[32];
            float v;
            if (sscanf(k_builtin[i].cmds[k], "set %31s %31s %f", fxn, pn, &v) != 3) return -1;
            int fi = pipeline_find_fx(p, fxn), pi = pipeline_find_param(p, fi, pn);
            if (fi < 0 || pi < 0) return -1;
            pipeline_set_param(p, fi, pi, v);
        }
        return 0;
    }
    char path[VF_PATH_MAX];
    VfIni ini;
    if (preset_path(name, path, sizeof path) || vf_ini_load(&ini, path)) return -1;
    vf_fx_reset(p);
    vf_fx_apply_ini(p, &ini);
    vf_ini_free(&ini);
    return 0;
}

int vf_preset_save(const Pipeline *p, const char *name)
{
    char safe[64], path[VF_PATH_MAX], tmp[VF_PATH_MAX + 8];
    if (vf_preset_sanitize(name, safe, sizeof safe) || vf_preset_is_builtin(safe)) return -1;
    if (preset_path(safe, path, sizeof path)) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = vf_fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "; mic thingy preset \"%s\"\n", safe);
    vf_fx_write_ini(f, p);
    if (fclose(f) != 0) { vf_remove(tmp); return -1; }
    return vf_replace_file(tmp, path);
}

int vf_preset_delete(const char *name)
{
    char path[VF_PATH_MAX];
    if (vf_preset_is_builtin(name) || preset_path(name, path, sizeof path)) return -1;
    return vf_remove(path);
}
