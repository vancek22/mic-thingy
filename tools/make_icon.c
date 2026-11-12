/* make_icon: writes a multi-size Windows .ico (16..256 px, 32-bit BMP entries)
 * from vf_icon_render(), and optionally a PNG-like preview as raw TGA.
 *   make_icon assets/mic-thingy.ico [preview.tga]
 * Run once after changing the artwork; the .ico is committed and compiled into
 * the executables via assets/mic-thingy.rc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/icon_art.h"

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

    if (argc > 2) {                                                   /* 256 px preview as TGA */
        const int s = 256;
        unsigned char *px = (unsigned char *)malloc((size_t)s * s * 4);
        vf_icon_render(px, s, 1);
        FILE *t = fopen(argv[2], "wb");
        if (t) {
            unsigned char h[18] = { 0, 0, 2 };
            h[12] = s & 255; h[13] = s >> 8; h[14] = s & 255; h[15] = s >> 8; h[16] = 32; h[17] = 0x28;
            fwrite(h, 1, 18, t);
            for (int k = 0; k < s * s; k++) { fputc(px[4 * k + 2], t); fputc(px[4 * k + 1], t); fputc(px[4 * k], t); fputc(px[4 * k + 3], t); }
            fclose(t);
        }
        free(px);
    }
    return 0;
}
