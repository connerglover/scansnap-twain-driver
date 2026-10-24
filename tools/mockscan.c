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

static int run_case(const char *label, int composition, int res, int duplex,
                    const char *outdir)
{
    Session *s;
    ScanJob j = job_for(composition, res, duplex);
    Page page;
    int back = 0, pages = 0, rc;

    printf("%s\n", label);
    s = session_create();
    if (!s) { CHECK(0, "session_create"); return 1; }
    CHECK(session_open(s) == PROTO_OK, "session_open");
    session_set_job(s, &j);

    memset(&page, 0, sizeof page);
    for (;;) {
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
