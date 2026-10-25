/*
 * selftest - exercise the protocol state machine against a scripted transport.
 *
 * The script is a byte-for-byte replay of the iX1600 front/grayscale
 * transcript (the profile the iX1300 reproduces), so this checks the command
 * builder and the sense/EOF bookkeeping without hardware.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HINSTANCE g_hinst;
Config    g_cfg;

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok   "); } else { printf("  FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

static void push_envelope_status(BYTE statusByte)
{
    mock_push_status(statusByte);
}

/* Builds a scanner INQUIRY response claiming a scanner device. */
static void push_inquiry(void)
{
    BYTE buf[96];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x06;
    memcpy(buf + 8, "FUJITSU ", 8);
    memcpy(buf + 16, "ScanSnap iX1300 ", 16);
    memcpy(buf + 32, "0M00", 4);       /* as reported by the real iX1300 */
    buf[0x2D] = 0x01;                  /* has ADF */
    buf[0x2A] = 0x00; buf[0x2B] = 0x02;  /* colour offset */
    buf[0x2E] = 0x00; buf[0x2F] = 0x00;  /* duplex offset: 0 on the unit */
    mock_push(buf, sizeof buf);
    push_envelope_status(0);
}

static void push_vpd(void)
{
    BYTE buf[204];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x06;
    buf[4] = 0;
    buf[0x05] = 0x04; buf[0x06] = 0xB0;   /* 1200 basic x */
    buf[0x07] = 0x04; buf[0x08] = 0xB0;   /* 1200 basic y */
    buf[0x0A] = 0x02; buf[0x0B] = 0x58;   /* 600 max x */
    buf[0x0C] = 0x02; buf[0x0D] = 0x58;
    buf[0x14] = 0; buf[0x15] = 0; buf[0x16] = 0x27; buf[0x17] = 0xD8; /* 10200 */
    buf[0x18] = 0; buf[0x19] = 0; buf[0x1A] = 0x41; buf[0x1B] = 0xA0; /* 16800 */
    buf[0x1C] = 0x8A;                      /* lineart + gray + rgb */
    buf[0x12] = 0x08;                      /* 150 dpi */
    buf[0x13] = 0x06;                      /* 300 + 600 */
    mock_push(buf, 204);
    push_envelope_status(0);
}

static void push_ok(void)
{
    push_envelope_status(0);
}

static void push_busy(void)
{
    BYTE sense[0x12];
    push_envelope_status(1);
    memset(sense, 0, sizeof sense);
    sense[0] = 0x70;
    sense[2] = 0x02;            /* sense key 2 = not ready */
    mock_push(sense, sizeof sense);
    push_envelope_status(0);
}

int main(void)
{
    Proto *p;
    Transport *t;
    ScanJob job;
    LONG w = 0, h = 0;
    BYTE st[12];
    DWORD got = 0;
    int rc;

    g_hinst = GetModuleHandleA(NULL);
    g_cfg.logEnable = 0;
    g_cfg.mock = 1;
    g_cfg.interlace = 0;
    g_cfg.bufferMode = 0;
    g_cfg.lineartInvert = 0;
    g_cfg.serial[0] = 0;

    printf("protocol self-test (scripted transport)\n");

    t = transport_open_mock();
    p = proto_create(t);
    mock_reset();

    /* --- open: TUR, INQUIRY, VPD --- */
    push_ok();
    push_inquiry();
    push_vpd();
    rc = proto_open(p);
    CHECK(rc == PROTO_OK, "proto_open rc=%d", rc);
    CHECK(strcmp(p->info.vendor, "FUJITSU") == 0, "vendor '%s'", p->info.vendor);
    CHECK(strcmp(p->info.product, "ScanSnap iX1300") == 0, "product '%s'", p->info.product);
    CHECK(p->info.basicXRes == 1200, "basic x res %d", p->info.basicXRes);
    CHECK(p->info.maxWidthUnits == 10200, "max width units %ld", p->info.maxWidthUnits);
    CHECK(p->info.stdResCount >= 3, "std res count %d", p->info.stdResCount);
    CHECK(strcmp(p->info.version, "0M00") == 0, "rom revision '%s'", p->info.version);

    /* --- begin: ADF, 3 mode selects, window probe, lamp --- */
    memset(&job, 0, sizeof job);
    job.resX = 150; job.resY = 150;
    job.composition = COMP_COLOR;
    job.pixelsWide = 1275;
    job.widthUnits = 10200;
    job.lengthUnits = 16800;
    job.paperWidthUnits = 10201;
    job.paperHeightUnits = 16802;
    job.autoLength = 1;
    job.brightness = 0; job.contrast = 0;

    push_ok();      /* f1/00 ADF */
    push_ok();      /* mode select 3c */
    push_ok();      /* mode select 38 */
    push_ok();      /* mode select 39 */
    push_ok();      /* mode select 3a */
    push_ok();      /* set window */
    push_ok();      /* lamp on */
    rc = proto_begin(p, &job);
    CHECK(rc == PROTO_OK, "proto_begin rc=%d", rc);
    CHECK(p->armed == 1, "window armed");
    CHECK(p->interlace == 0, "interlace %d", p->interlace);

    /* --- feed: hopper, OBJECT POSITION, TUR --- */
    {
        BYTE hw[12];
        memset(hw, 0, sizeof hw);
        hw[3] = 0x00;                 /* paper present */
        mock_push(hw, 12);
        push_ok();                    /* GET HW STATUS status */
        push_ok();                    /* object position */
        push_ok();                    /* test unit ready */
    }
    rc = proto_feed(p);
    CHECK(rc == PROTO_OK, "proto_feed rc=%d", rc);

    push_ok();
    rc = proto_start_scan(p);
    CHECK(rc == PROTO_OK, "proto_start_scan rc=%d", rc);
    CHECK(p->batch == 1, "batch open");

    /* --- pixel size --- */
    {
        BYTE sz[32];
        memset(sz, 0, sizeof sz);
        sz[3] = 100; sz[7] = 50;      /* 100 x 50 */
        mock_push(sz, 32);
        push_ok();
    }
    rc = proto_pixel_size(p, 0, &w, &h);
    CHECK(rc == PROTO_OK, "proto_pixel_size rc=%d", rc);
    CHECK(w == 100 && h == 50, "size %ldx%ld", w, h);

    /* --- read-image-count then one READ --- */
    push_ok();
    rc = proto_wait_image(p, 0, 300, 3);
    CHECK(rc == PROTO_OK, "proto_wait_image rc=%d", rc);

    {
        BYTE img[300];
        DWORD n = 0;
        memset(img, 0x11, sizeof img);
        mock_push(img, 300);
        push_ok();
        rc = proto_read_image(p, 0, img, 300, &n);
        CHECK(rc == PROTO_OK && n == 300, "proto_read_image rc=%d n=%lu", rc, n);
    }

    /* --- short read signalled by ILI: 100 of 300 bytes delivered --- */
    {
        BYTE img[300];
        DWORD n = 0;
        BYTE sense[0x12];
        memset(img, 0, sizeof img);
        mock_push(img, 300);
        push_envelope_status(1);
        memset(sense, 0, sizeof sense);
        sense[0] = 0x70;
        sense[2] = 0x20;             /* ILI, sense key 0 */
        sense[3] = 0; sense[4] = 0; sense[5] = 0; sense[6] = 200;
        mock_push(sense, sizeof sense);
        push_ok();
        rc = proto_read_image(p, 0, img, 300, &n);
        CHECK(rc == PROTO_OK && n == 100, "ILI short read rc=%d n=%lu", rc, n);
    }

    /* --- status data with a CHECK CONDITION (sense key 2 = not ready) --- */
    {
        BYTE hw[12];
        memset(hw, 0, sizeof hw);
        hw[3] = 0x00;
        mock_push(hw, 12);
        push_busy();
    }
    rc = proto_hw_status(p, st, &got);
    CHECK(rc == PROTO_BUSY, "hw status busy rc=%d", rc);
    CHECK(got == 12, "busy status data length %lu", got);

    /* --- out of step: leftover image bytes ahead of the status envelope ---
     * The scanner shortens a data-in, or a previous session died mid-batch,
     * so the pipe starts with stream bytes. The command must still see a
     * valid status and the next command must start aligned. */
    {
        BYTE junk[13];
        memset(junk, 0x11, sizeof junk);      /* residual image, not 0x53 */
        mock_push(junk, sizeof junk);
        push_ok();
        rc = proto_test_unit_ready(p);
        CHECK(rc == PROTO_OK, "resync past %u stale bytes rc=%d",
              (unsigned)sizeof junk, rc);

        memset(junk, 0x22, sizeof junk);
        mock_push(junk, sizeof junk);
        push_ok();
        rc = proto_test_unit_ready(p);
        CHECK(rc == PROTO_OK, "second command aligned rc=%d", rc);
    }

    /* --- empty feeder --- */
    {
        BYTE hw[12];
        memset(hw, 0, sizeof hw);
        hw[3] = 0x80;                 /* hopper empty */
        mock_push(hw, 12);
        push_ok();
    }
    rc = proto_feed(p);
    CHECK(rc == PROTO_NODOCS, "empty hopper rc=%d", rc);

    /* --- end --- */
    push_ok();
    rc = proto_end(p);
    CHECK(rc == PROTO_OK, "proto_end rc=%d", rc);

    proto_destroy(p);
    transport_close(t);

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
