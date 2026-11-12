#include "util/ini.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "util/platform.h"

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static int add(VfIni *ini, const char *sec, const char *key, const char *val)
{
    if (ini->count == ini->cap) {
        int nc = ini->cap ? ini->cap * 2 : 32;
        VfIniEntry *ne = (VfIniEntry *)realloc(ini->e, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        ini->e = ne;
        ini->cap = nc;
    }
    VfIniEntry *e = &ini->e[ini->count++];
    copy(e->section, sizeof e->section, sec);
    copy(e->key, sizeof e->key, key);
    copy(e->value, sizeof e->value, val);
    return 0;
}

static void parse_line(VfIni *ini, char *line, char *section, size_t secn)
{
    char *s = trim(line);
    if (!*s || *s == ';' || *s == '#') return;
    if (*s == '[') {
        char *end = strchr(s, ']');
        if (end) { *end = '\0'; copy(section, secn, trim(s + 1)); }
        return;
    }
    char *eq = strchr(s, '=');
    if (!eq) return;
    *eq = '\0';
    add(ini, section, trim(s), trim(eq + 1));
}

int vf_ini_parse(VfIni *ini, const char *text)
{
    memset(ini, 0, sizeof *ini);
    char section[64] = "", line[512];
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;  /* BOM */
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n') n++;
        size_t c = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, c);
        line[c] = '\0';
        parse_line(ini, line, section, sizeof section);
        p += n;
        if (*p == '\n') p++;
    }
    return 0;
}

int vf_ini_load(VfIni *ini, const char *path)
{
    memset(ini, 0, sizeof *ini);
    FILE *f = vf_fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (1 << 20)) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    vf_ini_parse(ini, buf);
    free(buf);
    return 0;
}

void vf_ini_free(VfIni *ini)
{
    free(ini->e);
    memset(ini, 0, sizeof *ini);
}

const char *vf_ini_get(const VfIni *ini, const char *section, const char *key)
{
    for (int i = ini->count - 1; i >= 0; i--)          /* last occurrence wins */
        if (eq_ci(ini->e[i].section, section) && eq_ci(ini->e[i].key, key)) return ini->e[i].value;
    return NULL;
}

int vf_ini_get_int(const VfIni *ini, const char *section, const char *key, int def)
{
    const char *v = vf_ini_get(ini, section, key);
    if (!v) return def;
    char *end;
    long r = strtol(v, &end, 10);
    return end == v ? def : (int)r;
}
