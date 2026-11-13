#include "util/console.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>

void vf_console_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
void vf_console_restore(void) {}
int vf_console_getkey(void)
{
    if (!_kbhit()) return -1;
    int c = _getch();
    if (c == 0 || c == 0xE0) { _getch(); return -1; }   /* swallow special keys */
    return c;
}
void vf_sleep_ms(int ms) { Sleep((DWORD)ms); }
double vf_now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

#else
#include <termios.h>
#include <unistd.h>
#include <time.h>

static struct termios g_old;
static int g_raw;

void vf_console_init(void)
{
    if (tcgetattr(STDIN_FILENO, &g_old) == 0) {
        struct termios t = g_old;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        g_raw = 1;
    }
}
void vf_console_restore(void)
{
    if (g_raw) { tcsetattr(STDIN_FILENO, TCSANOW, &g_old); g_raw = 0; }
}
int vf_console_getkey(void)
{
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? (int)c : -1;
}
void vf_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
double vf_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif
