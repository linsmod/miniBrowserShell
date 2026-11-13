/* crash_report.c - the privacy-free report shown by the crash page.
 *
 * Everything here runs inside the vectored exception handler: the process may
 * already be corrupted, so the code sticks to fixed buffers, Win32 calls and
 * CaptureStackBackTrace. No stdio: the CRT can be the thing that broke.
 *
 * Privacy rules (the report is meant to be pasted into a public issue):
 *   - module *names* and versions, never file paths
 *   - no URLs, page titles, profile paths, user or machine names
 *   - the only "environment" facts are OS build, architecture, DPI and uptime
 */
#include "crash_report.h"
#include "build_info.h"

#include <winternl.h>
#include <tlhelp32.h>
#include <winver.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "version.lib")

#define CRASH_MAX_MODULES 192
#define CRASH_MAX_FRAMES  48
#define CRASH_REPORT_CAP  (96 * 1024)

typedef struct {
    unsigned long long base;
    unsigned long long size;
    wchar_t            name[MAX_MODULE_NAME32 + 1];
    wchar_t            path[MAX_PATH];
} CrashModule;

typedef LONG (WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);

static CrashModule s_modules[CRASH_MAX_MODULES];
static int         s_moduleCount;

/* ------------------------------------------------------------------------- */
/* tiny formatter                                                            */
/* ------------------------------------------------------------------------- */

static void app(char* buf, size_t cap, size_t* len, const char* fmt, ...)
{
    va_list args;
    int n;

    if (!buf || !len || *len + 1 >= cap)
        return;

    va_start(args, fmt);
    n = _vsnprintf(buf + *len, cap - *len - 1, fmt, args);
    va_end(args);

    if (n < 0)
        return;

    if ((size_t)n >= cap - *len - 1)
        n = (int)(cap - *len - 2);

    *len += (size_t)n;
    buf[*len] = 0;
}

static void utf8FromWide(const wchar_t* wide, char* out, size_t cap)
{
    int n;

    if (!out || cap == 0)
        return;

    out[0] = 0;
    if (!wide || !wide[0])
        return;

    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL);
    if (n <= 0)
        out[0] = 0;
}

/* "module+0x1234" with the directory stripped: the maintainer has the module
 * list, not this machine's install paths. */
static void formatAddress(unsigned long long address, char* out, size_t cap)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    const wchar_t* base;
    char name[96];
    int i;

    if (!out || cap == 0)
        return;

    out[0] = 0;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(uintptr_t)address, &module) || !module ||
        !GetModuleFileNameW(module, path, MAX_PATH)) {
        _snprintf(out, cap, "0x%llx", address);
        out[cap - 1] = 0;
        return;
    }

    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    utf8FromWide(base, name, sizeof(name));

    for (i = 0; name[i]; ++i) {
        if (name[i] >= 'A' && name[i] <= 'Z')
            name[i] = (char)(name[i] - 'A' + 'a');
    }

    _snprintf(out, cap, "%s+0x%llx", name, address - (unsigned long long)module);
    out[cap - 1] = 0;
}

static const char* exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:   return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:      return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:     return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:   return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_BREAKPOINT:         return "BREAKPOINT";
    default:                           return "EXCEPTION";
    }
}

/* ------------------------------------------------------------------------- */
/* environment                                                               */
/* ------------------------------------------------------------------------- */

static void osVersion(char* out, size_t cap)
{
    OSVERSIONINFOW info;
    RtlGetVersionFn fn = NULL;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");

    if (ntdll)
        fn = (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion");

    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);

    if (fn && fn(&info) == 0)
        _snprintf(out, cap, "Windows %lu.%lu.%lu", info.dwMajorVersion,
                  info.dwMinorVersion, info.dwBuildNumber);
    else
        _snprintf(out, cap, "Windows (version unknown)");

    out[cap - 1] = 0;
}

static unsigned systemDpi(void)
{
    typedef UINT (WINAPI* GetDpiForSystemFn)(void);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    GetDpiForSystemFn fn = NULL;

    if (user32)
        fn = (GetDpiForSystemFn)(void*)GetProcAddress(user32, "GetDpiForSystem");

    if (fn) {
        UINT dpi = fn();
        if (dpi)
            return dpi;
    }

    return 96;
}

static unsigned long long processUptimeSeconds(void)
{
    FILETIME created, exited, kernel, user, now;
    ULARGE_INTEGER a, b;

    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;

    GetSystemTimeAsFileTime(&now);

    a.LowPart = created.dwLowDateTime;
    a.HighPart = created.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;

    if (b.QuadPart <= a.QuadPart)
        return 0;

    return (b.QuadPart - a.QuadPart) / 10000000ULL;
}

static void fileVersion(const wchar_t* path, char* out, size_t cap)
{
    /* Static, not malloc: this runs inside the exception handler and the heap
     * may be the thing that broke. Version blocks are a few KB at most. */
    static unsigned char block[8 * 1024];
    DWORD dummy = 0;
    DWORD size;
    VS_FIXEDFILEINFO* fixed = NULL;
    UINT fixedLen = 0;

    out[0] = 0;

    size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size || size > sizeof(block))
        return;

    if (GetFileVersionInfoW(path, 0, size, block) &&
        VerQueryValueW(block, L"\\", (LPVOID*)&fixed, &fixedLen) && fixed &&
        fixedLen >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD) {
        _snprintf(out, cap, "%u.%u.%u.%u",
                  (unsigned)HIWORD(fixed->dwFileVersionMS),
                  (unsigned)LOWORD(fixed->dwFileVersionMS),
                  (unsigned)HIWORD(fixed->dwFileVersionLS),
                  (unsigned)LOWORD(fixed->dwFileVersionLS));
        out[cap - 1] = 0;
    }
}

static int moduleCompare(const void* a, const void* b)
{
    const CrashModule* ma = (const CrashModule*)a;
    const CrashModule* mb = (const CrashModule*)b;

    if (ma->base < mb->base) return -1;
    if (ma->base > mb->base) return 1;
    return 0;
}

static void collectModules(void)
{
    HANDLE snap;
    MODULEENTRY32W entry;

    s_moduleCount = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                    GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snap, &entry)) {
        do {
            CrashModule* m;

            if (s_moduleCount >= CRASH_MAX_MODULES)
                break;

            m = &s_modules[s_moduleCount++];
            m->base = (unsigned long long)(uintptr_t)entry.modBaseAddr;
            m->size = (unsigned long long)entry.modBaseSize;
            wcsncpy(m->name, entry.szModule, MAX_MODULE_NAME32);
            m->name[MAX_MODULE_NAME32] = 0;
            wcsncpy(m->path, entry.szExePath, MAX_PATH - 1);
            m->path[MAX_PATH - 1] = 0;
        } while (Module32NextW(snap, &entry));
    }

    CloseHandle(snap);

    if (s_moduleCount > 1)
        qsort(s_modules, (size_t)s_moduleCount, sizeof(s_modules[0]), moduleCompare);
}

/* ------------------------------------------------------------------------- */
/* report                                                                    */
/* ------------------------------------------------------------------------- */

size_t crashReportFormat(char* buf, size_t cap, EXCEPTION_RECORD* rec)
{
    SYSTEMTIME st;
    char when[64];
    char os[96];
    char addr[160];
    size_t len = 0;
    int i;

    if (!buf || cap == 0)
        return 0;

    buf[0] = 0;

    collectModules();

    GetLocalTime(&st);
    _snprintf(when, sizeof(when), "%04u-%02u-%02u %02u:%02u:%02u",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    when[sizeof(when) - 1] = 0;

    osVersion(os, sizeof(os));

    if (rec)
        formatAddress((unsigned long long)(uintptr_t)rec->ExceptionAddress, addr, sizeof(addr));
    else
        strcpy(addr, "unknown");

    app(buf, cap, &len, "MiniShell crash report\n");
    app(buf, cap, &len, "======================\n\n");
    app(buf, cap, &len, "Generated   %s (local)\n", when);
    app(buf, cap, &len, "Shell       miniblink 132 shell, built %s %s, %s\n",
        __DATE__, __TIME__,
#if defined(_WIN64)
        "x64"
#else
        "x86"
#endif
    );
    app(buf, cap, &len, "Engine      mb132_x64.dll %s\n", buildInfoEngineVersion());
    app(buf, cap, &len, "Engine hash SHA-256 %s\n", buildInfoEngineSha256());
    app(buf, cap, &len, "Project     %s\n", buildInfoProjectUrl());
    app(buf, cap, &len, "OS          %s, DPI %u (%u%%)\n", os, systemDpi(),
        (systemDpi() * 100 + 48) / 96);
    app(buf, cap, &len, "Process     pid %lu, uptime %llu s\n",
        (unsigned long)GetCurrentProcessId(), processUptimeSeconds());
    app(buf, cap, &len, "Privacy     no URLs, file paths, account or machine names;\n"
                        "            launch flags omit their paths/URLs\n\n");

    app(buf, cap, &len, "Launch\n");
    app(buf, cap, &len, "------\n");
    app(buf, cap, &len, "args        %s\n", buildInfoLaunchArgs());
    app(buf, cap, &len, "env         %s\n\n", buildInfoLaunchEnv());

    app(buf, cap, &len, "Exception\n");
    app(buf, cap, &len, "---------\n");
    app(buf, cap, &len, "Thread      %lu\n", (unsigned long)GetCurrentThreadId());

    if (rec) {
        app(buf, cap, &len, "Code        0x%08lx  %s\n",
            (unsigned long)rec->ExceptionCode, exceptionName(rec->ExceptionCode));
        app(buf, cap, &len, "Address     %s\n", addr);

        if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            rec->NumberParameters >= 2) {
            const char* kind = "unknown";

            if (rec->ExceptionInformation[0] == 0)
                kind = "read";
            else if (rec->ExceptionInformation[0] == 1)
                kind = "write";
            else if (rec->ExceptionInformation[0] == 8)
                kind = "execute";

            app(buf, cap, &len, "Access      %s 0x%llx\n", kind,
                (unsigned long long)rec->ExceptionInformation[1]);
        }
    }

    app(buf, cap, &len, "\nStack (innermost first; the first few frames are the\n"
                        "crash reporter itself, OS frames follow)\n");
    app(buf, cap, &len, "---------------------------------------------------------\n");

    {
        void* frames[CRASH_MAX_FRAMES];
        unsigned short count = CaptureStackBackTrace(0, CRASH_MAX_FRAMES, frames, NULL);
        char one[160];

        for (i = 0; i < (int)count; ++i) {
            formatAddress((unsigned long long)frames[i], one, sizeof(one));
            app(buf, cap, &len, "  #%02d  %s\n", i, one);
        }

        if (count == 0)
            app(buf, cap, &len, "  (no frames captured)\n");
    }

    app(buf, cap, &len, "\nModules (name, base, size, version)\n");
    app(buf, cap, &len, "------------------------------------\n");

    for (i = 0; i < s_moduleCount; ++i) {
        char name[96];
        char version[64];

        utf8FromWide(s_modules[i].name, name, sizeof(name));
        fileVersion(s_modules[i].path, version, sizeof(version));

        app(buf, cap, &len, "  %016llx  %10llu  %-32s %s\n",
            s_modules[i].base, s_modules[i].size, name,
            version[0] ? version : "-");
    }

    if (s_moduleCount == 0)
        app(buf, cap, &len, "  (module list unavailable)\n");

    app(buf, cap, &len, "\nNotes\n");
    app(buf, cap, &len, "-----\n");
    app(buf, cap, &len, "What you were doing when it crashed - describe it in the bug report.\n\n");

    return len;
}

int crashReportWrite(const char* path, EXCEPTION_RECORD* rec)
{
    static char report[CRASH_REPORT_CAP];
    HANDLE file;
    DWORD wrote = 0;
    size_t len;

    report[0] = 0;
    len = crashReportFormat(report, sizeof(report), rec);

    if (len == 0)
        return 0;

    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    WriteFile(file, report, (DWORD)len, &wrote, NULL);
    CloseHandle(file);

    return wrote == (DWORD)len;
}
