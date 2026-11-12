#include "app/overlay.h"

#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <math.h>
#include "app/icon_art.h"

#define OVL_SIZE 44
#define OVL_MARGIN 14
#define TIMER_ID 1

static atomic_int g_state, g_corner, g_running;
static HANDLE g_thread, g_ready;
static DWORD g_thread_id;
static HWND g_hwnd;
static int g_drawn = -1, g_pos_corner = -1;

static void place(HWND h, int corner)
{
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = (corner & 1) ? wa.right - OVL_SIZE - OVL_MARGIN : wa.left + OVL_MARGIN;
    const int y = (corner & 2) ? wa.bottom - OVL_SIZE - OVL_MARGIN : wa.top + OVL_MARGIN;
    SetWindowPos(h, HWND_TOPMOST, x, y, OVL_SIZE, OVL_SIZE, SWP_NOACTIVATE);
}

/* Renders the indicator into a premultiplied-alpha 32-bit DIB and pushes it to the window. */
static void draw(HWND h, int state)
{
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = OVL_SIZE;
    bi.bmiHeader.biHeight = -OVL_SIZE;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    unsigned char *bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        static unsigned char rgba[OVL_SIZE * OVL_SIZE * 4];
        vf_icon_render(rgba, OVL_SIZE, state != VF_OVL_MUTED);
        const float alpha = state == VF_OVL_IDLE ? 0.45f : 1.0f;
        const float cx = OVL_SIZE / 2.0f - 0.5f, r_out = OVL_SIZE / 2.0f - 1.0f;
        for (int y = 0; y < OVL_SIZE; y++)
            for (int x = 0; x < OVL_SIZE; x++) {
                const unsigned char *s = rgba + 4 * (y * OVL_SIZE + x);
                unsigned char *d = bits + 4 * (y * OVL_SIZE + x);
                float a = (s[3] / 255.0f) * alpha;
                float r = s[0], g = s[1], b = s[2];
                if (state == VF_OVL_SPEAKING) {              /* glowing ring around the icon */
                    const float dist = sqrtf((x - cx) * (x - cx) + (y - cx) * (y - cx));
                    if (dist > r_out - 3.0f && dist < r_out + 0.5f && a < 0.9f) { r = 57; g = 255; b = 20; a = 0.85f; }
                }
                if (state == VF_OVL_MUTED) {                 /* diagonal slash */
                    const float dd = fabsf((float)(x - (OVL_SIZE - 1 - y))) / 1.414f;
                    if (dd < 2.2f && x > 5 && x < OVL_SIZE - 5) { r = 255; g = 214; b = 224; a = 1.0f; }
                }
                d[0] = (unsigned char)(b * a); d[1] = (unsigned char)(g * a); d[2] = (unsigned char)(r * a);
                d[3] = (unsigned char)(a * 255.0f);
            }
        RECT wr;
        GetWindowRect(h, &wr);
        POINT pos = { wr.left, wr.top }, src = { 0, 0 };
        SIZE sz = { OVL_SIZE, OVL_SIZE };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(h, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_TIMER) {
        const int st = atomic_load(&g_state), co = atomic_load(&g_corner);
        if (st == VF_OVL_HIDDEN) { if (g_drawn != 0) { ShowWindow(h, SW_HIDE); g_drawn = 0; } return 0; }
        if (co != g_pos_corner) { place(h, co); g_pos_corner = co; g_drawn = -1; }
        if (st != g_drawn) { draw(h, st); ShowWindow(h, SW_SHOWNOACTIVATE); g_drawn = st; }
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE) { g_pos_corner = -1; return 0; }
    return DefWindowProcW(h, msg, w, l);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    (void)arg;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MicThingyOverlay";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                             wc.lpszClassName, L"mic thingy overlay", WS_POPUP, 0, 0, OVL_SIZE, OVL_SIZE,
                             NULL, NULL, wc.hInstance, NULL);
    if (g_hwnd) SetTimer(g_hwnd, TIMER_ID, 100, NULL);
    SetEvent(g_ready);
    MSG m;
    while (g_hwnd && GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_hwnd) DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

int vf_overlay_start(int corner)
{
    if (atomic_load(&g_running)) { atomic_store(&g_corner, corner & 3); return 0; }
    atomic_store(&g_corner, corner & 3);
    atomic_store(&g_state, VF_OVL_HIDDEN);
    g_drawn = g_pos_corner = -1;
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, thread_main, NULL, 0, &g_thread_id);
    if (!g_thread) return -1;
    WaitForSingleObject(g_ready, 3000);
    CloseHandle(g_ready);
    if (!g_hwnd) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = NULL; return -1; }
    atomic_store(&g_running, 1);
    return 0;
}

void vf_overlay_stop(void)
{
    if (!atomic_load(&g_running)) return;
    atomic_store(&g_running, 0);
    PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_hwnd = NULL;
}

void vf_overlay_set_state(int state) { atomic_store(&g_state, state); }
void vf_overlay_set_corner(int corner) { atomic_store(&g_corner, corner & 3); }
int  vf_overlay_running(void) { return atomic_load(&g_running); }

#else
int  vf_overlay_start(int corner) { (void)corner; return -1; }
void vf_overlay_set_state(int state) { (void)state; }
void vf_overlay_set_corner(int corner) { (void)corner; }
void vf_overlay_stop(void) {}
int  vf_overlay_running(void) { return 0; }
#endif
