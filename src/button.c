/*
 * ScanSnapButton - starts a NAPS2 scan when the iX1300's Scan button is pressed.
 *
 * The scanner raises no button event of its own (PFU's USD has none either):
 * ScanSnap Home's monitor polls GET HW STATUS every 200 ms with the device held
 * open and reads byte 4 bit 0 (docs/PROTOCOL.md 3b). This does the same, gives
 * the device up whenever the data source or probe asks for it
 * (transport_request_device), and on a press runs NAPS2's own scanner-button
 * entry, `NAPS2.exe /StiDevice:<id>`: a running NAPS2 window scans with its
 * default profile for that device, otherwise NAPS2 starts and scans
 * (NAPS2 WindowsApplicationLifecycle.ExitIfRedundant, DesktopScanController.
 * ScanWithDevice).
 *
 *   ScanSnapButton.exe              run the watcher (one instance per session)
 *   ScanSnapButton.exe --install    copy to %LOCALAPPDATA%\ScanSnap-iX1300,
 *                                   write a default ini, start at logon, start now
 *   ScanSnapButton.exe --uninstall  stop it and remove the logon entry
 *   ScanSnapButton.exe --stop       stop the running watcher
 */
#include "ssdrv.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

HINSTANCE g_hinst;

#define INSTANCE_MUTEX "Local\\ScanSnapIX1300.ButtonInstance"
#define QUIT_EVENT     "Local\\ScanSnapIX1300.ButtonQuit"
#define RUN_KEY        "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE      "ScanSnapButton"
#define INI_NAME       "ScanSnapButton.ini"

#define POLL_MS     200     /* ScanSnap Home's interval; the press bit latches until read */
#define HOLD_OFF_MS 5000    /* stay off the device after handing it over */
#define RETRY_MS    2000    /* device absent or held by someone else */

typedef struct {
    char naps2[MAX_PATH];
    char device[128];
} Settings;

static HANDLE g_release, g_released, g_quit;
static Transport *g_t;
static Proto *g_p;

static void ini_path(char *out, const char *dir)
{
    char *slash;
    if (dir) { sprintf_s(out, MAX_PATH, "%s\\%s", dir, INI_NAME); return; }
    GetModuleFileNameA(NULL, out, MAX_PATH);
    slash = strrchr(out, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - out), INI_NAME);
}

static void ini_string(const char *ini, const char *key, const char *def, char *out, DWORD len)
{
    char raw[MAX_PATH];
    GetPrivateProfileStringA("Button", key, def, raw, sizeof raw, ini);
    if (!ExpandEnvironmentStringsA(raw, out, len)) strcpy_s(out, len, raw);
}

static void settings_load(Settings *s)
{
    char ini[MAX_PATH], def[MAX_PATH], log[MAX_PATH];
    ini_path(ini, NULL);

    /* a 32-bit process sees %ProgramFiles% as Program Files (x86) */
    if (!GetEnvironmentVariableA("ProgramW6432", def, sizeof def) &&
        !GetEnvironmentVariableA("ProgramFiles", def, sizeof def))
        strcpy_s(def, sizeof def, "C:\\Program Files");
    strcat_s(def, sizeof def, "\\NAPS2\\NAPS2.exe");
    ini_string(ini, "Naps2", def, s->naps2, sizeof s->naps2);
    /* NAPS2's TWAIN device id is the source's ProductName (src/ds.c) */
    ini_string(ini, "Device", "ScanSnap iX1300", s->device, sizeof s->device);

    g_cfg.logEnable = GetPrivateProfileIntA("Button", "LogEnable", 0, ini);
    GetTempPathA(sizeof log, log);
    strcat_s(log, sizeof log, "ScanSnapButton.log");
    ini_string(ini, "LogFile", log, g_cfg.logPath, sizeof g_cfg.logPath);
}

static void dev_close(void)
{
    if (g_p) { proto_destroy(g_p); g_p = NULL; }
    if (g_t) { transport_close(g_t); g_t = NULL; }
}

static int dev_open(void)
{
    char path[MAX_PATH];
    g_t = transport_open(path, sizeof path);
    if (!g_t) return 0;
    g_p = proto_create(g_t);
    if (!g_p || proto_open(g_p) != PROTO_OK) {
        LOGF("button: scanner opened but did not answer\n");
        dev_close();
        return 0;
    }
    LOGF("button: watching %s\n", path);
    {
        /* a press made while the device was elsewhere is still latched; drop it */
        BYTE st[48];
        DWORD got = 0;
        proto_hw_status(g_p, st, sizeof st, &got);
    }
    return 1;
}

static int due(DWORD at)
{
    return (LONG)(GetTickCount() - at) >= 0;
}

/* Hands the device over on request: close it, say so, keep off for a while. */
static void on_release(DWORD *holdOff)
{
    if (g_t) { dev_close(); LOGF("button: device handed over\n"); }
    SetEvent(g_released);
    *holdOff = GetTickCount() + HOLD_OFF_MS;
}

/* Waits `ms`, serving release requests. Returns 0 when asked to quit. */
static int idle(DWORD ms, DWORD *holdOff)
{
    HANDLE h[2];
    DWORD end = GetTickCount() + ms;
    h[0] = g_quit;
    h[1] = g_release;
    for (;;) {
        LONG left = (LONG)(end - GetTickCount());
        DWORD w = WaitForMultipleObjects(2, h, FALSE, left > 0 ? (DWORD)left : 0);
        if (w == WAIT_OBJECT_0) return 0;
        if (w == WAIT_OBJECT_0 + 1) { on_release(holdOff); continue; }
        return 1;
    }
}

/*
 * Asks NAPS2 to scan. Not waited for: the process exits at once when it hands
 * the request to a running NAPS2, or stays as the new NAPS2 window. Either
 * way the data source asks for the device when the scan starts.
 */
static void run_scan(const Settings *s)
{
    char cmd[MAX_PATH + 160], dir[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (GetFileAttributesA(s->naps2) == INVALID_FILE_ATTRIBUTES) {
        LOGF("button: NAPS2 not found at %s\n", s->naps2);
        MessageBoxA(NULL, "NAPS2.exe was not found. Set Naps2 in ScanSnapButton.ini.",
                    "ScanSnap iX1300", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }
    sprintf_s(cmd, sizeof cmd, "\"%s\" \"/StiDevice:%s\"", s->naps2, s->device);
    strcpy_s(dir, sizeof dir, s->naps2);
    if (strrchr(dir, '\\')) *strrchr(dir, '\\') = 0;
    LOGF("button: %s\n", cmd);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOGF("button: could not start NAPS2 (error %lu)\n", GetLastError());
    }
}

static int watch(void)
{
    Settings s;
    HANDLE instance;
    DWORD holdOff = 0;

    instance = CreateMutexA(NULL, TRUE, INSTANCE_MUTEX);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;   /* already running */

    config_load();
    settings_load(&s);
    g_release = CreateEventA(NULL, FALSE, FALSE, SSDRV_BUTTON_RELEASE);
    g_released = CreateEventA(NULL, FALSE, FALSE, SSDRV_BUTTON_RELEASED);
    g_quit = CreateEventA(NULL, TRUE, FALSE, QUIT_EVENT);
    if (!g_release || !g_released || !g_quit) return 1;
    LOGF("button: started; %s, device '%s'\n", s.naps2, s.device);

    for (;;) {
        if (!g_t && due(holdOff) && !dev_open()) holdOff = GetTickCount() + RETRY_MS;
        if (g_t) {
            BYTE st[48];
            DWORD got = 0;
            if (proto_hw_status(g_p, st, sizeof st, &got) != PROTO_OK || got < 5) {
                LOGF("button: status read failed; reopening\n");
                dev_close();
                holdOff = GetTickCount() + RETRY_MS;
            } else if (st[4] & 0x01) {
                LOGF("button: Scan pressed\n");
                dev_close();
                settings_load(&s);                 /* pick up ini edits */
                run_scan(&s);
                holdOff = GetTickCount() + HOLD_OFF_MS;
            }
        }
        if (!idle(POLL_MS, &holdOff)) break;
    }
    LOGF("button: stopping\n");
    dev_close();
    ReleaseMutex(instance);
    return 0;
}

/* --------------------------------------------------------- management */

static void say(const char *fmt, const char *arg)
{
    printf(fmt, arg);
    fflush(stdout);
}

/* Asks a running watcher to quit and waits until it has. */
static void stop_running(void)
{
    HANDLE q = OpenEventA(EVENT_MODIFY_STATE, FALSE, QUIT_EVENT);
    HANDLE m;
    if (!q) return;
    SetEvent(q);
    CloseHandle(q);
    m = OpenMutexA(SYNCHRONIZE, FALSE, INSTANCE_MUTEX);
    if (m) {
        DWORD w = WaitForSingleObject(m, 5000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(m);
        CloseHandle(m);
    }
    Sleep(200);                                   /* let the process exit and unlock its image */
    say("%s\n", "stopped the running watcher");
}

static int install(void)
{
    char self[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], ini[MAX_PATH], run[MAX_PATH + 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HKEY key;

    GetModuleFileNameA(NULL, self, sizeof self);
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof dir)) return 1;
    strcat_s(dir, sizeof dir, "\\ScanSnap-iX1300");
    SHCreateDirectoryExA(NULL, dir, NULL);
    sprintf_s(exe, sizeof exe, "%s\\ScanSnapButton.exe", dir);

    stop_running();
    if (_stricmp(self, exe) && !CopyFileA(self, exe, FALSE)) {
        say("could not copy to %s\n", exe);
        return 1;
    }
    ini_path(ini, dir);
    {
        /* write the default, or replace the earlier NAPS2.Console format */
        char probe[8];
        GetPrivateProfileStringA("Button", "Device", "", probe, sizeof probe, ini);
        if (!probe[0]) {
            int logEnable = GetPrivateProfileIntA("Button", "LogEnable", 0, ini);
            FILE *f;
            if (fopen_s(&f, ini, "w") == 0 && f) {
                fprintf(f, "; ScanSnap iX1300 Scan button -> NAPS2. Edits apply on the next press.\n"
                           "; A press runs NAPS2.exe /StiDevice:<Device>: an open NAPS2 window scans\n"
                           "; with its default profile for that device, else NAPS2 starts and scans.\n"
                           "[Button]\n"
                           "Naps2=%%ProgramW6432%%\\NAPS2\\NAPS2.exe\n"
                           "; NAPS2's device id for this scanner (the TWAIN source name)\n"
                           "Device=ScanSnap iX1300\n"
                           "LogEnable=%d\n", logEnable);
                fclose(f);
            }
        }
    }
    sprintf_s(run, sizeof run, "\"%s\"", exe);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)run, (DWORD)strlen(run) + 1);
        RegCloseKey(key);
    }
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    say("installed %s\n", exe);
    say("settings: %s\n", ini);
    say("%s\n", "starts at logon; running now");
    return 0;
}

static int uninstall(void)
{
    HKEY key;
    stop_running();
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueA(key, RUN_VALUE);
        RegCloseKey(key);
    }
    say("%s\n", "removed the logon entry; files in %LOCALAPPDATA%\\ScanSnap-iX1300 are left in place");
    return 0;
}

int main(int argc, char **argv)
{
    g_hinst = GetModuleHandleA(NULL);
    if (argc > 1) {
        /* a GUI-subsystem program: borrow the calling console for messages */
        FILE *f;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen_s(&f, "CONOUT$", "w", stdout);
        if (!strcmp(argv[1], "--install")) return install();
        if (!strcmp(argv[1], "--uninstall")) return uninstall();
        if (!strcmp(argv[1], "--stop")) { stop_running(); return 0; }
        say("unknown option %s\n", argv[1]);
        return 1;
    }
    return watch();
}
