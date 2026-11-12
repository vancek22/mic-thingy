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
        do { n = add_unique(out, n, max, pe.szExeFile); } while (Process32Next(snap, &pe));
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
