#include "app/tray.h"

#include <stdatomic.h>

static atomic_int g_events[8];       /* tiny event queue: counters per event type */
static atomic_int g_running, g_bypass;

static void post(int ev) { atomic_fetch_add(&g_events[ev], 1); }

int vf_tray_poll(void)
{
    for (int ev = VF_TRAY_SHOW; ev <= VF_TRAY_TOGGLE_RUN; ev++) {
        int n = atomic_load(&g_events[ev]);
        while (n > 0) {
            if (atomic_compare_exchange_weak(&g_events[ev], &n, n - 1)) return ev;
        }
    }
    return VF_TRAY_NONE;
}

void vf_tray_set_state(int running, int bypass)
{
    atomic_store(&g_running, running);
    atomic_store(&g_bypass, bypass);
}

/* ======================================================================= Windows */
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>
#include "app/i18n.h"
#include "app/icon_art.h"

#define WM_TRAY      (WM_APP + 1)
#define WM_SHOWMAIN  (WM_APP + 2)        /* sent by a second instance */
#define WM_TRAYQUIT  (WM_APP + 3)
#define ID_SHOW      1
#define ID_RUN       2
#define ID_BYPASS    3
#define ID_QUIT      4
static const wchar_t *k_class = L"MicThingyTrayWindow";

static HANDLE g_thread, g_ready;
static DWORD g_thread_id;
static HWND g_hwnd;
static NOTIFYICONDATAW g_nid;
static HICON g_icon_on, g_icon_off;
static int g_last_running = -1, g_last_bypass = -1, g_last_lang = -1;
static UINT g_taskbar_created;

/* The app icon (icon_art.c) at tray size: green waves = filtering, grey = stopped. */
static HICON make_icon(int active)
{
    const int S = GetSystemMetrics(SM_CXSMICON) > 16 ? 32 : 16;
    BITMAPV5HEADER bh;
    ZeroMemory(&bh, sizeof bh);
    bh.bV5Size = sizeof bh;
    bh.bV5Width = S;
    bh.bV5Height = -S;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color || !bits) return LoadIcon(NULL, IDI_APPLICATION);
    DWORD *px = (DWORD *)bits;
    unsigned char rgba[32 * 32 * 4];
    vf_icon_render(rgba, S, active);
    for (int i = 0; i < S * S; i++) {                /* premultiplied BGRA for the DIB */
        const unsigned a = rgba[4 * i + 3];
        px[i] = a << 24 | (rgba[4 * i] * a / 255) << 16 | (rgba[4 * i + 1] * a / 255) << 8 | (rgba[4 * i + 2] * a / 255);
    }
    static const unsigned char zero_mask[32 * 32 / 8] = { 0 };  /* alpha channel does the masking (16 or 32 px) */
    HBITMAP mask = CreateBitmap(S, S, 1, 1, zero_mask);
    ICONINFO ii;
    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon ? icon : LoadIcon(NULL, IDI_APPLICATION);
}

static void update_icon(void)
{
    int run = atomic_load(&g_running), byp = atomic_load(&g_bypass), lang = (int)vf_i18n_get();
    if (run == g_last_running && byp == g_last_bypass && lang == g_last_lang) return;
    g_last_running = run;
    g_last_bypass = byp;
    g_last_lang = lang;
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = run && !byp ? g_icon_on : g_icon_off;
    char tip[128];
    snprintf(tip, sizeof tip, "mic thingy - %s", tr(!run ? S_TRAY_TIP_STOPPED : byp ? S_TRAY_TIP_BYPASS : S_TRAY_TIP_ON));
    MultiByteToWideChar(CP_UTF8, 0, tip, -1, g_nid.szTip, (int)(sizeof g_nid.szTip / sizeof g_nid.szTip[0]));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void add_icon(void)
{
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_last_running = g_last_bypass = g_last_lang = -1;
    update_icon();
}

static void show_menu(HWND h)
{
    HMENU m = CreatePopupMenu();
    const int run = atomic_load(&g_running), byp = atomic_load(&g_bypass);
    wchar_t w[4][96];
    const VfStr ids[4] = { S_TRAY_SHOW, S_TRAY_PROCESSING, S_TRAY_FILTER, S_TRAY_QUIT };
    for (int i = 0; i < 4; i++) MultiByteToWideChar(CP_UTF8, 0, tr(ids[i]), -1, w[i], 96);
    AppendMenuW(m, MF_STRING, ID_SHOW, w[0]);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (run ? MF_CHECKED : 0), ID_RUN, w[1]);
    AppendMenuW(m, MF_STRING | (run && !byp ? MF_CHECKED : 0) | (run ? 0 : MF_GRAYED), ID_BYPASS, w[2]);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_QUIT, w[3]);
    SetMenuDefaultItem(m, ID_SHOW, FALSE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(h);                     /* so the menu closes when clicking elsewhere */
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, h, NULL);
    DestroyMenu(m);
    if (cmd == ID_SHOW) post(VF_TRAY_SHOW);
    else if (cmd == ID_RUN) post(VF_TRAY_TOGGLE_RUN);
    else if (cmd == ID_BYPASS) post(VF_TRAY_TOGGLE_BYPASS);
    else if (cmd == ID_QUIT) post(VF_TRAY_QUIT);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TRAY) {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) post(VF_TRAY_SHOW);
        else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) show_menu(h);
        return 0;
    }
    if (msg == WM_SHOWMAIN) { post(VF_TRAY_SHOW); return 0; }
    if (msg == WM_TIMER) { update_icon(); return 0; }
    if (msg == WM_TRAYQUIT) { DestroyWindow(h); return 0; }
    if (msg == WM_DESTROY) { Shell_NotifyIconW(NIM_DELETE, &g_nid); PostQuitMessage(0); return 0; }
    if (g_taskbar_created && msg == g_taskbar_created) { add_icon(); return 0; }   /* Explorer restarted */
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI tray_thread(LPVOID arg)
{
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = k_class;
    RegisterClassW(&wc);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    /* a normal top-level (but never shown) window: message-only windows don't get TaskbarCreated */
    g_hwnd = CreateWindowExW(0, k_class, L"mic thingy", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd) { SetEvent(g_ready); return 1; }
    g_icon_on = make_icon(1);
    g_icon_off = make_icon(0);
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon_off;
    add_icon();
    SetTimer(g_hwnd, 1, 500, NULL);
    SetEvent(g_ready);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyIcon(g_icon_on);
    DestroyIcon(g_icon_off);
    UnregisterClassW(k_class, inst);
    return 0;
}

int vf_tray_start(void)
{
    if (g_thread) return 0;
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, tray_thread, NULL, 0, &g_thread_id);
    if (!g_thread) return -1;
    WaitForSingleObject(g_ready, 5000);
    CloseHandle(g_ready);
    return g_hwnd ? 0 : -1;
}

void vf_tray_stop(void)
{
    if (!g_thread) return;
    if (g_hwnd) PostMessageW(g_hwnd, WM_TRAYQUIT, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_hwnd = NULL;
}

int vf_single_instance(void)
{
    static HANDLE mutex;
    mutex = CreateMutexW(NULL, TRUE, L"Local\\MicThingySingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(k_class, NULL);
        if (other) PostMessageW(other, WM_SHOWMAIN, 0, 0);
        return 0;
    }
    return 1;                                   /* mutex stays held until the process exits */
}

/* ======================================================================= others */
#else
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>
#include "util/platform.h"

int  vf_tray_start(void) { return -1; }
void vf_tray_stop(void) {}

int vf_single_instance(void)
{
    char dir[VF_PATH_MAX], path[VF_PATH_MAX + 32];
    if (vf_config_dir(dir, sizeof dir)) return 1;
    snprintf(path, sizeof path, "%s/mic-thingy.lock", dir);
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return 1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return 0; }
    return 1;                                   /* fd intentionally kept open: lock lives with the process */
}
#endif
