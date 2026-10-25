/*
 * probe - hardware probe for the standalone driver.
 *
 *   probe.exe info            device path, INQUIRY, VPD, hardware status
 *   probe.exe status          loop on GET HW STATUS
 *   probe.exe scan [out.bmp]  one full page through the session layer
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

static void print_status(const BYTE *s)
{
    printf("hw: top %d hopper %d adf_open %d paper_end %d sleep %d scan_sw %d err 0x%02x\n",
           (s[2] & 0x80) != 0,
           (s[3] & 0x80) == 0,
           (s[3] & 0x20) != 0,
           (s[3] & 0x04) != 0,
           (s[4] & 0x80) != 0,
           (s[4] & 0x01) != 0,
           s[7]);
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
    if (argc > 3) strcpy_s(g_cfg.logPath, sizeof g_cfg.logPath, argv[3]);
    LOGF("probe: start\n");

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
    printf("vpd: colour %d gray %d lineart %d duplex %d\n",
           p->info.hasColor, p->info.hasGray, p->info.hasLineart, p->info.hasDuplex);

    if (!strcmp(cmd, "status")) {
        for (;;) {
            BYTE st[12];
            DWORD got = 0;
            if (proto_hw_status(p, st, &got) == PROTO_OK) {
                print_status(st);
            } else {
                printf("status read failed\n");
            }
            fflush(stdout);
            Sleep(250);
        }
    }

    {
        BYTE st[12];
        DWORD got = 0;
        if (proto_hw_status(p, st, &got) == PROTO_OK) print_status(st);
    }

    if (!strcmp(cmd, "scan")) {
        Session *s = session_create();
        ScanJob job;
        Page page;
        int back = 0, pages = 0;

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
        session_set_job(s, &job);

        memset(&page, 0, sizeof page);
        while (session_next_page(s, &page, &back) == PROTO_OK) {
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
        printf("%d page(s)\n", pages);
    }

    proto_destroy(p);
    transport_close(t);
    return 0;
}
