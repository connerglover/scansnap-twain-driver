/*
 * TWAIN 2.x data source for the ScanSnap iX1300.
 *
 * Registered as a plain (non-DF_DS2) source so both the 2.x DSM and the
 * legacy twain_32.dll DSM can hook it. Memory transfer is the primary path
 * (that is what NAPS2 uses); native DIB transfer is also implemented because
 * it costs almost nothing.
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HINSTANCE g_hinst;
Config    g_cfg;

/* --------------------------------------------------------------- state */

typedef struct {
    int      dsOpen;         /* state >= 4 */
    int      enabled;        /* state 5..7 */
    int      transferring;   /* state 7 */
    HWND     parent;
    int      showUI;
    int      modal;

    /* settings, in TWAIN units */
    TW_UINT16 units;
    TW_FIX32  xres, yres;
    TW_FIX32  left, top, right, bottom;   /* inches */
    TW_INT16  pixelType;
    TW_INT16  bitDepth;
    TW_BOOL   feederEnabled;
    TW_BOOL   duplexEnabled;
    TW_BOOL   autoFeed;
    TW_UINT16 xferMech;
    int       brightness;    /* -1000..1000, as TWAIN convention */
    int       contrast;
    TW_UINT16 threshold;

    /* session */
    Session  *session;
    Page      page;
    int       hasPage;
    int       batchDone;
    int       lastError;      /* TWCC_* for DAT_STATUS */

    HGLOBAL   nativeHandle;
    char      fileName[MAX_PATH];

    DWORD     transferOffset;
    LONG      rowsDone;
} DS;

static DS g_ds;

static TW_IDENTITY g_identity;

static void identity_init(void)
{
    memset(&g_identity, 0, sizeof g_identity);
    g_identity.Id = (TW_UINT32)(ULONG_PTR)g_hinst;
    g_identity.Version.MajorNum = SSDRV_VERSION_MAJOR;
    g_identity.Version.MinorNum = SSDRV_VERSION_MINOR;
    g_identity.Version.Language = TWLG_ENGLISH_USA;
    g_identity.Version.Country = TWCY_USA;
    strcpy_s(g_identity.Version.Info, sizeof g_identity.Version.Info, SSDRV_VERSION_STR);
    g_identity.ProtocolMajor = 2;
    g_identity.ProtocolMinor = 5;
    g_identity.SupportedGroups = DG_CONTROL | DG_IMAGE;
    strcpy_s(g_identity.Manufacturer, sizeof g_identity.Manufacturer, "Conner");
    strcpy_s(g_identity.ProductFamily, sizeof g_identity.ProductFamily, "ScanSnap iX1300 (standalone)");
    strcpy_s(g_identity.ProductName, sizeof g_identity.ProductName, "ScanSnap iX1300");
}

/* --------------------------------------------------------------- helpers */

static TW_UINT16 twcc(int protoRc)
{
    switch (protoRc) {
    case PROTO_OK:    return TWCC_SUCCESS;
    case PROTO_BUSY:  return TWCC_CHECKDEVICEONLINE;
    case PROTO_NODOCS:return TWCC_NOMEDIA;
    case PROTO_COVER: return TWCC_INTERLOCK;
    case PROTO_JAM:   return TWCC_PAPERJAM;
    case PROTO_CANCEL:return TWCC_SUCCESS;
    case PROTO_NODATA:return TWCC_CHECKDEVICEONLINE;
    case PROTO_IO:    return TWCC_OPERATIONERROR;
    default:          return TWCC_OPERATIONERROR;
    }
}

static TW_FIX32 f32(double v)
{
    TW_FIX32 f;
    f.Whole = (TW_INT16)v;
    f.Frac = (TW_UINT16)((v - (int)v) * 65536.0 + 0.5);
    if (f.Frac >= 65535) f.Frac = 0;
    return f;
}

static double unf32(TW_FIX32 f)
{
    return (double)f.Whole + (double)f.Frac / 65536.0;
}

static double res_of(TW_FIX32 f)
{
    double r = unf32(f);
    return r < 10.0 ? 300.0 : r;
}

static int bpp_for(const DS *d)
{
    if (d->pixelType == TWPT_RGB) return 24;
    if (d->pixelType == TWPT_BW) return 1;
    return 8;
}

static int composition_for(const DS *d)
{
    if (d->pixelType == TWPT_RGB) return COMP_COLOR;
    if (d->pixelType == TWPT_BW) return COMP_LINEART;
    return COMP_GRAY;
}

static void postToParent(UINT msg)
{
    if (g_ds.parent) PostMessageA(g_ds.parent, msg, 0, 0);
}

/* ------------------------------------------------------------- capability */

typedef struct {
    TW_UINT16 id;
    TW_UINT16 type;
    TW_UINT32 query;
} CapDef;

static const CapDef kCaps[] = {
    { CAP_XFERCOUNT,        TWTY_INT16,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { CAP_FEEDERENABLED,    TWTY_BOOL,   TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { CAP_FEEDERLOADED,     TWTY_BOOL,   TWQC_GET | TWQC_GETCURRENT },
    { CAP_SUPPORTEDCAPS,    TWTY_UINT16, TWQC_GET },
    { CAP_INDICATORS,       TWTY_BOOL,   TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { CAP_DEVICEONLINE,     TWTY_BOOL,   TWQC_GET | TWQC_GETCURRENT },
    { CAP_DUPLEX,           TWTY_UINT16, TWQC_GET | TWQC_GETCURRENT | TWQC_GETDEFAULT },
    { CAP_DUPLEXENABLED,    TWTY_BOOL,   TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { CAP_AUTOMATICSENSEMEDIUM, TWTY_BOOL, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { CAP_SERIALNUMBER,     TWTY_STR128, TWQC_GET | TWQC_GETCURRENT },
    { ICAP_COMPRESSION,     TWTY_UINT16, TWQC_GET | TWQC_GETCURRENT | TWQC_GETDEFAULT },
    { ICAP_PIXELTYPE,       TWTY_UINT16, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_UNITS,           TWTY_UINT16, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_XFERMECH,        TWTY_UINT16, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_BITDEPTH,        TWTY_UINT16, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_BITORDER,        TWTY_UINT16, TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_PIXELFLAVOR,     TWTY_UINT16, TWQC_GET | TWQC_GETCURRENT | TWQC_GETDEFAULT },
    { ICAP_PLANARCHUNKY,    TWTY_UINT16, TWQC_GET | TWQC_GETCURRENT },
    { ICAP_PHYSICALWIDTH,   TWTY_FIX32,  TWQC_GET | TWQC_GETCURRENT },
    { ICAP_PHYSICALHEIGHT,  TWTY_FIX32,  TWQC_GET | TWQC_GETCURRENT },
    { ICAP_XRESOLUTION,     TWTY_FIX32,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_YRESOLUTION,     TWTY_FIX32,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_BRIGHTNESS,      TWTY_FIX32,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_CONTRAST,        TWTY_FIX32,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_THRESHOLD,       TWTY_FIX32,  TWQC_GET | TWQC_SET | TWQC_GETCURRENT | TWQC_GETDEFAULT | TWQC_RESET },
    { ICAP_ROTATION,        TWTY_FIX32,  TWQC_GET | TWQC_GETCURRENT | TWQC_GETDEFAULT },
};

static const CapDef *find_cap(TW_UINT16 id)
{
    size_t i;
    for (i = 0; i < sizeof kCaps / sizeof kCaps[0]; i++)
        if (kCaps[i].id == id) return &kCaps[i];
    return NULL;
}

static int gcd_int(int a, int b)
{
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

/* Ascending, de-duplicated list of usable resolutions. */
static int resolution_list(int *out, int max)
{
    static const int fallback[] = { 150, 200, 300, 400, 600, 1200 };
    const ProtoInfo *info = g_ds.session ? session_info(g_ds.session) : NULL;
    int tmp[32], n = 0, i, j;

    if (info && info->valid && info->stdResCount) {
        for (i = 0; i < info->stdResCount && n < (int)(sizeof tmp / sizeof tmp[0]); i++)
            if (info->stdRes[i] >= 50 && info->stdRes[i] <= 1200)
                tmp[n++] = info->stdRes[i];
    }
    if (!n) {
        for (i = 0; i < (int)(sizeof fallback / sizeof fallback[0]); i++)
            tmp[n++] = fallback[i];
    }
    /* insertion sort, then drop duplicates */
    for (i = 1; i < n; i++) {
        int v = tmp[i];
        for (j = i - 1; j >= 0 && tmp[j] > v; j--) tmp[j + 1] = tmp[j];
        tmp[j + 1] = v;
    }
    {
        int m = 0;
        for (i = 0; i < n && m < max; i++)
            if (m == 0 || tmp[i] != out[m - 1]) out[m++] = tmp[i];
        n = m;
    }
    return n;
}

/* A step that never skips past MaxValue (NTwain enumerates the range). */
static int resolution_step(const int *res, int n)
{
    int i, step = 0;
    for (i = 1; i < n; i++) step = gcd_int(step, res[i] - res[i - 1]);
    return step > 0 ? step : 1;
}

/* Physical page size in inches, from the scanner's VPD or the A4 default. */
static void physical_size(double *wIn, double *hIn)
{
    const ProtoInfo *info = g_ds.session ? session_info(g_ds.session) : NULL;
    double maxW = 8.5, maxH = 14.0;
    if (info && info->valid && info->maxWidthUnits > 0 && info->maxLengthUnits > 0) {
        maxW = info->maxWidthUnits / 1200.0;
        maxH = info->maxLengthUnits / 1200.0;
    }
    *wIn = maxW;
    *hIn = maxH;
}

static TW_UINT16 cell_set(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
    return TWRC_SUCCESS;
}

static TW_UINT16 cap_supported_container(TW_CAPABILITY *cap)
{
    size_t count = sizeof kCaps / sizeof kCaps[0];
    TW_UINT16 *ids;
    TW_ARRAY *arr;
    size_t i;

    if (!cap || !cap->hContainer) return TWRC_FAILURE;
    arr = (TW_ARRAY *)GlobalLock(cap->hContainer);
    if (!arr) return TWRC_FAILURE;
    ids = (TW_UINT16 *)((BYTE *)arr + sizeof(TW_ARRAY) - sizeof(TW_UINT16));
    arr->ItemType = TWTY_UINT16;
    arr->NumItems = (TW_UINT32)count;
    for (i = 0; i < count; i++) ids[i] = kCaps[i].id;
    GlobalUnlock(cap->hContainer);
    return TWRC_SUCCESS;
}

static TW_UINT16 cap_get(TW_CAPABILITY *cap)
{
    const CapDef *def;
    int res[16], n;
    TW_UINT16 u16 = 0;
    TW_BOOL b = 0;
    TW_FIX32 f;
    TW_ARRAY *arr;

    if (!cap) return TWRC_FAILURE;
    if (cap->Cap == CAP_SUPPORTEDCAPS) return cap_supported_container(cap);
    def = find_cap(cap->Cap);
    if (!def) return TWRC_FAILURE;

    /* allocate one item container (array with a single value) */
    {
        TW_UINT16 type = def->type;
        size_t itemSize;
        switch (type) {
        case TWTY_UINT16: itemSize = 2; break;
        case TWTY_INT16:  itemSize = 2; break;
        case TWTY_BOOL:   itemSize = 2; break;
        case TWTY_FIX32:  itemSize = 4; break;
        case TWTY_STR128: itemSize = 128; break;
        default:          itemSize = 4; break;
        }
        if (type == TWTY_STR128) {
            HGLOBAL h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ONEVALUE) + 128);
            TW_ONEVALUE *ov;
            if (!h) return TWRC_FAILURE;
            ov = (TW_ONEVALUE *)GlobalLock(h);
            ov->ItemType = type;
            memset((BYTE *)ov + sizeof(TW_ONEVALUE), 0, 128);
            cap->ConType = TWON_ONEVALUE;
            cap->hContainer = h;
            GlobalUnlock(h);
            goto fill;
        }
        if (type == TWTY_FIX32 || def->id == ICAP_XRESOLUTION || def->id == ICAP_YRESOLUTION) {
            HGLOBAL h;
            TW_ONEVALUE *ov;
            /* resolution is a range, everything else FIX32 is a single value */
            if (def->id == ICAP_XRESOLUTION || def->id == ICAP_YRESOLUTION) {
                n = resolution_list(res, 16);
                h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_RANGE));
                if (!h) return TWRC_FAILURE;
                {
                    TW_RANGE *r = (TW_RANGE *)GlobalLock(h);
                    TW_FIX32 lo, hi, st, dv, cv;
                    lo = f32(res[0]);
                    hi = f32(res[n - 1]);
                    st = f32((double)resolution_step(res, n));
                    dv = f32(res[n > 2 ? 2 : 0]);
                    cv = f32(res_of(def->id == ICAP_XRESOLUTION ? g_ds.xres : g_ds.yres));
                    r->ItemType = TWTY_FIX32;
                    memcpy(&r->MinValue, &lo, 4);
                    memcpy(&r->MaxValue, &hi, 4);
                    memcpy(&r->StepSize, &st, 4);
                    memcpy(&r->DefaultValue, &dv, 4);
                    memcpy(&r->CurrentValue, &cv, 4);
                    cap->ConType = TWON_RANGE;
                    cap->hContainer = h;
                    GlobalUnlock(h);
                    goto fill;
                }
            }
            h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ONEVALUE) + sizeof(TW_FIX32));
            if (!h) return TWRC_FAILURE;
            ov = (TW_ONEVALUE *)GlobalLock(h);
            ov->ItemType = TWTY_FIX32;
            cap->ConType = TWON_ONEVALUE;
            cap->hContainer = h;
            GlobalUnlock(h);
            goto fill;
        }
        (void)arr;
        {
            HGLOBAL h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ONEVALUE) + 4);
            TW_ONEVALUE *ov;
            if (!h) return TWRC_FAILURE;
            ov = (TW_ONEVALUE *)GlobalLock(h);
            ov->ItemType = type;
            cap->ConType = TWON_ONEVALUE;
            cap->hContainer = h;
            GlobalUnlock(h);
        }
    }

fill:
    switch (cap->Cap) {
    case CAP_XFERCOUNT:
        u16 = (TW_UINT16)0xFFFF;
        break;
    case CAP_FEEDERENABLED:
        u16 = (g_ds.feederEnabled = TRUE);
        break;
    case CAP_FEEDERLOADED:
        u16 = (TW_BOOL)(g_ds.session ? (session_hopper_has_paper(g_ds.session) ? TRUE : FALSE) : FALSE);
        break;
    case CAP_INDICATORS:
        b = TRUE; break;
    case CAP_DEVICEONLINE:
        b = (TW_BOOL)(g_ds.session ? TRUE : FALSE); break;
    case CAP_DUPLEX:
        u16 = TWDX_1PASSDUPLEX; break;
    case CAP_DUPLEXENABLED:
        u16 = g_ds.duplexEnabled; break;
    case CAP_AUTOMATICSENSEMEDIUM:
        u16 = g_ds.autoFeed; break;
    case ICAP_COMPRESSION:
        u16 = TWCP_NONE; break;
    case ICAP_PIXELTYPE:
        u16 = (TW_UINT16)g_ds.pixelType; break;
    case ICAP_UNITS:
        u16 = g_ds.units; break;
    case ICAP_XFERMECH:
        u16 = g_ds.xferMech; break;
    case ICAP_BITDEPTH:
        u16 = (TW_UINT16)g_ds.bitDepth; break;
    case ICAP_BITORDER:
        u16 = TWBO_LSBFIRST; break;
    case ICAP_PIXELFLAVOR:
        u16 = TWPF_CHOCOLATE; break;
    case ICAP_PLANARCHUNKY:
        u16 = TWPC_CHUNKY; break;
    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION:
        break;   /* handled above */
    case ICAP_BRIGHTNESS:
    case ICAP_CONTRAST:
        break;   /* FIX32, written below */
    case ICAP_THRESHOLD:
        u16 = g_ds.threshold; break;
    case ICAP_ROTATION:
        u16 = 0; break;
    case ICAP_PHYSICALWIDTH:
    case ICAP_PHYSICALHEIGHT:
        break;
    default:
        break;
    }

    /* write the value into the locked container */
    {
        HGLOBAL h = cap->hContainer;
        BYTE *p = (BYTE *)GlobalLock(h);
        if (!p) return TWRC_FAILURE;
        if (cap->ConType == TWON_ONEVALUE) {
            TW_ONEVALUE *ov = (TW_ONEVALUE *)p;
            switch (ov->ItemType) {
            case TWTY_UINT16:
            case TWTY_INT16:
            case TWTY_BOOL:
                if (cap->Cap == CAP_INDICATORS || cap->Cap == CAP_DEVICEONLINE)
                    ov->Item = b;
                else
                    ov->Item = u16;
                break;
            case TWTY_FIX32: {
                double d = 0;
                switch (cap->Cap) {
                case ICAP_BRIGHTNESS: d = (double)g_ds.brightness; break;
                case ICAP_CONTRAST:   d = (double)g_ds.contrast; break;
                case ICAP_THRESHOLD:  d = (double)g_ds.threshold; break;
                case ICAP_ROTATION:   d = 0.0; break;
                case ICAP_PHYSICALWIDTH: {
                    double w, hh; physical_size(&w, &hh);
                    d = g_ds.units == TWUN_INCHES ? w : w * 2.54;
                } break;
                case ICAP_PHYSICALHEIGHT: {
                    double w, hh; physical_size(&w, &hh);
                    d = g_ds.units == TWUN_INCHES ? hh : hh * 2.54;
                } break;
                default: d = 0.0; break;
                }
                f = f32(d);
                memcpy(&ov->Item, &f, sizeof f);
            } break;
            case TWTY_STR128:
                strcpy_s((char *)p + sizeof(TW_ONEVALUE), 128, "N/A");
                break;
            default:
                break;
            }
        }
        GlobalUnlock(h);
    }
    return TWRC_SUCCESS;
}

static TW_UINT16 cap_set(TW_CAPABILITY *cap)
{
    const CapDef *def;
    TW_ONEVALUE *ov;
    TW_UINT32 v = 0;
    double d = 0;

    if (!cap || !cap->hContainer) return TWRC_FAILURE;
    def = find_cap(cap->Cap);
    if (!def) return TWRC_FAILURE;
    if (!(def->query & TWQC_SET)) return TWRC_FAILURE;

    ov = (TW_ONEVALUE *)GlobalLock(cap->hContainer);
    if (!ov) return TWRC_FAILURE;
    if (ov->ItemType == TWTY_FIX32) {
        TW_FIX32 f;
        memcpy(&f, &ov->Item, sizeof f);
        d = unf32(f);
    } else {
        v = ov->Item;
    }
    GlobalUnlock(cap->hContainer);

    switch (cap->Cap) {
    case CAP_FEEDERENABLED:
        /* feeder-only device: refuse to turn it off */
        if (!v) return TWRC_FAILURE;
        g_ds.feederEnabled = TRUE;
        return TWRC_SUCCESS;
    case CAP_DUPLEXENABLED:
        g_ds.duplexEnabled = (TW_BOOL)(v ? TRUE : FALSE);
        return TWRC_SUCCESS;
    case CAP_AUTOMATICSENSEMEDIUM:
        g_ds.autoFeed = (TW_BOOL)(v ? TRUE : FALSE);
        return TWRC_SUCCESS;
    case CAP_INDICATORS:
        return TWRC_SUCCESS;
    case ICAP_PIXELTYPE:
        if (v != TWPT_BW && v != TWPT_GRAY && v != TWPT_RGB) return TWRC_FAILURE;
        g_ds.pixelType = (TW_INT16)v;
        g_ds.bitDepth = (TW_INT16)bpp_for(&g_ds);
        return TWRC_SUCCESS;
    case ICAP_BITDEPTH:
        if (v != 1 && v != 8 && v != 24) return TWRC_FAILURE;
        g_ds.bitDepth = (TW_INT16)v;
        if (v == 24) g_ds.pixelType = TWPT_RGB;
        else if (v == 1) g_ds.pixelType = TWPT_BW;
        else g_ds.pixelType = TWPT_GRAY;
        return TWRC_SUCCESS;
    case ICAP_UNITS:
        if (v > TWUN_CENTIMETERS) return TWRC_FAILURE;
        g_ds.units = (TW_UINT16)v;
        return TWRC_SUCCESS;
    case ICAP_XFERMECH:
        if (v != TWSX_NATIVE && v != TWSX_MEMORY && v != TWSX_FILE) return TWRC_FAILURE;
        g_ds.xferMech = (TW_UINT16)v;
        return TWRC_SUCCESS;
    case ICAP_BITORDER:
        return TWRC_SUCCESS;
    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION: {
        int res[16], n, best = 300, i;
        double want = d;
        n = resolution_list(res, 16);
        for (i = 0; i < n; i++)
            if (i == 0 || (want - res[i] < want - best && want - res[i] >= 0) || res[i] == want)
                best = res[i];
        if (want > res[n - 1]) best = res[n - 1];
        if (cap->Cap == ICAP_XRESOLUTION) g_ds.xres = f32(best);
        else g_ds.yres = f32(best);
        return TWRC_SUCCESS;
    }
    case ICAP_BRIGHTNESS: {
        int v2 = (int)(d + (d < 0 ? -0.5 : 0.5));
        if (v2 < -1000) v2 = -1000;
        if (v2 > 1000) v2 = 1000;
        g_ds.brightness = v2;
        return TWRC_SUCCESS;
    }
    case ICAP_CONTRAST: {
        int v2 = (int)(d + (d < 0 ? -0.5 : 0.5));
        if (v2 < -1000) v2 = -1000;
        if (v2 > 1000) v2 = 1000;
        g_ds.contrast = v2;
        return TWRC_SUCCESS;
    }
    case ICAP_THRESHOLD:
        g_ds.threshold = (TW_UINT16)(d < 0 ? 0 : (d > 255 ? 255 : d));
        return TWRC_SUCCESS;
    default:
        return TWRC_FAILURE;
    }
}

/* --------------------------------------------------------------- scanning */

static void reset_defaults(void)
{
    g_ds.units = TWUN_INCHES;
    g_ds.xres = f32(300);
    g_ds.yres = f32(300);
    g_ds.left = f32(0);
    g_ds.top = f32(0);
    {
        double w, h;
        physical_size(&w, &h);
        g_ds.right = f32(w);
        g_ds.bottom = f32(h);
    }
    g_ds.pixelType = TWPT_RGB;
    g_ds.bitDepth = 24;
    g_ds.feederEnabled = TRUE;
    g_ds.duplexEnabled = FALSE;
    g_ds.autoFeed = TRUE;
    g_ds.xferMech = TWSX_MEMORY;
    g_ds.brightness = 0;
    g_ds.contrast = 0;
    g_ds.threshold = 128;
}

static ScanJob current_job(void)
{
    ScanJob j;
    double wIn, hIn;
    double resX = res_of(g_ds.xres), resY = res_of(g_ds.yres);

    memset(&j, 0, sizeof j);
    j.resX = (int)resX;
    j.resY = (int)resY;
    j.composition = composition_for(&g_ds);
    j.duplex = g_ds.duplexEnabled ? 1 : 0;
    j.feedFromBack = 0;

    physical_size(&wIn, &hIn);
    if (unf32(g_ds.right) > unf32(g_ds.left)) wIn = unf32(g_ds.right) - unf32(g_ds.left);
    if (unf32(g_ds.bottom) > unf32(g_ds.top)) hIn = unf32(g_ds.bottom) - unf32(g_ds.top);

    j.pixelsWide = (LONG)(wIn * resX + 0.5);
    if (j.composition == COMP_LINEART) j.pixelsWide -= j.pixelsWide % 8;
    if (j.pixelsWide < 8) j.pixelsWide = 8;

    j.widthUnits = (LONG)(wIn * 1200.0 + 0.5);
    j.lengthUnits = (LONG)(hIn * 1200.0 + 0.5);
    j.paperWidthUnits = (LONG)(wIn * 1200.0 + 0.5);
    j.paperHeightUnits = (LONG)(hIn * 1200.0 + 0.5);
    j.autoLength = 1;

    /* window descriptor brightness/contrast: 128 is neutral, 0..255 range */
    j.brightness = g_ds.brightness * 127 / 1000;
    j.contrast = g_ds.contrast * 127 / 1000;
    j.threshold = (int)g_ds.threshold;
    return j;
}

static int ensure_session(void)
{
    if (!g_ds.session) {
        g_ds.session = session_create();
        if (!g_ds.session) {
            g_ds.lastError = TWCC_OPERATIONERROR;
            return 0;
        }
        session_open(g_ds.session);
    }
    return 1;
}

static TW_UINT16 start_batch(void)
{
    if (!ensure_session()) return TWRC_FAILURE;
    session_reset(g_ds.session);
    g_ds.hasPage = 0;
    g_ds.batchDone = 0;
    page_free(&g_ds.page);
    memset(&g_ds.page, 0, sizeof g_ds.page);
    {
        ScanJob first = current_job();
        session_set_job(g_ds.session, &first);
    }

    /* Pull the first page synchronously: it both warms the lamp and lets us
     * answer IMAGEINFO before the first transfer, which is where NAPS2 needs
     * real dimensions. Later pages are pulled on demand at ENDXFER. */
    {
        ScanJob j = current_job();
        Page page;
        int back = 0;
        int rc;
        memset(&page, 0, sizeof page);
        rc = session_next_page(g_ds.session, &page, &back);
        if (rc == PROTO_OK) {
            g_ds.page = page;
            g_ds.hasPage = 1;
        } else if (rc == PROTO_NODOCS) {
            g_ds.lastError = TWCC_NOMEDIA;
            return TWRC_FAILURE;
        } else {
            g_ds.lastError = twcc(rc);
            return TWRC_FAILURE;
        }
        (void)j;
    }
    return TWRC_SUCCESS;
}

static void next_page(void)
{
    Page page;
    int back = 0;
    int rc;

    if (g_ds.batchDone) return;
    memset(&page, 0, sizeof page);
    rc = session_next_page(g_ds.session, &page, &back);
    if (rc == PROTO_OK) {
        page_free(&g_ds.page);
        g_ds.page = page;
        g_ds.hasPage = 1;
        return;
    }
    if (rc == PROTO_NODOCS) {
        g_ds.batchDone = 1;
        return;
    }
    g_ds.batchDone = 1;
    g_ds.lastError = twcc(rc);
    LOGF("ds: page fetch failed %d\n", rc);
}

/* ---------------------------------------------------------- transfers */

static void free_native(void)
{
    if (g_ds.nativeHandle) {
        GlobalUnlock(g_ds.nativeHandle);
        GlobalFree(g_ds.nativeHandle);
        g_ds.nativeHandle = 0;
    }
}

static TW_UINT16 native_xfer(TW_MEMREF *out)
{
    HGLOBAL h;
    if (!g_ds.hasPage) return TWRC_FAILURE;
    free_native();
    h = page_to_hdib(&g_ds.page);
    if (!h) return TWRC_FAILURE;
    /* Ownership passes to the DSM/application, which frees it. */
    *out = (TW_MEMREF)h;
    return TWRC_XFERDONE;
}

static TW_UINT16 memory_xfer(pTW_IMAGEMEMXFER x)
{
    const Page *p = &g_ds.page;
    BYTE *dst;
    DWORD want;
    DWORD remaining;

    if (!g_ds.hasPage || !x) return TWRC_FAILURE;
    if (!x->Memory.TheMem) return TWRC_FAILURE;

    dst = (BYTE *)GlobalLock((HGLOBAL)x->Memory.TheMem);
    if (!dst) return TWRC_FAILURE;

    x->Compression = TWCP_NONE;
    x->BytesPerRow = (TW_UINT32)p->rowBytes;
    x->Columns = (TW_UINT32)p->width;
    x->XOffset = 0;

    if (g_ds.rowsDone == 0) g_ds.transferOffset = 0;

    remaining = (DWORD)((size_t)p->rowBytes * (size_t)p->height) - g_ds.transferOffset;
    want = x->Memory.Length;
    if (want > remaining) want = remaining;
    if (want > x->Memory.Length) want = x->Memory.Length;

    memcpy(dst, p->rows + g_ds.transferOffset, want);
    GlobalUnlock((HGLOBAL)x->Memory.TheMem);

    x->BytesWritten = want;
    x->Rows = (TW_UINT32)(want / p->rowBytes);
    x->YOffset = (TW_UINT32)(g_ds.transferOffset / p->rowBytes);
    g_ds.transferOffset += want;
    g_ds.rowsDone += (LONG)x->Rows;

    if (g_ds.transferOffset >= (DWORD)((size_t)p->rowBytes * (size_t)p->height)) {
        return TWRC_XFERDONE;
    }
    return TWRC_SUCCESS;
}

static TW_UINT16 file_xfer(void)
{
    if (!g_ds.hasPage) return TWRC_FAILURE;
    if (!g_ds.fileName[0]) return TWRC_FAILURE;
    if (!page_save(&g_ds.page, g_ds.fileName, TWFF_BMP)) return TWRC_FAILURE;
    return TWRC_XFERDONE;
}

/* --------------------------------------------------------------- entries */

static TW_UINT16 ds_capability(TW_UINT16 msg, TW_CAPABILITY *cap)
{
    switch (msg) {
    case MSG_GET:
    case MSG_GETCURRENT:
    case MSG_GETDEFAULT:
    case MSG_RESET: {
        const CapDef *def;
        if (!cap) return TWRC_FAILURE;
        if (cap->Cap == CAP_SUPPORTEDCAPS && msg == MSG_GET) {
            if (!cap->hContainer) {
                HGLOBAL h = GlobalAlloc(GHND, sizeof(TW_ARRAY) + (sizeof kCaps / sizeof kCaps[0]) * sizeof(TW_UINT16));
                if (!h) return TWRC_FAILURE;
                cap->hContainer = h;
            }
            return cap_supported_container(cap);
        }
        def = find_cap(cap->Cap);
        if (!def) return TWRC_FAILURE;
        if (msg == MSG_GET && !(def->query & TWQC_GET)) return TWRC_FAILURE;
        if (msg == MSG_GETCURRENT && !(def->query & TWQC_GETCURRENT)) return TWRC_FAILURE;
        if (msg == MSG_GETDEFAULT && !(def->query & TWQC_GETDEFAULT)) return TWRC_FAILURE;
        if (msg == MSG_RESET) {
            TW_UINT16 savedX = (TW_UINT16)g_ds.xres.Whole, savedY = (TW_UINT16)g_ds.yres.Whole;
            reset_defaults();
            (void)savedX; (void)savedY;
        }
        if (!cap->hContainer) {
            HGLOBAL h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ARRAY) + 256);
            if (!h) return TWRC_FAILURE;
            cap->hContainer = h;
        }
        return cap_get(cap);
    }
    case MSG_SET:
        return cap_set(cap);
    case MSG_QUERYSUPPORT: {
        const CapDef *def;
        TW_ONEVALUE *ov;
        if (!cap) return TWRC_FAILURE;
        def = find_cap(cap->Cap);
        if (!def) return TWRC_FAILURE;
        if (cap->Cap == CAP_SUPPORTEDCAPS) {
            /* fall through with GET only */
            if (!cap->hContainer) {
                HGLOBAL h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ONEVALUE) + 4);
                if (!h) return TWRC_FAILURE;
                cap->hContainer = h;
            }
            ov = (TW_ONEVALUE *)GlobalLock(cap->hContainer);
            if (!ov) return TWRC_FAILURE;
            ov->ItemType = TWTY_UINT32;
            ov->Item = TWQC_GET;
            cap->ConType = TWON_ONEVALUE;
            GlobalUnlock(cap->hContainer);
            return TWRC_SUCCESS;
        }
        if (!cap->hContainer) {
            HGLOBAL h = GlobalAlloc(GHND | GMEM_ZEROINIT, sizeof(TW_ONEVALUE) + 4);
            if (!h) return TWRC_FAILURE;
            cap->hContainer = h;
        }
        ov = (TW_ONEVALUE *)GlobalLock(cap->hContainer);
        if (!ov) return TWRC_FAILURE;
        ov->ItemType = TWTY_UINT32;
        ov->Item = def->query;
        cap->ConType = TWON_ONEVALUE;
        GlobalUnlock(cap->hContainer);
        return TWRC_SUCCESS;
    }
    default:
        return TWRC_FAILURE;
    }
}

static TW_UINT16 ds_imageinfo(pTW_IMAGEINFO info)
{
    if (!info) return TWRC_FAILURE;
    if (!g_ds.hasPage) return TWRC_FAILURE;
    memset(info, 0, sizeof *info);
    info->XResolution = g_ds.xres;
    info->YResolution = g_ds.yres;
    info->ImageWidth = g_ds.page.width;
    info->ImageLength = g_ds.page.height;
    info->SamplesPerPixel = (TW_INT16)(g_ds.page.bpp == 24 ? 3 : 1);
    info->BitsPerSample[0] = (TW_INT16)g_ds.page.bpp;
    if (g_ds.page.bpp == 24) {
        info->BitsPerSample[1] = 8;
        info->BitsPerSample[2] = 8;
    }
    info->BitsPerPixel = (TW_INT16)g_ds.page.bpp;
    info->Planar = FALSE;
    info->PixelType = g_ds.page.pixelType;
    info->Compression = TWCP_NONE;
    return TWRC_SUCCESS;
}

static TW_UINT16 ds_event(pTW_EVENT evt)
{
    MSG *m;
    if (!evt) return TWRC_FAILURE;
    m = (MSG *)evt->pEvent;
    evt->TWMessage = MSG_NULL;
    if (!m) return TWRC_NOTDSEVENT;
    if (m->message == MSG_XFERREADY || m->message == MSG_CLOSEDSREQ ||
        m->message == MSG_CLOSEDSOK || m->message == MSG_DEVICEEVENT) {
        evt->TWMessage = (TW_UINT16)m->message;
        return TWRC_DSEVENT;
    }
    return TWRC_NOTDSEVENT;
}

static TW_UINT16 ds_enable(TW_USERINTERFACE *ui)
{
    if (!ui) return TWRC_FAILURE;
    g_ds.parent = (HWND)ui->hParent;
    g_ds.enabled = 1;
    g_ds.transferOffset = 0;
    g_ds.rowsDone = 0;
    g_ds.batchDone = 0;

    if (start_batch() != TWRC_SUCCESS) {
        g_ds.enabled = 0;
        postToParent(MSG_CLOSEDSREQ);
        return TWRC_FAILURE;
    }
    postToParent(MSG_XFERREADY);
    return TWRC_SUCCESS;
}

static TW_UINT16 ds_disable(void)
{
    if (g_ds.session) session_cancel(g_ds.session);
    g_ds.enabled = 0;
    g_ds.hasPage = 0;
    page_free(&g_ds.page);
    free_native();
    return TWRC_SUCCESS;
}

static TW_UINT16 ds_pending(TW_UINT16 msg, pTW_PENDINGXFERS pending)
{
    if (!pending) return TWRC_FAILURE;
    if (msg == MSG_RESET) {
        if (g_ds.session) session_cancel(g_ds.session);
        g_ds.hasPage = 0;
        g_ds.batchDone = 1;
        page_free(&g_ds.page);
        pending->Count = 0;
        return TWRC_SUCCESS;
    }
    if (msg == MSG_ENDXFER) {
        page_free(&g_ds.page);
        g_ds.hasPage = 0;
        g_ds.transferOffset = 0;
        g_ds.rowsDone = 0;
        if (!g_ds.batchDone) {
            next_page();
            if (g_ds.hasPage) {
                pending->Count = 1;
                postToParent(MSG_XFERREADY);
                return TWRC_SUCCESS;
            }
        }
        pending->Count = 0;
        if (!g_ds.batchDone) g_ds.batchDone = 1;
        return TWRC_SUCCESS;
    }
    if (msg == MSG_GET) {
        pending->Count = (TW_UINT16)(g_ds.hasPage ? 1 : 0);
        return TWRC_SUCCESS;
    }
    return TWRC_FAILURE;
}

static TW_UINT16 ds_setupmemxfer(pTW_SETUPMEMXFER x)
{
    if (!x) return TWRC_FAILURE;
    x->MinBufSize = 32 * 1024;
    x->MaxBufSize = 1024 * 1024;
    x->Preferred = 256 * 1024;
    return TWRC_SUCCESS;
}

/* ---------------------------------------------------------------- entry */

TW_UINT16 FAR PASCAL DS_Entry(pTW_IDENTITY origin, TW_UINT32 dg, TW_UINT16 dat,
                              TW_UINT16 msg, TW_MEMREF data)
{
    g_identity.Id = (TW_UINT32)(ULONG_PTR)g_hinst;
    if (g_cfg.logEnable)
        LOGF("ds: entry dg 0x%x dat 0x%x msg 0x%x data %p origin %p\n",
             dg, dat, msg, data, (void *)origin);

    switch (dg) {
    case DG_CONTROL:
        switch (dat) {
        case DAT_PARENT:
            if (msg == MSG_OPENDSM) { g_ds.parent = data ? *(HWND *)data : NULL; return TWRC_SUCCESS; }
            if (msg == MSG_CLOSEDSM) { g_ds.parent = NULL; return TWRC_SUCCESS; }
            return TWRC_FAILURE;
        case DAT_IDENTITY:
            if (msg == MSG_GET) {
                if (!data) { *(pTW_IDENTITY)NULL; return TWRC_SUCCESS; }
                *(pTW_IDENTITY)data = g_identity;
                return TWRC_SUCCESS;
            }
            if (msg == MSG_OPENDS) {
                g_ds.dsOpen = 1;
                LOGF("ds: OPENDS accepted\n");
                return TWRC_SUCCESS;
            }
            if (msg == MSG_CLOSEDS) {
                ds_disable();
                g_ds.dsOpen = 0;
                if (g_ds.session) { session_destroy(g_ds.session); g_ds.session = NULL; }
                return TWRC_SUCCESS;
            }
            if (msg == MSG_USERSELECT) return TWRC_SUCCESS;
            return TWRC_FAILURE;
        case DAT_STATUS: {
            TW_STATUS *st = (TW_STATUS *)data;
            if (msg != MSG_GET || !st) return TWRC_FAILURE;
            st->ConditionCode = (TW_UINT16)g_ds.lastError;
            return TWRC_SUCCESS;
        }
        case DAT_CAPABILITY:
            return ds_capability(msg, (TW_CAPABILITY *)data);
        case DAT_EVENT:
            if (msg == MSG_PROCESSEVENT) return ds_event((pTW_EVENT)data);
            return TWRC_FAILURE;
        case DAT_USERINTERFACE:
            if (msg == MSG_ENABLEDS) return ds_enable((TW_USERINTERFACE *)data);
            if (msg == MSG_DISABLEDS) return ds_disable();
            if (msg == MSG_ENABLEDSUIONLY) return ds_enable((TW_USERINTERFACE *)data);
            return TWRC_FAILURE;
        case DAT_PENDINGXFERS:
            return ds_pending(msg, (pTW_PENDINGXFERS)data);
        case DAT_SETUPMEMXFER:
            if (msg == MSG_GET) return ds_setupmemxfer((pTW_SETUPMEMXFER)data);
            return TWRC_FAILURE;
        case DAT_SETUPFILEXFER: {
            TW_SETUPFILEXFER *f = (TW_SETUPFILEXFER *)data;
            if (!f) return TWRC_FAILURE;
            if (msg == MSG_SET) {
                strcpy_s(g_ds.fileName, sizeof g_ds.fileName, f->FileName);
                return TWRC_SUCCESS;
            }
            if (msg == MSG_GET) {
                strcpy_s(f->FileName, sizeof f->FileName, g_ds.fileName);
                f->Format = TWFF_BMP;
                return TWRC_SUCCESS;
            }
            return TWRC_FAILURE;
        }
        case DAT_ENTRYPOINT:
            if (msg == MSG_GET && data) {
                TW_ENTRYPOINT *ep = (TW_ENTRYPOINT *)data;
                ep->Size = sizeof(TW_ENTRYPOINT);
                return TWRC_SUCCESS;
            }
            return TWRC_FAILURE;
        default:
            return TWRC_FAILURE;
        }
    case DG_IMAGE:
        switch (dat) {
        case DAT_IMAGEINFO:
            if (msg == MSG_GET) return ds_imageinfo((pTW_IMAGEINFO)data);
            return TWRC_FAILURE;
        case DAT_IMAGELAYOUT: {
            TW_IMAGELAYOUT *l;
            if (!data) return TWRC_FAILURE;
            l = (TW_IMAGELAYOUT *)data;
            if (msg == MSG_SET) {
                g_ds.left = l->Frame.Left;
                g_ds.top = l->Frame.Top;
                g_ds.right = l->Frame.Right;
                g_ds.bottom = l->Frame.Bottom;
                return TWRC_SUCCESS;
            }
            if (msg == MSG_GET || msg == MSG_GETDEFAULT || msg == MSG_RESET) {
                l->Frame.Left = g_ds.left;
                l->Frame.Top = g_ds.top;
                l->Frame.Right = g_ds.right;
                l->Frame.Bottom = g_ds.bottom;
                return TWRC_SUCCESS;
            }
            return TWRC_FAILURE;
        }
        case DAT_IMAGENATIVEXFER: {
            TW_UINT16 rc;
            if (msg != MSG_GET) return TWRC_FAILURE;
            rc = native_xfer((TW_MEMREF *)data);
            if (rc == TWRC_XFERDONE) {
                g_ds.hasPage = 0;
                page_free(&g_ds.page);
            }
            return rc;
        }
        case DAT_IMAGEMEMXFER:
            if (msg != MSG_GET) return TWRC_FAILURE;
            return memory_xfer((pTW_IMAGEMEMXFER)data);
        case DAT_IMAGEFILEXFER: {
            TW_UINT16 rc;
            if (msg != MSG_GET) return TWRC_FAILURE;
            rc = file_xfer();
            if (rc == TWRC_XFERDONE) {
                g_ds.hasPage = 0;
                page_free(&g_ds.page);
            }
            return rc;
        }
        default:
            return TWRC_FAILURE;
        }
    default:
        return TWRC_FAILURE;
    }
}

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = inst;
        DisableThreadLibraryCalls(inst);
        memset(&g_ds, 0, sizeof g_ds);
        config_load();
        identity_init();
        reset_defaults();
        LOGF("ds: attached (%s) mock=%d\n", SSDRV_VERSION_STR, g_cfg.mock);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_ds.session) {
            session_destroy(g_ds.session);
            g_ds.session = NULL;
        }
        page_free(&g_ds.page);
        free_native();
    }
    return TRUE;
}
