/*
 * Fujitsu SCSI-over-USB protocol for the ScanSnap iX1300.
 *
 * Wire format (verified against sane-backends `fujitsu` and the iX1600
 * transcript this generation reproduces byte for byte):
 *
 *   bulk OUT  31 bytes: 43 00 .. 00 <CDB at offset 19>
 *   bulk OUT  N  bytes: command data-out (mode page, window block, ...)
 *   bulk IN   N  bytes: command data-in (INQUIRY, pixel size, image, ...)
 *   bulk IN   13 bytes: 53 .. .. status at offset 9
 *
 * status 0 = good, 8 = busy, otherwise a CHECK CONDITION whose REQUEST SENSE
 * distinguishes "no documents" from a real fault.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMD_LEN     0x1F
#define CMD_OFFSET  0x13
#define STAT_LEN    0x0D
#define STAT_OFFSET 0x09
#define STAT_CODE   0x53

struct ProtoTag { int unused; };

static void putn(BYTE *p, unsigned v, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) { p[i] = (BYTE)(v & 0xFF); v >>= 8; }
}

static unsigned getn(const BYTE *p, int n)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

typedef struct {
    BYTE  key, asc, ascq;
    DWORD info;
    int   eom, ili;
    BYTE  raw[0x12];
} Sense;

static void parse_sense(const BYTE *b, Sense *s)
{
    memset(s, 0, sizeof *s);
    memcpy(s->raw, b, sizeof s->raw);
    s->key  = b[0x02] & 0x0F;
    s->eom  = (b[0x02] & 0x40) != 0;
    s->ili  = (b[0x02] & 0x20) != 0;
    s->info = getn(b + 0x03, 4);
    s->asc  = b[0x0C];
    s->ascq = b[0x0D];
}

static int classify_sense(const Sense *s)
{
    if (s->key == 0x02 && s->asc == 0x00) return PROTO_BUSY;
    if (s->key == 0x03 && s->asc == 0x80) {
        switch (s->ascq) {
        case 0x01: case 0x04: case 0x07: case 0x08: return PROTO_JAM;
        case 0x02: return PROTO_COVER;
        case 0x03: case 0x20: case 0x30: return PROTO_NODOCS;
        case 0x13: return PROTO_NODATA;
        }
    }
    return PROTO_ERROR;
}
typedef struct {
    Transport *t;
    int lastErr;
    Sense lastSense;
} Link;

/*
 * Reads the 13-byte status envelope. If the device is out of step - a short
 * data-in left image bytes queued, or a previous session died mid-batch - the
 * bytes at the head of the pipe are not the envelope. Scan forward for one
 * (bounded) so the *next* command starts aligned instead of failing forever.
 */
static int read_status(Link *l, BYTE *st, DWORD timeoutMs)
{
    int attempts;

    for (attempts = 0; attempts < 64; attempts++) {
        DWORD got = STAT_LEN;

        if (!transport_bulk_in(l->t, st, &got, timeoutMs)) return 0;
        if (got == STAT_LEN && st[0] == STAT_CODE) return 1;
        if (got < STAT_LEN) {
            LOGF("proto: short status packet (%lu)\n", got);
            return 0;
        }
        LOGF("proto: resync, discarded %lu bytes (head 0x%02x)\n", got, st[0]);
    }
    LOGF("proto: no status envelope after %d reads\n", attempts);
    return 0;
}

/* One SCSI command: envelope, optional data-out, optional data-in, status. */
static int do_cmd(Link *l, const BYTE *cdb, int cdbLen,
                  const BYTE *outBuf, DWORD outLen,
                  BYTE *inBuf, DWORD *inLen, DWORD timeoutMs)
{
    BYTE env[CMD_LEN];
    BYTE st[STAT_LEN];

    memset(env, 0, sizeof env);
    env[0] = 0x43;
    memcpy(env + CMD_OFFSET, cdb, cdbLen);
    if (!transport_bulk_out(l->t, env, sizeof env, timeoutMs)) return PROTO_IO;

    if (outBuf && outLen) {
        if (!transport_bulk_out(l->t, outBuf, outLen, timeoutMs)) return PROTO_IO;
    }
    if (inBuf && inLen) {
        DWORD want = *inLen;
        if (!transport_bulk_in(l->t, inBuf, &want, timeoutMs)) return PROTO_IO;
        *inLen = want;
    }

    if (!read_status(l, st, timeoutMs)) return PROTO_IO;
    if (st[STAT_OFFSET] == 0) return PROTO_OK;
    if (st[STAT_OFFSET] == 8) {
        l->lastErr = PROTO_BUSY;
        return PROTO_BUSY;
    }

    /* CHECK CONDITION: fetch sense, then classify it. */
    {
        BYTE rsCmd[6] = { 0x03, 0, 0, 0, 0x12, 0 };
        BYTE sense[0x12];
        DWORD sl = sizeof sense;
        memset(sense, 0, sizeof sense);
        if (do_cmd(l, rsCmd, sizeof rsCmd, NULL, 0, sense, &sl, 30000) != PROTO_OK || sl < 14) {
            LOGF("proto: sense unavailable, status %u\n", st[STAT_OFFSET]);
            l->lastErr = PROTO_ERROR;
            return PROTO_ERROR;
        }
        parse_sense(sense, &l->lastSense);
        LOGF("proto: sense key 0x%02x asc 0x%02x ascq 0x%02x eom %d ili %d info %lu\n",
             l->lastSense.key, l->lastSense.asc, l->lastSense.ascq,
             l->lastSense.eom, l->lastSense.ili, l->lastSense.info);
        l->lastErr = classify_sense(&l->lastSense);
        return l->lastErr;
    }
}

static int scsi_simple(Link *l, BYTE op, int timeoutMs)
{
    BYTE cdb[10];
    memset(cdb, 0, sizeof cdb);
    cdb[0] = op;
    return do_cmd(l, cdb, (op == 0x00 || op == 0x03) ? 6 : 10, NULL, 0, NULL, NULL, timeoutMs);
}

/* --------------------------------------------------------------- open/init */

static int read_inquiry(Link *l, ProtoInfo *info)
{
    BYTE cdb[6] = { 0x12, 0, 0, 0, 96, 0 };
    BYTE buf[96];
    DWORD len = sizeof buf;
    int rc;

    memset(buf, 0, sizeof buf);
    rc = do_cmd(l, cdb, sizeof cdb, NULL, 0, buf, &len, 30000);
    if (rc != PROTO_OK) return rc;
    if (len < 36 || (buf[0] & 0x1F) != 0x06) {
        LOGF("proto: inquiry is not a scanner (len %lu, type %u)\n", len, buf[0] & 0x1F);
        return PROTO_ERROR;
    }
    memcpy(info->vendor, buf + 8, 8);   info->vendor[8] = 0;
    memcpy(info->product, buf + 16, 16); info->product[16] = 0;
    memcpy(info->version, buf + 32, 4);  info->version[4] = 0;
    /* trim trailing spaces */
    {
        int i;
        for (i = 7; i >= 0 && info->vendor[i] == ' '; i--) info->vendor[i] = 0;
        for (i = 15; i >= 0 && info->product[i] == ' '; i--) info->product[i] = 0;
        for (i = 3; i >= 0 && info->version[i] == ' '; i--) info->version[i] = 0;
    }
    info->hasAdf = (buf[0x2D] & 0x01) != 0;
    info->hasFlatbed = (buf[0x2D] & 0x02) != 0;
    info->colorOffset = (int)getn(buf + 0x2A, 2);
    info->duplexOffset = (int)getn(buf + 0x2E, 2);
    LOGF("proto: inquiry '%s' '%s' rev '%s' adf %d flatbed %d coloroff %d duplexoff %d\n",
         info->vendor, info->product, info->version,
         info->hasAdf, info->hasFlatbed, info->colorOffset, info->duplexOffset);
    return PROTO_OK;
}

static int read_vpd(Link *l, ProtoInfo *info)
{
    BYTE cdb[6] = { 0x12, 0x01, 0xF0, 0, 204, 0 };
    BYTE buf[204];
    DWORD len = sizeof buf;
    int rc;

    memset(buf, 0, sizeof buf);
    rc = do_cmd(l, cdb, sizeof cdb, NULL, 0, buf, &len, 30000);
    if (rc != PROTO_OK) return rc;
    if (len < 0x20) {
        LOGF("proto: short VPD (%lu)\n", len);
        return PROTO_ERROR;
    }
    info->basicXRes    = (int)getn(buf + 0x05, 2);
    info->basicYRes    = (int)getn(buf + 0x07, 2);
    info->maxXRes      = (int)getn(buf + 0x0A, 2);
    info->maxYRes      = (int)getn(buf + 0x0C, 2);
    info->maxWidthUnits  = (LONG)getn(buf + 0x14, 4);
    info->maxLengthUnits = (LONG)getn(buf + 0x18, 4);
    info->hasLineart = (buf[0x1C] & 0x02) != 0;
    info->hasGray    = (buf[0x1C] & 0x08) != 0;
    info->hasColor   = (buf[0x1C] & 0x80) != 0;
    info->hasDuplex  = info->hasAdf && info->duplexOffset != 0;

    /* standard resolutions: 8 flags in byte 0x12, 8 in byte 0x13 */
    {
        static const int lowOrder[8]  = { 60, 75, 100, 120, 150, 160, 180, 200 };
        static const int highOrder[8] = { 1200, 800, 600, 480, 400, 320, 300, 240 };
        int i;
        info->stdResCount = 0;
        for (i = 0; i < 8; i++)
            if (buf[0x12] & (1 << (7 - i))) info->stdRes[info->stdResCount++] = lowOrder[i];
        for (i = 0; i < 8; i++)
            if (buf[0x13] & (1 << (7 - i))) info->stdRes[info->stdResCount++] = highOrder[i];
    }
    LOGF("proto: vpd basic %dx%d max %dx%d window %ldx%ld color %d gray %d lineart %d duplex %d\n",
         info->basicXRes, info->basicYRes, info->maxXRes, info->maxYRes,
         info->maxWidthUnits, info->maxLengthUnits,
         info->hasColor, info->hasGray, info->hasLineart, info->hasDuplex);
    return PROTO_OK;
}

int proto_open(Proto *p)
{
    Link l;
    int attempt, rc = PROTO_ERROR;

    memset(&l, 0, sizeof l);
    l.t = p->t;
    for (attempt = 1; attempt <= 3; attempt++) {
        rc = scsi_simple(&l, 0x00, 2000);
        if (rc == PROTO_OK) break;
        Sleep(500);
    }
    if (rc != PROTO_OK) {
        LOGF("proto: device never became ready\n");
        return rc;
    }
    if ((rc = read_inquiry(&l, &p->info)) != PROTO_OK) return rc;
    if ((rc = read_vpd(&l, &p->info)) != PROTO_OK) {
        LOGF("proto: VPD failed, continuing with defaults\n");
        p->info.basicXRes = 1200;
        p->info.basicYRes = 1200;
        p->info.maxXRes = 600;
        p->info.maxYRes = 600;
        p->info.maxWidthUnits = 10200;
        p->info.maxLengthUnits = 16800;
        p->info.hasColor = p->info.hasGray = p->info.hasLineart = 1;
        p->info.hasDuplex = 1;
    }
    p->info.stdResCount = 6;
    p->info.stdRes[0] = 150; p->info.stdRes[1] = 200; p->info.stdRes[2] = 300;
    p->info.stdRes[3] = 400; p->info.stdRes[4] = 600; p->info.stdRes[5] = 1200;
    p->info.valid = 1;
    p->interlace = g_cfg.interlace;
    return PROTO_OK;
}

Proto *proto_create(Transport *t)
{
    Proto *p = (Proto *)calloc(1, sizeof *p);
    if (p) { p->t = t; p->interlace = -1; }
    return p;
}

void proto_destroy(Proto *p)
{
    free(p);
}

/* ------------------------------------------------------------- mode pages */

static int mode_select_page(Proto *p, BYTE page, int payloadLen, const BYTE *payload)
{
    Link l;
    BYTE cdb[6];
    BYTE buf[16];
    int total = 4 + payloadLen;

    if (total > (int)sizeof buf) total = sizeof buf;
    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x15;
    cdb[1] = 0x10;               /* PF (page format) */
    cdb[4] = (BYTE)total;
    memset(buf, 0, sizeof buf);
    memcpy(buf + 4, payload, (size_t)(total - 4));
    (void)page;
    return do_cmd(&l, cdb, 6, buf, (DWORD)total, NULL, NULL, 30000);
}

static int mode_select_auto(Proto *p, int ald)
{
    BYTE page[8];
    memset(page, 0, sizeof page);
    page[0] = 0x3C;
    page[1] = 6;
    if (ald) page[3] |= 0x80;   /* ALD: stop at the real page end */
    return mode_select_page(p, 0x3C, 8, page);
}

static int mode_select_df(Proto *p)
{
    BYTE page[8];
    memset(page, 0, sizeof page);
    page[0] = 0x38;
    page[1] = 6;
    return mode_select_page(p, 0x38, 8, page);
}

static int mode_select_dropout(Proto *p)
{
    BYTE page[10];
    memset(page, 0, sizeof page);
    page[0] = 0x39;
    page[1] = 8;
    return mode_select_page(p, 0x39, 10, page);
}

static int mode_select_buffer(Proto *p, int on)
{
    BYTE page[8];
    memset(page, 0, sizeof page);
    page[0] = 0x3A;
    page[1] = 6;
    page[2] = (BYTE)((on ? 3 : 2) << 6);
    page[3] = 0xC0;              /* clear buffer */
    return mode_select_page(p, 0x3A, 8, page);
}

/* ---------------------------------------------------------------- window */

static void build_window(BYTE *d, const ScanJob *j, int interlace, int duplex,
                         int windowId, int withPaper)
{
    memset(d, 0, 64);
    d[0x00] = (BYTE)windowId;
    putn(d + 0x02, j->resX, 2);
    putn(d + 0x04, j->resY, 2);
    putn(d + 0x06, (unsigned)j->ulxUnits, 4);   /* ULX */
    putn(d + 0x0A, (unsigned)j->ulyUnits, 4);   /* ULY */
    putn(d + 0x0E, (unsigned)j->widthUnits, 4);
    putn(d + 0x12, (unsigned)j->lengthUnits, 4);
    d[0x16] = (BYTE)(j->brightness + 128);     /* 0 when neutral */
    d[0x17] = (BYTE)j->threshold;
    d[0x18] = (BYTE)(j->contrast + 128);
    d[0x19] = (BYTE)j->composition;
    d[0x1A] = (BYTE)(j->composition == COMP_LINEART ? 1 : 8);
    /* compression: none, so 0x20/0x21 stay 0 */

    if (j->composition == COMP_COLOR) {
        d[0x28] = 0xC1;                          /* colour vendor block */
        d[0x29] = 0x00;                          /* internal gamma curve */
        d[0x2A] = (BYTE)(interlace == 2 ? 0x00 : 0x01);   /* line vs dot */
        d[0x2B] = (BYTE)(interlace == 1 ? 0x05 : 0x00);   /* BGR vs RGB */
    } else {
        d[0x28] = 0x00;                          /* mono vendor block */
        if (j->composition == COMP_GRAY) d[0x29] = 0x00;
        /* lineart uses the threshold in 0x17 */;
    }

    if (withPaper) {
        d[0x35] = 0xC0;                          /* non-standard paper */
        putn(d + 0x36, (unsigned)j->paperWidthUnits, 4);
        putn(d + 0x3A, (unsigned)j->paperHeightUnits, 4);
    }
    (void)duplex;
}

static int set_window(Proto *p, int interlace)
{
    Link l;
    BYTE cdb[10];
    BYTE payload[8 + 64 + 64];

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(payload, 0, sizeof payload);
    putn(payload + 6, 64, 2);                    /* descriptor length */
    build_window(payload + 8, &p->job, interlace, p->job.duplex, 0x00, 1);
    if (p->job.duplex) {
        build_window(payload + 8 + 64, &p->job, interlace, 1, 0x80, 0);
    } else if (p->job.feedFromBack) {
        build_window(payload + 8, &p->job, interlace, 0, 0x80, 0);
    }

    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x24;
    putn(cdb + 6, p->job.duplex ? 136 : 72, 3);
    return do_cmd(&l, cdb, 10, payload, p->job.duplex ? 136 : 72, NULL, NULL, 30000);
}

static int choose_window(Proto *p)
{
    int order[3], i, rc = PROTO_ERROR;
    int start = p->interlace >= 0 ? p->interlace : 0;
    int count = p->interlace >= 0 ? 1 : 3;

    if (p->job.composition != COMP_COLOR) {
        rc = set_window(p, 0);
        if (rc == PROTO_OK) p->interlace = 0;
        return rc;
    }
    for (i = 0; i < count; i++) order[i] = (start + i) % 3;
    for (i = 0; i < count; i++) {
        rc = set_window(p, order[i]);
        if (rc == PROTO_OK) {
            p->interlace = order[i];
            LOGF("proto: colour interlace %d accepted\n", order[i]);
            return PROTO_OK;
        }
        LOGF("proto: colour interlace %d rejected (%d)\n", order[i], rc);
    }
    return rc;
}

/* ---------------------------------------------------------------- control */

static int scanner_control(Proto *p, int function)
{
    Link l;
    BYTE cdb[10];
    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0xF1;
    cdb[1] = (BYTE)(function & 0x0F);   /* low nibble; bit 4 selects RIC */
    cdb[2] = (BYTE)(function >> 4);
    return do_cmd(&l, cdb, 10, NULL, 0, NULL, NULL, 30000);
}

int proto_hw_status(Proto *p, BYTE out[12], DWORD *got)
{
    Link l;
    BYTE cdb[10];
    DWORD len = 12;
    int rc;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0xC2;
    putn(cdb + 7, 12, 2);
    memset(out, 0, 12);
    rc = do_cmd(&l, cdb, 10, NULL, 0, out, &len, 30000);
    if (got) *got = len;
    return rc;
}

int proto_hopper_has_paper(Proto *p)
{
    BYTE st[12];
    DWORD got = 0;
    if (proto_hw_status(p, st, &got) != PROTO_OK || got < 4) return 1;
    return (st[3] & 0x80) == 0;
}

/* ----------------------------------------------------------------- begin */

int proto_begin(Proto *p, const ScanJob *job)
{
    int rc, attempt;

    p->job = *job;
    p->armed = 0;
    p->batch = 0;

    rc = scanner_control(p, 0x00);               /* ADF source */
    if (rc != PROTO_OK) LOGF("proto: ADF select returned %d (ignored)\n", rc);

    if ((rc = mode_select_auto(p, job->autoLength)) != PROTO_OK)
        LOGF("proto: auto mode select %d (ignored)\n", rc);
    if ((rc = mode_select_df(p)) != PROTO_OK)
        LOGF("proto: double-feed mode select %d (ignored)\n", rc);
    if ((rc = mode_select_dropout(p)) != PROTO_OK)
        LOGF("proto: dropout mode select %d (ignored)\n", rc);
    if ((rc = mode_select_buffer(p, g_cfg.bufferMode)) != PROTO_OK)
        LOGF("proto: buffer mode select %d (ignored)\n", rc);

    if ((rc = choose_window(p)) != PROTO_OK) {
        LOGF("proto: set window failed %d\n", rc);
        return rc;
    }
    p->armed = 1;

    for (attempt = 1; attempt <= 120; attempt++) {
        rc = scanner_control(p, 0x05);           /* lamp on */
        if (rc == PROTO_OK) break;
        if (rc != PROTO_BUSY) { LOGF("proto: lamp on ignored (%d)\n", rc); break; }
        Sleep(500);
    }
    return PROTO_OK;
}

int proto_feed(Proto *p)
{
    Link l;
    BYTE cdb[10];
    int rc;

    if (!proto_hopper_has_paper(p)) return PROTO_NODOCS;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x31;
    cdb[1] = 0x01;                               /* OP_Feed */
    rc = do_cmd(&l, cdb, 10, NULL, 0, NULL, NULL, 30000);
    if (rc != PROTO_OK) return rc;

    /* this generation requires the scanner to report ready before SCAN */
    {
        int i;
        for (i = 0; i < 3; i++) {
            rc = scsi_simple(&l, 0x00, 500);
            if (rc == PROTO_OK) break;
            Sleep(500);
        }
        if (rc == PROTO_BUSY) return PROTO_OK;   /* still spinning: let SCAN queue */
    }
    return PROTO_OK;
}

int proto_start_scan(Proto *p)
{
    Link l;
    BYTE cdb[6];
    BYTE windows[2];
    int rc, n = 0;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x1B;
    if (p->job.duplex) { windows[n++] = 0x00; windows[n++] = 0x80; }
    else windows[n++] = (BYTE)(p->job.feedFromBack ? 0x80 : 0x00);
    cdb[4] = (BYTE)n;
    rc = do_cmd(&l, cdb, 6, windows, (DWORD)n, NULL, NULL, 30000);
    if (rc == PROTO_OK) p->batch = 1;
    return rc;
}

int proto_pixel_size(Proto *p, int back, LONG *w, LONG *h)
{
    Link l;
    BYTE cdb[10];
    BYTE buf[0x20];
    DWORD len = sizeof buf;
    int rc;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x28;
    cdb[2] = 0x80;                              /* R_datatype_pixelsize */
    cdb[5] = (BYTE)(back ? 0x80 : 0x00);
    putn(cdb + 6, sizeof buf, 3);
    memset(buf, 0, sizeof buf);
    rc = do_cmd(&l, cdb, 10, NULL, 0, buf, &len, 30000);
    if (rc != PROTO_OK || len < 8) return rc == PROTO_OK ? PROTO_ERROR : rc;
    *w = (LONG)getn(buf, 4);
    *h = (LONG)getn(buf + 4, 4);
    LOGF("proto: pixel size %s %ldx%ld\n", back ? "back" : "front", *w, *h);
    return PROTO_OK;
}

int proto_wait_image(Proto *p, int back, LONG bytes, int attempts)
{
    Link l;
    BYTE cdb[10];
    int i, rc;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0xF1;
    cdb[1] = 0x10;                               /* read-image-count */
    cdb[2] = (BYTE)(back ? 0x80 : 0x00);
    putn(cdb + 6, (unsigned)bytes, 3);

    for (i = 0; i < attempts; i++) {
        rc = do_cmd(&l, cdb, 10, NULL, 0, NULL, NULL, 30000);
        if (rc == PROTO_OK) return PROTO_OK;
        if (rc == PROTO_NODATA || rc == PROTO_BUSY) {
            Sleep(100);
            continue;
        }
        if (rc == PROTO_ERROR) {
            /* older firmware: read-image-count unsupported */
            LOGF("proto: read-image-count unsupported, continuing\n");
            return PROTO_OK;
        }
        return rc;
    }
    return PROTO_NODATA;
}

int proto_read_image(Proto *p, int back, BYTE *buf, DWORD want, DWORD *got)
{
    Link l;
    BYTE cdb[10];
    DWORD len = want;
    int rc;

    memset(&l, 0, sizeof l); l.t = p->t;
    memset(cdb, 0, sizeof cdb);
    cdb[0] = 0x28;
    cdb[2] = 0x00;                              /* R_datatype_imagedata */
    cdb[5] = (BYTE)(back ? 0x80 : 0x00);
    putn(cdb + 6, want, 3);
    rc = do_cmd(&l, cdb, 10, NULL, 0, buf, &len, 30000);
    *got = len;
    if (rc == PROTO_OK) return rc;
    if (rc == PROTO_ERROR) {
        /* benign end-of-page: sense 0/0 with EOM or ILI */
        if (l.lastSense.key == 0x00 && l.lastSense.asc == 0x00 &&
            (l.lastSense.eom || l.lastSense.ili)) {
            if (l.lastSense.ili && len == want && l.lastSense.info < want)
                *got = want - l.lastSense.info;
            return PROTO_OK;
        }
    }
    return rc;
}

int proto_end(Proto *p)
{
    int rc = PROTO_OK;
    if (p->batch) {
        rc = scanner_control(p, 0x04);           /* cancel */
        p->batch = 0;
    }
    if (g_cfg.bufferMode) mode_select_buffer(p, 0);
    p->armed = 0;
    return rc;
}

/* TEST UNIT READY, for tests that need one plain command round trip. */
int proto_test_unit_ready(Proto *p)
{
    Link l;
    memset(&l, 0, sizeof l); l.t = p->t;
    return scsi_simple(&l, 0x00, 30000);
}
