/*
 * Configuration (ScanSnapTWAIN.ini next to the .ds) and logging.
 *
 * The driver is standalone: nothing here looks for, starts or stops any PFU
 * software. The only ScanSnap Home artefact that may still influence the USB
 * binding is the INF it installed (see install/README.md).
 */
#include "ssdrv.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

Config g_cfg;
static CRITICAL_SECTION g_logLock;
static BOOL g_logInit;

void config_load(void)
{
    char ini[MAX_PATH], *slash;

    g_cfg.logEnable = 0;
    g_cfg.mock = 0;
    g_cfg.interlace = -1;
    g_cfg.bufferMode = 0;
    g_cfg.lineartInvert = 0;   /* 0 = wire 1-means-ink -> TWAIN 0-is-black */
    g_cfg.serial[0] = 0;
    g_cfg.logPath[0] = 0;

    ini[0] = 0;
    GetModuleFileNameA(g_hinst, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - ini), "ScanSnapTWAIN.ini");

    g_cfg.logEnable = GetPrivateProfileIntA("Driver", "LogEnable", 0, ini);
    g_cfg.mock = GetPrivateProfileIntA("Driver", "Mock", 0, ini);
    g_cfg.interlace = GetPrivateProfileIntA("Driver", "Interlace", -1, ini);
    if (g_cfg.interlace < -1 || g_cfg.interlace > 2) g_cfg.interlace = -1;
    g_cfg.bufferMode = GetPrivateProfileIntA("Driver", "ScannerBuffering", 0, ini);
    g_cfg.lineartInvert = GetPrivateProfileIntA("Driver", "LineartRawPolarity", 0, ini);
    GetPrivateProfileStringA("Driver", "Serial", "", g_cfg.serial, sizeof g_cfg.serial, ini);
    GetPrivateProfileStringA("Driver", "LogFile", "", g_cfg.logPath, sizeof g_cfg.logPath, ini);
    if (!g_cfg.logPath[0]) {
        GetTempPathA(MAX_PATH, g_cfg.logPath);
        strcat_s(g_cfg.logPath, MAX_PATH, "ScanSnapTWAIN.log");
    }
    if (!g_logInit) { InitializeCriticalSection(&g_logLock); g_logInit = TRUE; }
}

void log_printf(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME t;

    if (!g_logInit) return;
    EnterCriticalSection(&g_logLock);
    if (fopen_s(&f, g_cfg.logPath, "a") == 0 && f) {
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, (unsigned long)GetCurrentThreadId());
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}
