/*
 * Scanner session: turns the protocol into pages.
 *
 * One TWAIN job == one batch. Pages are pulled one at a time so memory stays
 * bounded and MSG_XFERREADY can be posted as soon as a page is complete.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One side of one sheet, uncompressed. A4/Letter colour at 600 dpi is
 * ~101-107 MB, so the ceiling has to clear that; it only exists to stop a
 * bogus pixel-size response from asking for gigabytes.
 */
#define MAX_PAGE_BYTES (320u * 1024 * 1024)

typedef struct Session Session;

struct Session {
    Transport *t;
    Proto     *p;
    ScanJob    job;
    int        opened;
    int        started;     /* batch begun */
    int        exhausted;   /* feeder empty / batch closed */
    int        cancelled;
    Page       pending;     /* next page to hand out */
    int        havePending;
    int        pixelWarned;
};

Session *session_create(void)
{
    Session *s = (Session *)calloc(1, sizeof *s);
    char path[MAX_PATH];
    if (!s) return NULL;
    if (g_cfg.mock) s->t = transport_open_mock_synthetic();
    else s->t = transport_open(path, sizeof path);
    if (!s->t) {
        LOGF("session: no scanner found\n");
        free(s);
        return NULL;
    }
    s->p = proto_create(s->t);
    if (!s->p) {
        transport_close(s->t);
        free(s);
        return NULL;
    }
    return s;
}

void session_destroy(Session *s)
{
    if (!s) return;
    if (s->started && s->p) proto_end(s->p);
    page_free(&s->pending);
    proto_destroy(s->p);
    transport_close(s->t);
    free(s);
}

int session_open(Session *s)
{
    int rc;
    if (s->opened) return PROTO_OK;
    rc = proto_open(s->p);
    if (rc != PROTO_OK) {
        LOGF("session: open failed %d\n", rc);
        return rc;
    }
    s->opened = 1;
    return PROTO_OK;
}

const ProtoInfo *session_info(Session *s)
{
    return &s->p->info;
}

int session_present(Session *s)
{
    return s && s->t != NULL;
}

/* ---------------------------------------------------------------- scanning */

static LONG page_bytes(const ScanJob *j, LONG w, LONG h)
{
    int bpl;
    if (j->composition == COMP_COLOR) bpl = w * 3;
    else if (j->composition == COMP_LINEART) bpl = (w + 7) / 8;
    else bpl = w;
    return (LONG)((size_t)bpl * (size_t)h);
}

/* Reads one side of a sheet into a Page. */
static int read_side(Session *s, int back, LONG w, LONG h, Page *out)
{
    Proto *p = s->p;
    LONG want = page_bytes(&s->job, w, h);
    LONG bytesPerLine = s->job.composition == COMP_COLOR ? w * 3
                        : s->job.composition == COMP_LINEART ? (LONG)((w + 7) / 8) : w;
    BYTE *raw;
    LONG got = 0;
    int rc;

    if (want <= 0 || want > (LONG)MAX_PAGE_BYTES) return PROTO_ERROR;
    raw = (BYTE *)calloc((size_t)want + 16, 1);
    if (!raw) return PROTO_ERROR;

    /* READ/read-image-count carry a 3-byte length: never ask for more. */
    rc = proto_wait_image(p, back, want > 0x200000 ? 0x200000 : want, 600);
    if (rc != PROTO_OK) {
        LOGF("session: image never became ready (%d)\n", rc);
        free(raw);
        return rc;
    }

    while (got < want) {
        DWORD chunk = (DWORD)(want - got);
        DWORD n = 0;
        if (chunk > 262144u) chunk = 262144u;
        if (bytesPerLine > 0 && (LONG)chunk > bytesPerLine) chunk -= chunk % (DWORD)bytesPerLine;
        if (!chunk) chunk = (DWORD)(want - got);
        if ((LONG)(got + (LONG)chunk) > want) chunk = (DWORD)(want - got);

        rc = proto_read_image(p, back, raw + got, chunk, &n);
        if (rc != PROTO_OK) {
            if (n) { got += n; continue; }
            LOGF("session: read failed %d after %ld bytes\n", rc, got);
            free(raw);
            return rc;
        }
        if (!n) break;
        got += n;
        if (n < chunk) break;
    }

    if (got < want) {
        /* short page: keep whole lines */
        if (bytesPerLine > 0) got -= got % bytesPerLine;
        LOGF("session: short page %ld/%ld bytes\n", got, want);
    }
    if (got <= 0) { free(raw); return PROTO_ERROR; }

    {
        LONG lines = bytesPerLine > 0 ? got / bytesPerLine : h;
        if (lines <= 0) lines = h;
        if (!page_from_raw(out, raw, (size_t)got, w, lines, s->job.composition,
                           p->interlace, s->job.resX, s->job.resY, back)) {
            free(raw);
            return PROTO_ERROR;
        }
    }
    free(raw);
    return PROTO_OK;
}

/* Pulls the next sheet. Returns PROTO_OK with *page filled, PROTO_NODOCS at
 * the end of the batch, or another error. */
static int fetch_page(Session *s, Page *page, int *isBack)
{
    Proto *p = s->p;
    int rc;

    if (s->exhausted) return PROTO_NODOCS;

    for (;;) {
        LONG w = s->job.pixelsWide, h = 0;
        int back = s->job.feedFromBack;
        Page front, rear;
        int haveFront = 0, haveRear = 0;

        if (!s->started) {
            rc = proto_begin(p, &s->job);
            if (rc != PROTO_OK) return rc;
            s->started = 1;
        }

        rc = proto_feed(p);
        if (rc == PROTO_NODOCS) {
            s->exhausted = 1;
            proto_end(p);
            s->started = 0;
            return PROTO_NODOCS;
        }
        if (rc != PROTO_OK) return rc;

        rc = proto_start_scan(p);
        if (rc != PROTO_OK) return rc;

        if (proto_pixel_size(p, back, &w, &h) != PROTO_OK) {
            w = s->job.pixelsWide;
            h = (LONG)((long long)s->job.lengthUnits * s->job.resY / 1200);
            if (!s->pixelWarned) {
                LOGF("session: pixel size unavailable, assuming %ldx%ld\n", w, h);
                s->pixelWarned = 1;
            }
        }
        if (w <= 0) w = s->job.pixelsWide;
        if (h <= 0) h = (LONG)((long long)s->job.lengthUnits * s->job.resY / 1200);

        memset(&front, 0, sizeof front);
        memset(&rear, 0, sizeof rear);

        rc = read_side(s, back, w, h, &front);
        if (rc != PROTO_OK) return rc;
        haveFront = 1;

        if (s->job.duplex) {
            LONG bw = w, bh = h;
            if (proto_pixel_size(p, 1, &bw, &bh) != PROTO_OK) { bw = w; bh = h; }
            if (bw <= 0) bw = w;
            if (bh <= 0) bh = h;
            rc = read_side(s, 1, bw, bh, &rear);
            if (rc == PROTO_OK) haveRear = 1;
            else LOGF("session: back side unavailable (%d)\n", rc);
        }

        if (haveFront) {
            page_free(page);
            *page = front;
            if (isBack) *isBack = back;
            if (haveRear) {
                /* queue the back side as the following page */
                page_free(&s->pending);
                s->pending = rear;
                s->havePending = 1;
            }
            return PROTO_OK;
        }
        if (haveRear) {
            *page = rear;
            if (isBack) *isBack = 1;
            return PROTO_OK;
        }
        return PROTO_ERROR;
    }
}

void session_reset(Session *s)
{
    if (!s) return;
    s->cancelled = 0;
    s->exhausted = 0;
    s->started = 0;
    page_free(&s->pending);
    s->havePending = 0;
}

int session_next_page(Session *s, Page *page, int *isBack)
{
    int rc;

    if (s->cancelled) return PROTO_CANCEL;
    if (s->havePending) {
        page_free(page);
        *page = s->pending;
        memset(&s->pending, 0, sizeof s->pending);
        s->havePending = 0;
        if (isBack) *isBack = 1;
        return PROTO_OK;
    }
    rc = fetch_page(s, page, isBack);
    if (rc == PROTO_NODOCS) return PROTO_NODOCS;
    return rc;
}

void session_cancel(Session *s)
{
    if (!s) return;
    s->cancelled = 1;
    if (s->p) proto_end(s->p);
    s->started = 0;
    s->exhausted = 1;
    page_free(&s->pending);
    s->havePending = 0;
}

int session_hopper_has_paper(Session *s)
{
    return s && s->p ? proto_hopper_has_paper(s->p) : 0;
}

void session_set_job(Session *s, const ScanJob *job)
{
    s->job = *job;
}
