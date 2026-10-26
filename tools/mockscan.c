/*
 * mockscan - full session-layer scan against the synthetic device.
 *
 *   mockscan [outdir]
 *
 * Verifies that begin/feed/scan/pixel-size/read/end works as a batch, that
 * pixel polarity and row order land in the BMP the way TWAIN expects, and
 * that the batch ends cleanly on the second (empty-hopper) feed.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HINSTANCE g_hinst;
Config    g_cfg;

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) printf("  ok   "); else { printf("  FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

static ScanJob job_for(int composition, int res, int duplex)
{
    ScanJob j;
    memset(&j, 0, sizeof j);
    j.resX = j.resY = res;
    j.composition = composition;
    j.duplex = duplex;
    j.widthUnits = 10200;
    j.lengthUnits = 16800;
    j.paperWidthUnits = 10201;
    j.paperHeightUnits = 16802;
    j.pixelsWide = (LONG)(8.5 * res);
    if (composition == COMP_LINEART) j.pixelsWide -= j.pixelsWide % 8;
    j.autoLength = 1;
    j.threshold = 128;
    return j;
}

/*
 * Mismatching bytes between a delivered page and the synthetic device's wire
 * pattern for that side, after inversion and the 180-degree turn. Catches
 * filler counted as image (the duplex gray bands), sides swapped or mixed,
 * rows out of order, and a missing or doubled rotation.
 */
static size_t page_mismatches(const Page *pg, int side, int composition)
{
    DWORD bpl = (DWORD)pg->rowBytes;
    LONG W = pg->width, H = pg->height, y, x;
    size_t bad = 0;

    for (y = 0; y < H; y++) {
        const BYTE *row = pg->rows + pg->rowBytes * (size_t)y;
        DWORD line = (DWORD)(H - 1 - y);
        if (composition == COMP_LINEART) {
            for (x = 0; x < W; x++) {
                LONG from = W - 1 - x;
                BYTE wire = mock_pattern(side, line, (DWORD)from >> 3, bpl);
                int ink = (wire >> (7 - (from & 7))) & 1;
                int got = (row[x >> 3] >> (7 - (x & 7))) & 1;
                if (got != !ink) bad++;              /* TWAIN: 0 = black */
            }
        } else {
            DWORD px = composition == COMP_COLOR ? 3 : 1, off;
            for (off = 0; off < bpl; off++) {
                DWORD from = ((DWORD)W - 1 - off / px) * px + off % px;
                if (row[off] != (BYTE)~mock_pattern(side, line, from, bpl)) bad++;
            }
        }
    }
    return bad;
}

static int run_case(const char *label, int composition, int res, int duplex,
                    const char *outdir)
{
    Session *s;
    ScanJob j = job_for(composition, res, duplex);
    Page page;
    int back = 0, pages = 0, rc;
    LONG lines = (LONG)((long long)j.lengthUnits * res / 1200);

    printf("%s\n", label);
    s = session_create();
    if (!s) { CHECK(0, "session_create"); return 1; }
    CHECK(session_open(s) == PROTO_OK, "session_open");
    session_set_job(s, &j);

    memset(&page, 0, sizeof page);
    for (;;) {
        size_t bad;
        rc = session_next_page(s, &page, &back);
        if (rc != PROTO_OK) break;
        if (pages == 0) {
            CHECK(page.width > 0 && page.height > 0, "page %dx%d", page.width, page.height);
            CHECK(page.rowBytes == (size_t)((page.width * page.bpp + 7) / 8),
                  "row bytes %zu for %d bpp", page.rowBytes, page.bpp);
            CHECK(page.pixelType == (composition == COMP_COLOR ? TWPT_RGB
                                    : composition == COMP_LINEART ? TWPT_BW : TWPT_GRAY),
                  "pixel type %d", page.pixelType);
        }
        /* the unit reports a longer window and ends the sheet with EOM/ILI */
        CHECK(page.height == lines, "page %d height %ld (sheet %ld lines)", pages, page.height, lines);
        CHECK(back == (duplex ? pages % 2 : 0), "page %d side %d", pages, back);
        bad = page_mismatches(&page, back, composition);
        CHECK(bad == 0, "page %d pixels match the %s side (%zu bad)", pages, back ? "back" : "front", bad);
        if (outdir) {
            char name[MAX_PATH];
            sprintf_s(name, sizeof name, "%s\\%s-%d.bmp", outdir, label, pages);
            page_save(&page, name, TWFF_BMP);
        }
        page_free(&page);
        pages++;
    }
    CHECK(rc == PROTO_NODOCS, "batch ends with PROTO_NODOCS (rc=%d)", rc);
    CHECK(pages == (duplex ? 4 : 2), "pages %d", pages);
    session_destroy(s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : NULL;

    g_hinst = GetModuleHandleA(NULL);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.mock = 1;
    g_cfg.interlace = 0;
    g_cfg.logEnable = 0;

    printf("session self-test against the synthetic iX1300\n");
    run_case("color300", COMP_COLOR, 300, 0, outdir);
    run_case("gray200", COMP_GRAY, 200, 0, outdir);
    run_case("lineart300", COMP_LINEART, 300, 0, outdir);
    run_case("duplex150", COMP_COLOR, 150, 1, outdir);
    run_case("color600", COMP_COLOR, 600, 0, outdir);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
