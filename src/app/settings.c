#include "app/settings.h"

#include <stdlib.h>
#include <string.h>
#include "app/session.h"

/* ------------------------------------------------------------ built-in presets */

typedef struct { const char *name; const char *cmds[12]; } BuiltinPreset;

static const BuiltinPreset k_builtin[] = {
    { "Domyślny", { NULL } },
    { "Głośne otoczenie", { "set highpass freq 100", "set rnnoise wet 1", "set gate threshold 0.6",
                            "set gate floor -70", "set gate hold 200", NULL } },
    { "Podcast", { "set highpass freq 70", "set comp threshold -24", "set comp ratio 3.5",
                   "set comp makeup 5", "set eq low 2", "set eq mid -1", "set eq high 3",
                   "set limiter ceiling -1.5", NULL } },
    { "Naturalny", { "set rnnoise wet 0.85", "set gate threshold 0.4", "set gate floor -25",
                     "set comp ratio 2", "set comp makeup 1", NULL } },
};
#define N_BUILTIN ((int)(sizeof k_builtin / sizeof k_builtin[0]))

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
    copy(s->preset, sizeof s->preset, "Domyślny");
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
        pipeline_set_bypass(p, i, 0);
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
    const char *pr = vf_ini_get(&ini, "app", "preset");
    if (pr) copy(s->preset, sizeof s->preset, pr);
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
    fprintf(f, "; VoiceFilter settings - written automatically, safe to edit while the app is closed\n"
               "[audio]\ninput = %s\noutput = %s\nmonitor = %s\nmode = %s\nbypass = %d\n"
               "\n[app]\nstart_processing = %d\npreset = %s\n",
            s->input, s->output, s->monitor, mode_name(s->mode), s->bypass,
            s->start_processing, s->preset);
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
    for (int i = 0; i < N_BUILTIN; i++)
        if (!strcmp(k_builtin[i].name, name)) return 1;
    return 0;
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
        copy(out[n].name, sizeof out[n].name, k_builtin[i].name);
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
    for (int i = 0; i < N_BUILTIN; i++) {
        if (strcmp(k_builtin[i].name, name)) continue;
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
    fprintf(f, "; VoiceFilter preset \"%s\"\n", safe);
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
