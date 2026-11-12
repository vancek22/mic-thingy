#include "util/platform.h"

#include <stdlib.h>
#include <string.h>

#define VF_AUTOSTART_NAME "mic thingy"
#define VF_AUTOSTART_ARGS "--autostart --minimized"

/* ======================================================================= Windows */
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>

static int to_w(const char *s, wchar_t *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0 ? 0 : -1;
}
static int to_u8(const wchar_t *w, char *s, int n)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) > 0 ? 0 : -1;
}

FILE *vf_fopen(const char *path, const char *mode)
{
    wchar_t wp[VF_PATH_MAX], wm[16];
    if (to_w(path, wp, VF_PATH_MAX) || to_w(mode, wm, 16)) return NULL;
    return _wfopen(wp, wm);
}

int vf_mkdir_p(const char *path)
{
    wchar_t wp[VF_PATH_MAX];
    if (to_w(path, wp, VF_PATH_MAX)) return -1;
    for (wchar_t *p = wp + 1; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p[-1] != L':') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(wp, NULL);
            *p = c;
        }
    }
    if (!CreateDirectoryW(wp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
    return 0;
}

int vf_remove(const char *path)
{
    wchar_t wp[VF_PATH_MAX];
    if (to_w(path, wp, VF_PATH_MAX)) return -1;
    return DeleteFileW(wp) ? 0 : -1;
}

int vf_replace_file(const char *tmp, const char *dst)
{
    wchar_t a[VF_PATH_MAX], b[VF_PATH_MAX];
    if (to_w(tmp, a, VF_PATH_MAX) || to_w(dst, b, VF_PATH_MAX)) return -1;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

int vf_config_dir(char *out, size_t n)
{
    const wchar_t *ov = _wgetenv(L"VF_CONFIG_DIR");
    const wchar_t *app = _wgetenv(L"APPDATA");
    char base[VF_PATH_MAX];
    if (ov && *ov) {
        if (to_u8(ov, out, (int)n)) return -1;
    } else {
        if (!app || to_u8(app, base, VF_PATH_MAX)) return -1;
        if ((size_t)snprintf(out, n, "%s\\mic thingy", base) >= n) return -1;
        /* the app was called VoiceFilter before: carry its settings and presets over once */
        wchar_t wnew[VF_PATH_MAX], wold[VF_PATH_MAX + 16];
        char old[VF_PATH_MAX + 16];
        snprintf(old, sizeof old, "%s\\VoiceFilter", base);
        if (!to_w(out, wnew, VF_PATH_MAX) && !to_w(old, wold, VF_PATH_MAX + 16) &&
            GetFileAttributesW(wnew) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(wold) != INVALID_FILE_ATTRIBUTES)
            MoveFileW(wold, wnew);
    }
    return vf_mkdir_p(out);
}

int vf_list_files(const char *dir, const char *ext, void (*fn)(const char *, void *), void *user)
{
    char pat[VF_PATH_MAX];
    wchar_t wpat[VF_PATH_MAX];
    snprintf(pat, sizeof pat, "%s\\*%s", dir, ext);
    if (to_w(pat, wpat, VF_PATH_MAX)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    const size_t el = strlen(ext);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[512];
        if (to_u8(fd.cFileName, name, sizeof name)) continue;
        size_t l = strlen(name);
        if (l <= el || _stricmp(name + l - el, ext) != 0) continue;
        name[l - el] = '\0';
        fn(name, user);
        count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

int vf_executable_path(char *out, size_t n)
{
    wchar_t w[VF_PATH_MAX];
    DWORD l = GetModuleFileNameW(NULL, w, VF_PATH_MAX);
    if (l == 0 || l >= VF_PATH_MAX) return -1;
    return to_u8(w, out, (int)n);
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

int vf_autostart_get_named(const char *name)
{
    wchar_t wn[256];
    if (to_w(name, wn, 256)) return 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, wn, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

int vf_autostart_set_named(const char *name, int enable)
{
    wchar_t wn[256];
    if (to_w(name, wn, 256)) return -1;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return -1;
    LONG r;
    if (enable) {
        wchar_t exe[VF_PATH_MAX], cmd[VF_PATH_MAX + 64];
        DWORD l = GetModuleFileNameW(NULL, exe, VF_PATH_MAX);
        if (l == 0 || l >= VF_PATH_MAX) { RegCloseKey(k); return -1; }
        wcscpy(cmd, L"\"");
        wcscat(cmd, exe);
        wcscat(cmd, L"\" --autostart --minimized");     /* = VF_AUTOSTART_ARGS */
        r = RegSetValueExW(k, wn, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, wn);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS ? 0 : -1;
}

/* ======================================================================= POSIX */
#else
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

FILE *vf_fopen(const char *path, const char *mode) { return fopen(path, mode); }

int vf_mkdir_p(const char *path)
{
    char tmp[VF_PATH_MAX];
    if (strlen(path) >= sizeof tmp) return -1;
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int vf_remove(const char *path) { return remove(path); }
int vf_replace_file(const char *tmp, const char *dst) { return rename(tmp, dst); }

static int xdg_config_home(char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) return (size_t)snprintf(out, n, "%s", x) < n ? 0 : -1;
    if (!h || !*h) return -1;
    return (size_t)snprintf(out, n, "%s/.config", h) < n ? 0 : -1;
}

int vf_config_dir(char *out, size_t n)
{
    const char *ov = getenv("VF_CONFIG_DIR");
    if (ov && *ov) {
        if ((size_t)snprintf(out, n, "%s", ov) >= n) return -1;
    } else {
        char base[VF_PATH_MAX], old[VF_PATH_MAX + 16];
        if (xdg_config_home(base, sizeof base)) return -1;
        if ((size_t)snprintf(out, n, "%s/mic-thingy", base) >= n) return -1;
        /* the app was called VoiceFilter before: carry its settings and presets over once */
        snprintf(old, sizeof old, "%s/voicefilter", base);
        if (access(out, F_OK) != 0 && access(old, F_OK) == 0) rename(old, out);
    }
    return vf_mkdir_p(out);
}

int vf_list_files(const char *dir, const char *ext, void (*fn)(const char *, void *), void *user)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    const size_t el = strlen(ext);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char name[512];
        snprintf(name, sizeof name, "%s", e->d_name);
        size_t l = strlen(name);
        if (name[0] == '.' || l <= el || strcasecmp(name + l - el, ext) != 0) continue;
        name[l - el] = '\0';
        fn(name, user);
        count++;
    }
    closedir(d);
    return count;
}

int vf_executable_path(char *out, size_t n)
{
    ssize_t l = readlink("/proc/self/exe", out, n - 1);
    if (l <= 0) return -1;
    out[l] = '\0';
    return 0;
}

static int desktop_path(const char *name, char *out, size_t n)
{
    char base[VF_PATH_MAX], file[128];
    if (xdg_config_home(base, sizeof base)) return -1;
    snprintf(file, sizeof file, "%s", name);
    for (char *c = file; *c; c++) if (*c == ' ') *c = '-';   /* "mic thingy" -> mic-thingy.desktop */
    return (size_t)snprintf(out, n, "%s/autostart/%s.desktop", base, file) < n ? 0 : -1;
}

int vf_autostart_get_named(const char *name)
{
    char p[VF_PATH_MAX];
    return desktop_path(name, p, sizeof p) == 0 && access(p, F_OK) == 0;
}

int vf_autostart_set_named(const char *name, int enable)
{
    char p[VF_PATH_MAX];
    if (desktop_path(name, p, sizeof p)) return -1;
    if (!enable) return (remove(p) == 0 || errno == ENOENT) ? 0 : -1;
    char dir[VF_PATH_MAX], exe[VF_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    if (vf_mkdir_p(dir) || vf_executable_path(exe, sizeof exe)) return -1;
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=mic thingy\n"
               "Comment=Real-time microphone noise filter\n"
               "Exec=\"%s\" " VF_AUTOSTART_ARGS "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n", exe);
    return fclose(f) == 0 ? 0 : -1;
}
#endif

int vf_autostart_migrate(void)
{
#ifdef _WIN32
    const char *old = "VoiceFilter";
#else
    const char *old = "voicefilter";
#endif
    if (!vf_autostart_get_named(old)) return 0;
    vf_autostart_set_named(old, 0);
    vf_autostart_set_named(VF_AUTOSTART_NAME, 1);
    return 1;
}

int vf_autostart_get(void) { return vf_autostart_get_named(VF_AUTOSTART_NAME); }
int vf_autostart_set(int enable) { return vf_autostart_set_named(VF_AUTOSTART_NAME, enable); }

/* ---- documents folder, lock state ---- */
int vf_documents_dir(char *out, size_t n)
{
    char base[VF_PATH_MAX] = "";
#ifdef _WIN32
    const char *h = getenv("USERPROFILE");
    if (h && h[0]) snprintf(base, sizeof base, "%s\\Documents\\mic thingy", h);
#else
    const char *h = getenv("HOME");
    if (h && h[0]) snprintf(base, sizeof base, "%s/Documents/mic thingy", h);
#endif
    if (base[0] && vf_mkdir_p(base) == 0) { snprintf(out, n, "%s", base); return 0; }
    return vf_config_dir(out, n);
}

#ifdef _WIN32
int vf_screen_locked(void)
{
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);   /* fails while the lock screen owns input */
    if (!d) return 1;
    CloseDesktop(d);
    return 0;
}
#else
int vf_screen_locked(void) { return 0; }
#endif
