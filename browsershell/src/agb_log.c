#include "agb_log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned long s_origin;
static FILE* s_file;
static CRITICAL_SECTION s_lock;
static int s_lockReady;
static unsigned s_uiSinceFlush;

static void ensureReady(void)
{
    if (s_lockReady)
        return;

    InitializeCriticalSection(&s_lock);
    s_origin = GetTickCount();
    s_lockReady = 1;
}

static const char* channelName(int channel)
{
    switch (channel) {
    case AGB_LOG_SHELL: return "shell";
    case AGB_LOG_PROBE: return "probe";
    case AGB_LOG_NAV:   return "nav";
    case AGB_LOG_UI:    return "ui";
    case AGB_LOG_EVT:
    case AGB_LOG_EVENT: return "evt";
    default:            return "log";
    }
}

void agbLogSetFile(const char* path_or_null)
{
    ensureReady();
    EnterCriticalSection(&s_lock);
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    if (path_or_null && path_or_null[0])
        s_file = fopen(path_or_null, "a");
    LeaveCriticalSection(&s_lock);
}

unsigned long agbLogElapsedMs(void)
{
    ensureReady();
    return GetTickCount() - s_origin;
}

void agbLog(int channel, const char* fmt, ...)
{
    char text[4096];
    char line[4300];
    va_list args;

    ensureReady();

    va_start(args, fmt);
    _vsnprintf(text, sizeof(text) - 1, fmt ? fmt : "", args);
    va_end(args);
    text[sizeof(text) - 1] = 0;

    _snprintf(line, sizeof(line) - 1, "[%7lu.%03lums] [%s] %s\n",
              agbLogElapsedMs() / 1000, agbLogElapsedMs() % 1000,
              channelName(channel), text);
    line[sizeof(line) - 1] = 0;

    OutputDebugStringA(line);

    EnterCriticalSection(&s_lock);
    if (s_file) {
        fputs(line, s_file);

        /* Console output from a heavy page arrives in bursts (hundreds of
         * lines for one ad-heavy site) and a flush per line made the UI thread
         * wait on disk inside the log lock. Diagnostics (shell/probe/nav) still
         * flush immediately; page console output is batched. */
        if (channel != AGB_LOG_UI || ++s_uiSinceFlush >= 64) {
            fflush(s_file);
            s_uiSinceFlush = 0;
        }
    }
    LeaveCriticalSection(&s_lock);
}

void agbLogFlush(void)
{
    ensureReady();
    EnterCriticalSection(&s_lock);
    if (s_file)
        fflush(s_file);
    LeaveCriticalSection(&s_lock);
}
