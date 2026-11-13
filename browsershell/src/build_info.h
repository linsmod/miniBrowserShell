/* build_info.h - what a report/support page needs to identify this build.
 *
 * Shared by minishell://settings (About) and the crash report, so both answer
 * the same questions: which project, which engine DLL and hash, and how was
 * this instance launched. The launch summary is deliberately sanitized: URLs,
 * profile/download paths and proxy values are reduced to "(set)", matching the
 * crash page's "no browsing data" promise.
 */
#ifndef BUILD_INFO_H
#define BUILD_INFO_H

/* https://github.com/linsmod/miniBrowserShell */
const char* buildInfoProjectUrl(void);

/* "Nov 13 2026 11:16:35" (compile time) */
const char* buildInfoShellBuild(void);

/* File version of the loaded mb132_x64.dll, or "-". */
const char* buildInfoEngineVersion(void);

/* SHA-256 of the mb132_x64.dll file, 64 lowercase hex chars, or "-".
 * Computed once on first use (the DLL is ~75 MB, so keep it cached). */
const char* buildInfoEngineSha256(void);

/* Sanitized switch list, e.g. "--zoom 110 --userdata (set)".
 * Never contains URLs or paths. */
const char* buildInfoLaunchArgs(void);

/* Sanitized AGB_/MBV_ environment, e.g. "AGB_NAV_GUARD_MS=4000
 * AGB_USERDATA=(set)". Never contains paths or URLs. */
const char* buildInfoLaunchEnv(void);

#endif /* BUILD_INFO_H */
