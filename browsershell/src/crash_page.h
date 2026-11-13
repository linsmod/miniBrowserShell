/* crash_page.h - the standalone "MiniShell crashed" window.
 *
 * `crashPageRun()` is the `--crash-page <report>` mode: a tiny host window that
 * renders the report as a local page with a Copy button. `crashPageLaunch()`
 * starts that mode in a fresh process, which is what the exception handlers do:
 * the process that crashed can be in any state, so the page must not depend on
 * it (or on its webviews) still being usable.
 */
#ifndef CRASH_PAGE_H
#define CRASH_PAGE_H

/* Shows the report page in this process. Returns the process exit code. */
int crashPageRun(const char* report_path);

/* Starts `--crash-page <report_path>` in a new process. Returns 1 when the
 * child started; 0 means the caller should fall back to a MessageBox. */
int crashPageLaunch(const char* report_path);

#endif /* CRASH_PAGE_H */
