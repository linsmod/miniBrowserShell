/*
 * shell_ops.h - the browser shell's operating-system seam.
 *
 * Everything the shell would otherwise ask the OS for directly (profile
 * paths, the system theme and DPI, message boxes, win32 chrome) goes through
 * this table of function pointers. The shell and its UI components only see
 * this contract; the Win32 implementation lives in shell_ops.c and a
 * deterministic in-memory one is used by the headless UI tests.
 *
 * This is the same "operations" pattern as the engine's charset_ops: one
 * struct, one pointer per operation, a default implementation and an
 * alternative that a test can install without touching the call sites.
 */

#ifndef AGAVE_SHELL_OPS_H
#define AGAVE_SHELL_OPS_H

#include <stddef.h>

typedef struct shell_ops_s shell_ops_t;

/* message_box flags (mapped to MB_* by the Win32 implementation). */
#define SHELL_MB_INFO      0x0001u
#define SHELL_MB_WARNING   0x0002u
#define SHELL_MB_ERROR     0x0004u
#define SHELL_MB_QUESTION  0x0008u
#define SHELL_MB_OKCANCEL  0x0010u
#define SHELL_MB_TOPMOST   0x0020u

struct shell_ops_s {
    const char* name;
    void*       user;

    /* Environment lookup. 1 = found. The Win32 default reads the CRT
     * environment, so AGB_USERDATA written with putenv(--userdata) is seen. */
    int (*get_env)(void* user, const char* name, char* out, size_t cap);

    /* Profile root, created on demand. 1 = ready. */
    int (*profile_root)(void* user, char* out, size_t cap);

    /* Where downloads land by default. 1 = ready. */
    int (*downloads_dir)(void* user, char* out, size_t cap);

    /* 1 = the OS prefers dark app chrome. */
    int (*system_is_dark)(void* user);

    /* Effective DPI for a window (0/NULL window = system DPI). */
    unsigned (*dpi_for_window)(void* user, void* hwnd);

    /* Process-wide DPI awareness, called once before any window exists. */
    void (*enable_dpi_awareness)(void* user);

    /* Frame-only DWM setup for the custom title bar: extend the frame 1px into
     * the client so the shadow survives, round Win11 corners, and drop the
     * DWM border line. Does not touch the caption's dark/light colors.
     * Returns 1 when DwmExtendFrameIntoClientArea was accepted. */
    int (*prepare_frame)(void* user, void* hwnd);

    /* Modal message box. Returns 1 for OK/yes, 0 for cancel/no. */
    int (*message_box)(void* user, void* parent, const char* text,
                       const char* caption, unsigned flags);

    /* Monotonic-ish milliseconds (GetTickCount64). */
    unsigned long long (*tick_ms)(void* user);
};

/* Current table (lazily initialised from AGB_SHELL_OPS). Never NULL. */
const shell_ops_t* shellOps(void);

/* Install a table for tests; NULL restores the environment-selected default. */
void shellOpsSet(const shell_ops_t* ops);
void shellOpsReset(void);

/* 1 when the mock table is active. */
int shellOpsIsMock(void);

/* The built-in mock (no registry, no disk): exposed so tests can tweak the
 * fields before installing it. */
shell_ops_t* shellOpsMock(void);

#endif /* AGAVE_SHELL_OPS_H */
