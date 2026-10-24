/*
 * Sample conversion: scanner-native bytes -> TWAIN rows.
 *
 * The iX1300 (like the rest of this generation) sends colour and grayscale
 * samples inverted, and colour in one of three interleavings. Line art arrives
 * one bit per pixel, MSB first, with the ink bit set.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void page_free(Page *p)
{
    free(p->rows);
    memset(p, 0, sizeof *p);
}

static BYTE *alloc_rows(LONG w, LONG h, int bpp)
{
    size_t rowBytes = ((size_t)w * bpp + 7) / 8;
    return (BYTE *)calloc(rowBytes * (size_t)h + 64, 1);
}

static void invert_row(BYTE *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = (BYTE)~p[i];
}

BOOL page_from_raw(Page *p, const BYTE *raw, size_t rawLen, LONG w, LONG h,
                   int composition, int interlace, double xres, double yres, int side)
{
    size_t rowBytes;
    LONG y;

    memset(p, 0, sizeof *p);
    if (w <= 0 || h <= 0) return FALSE;

    p->width = w;
    p->height = h;
    p->xres = xres;
    p->yres = yres;
    p->side = side;
    p->bpp = composition == COMP_COLOR ? 24 : (composition == COMP_LINEART ? 1 : 8);
    p->pixelType = composition == COMP_COLOR ? TWPT_RGB
                 : composition == COMP_LINEART ? TWPT_BW : TWPT_GRAY;
    rowBytes = ((size_t)w * p->bpp + 7) / 8;
    p->rowBytes = rowBytes;
    p->rows = alloc_rows(w, h, p->bpp);
    if (!p->rows) return FALSE;

    for (y = 0; y < h; y++) {
        BYTE *dst = p->rows + rowBytes * (size_t)y;
        size_t avail;

        if (composition == COMP_COLOR) {
            size_t srcRow = (size_t)w * 3;
            const BYTE *s = raw + srcRow * (size_t)y;
            if (srcRow * (size_t)(y + 1) > rawLen) break;
            if (interlace == 2) {
                /* line interleaved rrr..ggg..bbb.. */
                LONG x;
                for (x = 0; x < w; x++) {
                    dst[x * 3 + 0] = (BYTE)~s[x];
                    dst[x * 3 + 1] = (BYTE)~s[w + x];
                    dst[x * 3 + 2] = (BYTE)~s[2 * (size_t)w + x];
                }
            } else if (interlace == 1) {
                LONG x;
                for (x = 0; x < w; x++) {
                    dst[x * 3 + 0] = (BYTE)~s[x * 3 + 2];
                    dst[x * 3 + 1] = (BYTE)~s[x * 3 + 1];
                    dst[x * 3 + 2] = (BYTE)~s[x * 3 + 0];
                }
            } else {
                memcpy(dst, s, (size_t)w * 3);
                invert_row(dst, (size_t)w * 3);
            }
        } else if (composition == COMP_LINEART) {
            size_t srcRow = ((size_t)w + 7) / 8;
            const BYTE *s = raw + srcRow * (size_t)y;
            if (srcRow * (size_t)(y + 1) > rawLen) break;
            /* Wire: 1 = ink. TWAIN chocolate: 0 = black, so invert. */
            memcpy(dst, s, srcRow);
            if (!g_cfg.lineartInvert) invert_row(dst, srcRow);
            avail = srcRow;
            (void)avail;
        } else {
            const BYTE *s = raw + (size_t)w * (size_t)y;
            if ((size_t)w * (size_t)(y + 1) > rawLen) break;
            memcpy(dst, s, (size_t)w);
            invert_row(dst, (size_t)w);
        }
    }
    return TRUE;
}

HGLOBAL page_to_hdib(const Page *p)
{
    size_t stride = ((size_t)p->width * p->bpp + 31) / 32 * 4;
    size_t palette = p->bpp <= 8 ? (size_t)256 * sizeof(RGBQUAD) : 0;
    size_t size = sizeof(BITMAPINFOHEADER) + palette + stride * (size_t)p->height;
    HGLOBAL h = GlobalAlloc(GHND, size);
    BITMAPINFOHEADER *bi;
    BYTE *bits;
    LONG y;

    if (!h) return NULL;
    bi = (BITMAPINFOHEADER *)GlobalLock(h);
    if (!bi) { GlobalFree(h); return NULL; }
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = p->width;
    bi->biHeight = p->height;             /* bottom-up */
    bi->biPlanes = 1;
    bi->biBitCount = (WORD)p->bpp;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(stride * (size_t)p->height);
    bi->biXPelsPerMeter = (LONG)(p->xres * 100.0 / 2.54 + 0.5);
    bi->biYPelsPerMeter = (LONG)(p->yres * 100.0 / 2.54 + 0.5);
    bi->biClrUsed = p->bpp <= 8 ? 256 : 0;
    bi->biClrImportant = 0;
    if (p->bpp <= 8) {
        RGBQUAD *pal = (RGBQUAD *)(bi + 1);
        int i;
        for (i = 0; i < 256; i++) {
            pal[i].rgbRed = pal[i].rgbGreen = pal[i].rgbBlue = (BYTE)i;
            pal[i].rgbReserved = 0;
        }
    }
    bits = (BYTE *)bi + sizeof(BITMAPINFOHEADER) + palette;
    for (y = 0; y < p->height; y++) {
        const BYTE *src = p->rows + p->rowBytes * (size_t)(p->height - 1 - y);
        memcpy(bits + stride * (size_t)y, src, p->rowBytes);
    }
    GlobalUnlock(h);
    return h;
}

static BOOL write_bmp(const Page *p, const char *path)
{
    HGLOBAL h = page_to_hdib(p);
    BITMAPINFOHEADER *bi;
    BITMAPFILEHEADER bf;
    FILE *f;
    size_t size;

    if (!h) return FALSE;
    bi = (BITMAPINFOHEADER *)GlobalLock(h);
    size = sizeof(BITMAPINFOHEADER) + (p->bpp <= 8 ? 256 * sizeof(RGBQUAD) : 0) + bi->biSizeImage;
    memset(&bf, 0, sizeof bf);
    bf.bfType = 0x4D42;
    bf.bfOffBits = (DWORD)(sizeof bf + sizeof(BITMAPINFOHEADER) + (p->bpp <= 8 ? 256 * sizeof(RGBQUAD) : 0));
    bf.bfSize = (DWORD)(bf.bfOffBits + bi->biSizeImage);
    if (fopen_s(&f, path, "wb") || !f) { GlobalUnlock(h); GlobalFree(h); return FALSE; }
    fwrite(&bf, sizeof bf, 1, f);
    fwrite(bi, 1, size, f);
    fclose(f);
    GlobalUnlock(h);
    GlobalFree(h);
    return TRUE;
}

BOOL page_save(const Page *p, const char *path, TW_UINT16 format)
{
    if (format != TWFF_BMP) return FALSE;
    return write_bmp(p, path);
}
