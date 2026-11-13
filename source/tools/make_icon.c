/* make_icon: writes a multi-size Windows .ico (16..256 px, 32-bit BMP entries)
 * from vf_icon_render(), plus any number of PNG renders named <name>-<size>.png.
 *   make_icon assets/mic-thingy.ico assets/logo-1024.png assets/logo-256.png
 * Run once after changing the artwork; the .ico is committed and compiled into
 * the executables via assets/mic-thingy.rc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/icon_art.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"     /* from raylib (third_party/raylib/src) */

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: make_icon out.ico [preview.tga]\n"); return 2; }
    static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
    const int n = (int)(sizeof sizes / sizeof sizes[0]);
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }

    unsigned bytes[16], offset = 6 + 16 * (unsigned)n;
    for (int i = 0; i < n; i++) {
        const unsigned s = (unsigned)sizes[i], mask_row = ((s + 31) / 32) * 4;
        bytes[i] = 40 + s * s * 4 + mask_row * s;
    }
    w16(f, 0); w16(f, 1); w16(f, (unsigned)n);                  /* ICONDIR */
    for (int i = 0; i < n; i++) {                               /* ICONDIRENTRY */
        const int s = sizes[i];
        fputc(s >= 256 ? 0 : s, f); fputc(s >= 256 ? 0 : s, f);
        fputc(0, f); fputc(0, f);
        w16(f, 1); w16(f, 32);
        w32(f, bytes[i]); w32(f, offset);
        offset += bytes[i];
    }
    for (int i = 0; i < n; i++) {
        const int s = sizes[i];
        unsigned char *px = (unsigned char *)malloc((size_t)s * s * 4);
        vf_icon_render(px, s, 1);
        w32(f, 40); w32(f, (unsigned)s); w32(f, (unsigned)s * 2);   /* BITMAPINFOHEADER (height x2) */
        w16(f, 1); w16(f, 32); w32(f, 0); w32(f, (unsigned)s * s * 4);
        w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
        for (int y = s - 1; y >= 0; y--)                             /* bottom-up BGRA */
            for (int x = 0; x < s; x++) {
                const unsigned char *p = px + 4 * (y * s + x);
                fputc(p[2], f); fputc(p[1], f); fputc(p[0], f); fputc(p[3], f);
            }
        const unsigned mask_row = ((unsigned)(s + 31) / 32) * 4;      /* AND mask: all zero (alpha used) */
        for (unsigned k = 0; k < mask_row * (unsigned)s; k++) fputc(0, f);
        free(px);
    }
    fclose(f);
    printf("wrote %s (%d sizes)\n", argv[1], n);

    for (int a = 2; a < argc; a++) {                                  /* extra PNGs: name-<size>.png */
        const char *name = argv[a];
        const char *dash = strrchr(name, '-');
        const int s = dash ? atoi(dash + 1) : 0;
        if (s < 16 || s > 4096) { fprintf(stderr, "skip %s (want name-<size>.png)\n", name); continue; }
        unsigned char *px = (unsigned char *)malloc((size_t)s * s * 4);
        vf_icon_render(px, s, 1);
        if (stbi_write_png(name, s, s, 4, px, s * 4)) printf("wrote %s\n", name);
        free(px);
    }
    return 0;
}
