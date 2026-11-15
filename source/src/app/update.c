#include "app/update.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/version.h"
#include "util/platform.h"
#include "util/sha256.h"

#define API_HOST "api.github.com"
#define API_PATH "/repos/vancek22/mic-thingy/releases/latest"
#define DL_HOST "github.com"
#define DL_PREFIX "/vancek22/mic-thingy/releases/download/"
#define MAX_BODY 65536
#define MIN_EXE (256L * 1024)                /* a program smaller / bigger than this is not ours */
#define MAX_EXE (64L * 1024 * 1024)
#ifdef _WIN32
#define ASSET_NAME "mic-thingy.exe"
#else
#define ASSET_NAME "mic-thingy-linux"
#endif

/* ---------------------------------------------------------------- pure helpers */

int vf_version_cmp(const char *a, const char *b)
{
    long pa[4] = { 0 }, pb[4] = { 0 };
    int ok_a = 0, ok_b = 0;
    for (int s = 0; s < 2; s++) {
        const char *p = s ? b : a;
        long *out = s ? pb : pa;
        while (*p && !isdigit((unsigned char)*p)) p++;          /* skips "v" */
        for (int i = 0; i < 4 && isdigit((unsigned char)*p); i++) {
            out[i] = strtol(p, (char **)&p, 10);
            if (s) ok_b = 1; else ok_a = 1;
            if (*p == '.') p++; else break;
        }
    }
    if (!ok_a || !ok_b) return -2;
    for (int i = 0; i < 4; i++) if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

static int tag_is_safe(const char *t)
{
    const size_t l = strlen(t);
    if (l == 0 || l >= 40) return 0;
    for (size_t i = 0; i < l; i++)
        if (!(isalnum((unsigned char)t[i]) || t[i] == '.' || t[i] == '-' || t[i] == '_')) return 0;
    return 1;
}

int vf_update_parse(const char *json, char *tag, size_t n)
{
    const char *k = strstr(json, "\"tag_name\"");
    if (!k) return -1;
    k += 10;
    while (*k == ' ' || *k == ':' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    if (*k != '"') return -1;
    k++;
    const char *e = strchr(k, '"');
    if (!e || (size_t)(e - k) >= n || (size_t)(e - k) >= 40) return -1;
    memcpy(tag, k, (size_t)(e - k));
    tag[e - k] = '\0';
    return tag_is_safe(tag) ? 0 : -1;
}

int vf_update_page_url(const char *tag, char *out, size_t n)
{
    if (!tag || !tag_is_safe(tag)) return -1;
    return (size_t)snprintf(out, n, VF_RELEASES_URL "/tag/%s", tag) < n ? 0 : -1;
}

const char *vf_update_asset_name(void) { return ASSET_NAME; }

int vf_update_asset_path(const char *tag, const char *asset, char *out, size_t n)
{
    if (!tag || !tag_is_safe(tag) || !asset || !*asset) return -1;
    for (const char *a = asset; *a; a++)
        if (!(isalnum((unsigned char)*a) || *a == '.' || *a == '-' || *a == '_')) return -1;
    return (size_t)snprintf(out, n, DL_PREFIX "%s/%s", tag, asset) < n ? 0 : -1;
}

int vf_update_parse_sha(const char *text, char hex[65])
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    for (int i = 0; i < 64; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (!isxdigit(c)) return -1;
        hex[i] = (char)tolower(c);
    }
    hex[64] = '\0';
    const unsigned char e = (unsigned char)text[64];
    return (e == '\0' || isspace(e) || e == '*') ? 0 : -1;       /* a longer hex run is not a SHA-256 */
}

int vf_update_verify(const char *path, const char *want_hex)
{
    FILE *f = vf_fopen(path, "rb");
    if (!f) return -1;
    unsigned char m[4] = { 0 };
    const size_t got = fread(m, 1, 4, f);
    const int seek = fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fclose(f);
    if (got != 4 || seek != 0 || size < MIN_EXE || size > MAX_EXE) return -1;
#ifdef _WIN32
    if (m[0] != 'M' || m[1] != 'Z') return -1;
#else
    if (m[0] != 0x7f || m[1] != 'E' || m[2] != 'L' || m[3] != 'F') return -1;
#endif
    char have[65];
    if (strlen(want_hex) != 64 || vf_sha256_file(path, have) != 0) return -1;
    for (int i = 0; i < 64; i++)
        if (have[i] != (char)tolower((unsigned char)want_hex[i])) return -1;
    return 0;
}

/* ---------------------------------------------------------------- transport */

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

/* winhttp.dll is not a Windows "known DLL": linked normally it would be looked up in the program's own folder first
 * (before main() runs), so anything called winhttp.dll next to a downloaded mic-thingy.exe would be loaded. It is
 * loaded from System32 on the first update check instead. */
static struct {
    int tried;
    HINTERNET (WINAPI *Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    BOOL (WINAPI *SetTimeouts)(HINTERNET, int, int, int, int);
    HINTERNET (WINAPI *Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
    HINTERNET (WINAPI *OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
    BOOL (WINAPI *SendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    BOOL (WINAPI *ReceiveResponse)(HINTERNET, LPVOID);
    BOOL (WINAPI *QueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
    BOOL (WINAPI *ReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
    BOOL (WINAPI *CloseHandle)(HINTERNET);
} g_wh;

typedef void (*AnyFn)(void);

/* 0 = every function is there. Called from the single check / install worker thread (never two at once). */
static int winhttp_load(void)
{
    if (!g_wh.tried) {
        g_wh.tried = 1;
        HMODULE m = (HMODULE)vf_load_system_library("winhttp.dll");
        if (m) {
            g_wh.Open = (HINTERNET (WINAPI *)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD))(AnyFn)GetProcAddress(m, "WinHttpOpen");
            g_wh.SetTimeouts = (BOOL (WINAPI *)(HINTERNET, int, int, int, int))(AnyFn)GetProcAddress(m, "WinHttpSetTimeouts");
            g_wh.Connect = (HINTERNET (WINAPI *)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD))(AnyFn)GetProcAddress(m, "WinHttpConnect");
            g_wh.OpenRequest = (HINTERNET (WINAPI *)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD))(AnyFn)GetProcAddress(m, "WinHttpOpenRequest");
            g_wh.SendRequest = (BOOL (WINAPI *)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR))(AnyFn)GetProcAddress(m, "WinHttpSendRequest");
            g_wh.ReceiveResponse = (BOOL (WINAPI *)(HINTERNET, LPVOID))(AnyFn)GetProcAddress(m, "WinHttpReceiveResponse");
            g_wh.QueryHeaders = (BOOL (WINAPI *)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD))(AnyFn)GetProcAddress(m, "WinHttpQueryHeaders");
            g_wh.ReadData = (BOOL (WINAPI *)(HINTERNET, LPVOID, DWORD, LPDWORD))(AnyFn)GetProcAddress(m, "WinHttpReadData");
            g_wh.CloseHandle = (BOOL (WINAPI *)(HINTERNET))(AnyFn)GetProcAddress(m, "WinHttpCloseHandle");
        }
    }
    return g_wh.Open && g_wh.SetTimeouts && g_wh.Connect && g_wh.OpenRequest && g_wh.SendRequest &&
           g_wh.ReceiveResponse && g_wh.QueryHeaders && g_wh.ReadData && g_wh.CloseHandle ? 0 : -1;
}
#define WinHttpOpen            g_wh.Open
#define WinHttpSetTimeouts     g_wh.SetTimeouts
#define WinHttpConnect         g_wh.Connect
#define WinHttpOpenRequest     g_wh.OpenRequest
#define WinHttpSendRequest     g_wh.SendRequest
#define WinHttpReceiveResponse g_wh.ReceiveResponse
#define WinHttpQueryHeaders    g_wh.QueryHeaders
#define WinHttpReadData        g_wh.ReadData
#define WinHttpCloseHandle     g_wh.CloseHandle

/* returns the HTTP status (200, 404, ...) with the body in buf, or -1 on a network failure */
static int http_get(char *buf, size_t cap)
{
    int status = -1;
    size_t used = 0;
    buf[0] = '\0';
    if (winhttp_load() != 0) return -1;
    HINTERNET ses = WinHttpOpen(L"mic-thingy", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return -1;
    WinHttpSetTimeouts(ses, 6000, 6000, 8000, 8000);
    HINTERNET con = WinHttpConnect(ses, L"" API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"" API_PATH, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, sz = sizeof code;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, WINHTTP_NO_HEADER_INDEX))
            status = (int)code;
        DWORD got;
        while (used + 1 < cap && WinHttpReadData(req, buf + used, (DWORD)(cap - 1 - used), &got) && got > 0) used += got;
    }
    buf[used] = '\0';
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;
}

/* GET https://github.com<path> into the file `outfile` (redirects to the release CDN are followed by WinHTTP, never
 * from https to http). Returns the HTTP status (the file holds the body only for 200) or -1 on a network failure,
 * a body over max_bytes, or a body shorter than the announced length. */
static int http_download(const char *path, const char *outfile, long max_bytes, VfUpdate *u)
{
    wchar_t wpath[512];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512) <= 0) return -1;
    int status = -1;
    atomic_store(&u->got, 0);
    atomic_store(&u->total, 0);
    if (winhttp_load() != 0) return -1;
    HINTERNET ses = WinHttpOpen(L"mic-thingy", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return -1;
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 30000);
    HINTERNET con = WinHttpConnect(ses, L"" DL_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD code = 0, sz = sizeof code;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, WINHTTP_NO_HEADER_INDEX))
            status = (int)code;
        if (status == 200) {
            DWORD len = 0;
            sz = sizeof len;
            const int known = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, NULL, &len, &sz,
                                                  WINHTTP_NO_HEADER_INDEX) != 0;
            if (known) atomic_store(&u->total, (long)len);
            FILE *f = vf_fopen(outfile, "wb");
            unsigned char *buf = (unsigned char *)malloc(65536);
            long used = 0;
            int ok = f && buf && !(known && (long)len > max_bytes);
            while (ok) {
                DWORD got = 0;
                if (!WinHttpReadData(req, buf, 65536, &got)) { ok = 0; break; }
                if (got == 0) break;
                used += (long)got;
                if (used > max_bytes || fwrite(buf, 1, got, f) != got) { ok = 0; break; }
                atomic_store(&u->got, used);
            }
            if (ok && known && used != (long)len) ok = 0;
            if (f && fclose(f) != 0) ok = 0;
            free(buf);
            if (!ok) status = -1;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return status;
}

static void open_url(const char *url)
{
    wchar_t w[300];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 300) > 0) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

static int http_get(char *buf, size_t cap)
{
    /* fixed command line, no user data in it; the status code is appended after the body */
    FILE *p = popen("curl -sS --proto =https --max-time 10 -H 'Accept: application/vnd.github+json' -A mic-thingy "
                    "-w '\\n%{http_code}' https://" API_HOST API_PATH " 2>/dev/null", "r");
    if (!p) return -1;
    size_t used = 0, r;
    while (used + 1 < cap && (r = fread(buf + used, 1, cap - 1 - used, p)) > 0) used += r;
    buf[used] = '\0';
    const int rc = pclose(p);
    if (rc != 0 || used == 0) return -1;
    char *nl = strrchr(buf, '\n');
    if (!nl) return -1;
    const int code = atoi(nl + 1);
    *nl = '\0';
    return code > 0 ? code : -1;
}

/* curl with an argument vector (no shell, so the output path needs no quoting); https only, also after redirects.
 * curl --fail exits with 22 on an HTTP error: reported as 404 (= "that file is not part of the release"). */
static int http_download(const char *path, const char *outfile, long max_bytes, VfUpdate *u)
{
    char url[512], maxb[24];
    atomic_store(&u->got, 0);
    atomic_store(&u->total, 0);
    if ((size_t)snprintf(url, sizeof url, "https://" DL_HOST "%s", path) >= sizeof url) return -1;
    snprintf(maxb, sizeof maxb, "%ld", max_bytes);
    const pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        const int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        execlp("curl", "curl", "-sSL", "--fail", "--proto", "=https", "--proto-redir", "=https", "--max-time", "300",
               "--max-filesize", maxb, "-A", "mic-thingy", "-o", outfile, url, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(c, &st, 0) < 0 || !WIFEXITED(st)) return -1;
    const int code = WEXITSTATUS(st);
    if (code == 0) {
        FILE *f = vf_fopen(outfile, "rb");
        long sz = -1;
        if (f) { if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f); fclose(f); }
        atomic_store(&u->got, sz > 0 ? sz : 0);
        return 200;
    }
    return code == 22 ? 404 : -1;
}

static void open_url(const char *url)
{
    const pid_t c = fork();
    if (c == 0) {
        if (fork() == 0) { execlp("xdg-open", "xdg-open", url, (char *)NULL); _exit(127); }
        _exit(0);
    }
    if (c > 0) waitpid(c, NULL, 0);
}
#endif

/* ---------------------------------------------------------------- state machine */

static void worker(void *arg)
{
    VfUpdate *u = (VfUpdate *)arg;
    char *body = (char *)malloc(MAX_BODY);
    int st = VF_UPD_ERROR;
    if (body) {
        const int code = http_get(body, MAX_BODY);
        if (code == 404) st = VF_UPD_NO_RELEASE;
        else if (code == 200 && vf_update_parse(body, u->tag, sizeof u->tag) == 0) {
            const int c = vf_version_cmp(u->tag, VF_VERSION);
            st = c == 1 ? VF_UPD_NEWER : (c == 0 || c == -1) ? VF_UPD_UP_TO_DATE : VF_UPD_ERROR;
        }
        free(body);
    }
    atomic_store(&u->state, st);
}

void vf_update_init(VfUpdate *u) { memset(u, 0, sizeof *u); atomic_init(&u->state, VF_UPD_IDLE); }
int  vf_update_state(VfUpdate *u) { return atomic_load(&u->state); }

void vf_update_check(VfUpdate *u)
{
    if (vf_update_state(u) == VF_UPD_CHECKING) return;
    vf_thread_join(&u->th);                                      /* previous run, already finished */
    u->th.started = 0;
    u->tag[0] = '\0';
    atomic_store(&u->state, VF_UPD_CHECKING);
    if (vf_thread_start(&u->th, worker, u) != 0) atomic_store(&u->state, VF_UPD_ERROR);
}

/* Everything of "Update now" except the thread. Returns the state to end in. */
static int do_install(VfUpdate *u)
{
    char exe[VF_PATH_MAX], tmp[VF_PATH_MAX + 16], shaf[VF_PATH_MAX + 32], path[300], want[65];
    if (vf_executable_path(exe, sizeof exe) != 0) return VF_UPD_FAIL;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.new", exe) >= sizeof tmp) return VF_UPD_FAIL;
    if ((size_t)snprintf(shaf, sizeof shaf, "%s.sha256", tmp) >= sizeof shaf) return VF_UPD_FAIL;

    int result = VF_UPD_FAIL;
    char text[4097];
    size_t n = 0;
    if (vf_update_asset_path(u->tag, ASSET_NAME ".sha256", path, sizeof path) != 0) goto done;
    int code = http_download(path, shaf, 4096, u);
    if (code == 404) { result = VF_UPD_NO_PACKAGE; goto done; }
    if (code != 200) goto done;
    FILE *f = vf_fopen(shaf, "rb");
    if (f) { n = fread(text, 1, 4096, f); fclose(f); }
    text[n] = '\0';
    if (vf_update_parse_sha(text, want) != 0) goto done;

    if (vf_update_asset_path(u->tag, ASSET_NAME, path, sizeof path) != 0) goto done;
    code = http_download(path, tmp, MAX_EXE, u);
    if (code == 404) { result = VF_UPD_NO_PACKAGE; goto done; }
    if (code != 200) goto done;
    if (vf_update_verify(tmp, want) != 0) goto done;
    if (vf_swap_file(tmp, exe) != 0) goto done;
    result = VF_UPD_READY;
done:
    vf_remove(shaf);
    if (result != VF_UPD_READY) vf_remove(tmp);
    return result;
}

static void install_worker(void *arg)
{
    VfUpdate *u = (VfUpdate *)arg;
    atomic_store(&u->state, do_install(u));
}

void vf_update_install(VfUpdate *u)
{
    int want = VF_UPD_NEWER;
    if (!atomic_compare_exchange_strong(&u->state, &want, VF_UPD_DOWNLOADING)) return;   /* only after a check found a newer tag */
    vf_thread_join(&u->th);
    u->th.started = 0;
    atomic_store(&u->got, 0);
    atomic_store(&u->total, 0);
    if (vf_thread_start(&u->th, install_worker, u) != 0) atomic_store(&u->state, VF_UPD_FAIL);
}

int vf_update_restart(void)
{
    char exe[VF_PATH_MAX];
    if (vf_executable_path(exe, sizeof exe) != 0) return -1;
    return vf_spawn_after_update(exe, vf_current_pid());
}

void vf_update_cleanup(void)
{
    char exe[VF_PATH_MAX], p[VF_PATH_MAX + 32];
    if (vf_executable_path(exe, sizeof exe) != 0) return;
    static const char *const suffix[] = { ".old", ".new", ".new.sha256" };
    for (int i = 0; i < 3; i++)
        if ((size_t)snprintf(p, sizeof p, "%s%s", exe, suffix[i]) < sizeof p) vf_remove(p);   /* the .old of a program that still runs fails: harmless */
}

void vf_update_open_page(const VfUpdate *u)
{
    char url[200];
    if (vf_update_page_url(u->tag, url, sizeof url) != 0) snprintf(url, sizeof url, "%s", VF_RELEASES_URL);
    open_url(url);
}

void vf_update_free(VfUpdate *u)
{
    const int s = vf_update_state(u);
    if (s != VF_UPD_CHECKING && s != VF_UPD_DOWNLOADING) vf_thread_join(&u->th);   /* a running check / download ends with the process */
}
