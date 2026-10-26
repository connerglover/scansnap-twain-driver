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
    else {
        transport_request_device();
        s->t = transport_open(path, sizeof path);
    }
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

/* Image READs are line-aligned blocks of at most this many bytes. */
#define READ_BLOCK 262144u

/* One side of the sheet being read. */
typedef struct {
    int   back;         /* window 0x80 */
    LONG  w, h, bpl;
    LONG  want, got;
    BYTE *raw;
    int   ready;        /* read-image-count answered */
    int   lastIli;      /* this side's last READ was short */
    int   done;
} Side;

static LONG bytes_per_line(const ScanJob *j, LONG w)
{
    if (j->composition == COMP_COLOR) return w * 3;
    if (j->composition == COMP_LINEART) return (w + 7) / 8;
    return w;
}

/* Sizes one side from its pixel-size answer, else from the job geometry. */
static int side_init(Session *s, Side *sd, int back)
{
    LONG w = s->job.pixelsWide, h = 0;

    memset(sd, 0, sizeof *sd);
    sd->back = back;
    if (proto_pixel_size(s->p, back, &w, &h) != PROTO_OK) {
        w = s->job.pixelsWide;
        h = 0;
    }
    if (w <= 0) w = s->job.pixelsWide;
    if (h <= 0) {
        h = (LONG)((long long)s->job.lengthUnits * s->job.resY / 1200);
        if (!s->pixelWarned) {
            LOGF("session: pixel size unavailable, assuming %ldx%ld\n", w, h);
            s->pixelWarned = 1;
        }
    }
    sd->w = w;
    sd->h = h;
    sd->bpl = bytes_per_line(&s->job, w);
    if (sd->bpl <= 0 || h <= 0 || (size_t)sd->bpl * (size_t)h > MAX_PAGE_BYTES) return PROTO_ERROR;
    sd->want = sd->bpl * h;
    sd->raw = (BYTE *)calloc((size_t)sd->want + 16, 1);
    return sd->raw ? PROTO_OK : PROTO_ERROR;
}

static void side_free(Side *sd)
{
    free(sd->raw);
    sd->raw = NULL;
}

/*
 * Reads every side of the current sheet. Duplex alternates front and back
 * one block at a time, as SANE does for this generation: while one side's
 * data is not ready the unit answers "no image data yet" (03/80/13), and a
 * side read to completion before the other is touched stalls. A side ends
 * when its buffer is full, or - SANE's rule - once end of medium has been
 * seen and that side's last READ was short (ILI).
 */
static int read_sheet(Session *s, Side *sides, int n)
{
    Proto *p = s->p;
    int eom = 0, i, rc;
    DWORD idleMs = 0;

    for (;;) {
        int progress = 0, open = 0;

        for (i = 0; i < n; i++) {
            Side *sd = &sides[i];
            DWORD chunk, got = 0;
            int flags = 0;

            if (sd->done) continue;
            if (!sd->ready) {
                /* READ/read-image-count carry a 3-byte length: never ask for more. */
                rc = proto_wait_image(p, sd->back, sd->want > 0x200000 ? 0x200000 : sd->want, 600);
                if (rc != PROTO_OK) {
                    LOGF("session: %s image never became ready (%d)\n", sd->back ? "back" : "front", rc);
                    return rc;
                }
                sd->ready = 1;
            }

            chunk = (DWORD)(sd->want - sd->got);
            if (chunk > READ_BLOCK) {
                chunk = READ_BLOCK;
                if (chunk > (DWORD)sd->bpl) chunk -= chunk % (DWORD)sd->bpl;
            }
            rc = proto_read_image(p, sd->back, sd->raw + sd->got, chunk, &got, &flags);
            if (rc == PROTO_NODATA || rc == PROTO_BUSY) {
                sd->lastIli = 0;
                continue;
            }
            if (rc != PROTO_OK) {
                LOGF("session: %s read failed %d after %ld bytes\n",
                     sd->back ? "back" : "front", rc, sd->got);
                return rc;
            }
            sd->got += (LONG)got;
            sd->lastIli = (flags & PROTO_READ_ILI) != 0;
            if (flags & PROTO_READ_EOM) eom = 1;
            if (got) progress = 1;
            if (sd->got >= sd->want) sd->done = 1;
        }

        for (i = 0; i < n; i++) {
            if (eom && sides[i].lastIli) sides[i].done = 1;
            if (!sides[i].done) open = 1;
        }
        if (!open) return PROTO_OK;
        if (progress) {
            idleMs = 0;
            continue;
        }
        if (idleMs >= 30000) {
            LOGF("session: no image data for %lu ms (eom %d)\n", idleMs, eom);
            return eom ? PROTO_OK : PROTO_NODATA;
        }
        Sleep(20);
        idleMs += 20;
    }
}

/* Turns what one side delivered into a Page, keeping whole lines only. */
static int side_page(Session *s, const Side *sd, Page *out)
{
    LONG got = sd->got - sd->got % sd->bpl;

    if (got < sd->want)
        LOGF("session: short page %ld/%ld bytes (%s)\n", got, sd->want, sd->back ? "back" : "front");
    if (got <= 0) return PROTO_ERROR;
    if (!page_from_raw(out, sd->raw, (size_t)got, sd->w, got / sd->bpl, s->job.composition,
                       s->p->interlace, s->job.resX, s->job.resY, sd->back))
        return PROTO_ERROR;
    return PROTO_OK;
}

/* Pulls the next sheet. Returns PROTO_OK with *page filled, PROTO_NODOCS at
 * the end of the batch, or another error. */
static int fetch_page(Session *s, Page *page, int *isBack)
{
    Proto *p = s->p;
    Side sides[2];
    Page front, rear;
    int n = 0, rc, haveRear = 0;
    /* duplex reads both windows; simplex reads the one the job names */
    int first = s->job.duplex ? 0 : s->job.feedFromBack;

    if (s->exhausted) return PROTO_NODOCS;

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

    memset(sides, 0, sizeof sides);
    memset(&front, 0, sizeof front);
    memset(&rear, 0, sizeof rear);

    rc = side_init(s, &sides[n++], first);
    if (rc == PROTO_OK && s->job.duplex) rc = side_init(s, &sides[n++], 1);
    if (rc == PROTO_OK) rc = read_sheet(s, sides, n);
    if (rc == PROTO_OK) rc = side_page(s, &sides[0], &front);
    side_free(&sides[0]);
    if (rc == PROTO_OK && n > 1) {
        if (side_page(s, &sides[1], &rear) == PROTO_OK) haveRear = 1;
        else LOGF("session: back side unavailable\n");
    }
    side_free(&sides[1]);
    if (rc != PROTO_OK) return rc;

    page_free(page);
    *page = front;
    if (isBack) *isBack = first;
    if (haveRear) {
        /* queue the back side as the following page */
        page_free(&s->pending);
        s->pending = rear;
        s->havePending = 1;
    }
    return PROTO_OK;
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
