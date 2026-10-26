/*
 * twaintest - drive ScanSnap-iX1300.ds exactly the way NAPS2 does.
 *
 *   twaintest [path\to\ds] [outdir]
 *
 * Mini-application: states 3..4, the capability writes NAPS2 performs in
 * TwainScanRunner.ConfigureSource, MSG_ENABLEDS, the MSG_XFERREADY loop,
 * DAT_SETUPMEMXFER/IMAGEINFO/IMAGEMEMXFER, ENDXFER, DISABLEDS, CLOSEDS.
 * Uses the synthetic scanner when Mock=1 in ScanSnapTWAIN.ini next to the DS.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "twain.h"

typedef TW_UINT16 (FAR PASCAL *DSENTRYPROC)(pTW_IDENTITY, TW_UINT32, TW_UINT16, TW_UINT16, TW_MEMREF);

static DSENTRYPROC g_ds;
static TW_IDENTITY g_app, g_src;
static int failures;
static char g_outDir[MAX_PATH];

#define CHECK(cond, ...) do { \
    if (cond) printf("  ok   "); else { printf("  FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

static TW_UINT16 call(TW_UINT32 dg, TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF p)
{
    return g_ds(&g_app, dg, dat, msg, p);
}

static TW_UINT16 set_one(TW_UINT16 capId, TW_UINT16 type, TW_UINT32 value)
{
    TW_CAPABILITY cap;
    TW_ONEVALUE ov;
    TW_UINT16 rc;
    memset(&cap, 0, sizeof cap);
    cap.Cap = capId;
    cap.ConType = TWON_ONEVALUE;
    cap.hContainer = GlobalAlloc(GHND, sizeof ov);
    ov.ItemType = type;
    ov.Item = value;
    memcpy(GlobalLock(cap.hContainer), &ov, sizeof ov);
    GlobalUnlock(cap.hContainer);
    rc = call(DG_CONTROL, DAT_CAPABILITY, MSG_SET, &cap);
    GlobalFree(cap.hContainer);
    return rc;
}

static TW_UINT16 get_one(TW_UINT16 capId, TW_UINT32 *value, TW_UINT16 *type)
{
    TW_CAPABILITY cap;
    TW_ONEVALUE *ov;
    TW_UINT16 rc;
    memset(&cap, 0, sizeof cap);
    cap.Cap = capId;
    cap.ConType = TWON_DONTCARE16;
    rc = call(DG_CONTROL, DAT_CAPABILITY, MSG_GETCURRENT, &cap);
    if (rc != TWRC_SUCCESS || !cap.hContainer) return rc;
    ov = (TW_ONEVALUE *)GlobalLock(cap.hContainer);
    *value = ov->Item;
    *type = ov->ItemType;
    GlobalUnlock(cap.hContainer);
    GlobalFree(cap.hContainer);
    return TWRC_SUCCESS;
}

static TW_UINT16 set_fix32(TW_UINT16 capId, double v)
{
    TW_CAPABILITY cap;
    TW_ONEVALUE ov;
    TW_FIX32 f;
    TW_UINT16 rc;
    memset(&cap, 0, sizeof cap);
    cap.Cap = capId;
    cap.ConType = TWON_ONEVALUE;
    cap.hContainer = GlobalAlloc(GHND, sizeof ov);
    f.Whole = (TW_INT16)v;
    f.Frac = (TW_UINT16)((v - (int)v) * 65536.0);
    ov.ItemType = TWTY_FIX32;
    memcpy(&ov.Item, &f, sizeof f);
    memcpy(GlobalLock(cap.hContainer), &ov, sizeof ov);
    GlobalUnlock(cap.hContainer);
    rc = call(DG_CONTROL, DAT_CAPABILITY, MSG_SET, &cap);
    GlobalFree(cap.hContainer);
    return rc;
}

static int run_scan(const char *outdir, int pixelType, int duplex, const char *tag)
{
    TW_USERINTERFACE ui;
    TW_PENDINGXFERS pend;
    TW_SETUPMEMXFER mem;
    int pages = 0;
    TW_UINT16 rc;

    printf("%s\n", tag);

    /* ConfigureSource, in NAPS2's order. */
    rc = set_one(ICAP_XFERMECH, TWTY_UINT16, TWSX_MEMORY);
    CHECK(rc == TWRC_SUCCESS, "ICAP_XFERMECH = TWSX_MEMORY");
    rc = set_one(CAP_INDICATORS, TWTY_BOOL, 0);
    CHECK(rc == TWRC_SUCCESS, "CAP_INDICATORS = FALSE");
    rc = set_one(CAP_FEEDERENABLED, TWTY_BOOL, 1);
    CHECK(rc == TWRC_SUCCESS, "CAP_FEEDERENABLED = TRUE");
    rc = set_one(CAP_DUPLEXENABLED, TWTY_BOOL, duplex ? 1 : 0);
    CHECK(rc == TWRC_SUCCESS, "CAP_DUPLEXENABLED = %d", duplex);
    rc = set_one(CAP_AUTOMATICSENSEMEDIUM, TWTY_BOOL, 1);
    CHECK(rc == TWRC_SUCCESS, "CAP_AUTOMATICSENSEMEDIUM = TRUE");
    rc = set_one(ICAP_PIXELTYPE, TWTY_UINT16, pixelType);
    CHECK(rc == TWRC_SUCCESS, "ICAP_PIXELTYPE = %d", pixelType);
    rc = set_one(ICAP_BITDEPTH, TWTY_UINT16,
                 pixelType == TWPT_RGB ? 24 : (pixelType == TWPT_BW ? 1 : 8));
    CHECK(rc == TWRC_SUCCESS, "ICAP_BITDEPTH");
    rc = set_one(ICAP_UNITS, TWTY_UINT16, TWUN_INCHES);
    CHECK(rc == TWRC_SUCCESS, "ICAP_UNITS = INCHES");
    rc = set_fix32(ICAP_XRESOLUTION, 300);
    CHECK(rc == TWRC_SUCCESS, "ICAP_XRESOLUTION = 300");
    rc = set_fix32(ICAP_YRESOLUTION, 300);
    CHECK(rc == TWRC_SUCCESS, "ICAP_YRESOLUTION = 300");
    rc = set_fix32(ICAP_BRIGHTNESS, 0);
    CHECK(rc == TWRC_SUCCESS, "ICAP_BRIGHTNESS = 0");
    rc = set_fix32(ICAP_CONTRAST, 0);
    CHECK(rc == TWRC_SUCCESS, "ICAP_CONTRAST = 0");

    {
        TW_UINT32 v = 0;
        TW_UINT16 t = 0;
        TW_IMAGELAYOUT layout;
        memset(&layout, 0, sizeof layout);
        rc = call(DG_IMAGE, DAT_IMAGELAYOUT, MSG_GET, &layout);
        CHECK(rc == TWRC_SUCCESS, "IMAGELAYOUT GET");
        memset(&layout.Frame, 0, sizeof layout.Frame);
        layout.Frame.Right.Whole = 8;          /* 8.5 in as FIX32 */
        layout.Frame.Right.Frac = 0x8000;
        layout.Frame.Bottom.Whole = 11;
        rc = call(DG_IMAGE, DAT_IMAGELAYOUT, MSG_SET, &layout);
        CHECK(rc == TWRC_SUCCESS, "IMAGELAYOUT SET");
        memset(&layout, 0, sizeof layout);
        rc = call(DG_IMAGE, DAT_IMAGELAYOUT, MSG_GET, &layout);
        CHECK(layout.Frame.Right.Whole == 8 && layout.Frame.Right.Frac == 0x8000 &&
              layout.Frame.Bottom.Whole == 11,
              "layout round-trip %d.%04x x %d", layout.Frame.Right.Whole,
              layout.Frame.Right.Frac, layout.Frame.Bottom.Whole);
        rc = get_one(CAP_DUPLEX, &v, &t);
        CHECK(rc == TWRC_SUCCESS, "CAP_DUPLEX readable (0x%x)", v);
        rc = get_one(ICAP_PHYSICALWIDTH, &v, &t);
        CHECK(rc == TWRC_SUCCESS, "ICAP_PHYSICALWIDTH readable");
    }

    memset(&ui, 0, sizeof ui);
    ui.ShowUI = 0;
    ui.ModalUI = 0;
    ui.hParent = NULL;
    rc = call(DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &ui);
    CHECK(rc == TWRC_SUCCESS, "MSG_ENABLEDS");

    /* NAPS2's loop: XFERREADY -> mem xfer -> ENDXFER until Count == 0. */
    for (;;) {
        TW_EVENT ev;
        MSG msg;
        TW_IMAGEINFO info;
        DWORD total = 0;

        memset(&msg, 0, sizeof msg);
        msg.message = MSG_XFERREADY;
        ev.pEvent = &msg;
        ev.TWMessage = MSG_NULL;
        rc = call(DG_CONTROL, DAT_EVENT, MSG_PROCESSEVENT, &ev);
        CHECK(rc == TWRC_DSEVENT && ev.TWMessage == MSG_XFERREADY,
              "PROCESSEVENT -> XFERREADY (rc=%u msg=0x%x)", rc, ev.TWMessage);

        memset(&mem, 0, sizeof mem);
        rc = call(DG_CONTROL, DAT_SETUPMEMXFER, MSG_GET, &mem);
        CHECK(rc == TWRC_SUCCESS && mem.Preferred > 0,
              "SETUPMEMXFER preferred %lu", mem.Preferred);

        memset(&info, 0, sizeof info);
        rc = call(DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &info);
        CHECK(rc == TWRC_SUCCESS, "IMAGEINFO %ldx%ld px=%d bpp=%d comp=%u",
              info.ImageWidth, info.ImageLength, info.PixelType,
              info.BitsPerPixel, info.Compression);
        if (pages == 0) {
            int bpp = pixelType == TWPT_RGB ? 24 : (pixelType == TWPT_BW ? 1 : 8);
            int spp = pixelType == TWPT_RGB ? 3 : 1;
            int perSample = pixelType == TWPT_RGB ? 8 : bpp;
            /*
             * NAPS2 matches on (PixelType, BitsPerPixel, SamplesPerPixel,
             * BitsPerSample[0]) - (RGB,24,3,8), (GRAY,8,1,8), (BW,1,1,1) -
             * and rejects anything else with "Unsupported pixel type".
             * BitsPerSample is per sample, not per pixel, so 24-bit colour
             * must report 8. These are the assertions that catch it.
             */
            CHECK(info.PixelType == pixelType, "pixel type matches request");
            CHECK(info.Compression == TWCP_NONE, "uncompressed");
            CHECK(info.BitsPerPixel == bpp, "BitsPerPixel %d", info.BitsPerPixel);
            CHECK(info.SamplesPerPixel == spp, "SamplesPerPixel %d", info.SamplesPerPixel);
            CHECK(info.BitsPerSample[0] == perSample,
                  "BitsPerSample[0] %d (per sample)", info.BitsPerSample[0]);
            CHECK(info.Planar == FALSE, "Planar FALSE");
        }

        /*
         * NAPS2 places each strip by YOffset and copies Rows * BytesPerRow
         * bytes, so a strip must hold whole rows and continue exactly where
         * the previous one ended. Byte totals alone miss a strip that splits
         * a row: the total is right and every band after it is sheared.
         */
        {
            TW_UINT32 rowsSeen = 0, badStrips = 0;
            for (;;) {
                TW_IMAGEMEMXFER x;
                HGLOBAL h = GlobalAlloc(GHND, mem.Preferred);
                memset(&x, 0, sizeof x);
                x.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
                x.Memory.Length = mem.Preferred;
                x.Memory.TheMem = h;
                rc = call(DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, &x);
                GlobalFree(h);
                if (rc != TWRC_SUCCESS && rc != TWRC_XFERDONE) {
                    CHECK(0, "IMAGEMEMXFER rc=%u", rc);
                    break;
                }
                total += x.BytesWritten;
                if (x.BytesWritten != x.Rows * x.BytesPerRow || x.YOffset != rowsSeen)
                    badStrips++;
                rowsSeen += x.Rows;
                if (rc == TWRC_XFERDONE) break;
            }
            CHECK(total > 0, "page %d transferred %lu bytes", pages, total);
            CHECK(badStrips == 0, "page %d strips hold whole rows at the running YOffset (%lu bad)",
                  pages, badStrips);
            CHECK(rowsSeen == (TW_UINT32)info.ImageLength, "page %d strips cover %lu of %ld rows",
                  pages, rowsSeen, info.ImageLength);
        }
        pages++;

        memset(&pend, 0, sizeof pend);
        rc = call(DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pend);
        CHECK(rc == TWRC_SUCCESS, "ENDXFER count=%u", pend.Count);
        if (pend.Count == 0) break;
    }

    CHECK(pages == (duplex ? 4 : 2), "pages %d", pages);

    rc = call(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, NULL);
    CHECK(rc == TWRC_SUCCESS, "MSG_DISABLEDS");
    (void)outdir;
    return pages;
}

/* Native DIB: one DAT_IMAGENATIVEXFER per page, DIB owned by the app. */
static void run_native(const char *tag)
{
    TW_USERINTERFACE ui;
    TW_PENDINGXFERS pend;
    TW_UINT16 rc;
    int pages = 0;

    printf("%s\n", tag);
    rc = set_one(CAP_DUPLEXENABLED, TWTY_BOOL, 0);
    CHECK(rc == TWRC_SUCCESS, "CAP_DUPLEXENABLED = FALSE");
    rc = set_one(ICAP_XFERMECH, TWTY_UINT16, TWSX_NATIVE);
    CHECK(rc == TWRC_SUCCESS, "ICAP_XFERMECH = TWSX_NATIVE");
    rc = set_one(ICAP_PIXELTYPE, TWTY_UINT16, TWPT_RGB);
    CHECK(rc == TWRC_SUCCESS, "ICAP_PIXELTYPE = RGB");

    memset(&ui, 0, sizeof ui);
    rc = call(DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &ui);
    CHECK(rc == TWRC_SUCCESS, "MSG_ENABLEDS");

    for (;;) {
        TW_MEMREF mem = NULL;
        BITMAPINFOHEADER *bi;
        rc = call(DG_IMAGE, DAT_IMAGENATIVEXFER, MSG_GET, &mem);
        CHECK(rc == TWRC_XFERDONE && mem, "NATIVEXFER rc=%u handle %p", rc, (void *)mem);
        if (rc == TWRC_XFERDONE && mem) {
            bi = (BITMAPINFOHEADER *)GlobalLock((HGLOBAL)mem);
            CHECK(bi && bi->biSize == sizeof(BITMAPINFOHEADER),
                  "DIB header size %lu", bi ? bi->biSize : 0);
            if (bi) {
                CHECK(bi->biBitCount == 24, "DIB bpp %u", bi->biBitCount);
                CHECK(bi->biWidth == 2550, "DIB width %ld", bi->biWidth);
            }
            GlobalUnlock((HGLOBAL)mem);
            GlobalFree((HGLOBAL)mem);      /* the app owns it */
        }
        pages++;
        memset(&pend, 0, sizeof pend);
        rc = call(DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pend);
        if (pend.Count == 0) break;
    }
    CHECK(pages == 2, "pages %d", pages);
    call(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, NULL);
}

/* File transfer: BMP to the path the app sets. */
static void run_file(const char *tag)
{
    TW_USERINTERFACE ui;
    TW_PENDINGXFERS pend;
    TW_SETUPFILEXFER f;
    TW_UINT16 rc;
    int pages = 0;

    printf("%s\n", tag);
    rc = set_one(CAP_DUPLEXENABLED, TWTY_BOOL, 0);
    CHECK(rc == TWRC_SUCCESS, "CAP_DUPLEXENABLED = FALSE");
    rc = set_one(ICAP_XFERMECH, TWTY_UINT16, TWSX_FILE);
    if (rc != TWRC_SUCCESS) {
        /* file transfer is optional in this DS; skip cleanly */
        printf("  skip file transfer not supported (rc=%u)\n", rc);
        set_one(ICAP_XFERMECH, TWTY_UINT16, TWSX_MEMORY);
        return;
    }
    CHECK(rc == TWRC_SUCCESS, "ICAP_XFERMECH = TWSX_FILE");
    memset(&ui, 0, sizeof ui);
    rc = call(DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &ui);
    CHECK(rc == TWRC_SUCCESS, "MSG_ENABLEDS");

    for (;;) {
        char path[MAX_PATH];
        sprintf_s(path, sizeof path, "%s-file-%d.bmp", g_outDir[0] ? g_outDir : ".", pages);
        memset(&f, 0, sizeof f);
        strcpy_s(f.FileName, sizeof f.FileName, path);
        f.Format = TWFF_BMP;
        rc = call(DG_CONTROL, DAT_SETUPFILEXFER, MSG_SET, &f);
        CHECK(rc == TWRC_SUCCESS, "SETUPFILEXFER");
        rc = call(DG_IMAGE, DAT_IMAGEFILEXFER, MSG_GET, NULL);
        CHECK(rc == TWRC_XFERDONE, "IMAGEFILEXFER rc=%u", rc);
        pages++;
        memset(&pend, 0, sizeof pend);
        rc = call(DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pend);
        if (pend.Count == 0) break;
    }
    CHECK(pages == 2, "pages %d", pages);
    call(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, NULL);
    set_one(ICAP_XFERMECH, TWTY_UINT16, TWSX_MEMORY);
}

int main(int argc, char **argv)
{
    const char *dsPath = argc > 1 ? argv[1] : "build\\ScanSnap-iX1300.ds";
    HMODULE mod;
    TW_IDENTITY id;
    TW_UINT16 rc;

    memset(&g_app, 0, sizeof g_app);
    g_app.Id = 0x1234;
    g_app.Version.MajorNum = 1;
    g_app.Version.MinorNum = 0;
    g_app.Version.Language = TWLG_ENGLISH_USA;
    g_app.Version.Country = TWCY_USA;
    strcpy_s(g_app.Version.Info, sizeof g_app.Version.Info, "twaintest");
    g_app.ProtocolMajor = 2;
    g_app.ProtocolMinor = 5;
    g_app.SupportedGroups = DG_CONTROL | DG_IMAGE;
    strcpy_s(g_app.Manufacturer, sizeof g_app.Manufacturer, "Conner");
    strcpy_s(g_app.ProductFamily, sizeof g_app.ProductFamily, "twaintest");
    strcpy_s(g_app.ProductName, sizeof g_app.ProductName, "twaintest");

    mod = LoadLibraryA(dsPath);
    if (!mod) { printf("LoadLibrary(%s) failed %lu\n", dsPath, GetLastError()); return 2; }
    g_ds = (DSENTRYPROC)GetProcAddress(mod, "DS_Entry");
    if (!g_ds) { printf("DS_Entry missing\n"); return 2; }
    printf("twaintest: %s\n", dsPath);

    /* DAT_IDENTITY/MSG_GET with a NULL origin, as the DSM does at load time. */
    memset(&id, 0, sizeof id);
    rc = g_ds(NULL, DG_CONTROL, DAT_IDENTITY, MSG_GET, &id);
    CHECK(rc == TWRC_SUCCESS, "DAT_IDENTITY/MSG_GET (NULL origin)");
    CHECK(id.ProtocolMajor >= 2, "protocol %u.%u", id.ProtocolMajor, id.ProtocolMinor);
    CHECK((id.SupportedGroups & DG_IMAGE) != 0, "advertises DG_IMAGE");
    printf("       product '%s' / '%s' / '%s'\n", id.Manufacturer,
           id.ProductFamily, id.ProductName);

    rc = call(DG_CONTROL, DAT_PARENT, MSG_OPENDSM, NULL);
    CHECK(rc == TWRC_SUCCESS, "OPENDSM");
    rc = call(DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, &g_src);
    CHECK(rc == TWRC_SUCCESS, "OPENDS");

    {
        TW_CAPABILITY cap;
        TW_UINT16 *ids;
        TW_UINT32 i, n;
        memset(&cap, 0, sizeof cap);
        cap.Cap = CAP_SUPPORTEDCAPS;
        cap.ConType = TWON_DONTCARE16;
        rc = call(DG_CONTROL, DAT_CAPABILITY, MSG_GET, &cap);
        CHECK(rc == TWRC_SUCCESS && cap.hContainer, "CAP_SUPPORTEDCAPS");
        if (cap.hContainer) {
            TW_ARRAY *arr = (TW_ARRAY *)GlobalLock(cap.hContainer);
            ids = (TW_UINT16 *)((BYTE *)arr + sizeof(TW_ARRAY) - sizeof(TW_UINT16));
            n = arr->NumItems;
            printf("       %lu caps:", n);
            for (i = 0; i < n && i < 40; i++) printf(" 0x%04x", ids[i]);
            printf("\n");
            GlobalUnlock(cap.hContainer);
            GlobalFree(cap.hContainer);
        }
    }

    if (argc > 2) strcpy_s(g_outDir, sizeof g_outDir, argv[2]);
    run_scan(g_outDir, TWPT_RGB, 0, "colour 300 dpi, memory xfer");
    run_scan(NULL, TWPT_GRAY, 0, "grayscale 300 dpi, memory xfer");
    run_scan(NULL, TWPT_BW, 0, "black/white 300 dpi, memory xfer");
    run_scan(NULL, TWPT_RGB, 1, "colour 300 dpi duplex, memory xfer");
    run_native("colour 300 dpi, native DIB xfer");
    run_file("colour 300 dpi, BMP file xfer");

    rc = call(DG_CONTROL, DAT_IDENTITY, MSG_CLOSEDS, NULL);
    CHECK(rc == TWRC_SUCCESS, "CLOSEDS");
    rc = call(DG_CONTROL, DAT_PARENT, MSG_CLOSEDSM, NULL);
    CHECK(rc == TWRC_SUCCESS, "CLOSEDSM");

    FreeLibrary(mod);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
