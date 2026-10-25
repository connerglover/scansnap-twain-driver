/*
 * ScanSnap iX1300 TWAIN data source - shared declarations.
 *
 * Standalone: talks to the scanner over WinUSB using the Fujitsu
 * SCSI-over-USB envelope. No ScanSnap Home installation, process or service is
 * used. See docs/PROTOCOL.md for the reverse-engineered command set.
 */
#ifndef SSDRV_H
#define SSDRV_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include "twain.h"

#define SSDRV_VERSION_MAJOR 1
#define SSDRV_VERSION_MINOR 0
#define SSDRV_VERSION_STR   "1.0.0"

#define SSDRV_USB_VID 0x04C5
#define SSDRV_USB_PID 0x162C

/* {6E7C3D2A-5C41-4A0E-9C2B-1D2F5A8B7C10} - must match install/*.inf */
#define SSDRV_IFACE_GUID_STR "{6E7C3D2A-5C41-4A0E-9C2B-1D2F5A8B7C10}"

typedef TW_UINT16 (FAR PASCAL *DSENTRYPROC)(pTW_IDENTITY, TW_UINT32, TW_UINT16, TW_UINT16, TW_MEMREF);

/* ---------------------------------------------------------------- config */

typedef struct {
    int  logEnable;
    int  mock;              /* synthesize a page instead of using the scanner */
    int  interlace;         /* -1 = probe, 0 = RGB dot, 1 = BGR dot, 2 = line */
    int  bufferMode;        /* scanner-side buffering (mode page 0x3a) */
    int  lineartInvert;     /* 1 = invert 1-bit rows (white-is-zero input) */
    char serial[64];        /* optional serial-number filter */
    char logPath[MAX_PATH];
} Config;

extern Config g_cfg;
extern HINSTANCE g_hinst;

void config_load(void);
void log_printf(const char *fmt, ...);
#define LOGF(...) do { if (g_cfg.logEnable) log_printf(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- page */

typedef struct {
    BYTE   *rows;       /* tightly packed, top-down, first row is the top */
    size_t  rowBytes;   /* TWAIN row: no padding */
    LONG    width, height;
    int     bpp;        /* 1, 8, 24 */
    TW_INT16 pixelType; /* TWPT_BW / TWPT_GRAY / TWPT_RGB */
    double  xres, yres;
    int     side;       /* 0 front, 1 back */
} Page;

void page_free(Page *p);
/* Build a TWAIN-ready page from scanner samples (still scanner order/inverted). */
BOOL page_from_raw(Page *p, const BYTE *raw, size_t rawLen, LONG w, LONG h,
                   int composition, int interlace, double xres, double yres, int side);
HGLOBAL page_to_hdib(const Page *p);
BOOL page_save(const Page *p, const char *path, TW_UINT16 format);

/* ---------------------------------------------------------------- transport */

typedef struct Transport Transport;

/* Opens the first present ScanSnap iX1300. ppath receives the device path. */
Transport *transport_open(char *ppath, size_t pathLen);
void transport_close(Transport *t);
int  transport_bulk_out(Transport *t, const BYTE *data, DWORD len, DWORD timeoutMs);
int  transport_bulk_in(Transport *t, BYTE *data, DWORD *len, DWORD timeoutMs);
const char *transport_kind(const Transport *t);

/* Opens a scripted/mock transport used by tools/selftest and g_cfg.mock. */
Transport *transport_open_mock(void);
Transport *transport_open_mock_synthetic(void);

/* ---------------------------------------------------------------- protocol */

enum {
    PROTO_OK = 0,
    PROTO_BUSY,         /* scanner reported busy (status byte 8) */
    PROTO_NODATA,       /* transient "no image data yet" sense */
    PROTO_NODOCS,       /* feeder empty */
    PROTO_COVER,        /* cover/ADF open */
    PROTO_JAM,
    PROTO_CANCEL,
    PROTO_ERROR,
    PROTO_IO
};

enum { COMP_LINEART = 0, COMP_HALFTONE = 1, COMP_GRAY = 2, COMP_COLOR = 5 };

typedef struct {
    int  resX, resY;
    int  composition;      /* COMP_* */
    int  duplex;
    int  feedFromBack;     /* scan the card/back window only */
    LONG ulxUnits;         /* 1/1200 inch, window origin */
    LONG ulyUnits;
    LONG widthUnits;       /* 1/1200 inch */
    LONG lengthUnits;
    LONG paperWidthUnits;
    LONG paperHeightUnits;
    LONG pixelsWide;       /* requested pixels per line */
    int  autoLength;       /* set ALD so short pages stop early */
    int  brightness;       /* -127..127 */
    int  contrast;         /* -127..127 */
    int  threshold;        /* 0..255, lineart */
} ScanJob;

typedef struct {
    char vendor[9], product[17], version[5];
    int  basicXRes, basicYRes, maxXRes, maxYRes;
    LONG maxWidthUnits, maxLengthUnits;     /* 1/1200 inch */
    int  hasAdf, hasFlatbed, hasColor, hasGray, hasLineart, hasDuplex;
    int  stdRes[16];
    int  stdResCount;
    int  colorOffset, duplexOffset;
    int  valid;
} ProtoInfo;

typedef struct {
    Transport *t;
    ProtoInfo  info;
    ScanJob    job;         /* the job currently armed on the scanner */
    int        armed;       /* set_window succeeded */
    int        batch;       /* a batch (SCAN issued) is open */
    int        interlace;   /* negotiated colour order */
} Proto;

Proto *proto_create(Transport *t);
void   proto_destroy(Proto *p);

int  proto_open(Proto *p);                    /* TEST UNIT READY + INQUIRY + VPD */
int  proto_begin(Proto *p, const ScanJob *job);   /* mode selects + set window + lamp */
int  proto_feed(Proto *p);                    /* hopper check + OBJECT POSITION feed + TUR */
int  proto_start_scan(Proto *p);              /* SCAN */
int  proto_pixel_size(Proto *p, int back, LONG *w, LONG *h);
int  proto_wait_image(Proto *p, int back, LONG bytes, int attempts);  /* f1/0x10 poll */
int  proto_read_image(Proto *p, int back, BYTE *buf, DWORD want, DWORD *got);
int  proto_end(Proto *p);                     /* cancel + buffering off */
int  proto_hw_status(Proto *p, BYTE out[12], DWORD *got);
int  proto_hopper_has_paper(Proto *p);
int  proto_test_unit_ready(Proto *p);          /* one plain TUR round trip */

/* Scripted transport for tools/selftest.c and g_cfg.mock. */
void  mock_push(const BYTE *data, DWORD len);
void  mock_push_status(BYTE statusByte);
void  mock_reset(void);
DWORD mock_pending(void);

/* ---------------------------------------------------------------- session */

typedef struct Session Session;

Session *session_create(void);
void     session_destroy(Session *s);
int      session_open(Session *s);
int      session_present(Session *s);
const ProtoInfo *session_info(Session *s);
void     session_set_job(Session *s, const ScanJob *job);
int      session_next_page(Session *s, Page *page, int *isBack);
void     session_cancel(Session *s);
void     session_reset(Session *s);
int      session_hopper_has_paper(Session *s);

enum { SESSION_OK = 0 };

#endif /* SSDRV_H */
