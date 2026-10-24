/*
 * dsmtest - load the DS through a real TWAIN DSM, the way NAPS2 does.
 *
 *   dsmtest <twaindsm.dll> [ds-path]
 *
 * Uses DSM_Entry: OPENDSM, IDENTITY GETFIRST/GETNEXT/OPENDS, the capability
 * writes NAPS2 performs, ENABLEDS, the XFERREADY/memory-transfer loop,
 * DISABLEDS, CLOSEDS, CLOSEDSM. Verifies that a DSM (not just our own loader)
 * accepts the data source and that discovery finds it.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "twain.h"

/* TWAIN's DSM_Entry takes BOTH identities: origin and destination. */
typedef TW_UINT16 (TW_CALLINGSTYLE *DSMENTRYPROC_LOCAL)(pTW_IDENTITY, pTW_IDENTITY,
                                                         TW_UINT32, TW_UINT16,
                                                         TW_UINT16, TW_MEMREF);

static DSMENTRYPROC_LOCAL g_dsm;
static TW_IDENTITY g_app;
static TW_IDENTITY g_src;   /* the source the DSM opened */
static int g_srcOpen;
static int failures;

#define CHECK(cond, ...) do { \
    if (cond) printf("  ok   "); else { printf("  FAIL "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); \
} while (0)

/*
 * Once a source is open the DSM routes every triplet to it, keyed by the
 * destination identity. Passing a NULL destination makes the DSM answer the
 * call itself, which is why the destination must be the opened source.
 */
static TW_UINT16 dsm(TW_UINT32 dg, TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF p)
{
    return g_dsm(&g_app, g_srcOpen ? &g_src : NULL, dg, dat, msg, p);
}

static TW_UINT16 src(TW_IDENTITY *srcId, TW_UINT32 dg, TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF p)
{
    return g_dsm(&g_app, NULL, dg, dat, msg, p);
}

/*
 * The 2.x DSM resolves its data source directory as
 * GetWindowsDirectory() + "\twain_32", which needs admin rights to write.
 * For testing, redirect that one import to a scratch tree so an unprivileged
 * run can prove discovery and the full triplet flow. Test-only shim.
 */
static char g_fakeWinDir[MAX_PATH];

static UINT WINAPI fake_GetWindowsDirectoryA(LPSTR buf, UINT len)
{
    size_t n = strlen(g_fakeWinDir);
    if (len <= n) return 0;
    memcpy(buf, g_fakeWinDir, n + 1);
    return (UINT)n;
}

static void patch_windir(HMODULE mod, const char *dir)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *imp = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp->VirtualAddress);
    strcpy_s(g_fakeWinDir, sizeof g_fakeWinDir, dir);

    for (; desc->Name; desc++) {
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);
        IMAGE_THUNK_DATA *int_ = (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);
        int i;
        if (!int_) continue;
        for (i = 0; int_[i].u1.AddressOfData; i++) {
            IMAGE_IMPORT_BY_NAME *name;
            DWORD old;
            if (int_[i].u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            name = (IMAGE_IMPORT_BY_NAME *)(base + int_[i].u1.AddressOfData);
            if (strcmp((const char *)name->Name, "GetWindowsDirectoryA") == 0) {
                VirtualProtect(&iat[i], sizeof(IMAGE_THUNK_DATA), PAGE_READWRITE, &old);
                iat[i].u1.Function = (ULONG_PTR)fake_GetWindowsDirectoryA;
                VirtualProtect(&iat[i], sizeof(IMAGE_THUNK_DATA), old, &old);
                printf("       redirected GetWindowsDirectoryA -> %s\n", dir);
            }
        }
    }
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    return DefWindowProcA(w, m, a, b);
}

/* A real top-level window: DSM 2.x fixtures the app's message loop on it. */
static HWND make_window(void)
{
    WNDCLASSA wc;
    HWND h;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dsmtestWnd";
    RegisterClassA(&wc);
    h = CreateWindowExA(0, "dsmtestWnd", "dsmtest", WS_OVERLAPPEDWINDOW,
                        0, 0, 300, 200, NULL, NULL, wc.hInstance, NULL);
    return h;
}

int main(int argc, char **argv)
{
    HMODULE mod;
    HWND hwnd;
    TW_IDENTITY id;
    TW_UINT16 rc;
    int found = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: dsmtest <twaindsm.dll> [ds]\n"); return 2; }
    mod = LoadLibraryA(argv[1]);
    if (!mod) { printf("LoadLibrary(%s) failed %lu\n", argv[1], GetLastError()); return 2; }
    g_dsm = (DSMENTRYPROC_LOCAL)GetProcAddress(mod, "DSM_Entry");
    if (!g_dsm) { printf("DSM_Entry missing in %s\n", argv[1]); return 2; }
    printf("dsmtest: %s\n", argv[1]);
    if (argc > 2) patch_windir(mod, argv[2]);   /* scratch root, e.g. C:\scanroot */

    memset(&g_app, 0, sizeof g_app);
    g_app.Id = 0x4321;
    g_app.Version.MajorNum = 1;
    g_app.Version.MinorNum = 0;
    g_app.Version.Language = TWLG_ENGLISH_USA;
    g_app.Version.Country = TWCY_USA;
    strcpy_s(g_app.Version.Info, sizeof g_app.Version.Info, "dsmtest");
    g_app.ProtocolMajor = 2;
    g_app.ProtocolMinor = 5;
    g_app.SupportedGroups = DG_CONTROL | DG_IMAGE | DF_APP2;
    strcpy_s(g_app.Manufacturer, sizeof g_app.Manufacturer, "Conner");
    strcpy_s(g_app.ProductFamily, sizeof g_app.ProductFamily, "dsmtest");
    strcpy_s(g_app.ProductName, sizeof g_app.ProductName, "dsmtest");

    hwnd = make_window();
    printf("       calling OPENDSM (hwnd %p)\n", (void *)hwnd);
    rc = g_dsm(&g_app, NULL, DG_CONTROL, DAT_PARENT, MSG_OPENDSM, &hwnd);
    CHECK(rc == TWRC_SUCCESS, "DSM OPENDSM rc=%u", rc);
    {
        BYTE raw[16];
        TW_UINT16 src2;
        memset(raw, 0xcc, sizeof raw);
        src2 = g_dsm(&g_app, NULL, DG_CONTROL, DAT_STATUS, MSG_GET, raw);
        printf("       status rc=%u sizeof(TW_STATUS)=%d raw=%02x %02x %02x %02x\n",
               src2, (int)sizeof(TW_STATUS), raw[0], raw[1], raw[2], raw[3]);
    }

    memset(&id, 0, sizeof id);
    rc = dsm(DG_CONTROL, DAT_IDENTITY, MSG_GETFIRST, &id);
    while (rc == TWRC_SUCCESS) {
        printf("       source: '%s' / '%s' / '%s'\n", id.Manufacturer,
               id.ProductFamily, id.ProductName);
        if (strstr(id.ProductName, "iX1300")) found = 1;
        memset(&id, 0, sizeof id);
        rc = dsm(DG_CONTROL, DAT_IDENTITY, MSG_GETNEXT, &id);
    }
    CHECK(found, "DSM discovered the iX1300 data source");

    if (!found) {
        printf("\nFAILED (not discovered)\n");
        return 1;
    }

    {
        TW_IDENTITY open;
        memset(&open, 0, sizeof open);
        strcpy_s(open.ProductName, sizeof open.ProductName, "ScanSnap iX1300");
        open.Id = 0;
        rc = dsm(DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, &open);
        CHECK(rc == TWRC_SUCCESS, "DSM OPENDS rc=%u", rc);
        if (rc == TWRC_SUCCESS) {
            g_src = open;
            g_srcOpen = 1;
            printf("       opened source id %lu\n", (unsigned long)open.Id);
        }

        {
            TW_CAPABILITY cap;
            TW_ONEVALUE ov;
            memset(&cap, 0, sizeof cap);
            cap.Cap = ICAP_XFERMECH;
            cap.ConType = TWON_ONEVALUE;
            cap.hContainer = GlobalAlloc(GHND, sizeof ov);
            ov.ItemType = TWTY_UINT16;
            ov.Item = TWSX_MEMORY;
            memcpy(GlobalLock(cap.hContainer), &ov, sizeof ov);
            GlobalUnlock(cap.hContainer);
            rc = dsm(DG_CONTROL, DAT_CAPABILITY, MSG_SET, &cap);
            GlobalFree(cap.hContainer);
            CHECK(rc == TWRC_SUCCESS, "DSM ICAP_XFERMECH = MEMORY rc=%u", rc);
        }

        {
            TW_USERINTERFACE ui;
            TW_PENDINGXFERS pend;
            TW_SETUPMEMXFER mem;
            int pages = 0;
            memset(&ui, 0, sizeof ui);
            ui.hParent = (TW_HANDLE)hwnd;
            rc = dsm(DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &ui);
            CHECK(rc == TWRC_SUCCESS, "DSM ENABLEDS rc=%u", rc);

            for (;;) {
                TW_IMAGEINFO info;
                DWORD total = 0;
                memset(&mem, 0, sizeof mem);
                rc = dsm(DG_CONTROL, DAT_SETUPMEMXFER, MSG_GET, &mem);
                CHECK(rc == TWRC_SUCCESS && mem.Preferred, "SETUPMEMXFER %lu", mem.Preferred);
                memset(&info, 0, sizeof info);
                rc = dsm(DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &info);
                CHECK(rc == TWRC_SUCCESS, "IMAGEINFO %ldx%ld", info.ImageWidth, info.ImageLength);
                for (;;) {
                    TW_IMAGEMEMXFER x;
                    HGLOBAL h = GlobalAlloc(GHND, mem.Preferred);
                    memset(&x, 0, sizeof x);
                    x.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
                    x.Memory.Length = mem.Preferred;
                    x.Memory.TheMem = h;
                    rc = dsm(DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, &x);
                    total += x.BytesWritten;
                    GlobalFree(h);
                    if (rc == TWRC_XFERDONE) break;
                    if (rc != TWRC_SUCCESS) { CHECK(0, "mem xfer rc=%u", rc); break; }
                }
                CHECK(total > 0, "page %d %lu bytes", pages, total);
                pages++;
                memset(&pend, 0, sizeof pend);
                rc = dsm(DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pend);
                if (pend.Count == 0) break;
            }
            CHECK(pages >= 1, "pages %d", pages);
            rc = dsm(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, NULL);
            CHECK(rc == TWRC_SUCCESS, "DISABLEDS");
        }
        g_srcOpen = 0;
        rc = dsm(DG_CONTROL, DAT_IDENTITY, MSG_CLOSEDS, &g_src);
        CHECK(rc == TWRC_SUCCESS, "CLOSEDS");
    }
    (void)src;

    g_dsm(&g_app, NULL, DG_CONTROL, DAT_PARENT, MSG_CLOSEDSM, &hwnd);
    if (hwnd) DestroyWindow(hwnd);
    FreeLibrary(mod);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
