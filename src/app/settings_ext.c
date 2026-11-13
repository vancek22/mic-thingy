/* Per-microphone profiles, preset export/import as text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/settings.h"

/* ------------------------------------------------------------ text form of the [fx.*] sections */

int vf_fx_to_text(const Pipeline *p, char *buf, size_t n)
{
    size_t used = 0;
    if (n == 0) return -1;
    buf[0] = '\0';
    for (int i = 0; i < p->count; i++) {
        const Fx *fx = p->fx[i];
        int w = snprintf(buf + used, n - used, "\n[fx.%s]\nenabled = %d\n", fx->name, !atomic_load(&((Fx *)fx)->bypass));
        if (w < 0 || (size_t)w >= n - used) return -1;
        used += (size_t)w;
        for (int j = 0; j < fx->n_params; j++) {
            w = snprintf(buf + used, n - used, "%s = %.6g\n", fx->params[j].name, (double)pipeline_get_param(p, i, j));
            if (w < 0 || (size_t)w >= n - used) return -1;
            used += (size_t)w;
        }
    }
    return (int)used;
}

int vf_preset_export_text(const Pipeline *p, const char *name, char *buf, size_t n)
{
    char safe[64];
    if (!name || vf_preset_sanitize(name, safe, sizeof safe) != 0) snprintf(safe, sizeof safe, "exported");
    int w = snprintf(buf, n, "; mic thingy preset \"%s\"", safe);
    if (w < 0 || (size_t)w >= n) return -1;
    const int t = vf_fx_to_text(p, buf + w, n - (size_t)w);
    return t < 0 ? -1 : w + t;
}

int vf_preset_import_text(Pipeline *p, const char *text)
{
    VfIni ini;
    if (!text || vf_ini_parse(&ini, text) != 0) return -1;
    int known = 0;
    char sec[80];
    for (int i = 0; i < p->count; i++) {
        snprintf(sec, sizeof sec, "fx.%s", p->fx[i]->name);
        if (vf_ini_get(&ini, sec, "enabled")) known++;
        for (int j = 0; j < p->fx[i]->n_params; j++) if (vf_ini_get(&ini, sec, p->fx[i]->params[j].name)) known++;
    }
    if (known == 0) { vf_ini_free(&ini); return -1; }        /* not a preset: change nothing */
    vf_fx_apply_ini(p, &ini);
    vf_ini_free(&ini);
    return known;
}

/* ------------------------------------------------------------ per-microphone profiles */

static int profile_path(const char *device, char *out, size_t n)
{
    char dir[VF_PATH_MAX - 64], safe[96];
    if (!device || vf_preset_sanitize(device, safe, sizeof safe) != 0) return -1;
    if (vf_config_dir(dir, sizeof dir) != 0) return -1;
#ifdef _WIN32
    char sub[VF_PATH_MAX];
    snprintf(sub, sizeof sub, "%s\\profiles", dir);
    if (vf_mkdir_p(sub) != 0) return -1;
    return (size_t)snprintf(out, n, "%s\\%s.ini", sub, safe) < n ? 0 : -1;
#else
    char sub[VF_PATH_MAX];
    snprintf(sub, sizeof sub, "%s/profiles", dir);
    if (vf_mkdir_p(sub) != 0) return -1;
    return (size_t)snprintf(out, n, "%s/%s.ini", sub, safe) < n ? 0 : -1;
#endif
}

int vf_profile_exists(const char *device)
{
    char path[VF_PATH_MAX];
    if (profile_path(device, path, sizeof path) != 0) return 0;
    FILE *f = vf_fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int vf_profile_save(const Pipeline *p, const char *device)
{
    char path[VF_PATH_MAX], tmp[VF_PATH_MAX + 8];
    if (profile_path(device, path, sizeof path) != 0) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = vf_fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "; mic thingy profile\n");     /* the device name stays out of the file: it could contain a line break */
    vf_fx_write_ini(f, p);
    if (fclose(f) != 0) { vf_remove(tmp); return -1; }
    return vf_replace_file(tmp, path);
}

int vf_profile_load(Pipeline *p, const char *device)
{
    char path[VF_PATH_MAX];
    VfIni ini;
    if (profile_path(device, path, sizeof path) != 0 || vf_ini_load(&ini, path) != 0) return -1;
    vf_fx_apply_ini(p, &ini);
    vf_ini_free(&ini);
    return 0;
}

int vf_profile_delete(const char *device)
{
    char path[VF_PATH_MAX];
    if (profile_path(device, path, sizeof path) != 0) return -1;
    return vf_remove(path);
}
