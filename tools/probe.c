/*
 * probe - hardware probe for the standalone driver.
 *
 *   probe.exe info [log]      device path, INQUIRY, VPD, hardware status
 *   probe.exe status [log]    watch GET HW STATUS; prints raw bytes and the
 *                             decoded bits each time anything changes
 *   probe.exe scan [prefix] [log] [duplex] [back]
 *                             one batch through the session layer, colour
 *                             300 dpi; `duplex` reads both sides, `back`
 *                             sets feedFromBack (window 0x80 only); the paper
 *                             path (U-turn or front slot) follows the sensors
 *
 * Run with the ScanSnap's WinUSB binding installed (install/iX1300-TWAIN.inf),
 * or with ScanSnap Home's usbscan binding replaced (see install/README.md).
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HINSTANCE g_hinst;
Config    g_cfg;

#define STATUS_LEN 48   /* what ScanSnap Home's monitor polls */

/*
 * Byte 3/4 names from sane-backends fujitsu-scsi.h (get_GHS_*) and ScanSnap
 * Home (re-notes): byte3 bit7 U-turn hopper empty, bit0 front Return Scan slot
 * loaded; byte4 bit0 Scan pressed, bit5 long press; byte14 bit6 scan end,
 * bit3 scan cancel.
 */
static void print_status(const BYTE *s, DWORD got)
{
    DWORD i;
    printf("hw:");
    for (i = 0; i < got && i < 16; i++) printf(" %02x", s[i]);
    if (got > 16) printf(" ..");
    printf(" | hopper_empty %d front_slot %d adf_open %d paper_end %d"
           " scan_sw %d scan_sw_long %d send_sw %d sleep %d function %d double_feed %d err 0x%02x",
           (s[3] & 0x80) != 0, (s[3] & 0x01) != 0, (s[3] & 0x20) != 0, (s[3] & 0x04) != 0,
           (s[4] & 0x01) != 0, (s[4] & 0x20) != 0, (s[4] & 0x04) != 0,
           (s[4] & 0x80) != 0, s[5] & 0x0F, (s[6] & 0x01) != 0, s[7]);
    if (got > 14) printf(" scan_end %d scan_cancel %d", (s[14] & 0x40) != 0, (s[14] & 0x08) != 0);
    printf(" (%lu bytes)\n", got);
}

static void log_status(const BYTE *s, DWORD got)
{
    char line[3 * STATUS_LEN + 1];
    DWORD i;
    line[0] = 0;
    for (i = 0; i < got && i < STATUS_LEN; i++) sprintf_s(line + 3 * i, sizeof line - 3 * i, "%02x ", s[i]);
    LOGF("probe: status %s\n", line);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "info";
    char path[MAX_PATH];
    Transport *t;
    Proto *p;
    int rc;

    g_hinst = GetModuleHandleA(NULL);
    config_load();
    /*
     * build\ScanSnapTWAIN.ini defaults to Mock=1 so the test tools need no
     * hardware. probe is the hardware tool: always talk to the scanner.
     */
    g_cfg.mock = 0;
    g_cfg.logEnable = 1;          /* probe is the diagnostic: always log */
    {
        /* scan takes the log as its third argument, info/status as their first */
        int logArg = !strcmp(cmd, "scan") ? 3 : 2;
        if (argc > logArg) strcpy_s(g_cfg.logPath, sizeof g_cfg.logPath, argv[logArg]);
    }
    LOGF("probe: start\n");

    transport_request_device();
    t = transport_open(path, sizeof path);
    if (!t) {
        printf("no device: is %04X:%04X bound to WinUSB? see install/README.md\n",
               SSDRV_USB_VID, SSDRV_USB_PID);
        return 2;
    }
    printf("device: %s (%s)\n", path, transport_kind(t));

    p = proto_create(t);
    rc = proto_open(p);
    if (rc != PROTO_OK) {
        printf("proto_open failed: %d\n", rc);
        transport_close(t);
        return 1;
    }
    printf("inquiry: %s / %s / %s\n", p->info.vendor, p->info.product, p->info.version);
    printf("vpd: basic %dx%d max %dx%d window %ldx%ld\n",
           p->info.basicXRes, p->info.basicYRes, p->info.maxXRes, p->info.maxYRes,
           p->info.maxWidthUnits, p->info.maxLengthUnits);
    printf("vpd: colour %d gray %d lineart %d duplex %d returnpath %d\n",
           p->info.hasColor, p->info.hasGray, p->info.hasLineart, p->info.hasDuplex,
           p->info.hasReturnPath);

    if (!strcmp(cmd, "status")) {
        BYTE last[STATUS_LEN];
        DWORD lastGot = 0;
        int haveLast = 0, failed = 0;
        DWORD t0 = GetTickCount();
        printf("watching hardware status; Ctrl+C to stop\n");
        for (;;) {
            BYTE st[STATUS_LEN];
            DWORD got = 0;
            if (proto_hw_status(p, st, sizeof st, &got) == PROTO_OK) {
                failed = 0;
                if (!haveLast || got != lastGot || memcmp(st, last, got)) {
                    printf("%7.1fs ", (GetTickCount() - t0) / 1000.0);
                    print_status(st, got);
                    log_status(st, got);
                    memcpy(last, st, sizeof st);
                    lastGot = got;
                    haveLast = 1;
                }
            } else if (!failed) {
                printf("%7.1fs status read failed\n", (GetTickCount() - t0) / 1000.0);
                failed = 1;
            }
            fflush(stdout);
            Sleep(100);
        }
    }

    {
        BYTE st[STATUS_LEN];
        DWORD got = 0;
        if (proto_hw_status(p, st, sizeof st, &got) == PROTO_OK) {
            print_status(st, got);
            log_status(st, got);
        }
    }

    if (!strcmp(cmd, "scan")) {
        Session *s;
        ScanJob job;
        Page page;
        int back = 0, pages = 0, i, duplex = 0, fromBack = 0;

        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "duplex")) duplex = 1;
            else if (!strcmp(argv[i], "back")) fromBack = 1;
            else { printf("unknown option %s\n", argv[i]); return 1; }
        }

        /*
         * WinUSB grants one handle per interface: the probe already holds one,
         * so a second CreateFile fails with ACCESS_DENIED and session_create
         * finds nothing. Release ours before the session opens its own.
         */
        proto_destroy(p);
        transport_close(t);
        p = NULL;
        t = NULL;

        s = session_create();
        if (!s) { printf("session_create failed\n"); return 1; }
        session_open(s);
        memset(&job, 0, sizeof job);
        job.resX = job.resY = 300;
        job.composition = COMP_COLOR;
        job.pixelsWide = 2550;
        job.widthUnits = 10200;
        job.lengthUnits = 16800;
        job.paperWidthUnits = 10201;
        job.paperHeightUnits = 16802;
        job.autoLength = 1;
        job.duplex = duplex;
        job.feedFromBack = fromBack;
        LOGF("probe: scan duplex %d feedFromBack %d\n", duplex, fromBack);
        session_set_job(s, &job);

        memset(&page, 0, sizeof page);
        while ((rc = session_next_page(s, &page, &back)) == PROTO_OK) {
            char name[MAX_PATH];
            printf("page %d: %ldx%ld bpp %d side %d\n", pages,
                   page.width, page.height, page.bpp, back);
            sprintf_s(name, sizeof name, "%s%02d.bmp",
                      argc > 2 ? argv[2] : "page", pages);
            if (page_save(&page, name, TWFF_BMP)) printf("  wrote %s\n", name);
            page_free(&page);
            pages++;
        }
        session_destroy(s);
        printf("%d page(s), last rc %d\n", pages, rc);
        return 0;
    }

    proto_destroy(p);
    transport_close(t);
    return 0;
}
