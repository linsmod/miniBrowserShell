/* crash_report.h - privacy-free crash diagnostics for the crash page.
 *
 * The report is written from the first-chance/vectored exception handler, so it
 * must only use Win32 calls that are safe while the process is half dead: no
 * CRT stdio, no heap if it can be avoided, fixed buffers. It deliberately
 * contains no browsing data: module names instead of paths, no URLs, no profile
 * locations, no user or machine names.
 */
#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

#include <windows.h>

/* Formats the report into `buf` (NUL terminated). Returns its length. */
size_t crashReportFormat(char* buf, size_t cap, EXCEPTION_RECORD* rec);

/* Formats and writes the report to `path` (best effort, raw Win32 I/O).
 * Returns 1 when the file was written. */
int crashReportWrite(const char* path, EXCEPTION_RECORD* rec);

#endif /* CRASH_REPORT_H */
