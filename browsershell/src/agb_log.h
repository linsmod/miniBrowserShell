/*
 * agb_log.h - minimal stand-in for agave's AgbLog.h.
 *
 * The shell UI was written against agbLog(channel, fmt, ...) and
 * agbLogElapsedMs().  We keep the same call sites; the sink writes to
 * <exe>\shell.log and the debugger instead of agave's shared mapping.
 */

#ifndef AGB_LOG_H
#define AGB_LOG_H

#include <stddef.h>

enum {
    AGB_LOG_SHELL = 0,
    AGB_LOG_PROBE,
    AGB_LOG_NAV,
    AGB_LOG_UI,
    AGB_LOG_EVT,
    AGB_LOG_EVENT,
    AGB_LOG_COUNT_
};

#ifdef __cplusplus
extern "C" {
#endif

void agbLog(int channel, const char* fmt, ...);
unsigned long agbLogElapsedMs(void);
void agbLogFlush(void);
void agbLogSetFile(const char* path_or_null);

#ifdef __cplusplus
}
#endif

#endif /* AGB_LOG_H */
