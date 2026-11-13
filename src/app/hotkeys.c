#include "app/hotkeys.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* name -> Windows virtual-key code */
typedef struct { const char *name; int vk; } KeyName;

static const KeyName k_keys[] = {
    { "ctrl", 0x11 }, { "shift", 0x10 }, { "alt", 0x12 }, { "win", 0x5B },
    { "space", 0x20 }, { "enter", 0x0D }, { "esc", 0x1B }, { "tab", 0x09 }, { "backspace", 0x08 },
    { "insert", 0x2D }, { "delete", 0x2E }, { "home", 0x24 }, { "end", 0x23 },
    { "pageup", 0x21 }, { "pagedown", 0x22 }, { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "pause", 0x13 }, { "scrolllock", 0x91 }, { "capslock", 0x14 }, { "numlock", 0x90 },
    { "num0", 0x60 }, { "num1", 0x61 }, { "num2", 0x62 }, { "num3", 0x63 }, { "num4", 0x64 },
    { "num5", 0x65 }, { "num6", 0x66 }, { "num7", 0x67 }, { "num8", 0x68 }, { "num9", 0x69 },
    { "num*", 0x6A }, { "num+", 0x6B }, { "num-", 0x6D }, { "num/", 0x6F },
};

static int lookup(const char *tok)
{
    if (tok[1] == '\0' && isalnum((unsigned char)tok[0])) return toupper((unsigned char)tok[0]);   /* A-Z, 0-9 */
    if ((tok[0] == 'f' || tok[0] == 'F') && tok[1]) {                                              /* F1..F24 */
        char *end;
        const long n = strtol(tok + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 24) return 0x70 + (int)n - 1;
    }
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (!strcmp(tok, k_keys[i].name)) return k_keys[i].vk;
    return 0;
}

int vf_hotkey_parse(const char *text, VfHotkey *hk)
{
    memset(hk, 0, sizeof *hk);
    if (!text) return 0;
    char tok[24];
    int t = 0;
    for (const char *p = text;; p++) {
        if (*p == '+' || *p == '\0' || *p == ' ') {
            if (t > 0) {
                tok[t] = '\0';
                for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
                if (!strcmp(tok, "none")) { if (hk->n) return -1; }
                else {
                    const int vk = lookup(tok);
                    if (!vk || hk->n >= VF_HK_MAX) { memset(hk, 0, sizeof *hk); return -1; }
                    hk->vk[hk->n++] = vk;
                }
                t = 0;
            }
            if (*p == '\0') break;
        } else if (t < (int)sizeof tok - 1) tok[t++] = *p;
        else { memset(hk, 0, sizeof *hk); return -1; }
    }
    return 0;
}

void vf_hotkey_from_text_keys(const int *vks, int n, VfHotkey *hk)
{
    memset(hk, 0, sizeof *hk);
    for (int i = 0; i < n && i < VF_HK_MAX; i++) hk->vk[hk->n++] = vks[i];
}

static const char *name_of(int vk, char *tmp)
{
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++)
        if (k_keys[i].vk == vk) return k_keys[i].name;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { tmp[0] = (char)tolower(vk); tmp[1] = '\0'; return tmp; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(tmp, 8, "f%d", vk - 0x70 + 1); return tmp; }
    snprintf(tmp, 8, "?%d", vk);
    return tmp;
}

void vf_hotkey_format(const VfHotkey *hk, char *buf, size_t n)
{
    if (n == 0) return;
    buf[0] = 0;
    if (hk->n == 0) { snprintf(buf, n, "none"); return; }
    /* modifiers first in a fixed order (ctrl, alt, shift, win), then the other keys */
    static const int order[] = { 0x11, 0x12, 0x10, 0x5B };
    size_t used = 0;
    char tmp[8];
    for (int m = 0; m < 4; m++)
        for (int i = 0; i < hk->n; i++)
            if (hk->vk[i] == order[m] && used < n)
                used += (size_t)snprintf(buf + used, n - used, "%s%s", used ? "+" : "", name_of(hk->vk[i], tmp));
    for (int i = 0; i < hk->n; i++) {
        const int vk = hk->vk[i];
        if ((vk == 0x11 || vk == 0x12 || vk == 0x10 || vk == 0x5B) || used >= n) continue;
        used += (size_t)snprintf(buf + used, n - used, "%s%s", used ? "+" : "", name_of(vk, tmp));
    }
}

#ifdef _WIN32
static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
#else
static int key_down(int vk) { (void)vk; return 0; }
#endif

/* ---- grabbing: keep the combination away from the focused application ---- */
#ifdef _WIN32
#define GRAB_MAX 16
#define WM_GRAB_SET (WM_APP + 1)

typedef struct { int n; UINT mod[GRAB_MAX], key[GRAB_MAX]; } GrabSet;

static HANDLE g_grab_ready;
static DWORD  g_grab_tid;

static DWORD WINAPI grab_thread(LPVOID arg)
{
    (void)arg;
    MSG m;
    PeekMessage(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);     /* creates the thread's message queue */
    g_grab_tid = GetCurrentThreadId();
    SetEvent(g_grab_ready);
    int registered = 0;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (m.message != WM_GRAB_SET) continue;
        GrabSet *g = (GrabSet *)m.lParam;
        for (int i = 0; i < registered; i++) UnregisterHotKey(NULL, i + 1);
        registered = 0;
        for (int i = 0; i < g->n; i++)      /* a combination another program owns simply stays unswallowed */
            if (RegisterHotKey(NULL, registered + 1, g->mod[i] | MOD_NOREPEAT, g->key[i])) registered++;
        free(g);
    }
    return 0;
}

void vf_hotkeys_grab(const VfHotkey *hks, int n)
{
    static int started;
    if (!started) {
        g_grab_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, grab_thread, NULL, 0, NULL);
        if (!g_grab_ready || !t) return;
        CloseHandle(t);
        WaitForSingleObject(g_grab_ready, 2000);
        started = 1;
    }
    GrabSet *g = (GrabSet *)calloc(1, sizeof *g);
    if (!g) return;
    for (int i = 0; i < n && g->n < GRAB_MAX; i++) {
        UINT mod = 0, key = 0;
        int keys = 0;
        for (int k = 0; k < hks[i].n; k++) {
            const int vk = hks[i].vk[k];
            if (vk == 0x11) mod |= MOD_CONTROL;
            else if (vk == 0x12) mod |= MOD_ALT;
            else if (vk == 0x10) mod |= MOD_SHIFT;
            else if (vk == 0x5B) mod |= MOD_WIN;
            else { key = (UINT)vk; keys++; }
        }
        if (keys == 1 && mod) { g->mod[g->n] = mod; g->key[g->n] = key; g->n++; }   /* bare keys stay typeable */
    }
    if (!g_grab_tid || !PostThreadMessage(g_grab_tid, WM_GRAB_SET, 0, (LPARAM)g)) free(g);
}
#else
void vf_hotkeys_grab(const VfHotkey *hks, int n) { (void)hks; (void)n; }
#endif

int vf_hotkey_down(const VfHotkey *hk)
{
    if (hk->n == 0) return 0;
    for (int i = 0; i < hk->n; i++) if (!key_down(hk->vk[i])) return 0;
    return 1;
}

int vf_hotkey_pressed(const VfHotkey *hk, int *was)
{
    const int d = vf_hotkey_down(hk);
    const int edge = d && !*was;
    *was = d;
    return edge;
}

int vf_hotkey_scan(void)
{
    /* modifiers alone do not count; keys 8..254, skipping mouse buttons (1..6) */
    for (int vk = 8; vk < 255; vk++) {
        if (vk == 0x10 || vk == 0x11 || vk == 0x12 || vk == 0xA0 || vk == 0xA1 || vk == 0xA2 || vk == 0xA3 || vk == 0xA4 || vk == 0xA5) continue;
        if (key_down(vk)) return vk;
    }
    return 0;
}
