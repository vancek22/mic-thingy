#include "app/appwatch.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#endif

static void lower_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

int vf_appwatch_match(const VfAppRule *rules, int n_rules, char (*running)[VF_APP_NAME], int n_running)
{
    for (int r = 0; r < n_rules; r++) {
        char want[VF_APP_NAME];
        if (!rules[r].exe[0]) continue;
        lower_copy(want, sizeof want, rules[r].exe);
        for (int i = 0; i < n_running; i++) {
            char have[VF_APP_NAME];
            lower_copy(have, sizeof have, running[i]);
            if (!strcmp(want, have)) return r;
        }
    }
    return -1;
}

static int add_unique(char (*out)[VF_APP_NAME], int n, int max, const char *name)
{
    char low[VF_APP_NAME];
    lower_copy(low, sizeof low, name);
    for (int i = 0; i < n; i++) if (!strcmp(out[i], low)) return n;
    if (n >= max) return n;
    memcpy(out[n], low, sizeof low);
    return n + 1;
}

int vf_appwatch_list(char (*out)[VF_APP_NAME], int max)
{
    int n = 0;
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            const char *nm = pe.szExeFile;
            const size_t L = strlen(nm);
            if (L > 4 && !_stricmp(nm + L - 4, ".exe")) n = add_unique(out, n, max, nm);   /* skips [System Process] and friends */
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
#else
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[64], name[VF_APP_NAME] = "";
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) { name[strcspn(name, "\n")] = '\0'; n = add_unique(out, n, max, name); }
        fclose(f);
    }
    closedir(d);
#endif
    return n;
}

static int ci_contains(const char *hay, const char *needle)
{
    const size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int ci_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        const int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) return d;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int vf_appwatch_filter(char (*list)[VF_APP_NAME], int n, const char *needle, int *idx, int max)
{
    char nd[VF_APP_NAME];
    snprintf(nd, sizeof nd, "%s", needle ? needle : "");
    char *s = nd;
    while (*s == ' ') s++;
    size_t l = strlen(s);
    while (l > 0 && s[l - 1] == ' ') s[--l] = '\0';
    int c = 0;
    for (int i = 0; i < n && c < max; i++)
        if (ci_contains(list[i], s)) idx[c++] = i;
    for (int i = 1; i < c; i++) {                       /* insertion sort: at most a few hundred names */
        const int v = idx[i];
        int j = i - 1;
        while (j >= 0 && ci_cmp(list[idx[j]], list[v]) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    return c;
}
