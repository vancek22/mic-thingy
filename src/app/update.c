#include "app/update.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/version.h"

#define API_HOST "api.github.com"
#define API_PATH "/repos/vancek22/mic-thingy/releases/latest"
#define MAX_BODY 65536

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

/* ---------------------------------------------------------------- transport */

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>

/* returns the HTTP status (200, 404, ...) with the body in buf, or -1 on a network failure */
static int http_get(char *buf, size_t cap)
{
    int status = -1;
    size_t used = 0;
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

static void open_url(const char *url)
{
    wchar_t w[300];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 300) > 0) ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}
#else
#include <sys/wait.h>
#include <unistd.h>

static int http_get(char *buf, size_t cap)
{
    /* fixed command line, no user data in it; the status code is appended after the body */
    FILE *p = popen("curl -sS --max-time 10 -H 'Accept: application/vnd.github+json' -A mic-thingy "
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

void vf_update_open_page(const VfUpdate *u)
{
    char url[200];
    if (vf_update_page_url(u->tag, url, sizeof url) != 0) snprintf(url, sizeof url, "%s", VF_RELEASES_URL);
    open_url(url);
}

void vf_update_free(VfUpdate *u)
{
    if (vf_update_state(u) != VF_UPD_CHECKING) vf_thread_join(&u->th);   /* a running check ends with the process */
}
