/*
 * USB transport: WinUSB, discovered through the device interface GUID that
 * install/iX1300-TWAIN.inf registers for the scanner.
 *
 * The scanner's single bulk pair is used for everything:
 *   OUT endpoint  - 31-byte command envelope, then optional data-out
 *   IN  endpoint  - optional data-in, then a 13-byte status envelope
 * which is the same shape usbscan.sys presents to PFU's own USD.
 */
#include "ssdrv.h"
#include <winioctl.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initguid.h>

/* Install-time interface class GUID (install/iX1300-TWAIN.inf). */
DEFINE_GUID(GUID_DEVINTERFACE_SCANSNAP_IX1300,
            0x6e7c3d2a, 0x5c41, 0x4a0e, 0x9c, 0x2b, 0x1d, 0x2f, 0x5a, 0x8b, 0x7c, 0x10);

struct Transport {
    HANDLE  file;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR   pipeIn, pipeOut;
    USHORT  maxPacket;
    char    path[MAX_PATH];
    int     isMock;
    int     mockSynthetic;
    int     viaUsbscan;      /* opened through usbscan.sys, not WinUSB */
};

/* ------------------------------------------------------------------ mock */

#define MOCK_MAX_CHUNKS 64
#define MOCK_CHUNK_MAX  300000

typedef struct {
    BYTE  data[MOCK_CHUNK_MAX];
    DWORD len, off;
} MockChunk;

static MockChunk g_mockQueue[MOCK_MAX_CHUNKS];
static int g_mockHead, g_mockTail, g_mockCount;

/* State of the synthetic device (transport_open_mock_synthetic). */
static int  g_mockSheet, g_mockImageLeft, g_mockLines;
static LONG g_mockPixelsWide;
static BYTE g_mockLastOp;
static BYTE g_mockDesired;   /* composition of the pending image */

static Transport *mock_open(int synthetic)
{
    Transport *t = (Transport *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->isMock = 1;
    t->mockSynthetic = synthetic;
    if (synthetic) {
        g_mockSheet = 0;
        g_mockImageLeft = 0;
        g_mockLastOp = 0;
        g_mockDesired = 5;
        g_mockPixelsWide = 2550;
        g_mockLines = 3300;
    }
    strcpy_s(t->path, sizeof t->path, synthetic ? "mock://ix1300/synthetic" : "mock://ix1300/scripted");
    return t;
}

const char *transport_kind(const Transport *t)
{
    if (!t) return "none";
    if (t->isMock) return "mock";
    return t->viaUsbscan ? "usbscan" : "winusb";
}

typedef struct {
    USHORT usVendorId;
    USHORT usProductId;
    USHORT usBcdDevice;
    USHORT usLanguageId;
} USBSCAN_DESCRIPTOR;

/* ------------------------------------------------------------- usbscan.sys
 *
 * Windows' in-box scanner class driver. It needs no PFU software and no
 * WinUSB INF: it presents the bulk pair through ReadFile/WriteFile. The
 * ScanSnap Home INF normally binds it; install/README.md also shows how to
 * bind it by hand with a stock Windows driver.
 */
#define FILE_DEVICE_USB_SCAN 0x8000
#define IOCTL_USBSCAN_GET_DEVICE_DESCRIPTOR \
    CTL_CODE(FILE_DEVICE_USB_SCAN, 0x0800 + 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBSCAN_CANCEL_IO \
    CTL_CODE(FILE_DEVICE_USB_SCAN, 0x0800 + 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBSCAN_RESET_PIPE \
    CTL_CODE(FILE_DEVICE_USB_SCAN, 0x0800 + 7, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* PIPE_TYPE, as usbscan.h defines it. */
enum { PIPE_EVENT = 0, PIPE_READ, PIPE_WRITE, PIPE_ALL };

/*
 * A scanner that a previous session left mid-batch (crash, kill, power event)
 * still has its data-out phase or its image stream open. Its IN pipe then
 * answers nothing while the OUT pipe keeps accepting bytes, so every command
 * times out. Abort whatever usbscan.sys still holds, discard the stale data-in,
 * then reset the pipes: the drain's CancelIoEx leaves the IN endpoint halted,
 * and only IOCTL_RESET_PIPE clears that.
 */
static void recover_usbscan(HANDLE f)
{
    int pipe, round;
    DWORD returned;

    for (pipe = PIPE_READ; pipe <= PIPE_WRITE; pipe++)
        DeviceIoControl(f, IOCTL_USBSCAN_CANCEL_IO, &pipe, sizeof pipe,
                        NULL, 0, &returned, NULL);

    for (round = 0; round < 64; round++) {
        OVERLAPPED ov;
        BYTE buf[4096];
        DWORD done = 0;

        memset(&ov, 0, sizeof ov);
        ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) break;
        if (!ReadFile(f, buf, sizeof buf, &done, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); break; }
            if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) {
                CancelIoEx(f, &ov);
                CloseHandle(ov.hEvent);
                break;
            }
            if (!GetOverlappedResult(f, &ov, &done, FALSE)) done = 0;
        }
        CloseHandle(ov.hEvent);
        if (!done) break;
        LOGF("usbscan: discarded %lu stale bytes\n", done);
    }

    for (pipe = PIPE_READ; pipe <= PIPE_WRITE; pipe++)
        DeviceIoControl(f, IOCTL_USBSCAN_RESET_PIPE, &pipe, sizeof pipe,
                        NULL, 0, &returned, NULL);
}


static Transport *open_usbscan(void)
{
    int index;
    for (index = 0; index < 8; index++) {
        char name[32];
        HANDLE f;
        USBSCAN_DESCRIPTOR desc;
        DWORD returned = 0;

        sprintf_s(name, sizeof name, "\\\\.\\Usbscan%d", index);
        f = CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;

        memset(&desc, 0, sizeof desc);
        if (!DeviceIoControl(f, IOCTL_USBSCAN_GET_DEVICE_DESCRIPTOR, NULL, 0,
                             &desc, sizeof desc, &returned, NULL) ||
            desc.usVendorId != SSDRV_USB_VID || desc.usProductId != SSDRV_USB_PID) {
            LOGF("usbscan: %s is not the iX1300 (%04x:%04x)\n",
                 name, desc.usVendorId, desc.usProductId);
            CloseHandle(f);
            continue;
        }
        /*
         * Deliberately no IOCTL_SET_TIMEOUT: on this device any non-zero
         * TimeoutRead makes usbscan.sys abort every transfer with
         * ERROR_OPERATION_ABORTED (995). Timeouts are enforced in user mode
         * with WaitForSingleObject + CancelIoEx instead.
         */
        recover_usbscan(f);

        {
            Transport *t = (Transport *)calloc(1, sizeof *t);
            if (!t) { CloseHandle(f); return NULL; }
            t->file = f;
            t->viaUsbscan = 1;
            t->pipeIn = 0x81;
            t->pipeOut = 0x02;
            strcpy_s(t->path, sizeof t->path, name);
            LOGF("usbscan: opened %s\n", name);
            return t;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ open */

typedef struct {
    HANDLE file;
    WINUSB_INTERFACE_HANDLE winusb;
    UCHAR in, out;
    USHORT maxPacket;
} Candidate;

static BOOL inspect_path(const char *path, Candidate *c)
{
    WINUSB_INTERFACE_HANDLE wu = NULL;
    USB_INTERFACE_DESCRIPTOR desc;
    BOOL ok = FALSE;
    HANDLE f;

    memset(c, 0, sizeof *c);
    f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LOGF("winusb: CreateFile(%s) failed %lu\n", path, GetLastError());
        return FALSE;
    }
    if (!WinUsb_Initialize(f, &wu)) {
        LOGF("winusb: WinUsb_Initialize(%s) failed %lu\n", path, GetLastError());
        CloseHandle(f);
        return FALSE;
    }
    if (!WinUsb_QueryInterfaceSettings(wu, 0, &desc)) {
        LOGF("winusb: QueryInterfaceSettings failed %lu\n", GetLastError());
        goto done;
    }
    {
        UCHAR i;
        for (i = 0; i < desc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pi;
            if (!WinUsb_QueryPipe(wu, 0, i, &pi)) continue;
            if (pi.PipeType != UsbdPipeTypeBulk) continue;
            if (pi.PipeId & 0x80) {
                if (!c->in) { c->in = pi.PipeId; c->maxPacket = pi.MaximumPacketSize; }
            } else if (!c->out) {
                c->out = pi.PipeId;
            }
            LOGF("winusb: bulk pipe 0x%02x maxpacket %u\n", pi.PipeId, pi.MaximumPacketSize);
        }
    }
    if (!c->in || !c->out) {
        LOGF("winusb: %s has no bulk pair (in 0x%02x out 0x%02x)\n", path, c->in, c->out);
        goto done;
    }
    /* generous timeouts; every call passes its own. */
    {
        ULONG timeout = 30000;
        WinUsb_SetPipePolicy(wu, c->in, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
        WinUsb_SetPipePolicy(wu, c->out, PIPE_TRANSFER_TIMEOUT, sizeof timeout, &timeout);
        WinUsb_SetPipePolicy(wu, c->in, AUTO_CLEAR_STALL, sizeof(ULONG), &(ULONG){1});
        WinUsb_SetPipePolicy(wu, c->out, AUTO_CLEAR_STALL, sizeof(ULONG), &(ULONG){1});
    }
    c->file = f;
    c->winusb = wu;
    ok = TRUE;
done:
    if (!ok) {
        WinUsb_Free(wu);
        CloseHandle(f);
    }
    return ok;
}

Transport *transport_open(char *ppath, size_t pathLen)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD index = 0;
    Transport *t = NULL;

    if (ppath && pathLen) ppath[0] = 0;
    set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_SCANSNAP_IX1300, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        LOGF("winusb: SetupDiGetClassDevs failed %lu\n", GetLastError());
        return NULL;
    }
    ifd.cbSize = sizeof ifd;
    while (SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_SCANSNAP_IX1300, index++, &ifd)) {
        DWORD need = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail;
        Candidate cand;

        SetupDiGetDeviceInterfaceDetailA(set, &ifd, NULL, 0, &need, NULL);
        if (!need) continue;
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(need);
        if (!detail) break;
        detail->cbSize = sizeof *detail;
        if (SetupDiGetDeviceInterfaceDetailA(set, &ifd, detail, need, NULL, NULL) &&
            inspect_path(detail->DevicePath, &cand)) {
            t = (Transport *)calloc(1, sizeof *t);
            if (t) {
                t->file = cand.file;
                t->winusb = cand.winusb;
                t->pipeIn = cand.in;
                t->pipeOut = cand.out;
                t->maxPacket = cand.maxPacket;
                strcpy_s(t->path, sizeof t->path, detail->DevicePath);
                if (ppath && pathLen) strncpy_s(ppath, pathLen, detail->DevicePath, _TRUNCATE);
                LOGF("winusb: opened %s\n", t->path);
            } else {
                WinUsb_Free(cand.winusb);
                CloseHandle(cand.file);
            }
            free(detail);
            break;
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(set);

    if (!t) {
        t = open_usbscan();
        if (t && ppath && pathLen) strncpy_s(ppath, pathLen, t->path, _TRUNCATE);
    }
    if (!t && ppath && pathLen) ppath[0] = 0;
    return t;
}

Transport *transport_open_mock(void)
{
    return mock_open(0);            /* scripted: the caller pushes responses */
}

Transport *transport_open_mock_synthetic(void)
{
    return mock_open(1);            /* self-answering: fake iX1300 */
}

void transport_close(Transport *t)
{
    if (!t) return;
    if (!t->isMock) {
        if (t->winusb) WinUsb_Free(t->winusb);
        if (t->file && t->file != INVALID_HANDLE_VALUE) CloseHandle(t->file);
    }
    free(t);
}

/*
 * A cancelled transfer leaves the endpoint halted, so every later transfer
 * fails instantly with ERROR_GEN_FAILURE (31) until the pipe is reset. Both
 * transports need this after a timeout or a hard error; one dead transfer must
 * not poison the rest of the session.
 */
static void reset_pipes(Transport *t)
{
    if (t->viaUsbscan) {
        int pipe;
        DWORD returned;
        for (pipe = PIPE_READ; pipe <= PIPE_WRITE; pipe++)
            DeviceIoControl(t->file, IOCTL_USBSCAN_RESET_PIPE, &pipe, sizeof pipe,
                            NULL, 0, &returned, NULL);
    } else if (t->winusb) {
        WinUsb_ResetPipe(t->winusb, t->pipeIn);
        WinUsb_ResetPipe(t->winusb, t->pipeOut);
    }
}

static int sync_io(Transport *t, BOOL write, BYTE *data, DWORD *len, DWORD timeoutMs)
{
    OVERLAPPED ov;
    BOOL ok;
    DWORD done = 0;

    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    if (t->viaUsbscan) {
        ok = write ? WriteFile(t->file, data, *len, &done, &ov)
                   : ReadFile(t->file, data, *len, &done, &ov);
    } else if (write) {
        ULONG to = timeoutMs;
        WinUsb_SetPipePolicy(t->winusb, t->pipeOut, PIPE_TRANSFER_TIMEOUT, sizeof to, &to);
        ok = WinUsb_WritePipe(t->winusb, t->pipeOut, data, *len, &done, &ov);
    } else {
        ULONG to = timeoutMs;
        WinUsb_SetPipePolicy(t->winusb, t->pipeIn, PIPE_TRANSFER_TIMEOUT, sizeof to, &to);
        ok = WinUsb_ReadPipe(t->winusb, t->pipeIn, data, *len, &done, &ov);
    }
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(t->file, &ov, &done, FALSE);
            } else {
                /* a transfer left pending blocks every later one */
                CancelIoEx(t->file, &ov);
                WaitForSingleObject(ov.hEvent, 1000);
                LOGF("%s: %s timed out after %lu ms\n",
                     t->viaUsbscan ? "usbscan" : "winusb",
                     write ? "write" : "read", timeoutMs);
                CloseHandle(ov.hEvent);
                reset_pipes(t);
                return 0;
            }
        } else if (err == ERROR_GEN_FAILURE) {
            /* endpoint left halted by an earlier cancel */
            reset_pipes(t);
            ok = write ? WriteFile(t->file, data, *len, &done, &ov)
                       : ReadFile(t->file, data, *len, &done, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0)
                    ok = GetOverlappedResult(t->file, &ov, &done, FALSE);
                else {
                    CancelIoEx(t->file, &ov);
                    WaitForSingleObject(ov.hEvent, 1000);
                    CloseHandle(ov.hEvent);
                    return 0;
                }
            }
        }
    }
    if (!ok) {
        LOGF("%s: %s failed err %lu (done %lu)\n",
             t->viaUsbscan ? "usbscan" : "winusb",
             write ? "write" : "read", GetLastError(), done);
        CloseHandle(ov.hEvent);
        reset_pipes(t);
        return 0;
    }
    CloseHandle(ov.hEvent);
    *len = done;
    return 1;
}

/*
 * Mock transport: a FIFO of IN responses. tools/selftest.c scripts a whole
 * command stream into it so the protocol state machine is exercised without
 * hardware. Each IN read consumes from the front of the queue.
 */
void mock_push(const BYTE *data, DWORD len)
{
    MockChunk *c;
    if (g_mockCount == MOCK_MAX_CHUNKS) {
        LOGF("mock: queue full, dropping %lu bytes\n", len);
        return;
    }
    c = &g_mockQueue[g_mockTail];
    g_mockTail = (g_mockTail + 1) % MOCK_MAX_CHUNKS;
    g_mockCount++;
    c->len = len > sizeof c->data ? (DWORD)sizeof c->data : len;
    memcpy(c->data, data, c->len);
    c->off = 0;
}

void mock_push_status(BYTE statusByte)
{
    BYTE st[13];
    memset(st, 0, sizeof st);
    st[0] = 0x53;
    st[9] = statusByte;
    mock_push(st, sizeof st);
}

void mock_reset(void)
{
    g_mockHead = g_mockTail = g_mockCount = 0;
}

DWORD mock_pending(void)
{
    return g_mockCount;
}

static DWORD mock_read(BYTE *out, DWORD want)
{
    MockChunk *c;
    DWORD n;
    if (!g_mockCount) {
        LOGF("mock: read of %lu bytes with an empty queue\n", want);
        return 0;
    }
    c = &g_mockQueue[g_mockHead];
    n = c->len - c->off;
    if (n > want) n = want;
    memcpy(out, c->data + c->off, n);
    c->off += n;
    if (c->off == c->len) {
        g_mockHead = (g_mockHead + 1) % MOCK_MAX_CHUNKS;
        g_mockCount--;
    }
    return n;
}

/*
 * Synthetic device: answers each command envelope the way the iX1300 does, so
 * the data source can be driven end-to-end with no hardware attached
 * (Mock = 1 in ScanSnapTWAIN.ini). Two sheets are simulated.
 */

static void mock_queue_status(BYTE statusByte)
{
    mock_push_status(statusByte);
}

static void mock_reply_inquiry(void)
{
    BYTE buf[96];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x06;
    memcpy(buf + 8, "FUJITSU ", 8);
    memcpy(buf + 16, "ScanSnap iX1300 ", 16);
    memcpy(buf + 32, "0M00", 4);         /* what the real unit reports */
    buf[0x2D] = 0x01;
    buf[0x2B] = 0x00; buf[0x2A] = 0x00;
    buf[0x2E] = 0x00; buf[0x2F] = 0x00;  /* duplex offset reads 0 on the unit */
    mock_push(buf, sizeof buf);
    mock_queue_status(0);
}

static void mock_reply_vpd(void)
{
    BYTE buf[204];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x06;
    buf[0x05] = 0x04; buf[0x06] = 0xB0;
    buf[0x07] = 0x04; buf[0x08] = 0xB0;
    buf[0x0A] = 0x02; buf[0x0B] = 0x58;
    buf[0x0C] = 0x02; buf[0x0D] = 0x58;
    buf[0x14] = 0x00; buf[0x15] = 0x00; buf[0x16] = 0x27; buf[0x17] = 0xD8;
    buf[0x18] = 0x00; buf[0x19] = 0x00; buf[0x1A] = 0x41; buf[0x1B] = 0xA0;
    buf[0x1C] = 0x8A;
    buf[0x12] = 0x0A;       /* 100, 120, 150, 160, 180, 200 */
    buf[0x13] = 0xC6;       /* 240, 300, 600 */
    mock_push(buf, sizeof buf);
    mock_queue_status(0);
}

static void mock_reply_pixel_size(int back)
{
    BYTE sz[0x20];
    memset(sz, 0, sizeof sz);
    sz[0] = (BYTE)(g_mockPixelsWide >> 24);
    sz[1] = (BYTE)(g_mockPixelsWide >> 16);
    sz[2] = (BYTE)(g_mockPixelsWide >> 8);
    sz[3] = (BYTE)g_mockPixelsWide;
    sz[4] = (BYTE)(g_mockLines >> 24);
    sz[5] = (BYTE)(g_mockLines >> 16);
    sz[6] = (BYTE)(g_mockLines >> 8);
    sz[7] = (BYTE)g_mockLines;
    mock_push(sz, sizeof sz);
    mock_queue_status(0);
    (void)back;
}

/* Deterministic test pattern: a dark wedge on a light background plus a
 * marker stripe, so orientation and polarity are visible in the BMP. */
static DWORD mock_image_chunk(BYTE *out, DWORD want)
{
    DWORD bpl;
    DWORD line, off, i;
    if (g_mockImageLeft <= 0) return 0;
    if ((DWORD)g_mockImageLeft < want) want = (DWORD)g_mockImageLeft;
    bpl = g_mockDesired == 5 ? (DWORD)g_mockPixelsWide * 3
        : g_mockDesired == 2 ? (DWORD)g_mockPixelsWide
        : (DWORD)((g_mockPixelsWide + 7) / 8);
    for (i = 0; i < want; i++) {
        DWORD abs = (DWORD)((size_t)g_mockLines * bpl - (size_t)g_mockImageLeft + i);
        line = abs / bpl;
        off = abs % bpl;
        /* The scanner sends inverted samples: paper white is 0x00 on the wire,
         * ink is 0xFF. 1-bit line art uses 1 = ink. */
        if (line < 8) out[i] = 0xFF;                      /* ink stripe at top */
        else if (off < bpl / 3) out[i] = 0x00;            /* white paper */
        else out[i] = (BYTE)(0x10 + (line * 7 + off * 3) % 0x60);
    }
    g_mockImageLeft -= (int)want;
    return want;
}

static void mock_reply_read(int windowBack, DWORD want)
{
    BYTE *buf = (BYTE *)malloc(want ? want : 1);
    DWORD n;
    if (!buf) { mock_queue_status(8); return; }
    n = mock_image_chunk(buf, want);
    if (n) mock_push(buf, n);
    free(buf);
    mock_queue_status(0);
    (void)windowBack;
}

static void mock_handle_command(const BYTE *env)
{
    const BYTE *cdb = env + 0x13;
    switch (cdb[0]) {
    case 0x00:    /* TEST UNIT READY */
        mock_queue_status(0);
        break;
    case 0x03: {  /* REQUEST SENSE */
        BYTE sense[0x12];
        memset(sense, 0, sizeof sense);
        sense[0] = 0x70;
        mock_push(sense, sizeof sense);
        mock_queue_status(0);
    } break;
    case 0x12:
        if (cdb[1] & 0x01) mock_reply_vpd();
        else mock_reply_inquiry();
        break;
    case 0x24:    /* SET WINDOW - data phase carries the descriptor */
        g_mockLastOp = 0x24;
        mock_queue_status(0);
        break;
    case 0x15:    /* MODE SELECT */
    case 0x1B:    /* SCAN */
    case 0xF1:
        if ((cdb[1] & 0x0F) == 0x04) {      /* cancel closes the batch */
            g_mockSheet = 0;
            g_mockImageLeft = 0;
        }
        mock_queue_status(0);
        break;
    case 0x31: {  /* OBJECT POSITION */
        if (cdb[1] == 0x01) {
            if (g_mockSheet >= 2) {
                BYTE sense[0x12];
                mock_queue_status(1);
                memset(sense, 0, sizeof sense);
                sense[0] = 0x70;
                sense[2] = 0x03;
                sense[12] = 0x80; sense[13] = 0x03;   /* no documents */
                mock_push(sense, sizeof sense);
                mock_queue_status(0);
                break;
            }
            g_mockSheet++;
            g_mockImageLeft = (int)((size_t)g_mockLines *
                                    (g_mockDesired == 5 ? (size_t)g_mockPixelsWide * 3
                                     : g_mockDesired == 2 ? (size_t)g_mockPixelsWide
                                     : (size_t)((g_mockPixelsWide + 7) / 8)));
        }
        mock_queue_status(0);
    } break;
    case 0xC2: {  /* GET HW STATUS */
        BYTE hw[12];
        memset(hw, 0, sizeof hw);
        if (g_mockSheet >= 2) hw[3] |= 0x80;   /* hopper empty */
        mock_push(hw, sizeof hw);
        mock_queue_status(0);
    } break;
    case 0x28:
        if (cdb[2] == 0x80) mock_reply_pixel_size(cdb[5] & 0x80);
        else mock_reply_read(cdb[5] & 0x80,
                             ((DWORD)cdb[6] << 16) | ((DWORD)cdb[7] << 8) | cdb[8]);
        break;
    default:
        mock_queue_status(0);
        break;
    }
}

int transport_bulk_out(Transport *t, const BYTE *data, DWORD len, DWORD timeoutMs)
{
    DWORD n = len;
    if (t->isMock) {
        if (!t->mockSynthetic) return 1;
        if (len == 31 && data[0] == 0x43) {
            mock_handle_command(data);
        } else if (len >= 8 && g_mockLastOp == 0x24) {
            /* window block: descriptor starts at offset 8 */
            LONG resX = (LONG)(((DWORD)data[8 + 0x02] << 8) | data[8 + 0x03]);
            LONG widthUnits = (LONG)(((DWORD)data[8 + 0x0E] << 24) | ((DWORD)data[8 + 0x0F] << 16) |
                                     ((DWORD)data[8 + 0x10] << 8) | data[8 + 0x11]);
            LONG lenUnits = (LONG)(((DWORD)data[8 + 0x12] << 24) | ((DWORD)data[8 + 0x13] << 16) |
                                   ((DWORD)data[8 + 0x14] << 8) | data[8 + 0x15]);
            if (resX < 1) resX = 300;
            g_mockDesired = data[8 + 0x19];
            g_mockPixelsWide = widthUnits * resX / 1200;
            g_mockLines = lenUnits * resX / 1200;
            if (g_mockPixelsWide < 8) g_mockPixelsWide = 8;
            if (g_mockLines < 8) g_mockLines = 8;
            g_mockLastOp = 0;
        }
        return 1;
    }
    return sync_io(t, TRUE, (BYTE *)data, &n, timeoutMs);
}

int transport_bulk_in(Transport *t, BYTE *data, DWORD *len, DWORD timeoutMs)
{
    DWORD want = *len;
    if (t->isMock) {
        *len = mock_read(data, want);
        return 1;
    }
    return sync_io(t, FALSE, data, len, timeoutMs);
}
