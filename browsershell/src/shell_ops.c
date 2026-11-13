/*
 * shell_ops.c - Win32 implementation of the shell's OS seam, plus the mock.
 */

#include "shell_ops.h"

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void copyString(char* out, size_t cap, const char* text)
{
    if (!out || cap == 0)
        return;

    if (!text)
        text = "";

    _snprintf(out, cap, "%s", text);
    out[cap - 1] = 0;
}

static void makeDirTree(char* path)
{
    char* p;
    char saved;

    if (!path || !path[0])
        return;

    for (p = path + 1; ; ++p) {
        saved = *p;

        if (saved == '\\' || saved == '/' || saved == 0) {
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = saved;

            if (!saved)
                break;
        }
    }
}

static int dirExists(const char* path)
{
    DWORD attributes;

    if (!path || !path[0])
        return 0;

    attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* ------------------------------------------------------------------------- */
/* Win32 implementation                                                      */
/* ------------------------------------------------------------------------- */

static int win32_get_env(void* user, const char* name, char* out, size_t cap)
{
    const char* value;

    (void)user;

    if (!name || !out || cap == 0)
        return 0;

    value = getenv(name);

    if (!value || !value[0])
        return 0;

    copyString(out, cap, value);

    return 1;
}

static int win32_profile_root(void* user, char* out, size_t cap)
{
    char candidate[MAX_PATH];
    const char* value;

    (void)user;

    if (!out || cap == 0)
        return 0;

    out[0] = 0;

    /* AGB_USERDATA (env or --userdata) is the single profile root. */
    value = getenv("AGB_USERDATA");

    if (value && value[0]) {
        copyString(out, cap, value);
        makeDirTree(out);
        return out[0] != 0;
    }

    value = getenv("APPDATA");

    if (value && value[0]) {
        _snprintf(candidate, sizeof(candidate), "%s\\MiniShell", value);
        candidate[sizeof(candidate) - 1] = 0;

        if (CreateDirectoryA(candidate, NULL) || dirExists(candidate)) {
            copyString(out, cap, candidate);
            return 1;
        }
    }

    value = getenv("LOCALAPPDATA");

    if (value && value[0]) {
        _snprintf(candidate, sizeof(candidate), "%s\\MiniShellData", value);
        candidate[sizeof(candidate) - 1] = 0;

        if (CreateDirectoryA(candidate, NULL) || dirExists(candidate)) {
            copyString(out, cap, candidate);
            return 1;
        }
    }

    /* Last resort: next to the executable (portable setups). Never default
     * next to the build tree on purpose: -Fresh deletes it. */
    if (GetModuleFileNameA(NULL, candidate, sizeof(candidate))) {
        char* slash = strrchr(candidate, '\\');

        if (slash)
            *(slash + 1) = 0;

        if (strlen(candidate) + 20 < sizeof(candidate))
            strcat(candidate, "MiniShellData");

        CreateDirectoryA(candidate, NULL);
        copyString(out, cap, candidate);
        return 1;
    }

    return 0;
}

static int win32_downloads_dir(void* user, char* out, size_t cap)
{
    const char* home;

    (void)user;

    if (!out || cap == 0)
        return 0;

    home = getenv("USERPROFILE");

    if (!home || !home[0])
        return 0;

    _snprintf(out, cap, "%s\\Downloads", home);
    out[cap - 1] = 0;

    return 1;
}

static int win32_system_is_dark(void* user)
{
    DWORD light = 1;
    DWORD size = sizeof(light);

    (void)user;

    if (RegGetValueA(HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     "AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) != ERROR_SUCCESS)
        return 0;

    return light ? 0 : 1;
}

typedef UINT (WINAPI* GetDpiForWindowFn)(HWND);
typedef UINT (WINAPI* GetDpiForSystemFn)(void);
typedef BOOL (WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);

static GetDpiForWindowFn resolveGetDpiForWindow(void)
{
    static GetDpiForWindowFn cached;
    static int tried;

    if (!tried) {
        HMODULE user32 = GetModuleHandleA("user32.dll");

        tried = 1;

        if (user32)
            cached = (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow");
    }

    return cached;
}

static unsigned win32_dpi_for_window(void* user, void* hwnd)
{
    GetDpiForWindowFn getDpi = resolveGetDpiForWindow();
    HDC dc;
    int dpi = 0;

    (void)user;

    if (getDpi && hwnd)
        dpi = (int)getDpi((HWND)hwnd);

    if (dpi <= 0 && getDpi)
        dpi = (int)getDpi(NULL);

    if (dpi <= 0) {
        dc = GetDC((HWND)hwnd);

        if (dc) {
            dpi = GetDeviceCaps(dc, LOGPIXELSX);
            ReleaseDC((HWND)hwnd, dc);
        }
    }

    return dpi > 0 ? (unsigned)dpi : 96u;
}

static void win32_enable_dpi_awareness(void* user)
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    SetProcessDpiAwarenessContextFn setContext = NULL;

    (void)user;

    if (user32)
        setContext = (SetProcessDpiAwarenessContextFn)(void*)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");

    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4. */
    if (setContext && setContext((HANDLE)(INT_PTR)-4))
        return;

    SetProcessDPIAware();
}

typedef HRESULT (WINAPI* DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

typedef struct {
    DwmSetWindowAttributeFn setAttribute;
    int tried;
} DwmApi;

static DwmApi* resolveDwmApi(void)
{
    static DwmApi api;

    if (!api.tried) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");

        api.tried = 1;

        if (dwm) {
            api.setAttribute = (DwmSetWindowAttributeFn)(void*)GetProcAddress(
                dwm, "DwmSetWindowAttribute");
        }
    }

    return &api;
}

/* Keep WS_THICKFRAME | WS_CAPTION (shadow, resize/snap semantics) but let the
 * chrome paint the whole window. The DWM frame is deliberately NOT extended
 * into the client: any extended band is drawn at the window's outer edge, and a
 * maximized custom-frame window showed it as a transparent edge over the shell.
 * Round the Win11 corners and set DWMWA_BORDER_COLOR to "none" so the light-mode
 * 1px frame border cannot show as a white rim. The caption's dark/light color is
 * deliberately not touched (the caption is client-drawn). */
static int win32_prepare_frame(void* user, void* hwnd)
{
    DwmApi* api = resolveDwmApi();

    (void)user;

    if (!hwnd)
        return 0;

    if (api->setAttribute) {
        int corner = 2;              /* DWMWCP_ROUND */
        DWORD border = 0xFFFFFFFEu;  /* DWMWA_COLOR_NONE */
        BOOL noTransitions = TRUE;

        /* Optional: Win10 ignores both attributes. */
        api->setAttribute((HWND)hwnd, 33u, &corner, sizeof(corner));
        api->setAttribute((HWND)hwnd, 34u, &border, sizeof(border));

        /* DWMWA_TRANSITIONS_FORCEDISABLED: the maximize/restore animation walks
         * a custom-frame window through sizes where DWM renders its own frame,
         * which is the classic glass edge that flashes during the animation.
         * WM_NCCALCSIZE cannot paint over that state, so the window opts out of
         * the transition instead of fighting it. Cost: maximize/restore is no
         * longer animated for this window. */
        api->setAttribute((HWND)hwnd, 3u, &noTransitions, sizeof(noTransitions));
    }

    return 0;
}

static int win32_message_box(void* user, void* parent, const char* text,
                             const char* caption, unsigned flags)
{
    UINT type = MB_OK;
    int result;

    (void)user;

    if (flags & SHELL_MB_OKCANCEL)
        type = MB_OKCANCEL;
    if (flags & SHELL_MB_QUESTION)
        type |= MB_ICONQUESTION;
    else if (flags & SHELL_MB_ERROR)
        type |= MB_ICONERROR;
    else if (flags & SHELL_MB_WARNING)
        type |= MB_ICONWARNING;
    else if (flags & SHELL_MB_INFO)
        type |= MB_ICONINFORMATION;
    if (flags & SHELL_MB_TOPMOST)
        type |= MB_TOPMOST;

    result = MessageBoxA((HWND)parent, text ? text : "", caption ? caption : "agave", type);

    return result == IDOK || result == IDYES;
}

static unsigned long long win32_tick_ms(void* user)
{
    (void)user;
    return GetTickCount64();
}

/* ------------------------------------------------------------------------- */
/* mock implementation                                                       */
/* ------------------------------------------------------------------------- */

static int mock_get_env(void* user, const char* name, char* out, size_t cap)
{
    (void)user;

    /* Tests do not want the developer's real profile. AGB_SHELL_THEME is still
     * honoured because it is an explicit, deterministic switch. */
    if (name && !strcmp(name, "AGB_SHELL_THEME")) {
        const char* value = getenv("AGB_SHELL_THEME");

        if (value && value[0]) {
            copyString(out, cap, value);
            return 1;
        }
    }

    if (name && !strcmp(name, "AGB_USERDATA")) {
        const char* value = getenv("AGB_SHELL_MOCK_USERDATA");

        if (value && value[0]) {
            copyString(out, cap, value);
            return 1;
        }
    }

    if (out && cap)
        out[0] = 0;

    return 0;
}

static int mock_profile_root(void* user, char* out, size_t cap)
{
    (void)user;

    if (!out || cap == 0)
        return 0;

    copyString(out, cap, "mock-profile");
    return 1;
}

static int mock_downloads_dir(void* user, char* out, size_t cap)
{
    (void)user;

    if (!out || cap == 0)
        return 0;

    copyString(out, cap, "mock-downloads");
    return 1;
}

static int mock_system_is_dark(void* user)
{
    const char* theme = getenv("AGB_SHELL_THEME");

    (void)user;

    return theme && !_stricmp(theme, "dark") ? 1 : 0;
}

static unsigned mock_dpi_for_window(void* user, void* hwnd)
{
    const char* dpi = getenv("AGB_SHELL_MOCK_DPI");
    int value = dpi ? atoi(dpi) : 96;

    (void)user;
    (void)hwnd;

    return value > 0 ? (unsigned)value : 96u;
}

static void mock_noop_dpi(void* user)
{
    (void)user;
}

static int mock_prepare_frame(void* user, void* hwnd)
{
    (void)user;
    (void)hwnd;
    return 1;
}

static int mock_message_box(void* user, void* parent, const char* text,
                            const char* caption, unsigned flags)
{
    (void)user;
    (void)parent;

    fprintf(stderr, "[shell-ops/mock] message-box flags=%u caption=%s text=%s\n",
            flags, caption ? caption : "", text ? text : "");
    fflush(stderr);

    return 1;
}

static unsigned long long mock_tick_ms(void* user)
{
    (void)user;
    return GetTickCount64();
}

/* ------------------------------------------------------------------------- */
/* table selection                                                           */
/* ------------------------------------------------------------------------- */

static shell_ops_t g_win32_ops;
static shell_ops_t g_mock_ops;
static const shell_ops_t* g_active;
static int g_initialised;

static void fillTables(void)
{
    if (g_initialised)
        return;

    memset(&g_win32_ops, 0, sizeof(g_win32_ops));
    g_win32_ops.name = "win32";
    g_win32_ops.get_env = win32_get_env;
    g_win32_ops.profile_root = win32_profile_root;
    g_win32_ops.downloads_dir = win32_downloads_dir;
    g_win32_ops.system_is_dark = win32_system_is_dark;
    g_win32_ops.dpi_for_window = win32_dpi_for_window;
    g_win32_ops.enable_dpi_awareness = win32_enable_dpi_awareness;
    g_win32_ops.prepare_frame = win32_prepare_frame;
    g_win32_ops.message_box = win32_message_box;
    g_win32_ops.tick_ms = win32_tick_ms;

    memset(&g_mock_ops, 0, sizeof(g_mock_ops));
    g_mock_ops.name = "mock";
    g_mock_ops.get_env = mock_get_env;
    g_mock_ops.profile_root = mock_profile_root;
    g_mock_ops.downloads_dir = mock_downloads_dir;
    g_mock_ops.system_is_dark = mock_system_is_dark;
    g_mock_ops.dpi_for_window = mock_dpi_for_window;
    g_mock_ops.enable_dpi_awareness = mock_noop_dpi;
    g_mock_ops.prepare_frame = mock_prepare_frame;
    g_mock_ops.message_box = mock_message_box;
    g_mock_ops.tick_ms = mock_tick_ms;

    g_initialised = 1;
}

const shell_ops_t* shellOps(void)
{
    fillTables();

    if (!g_active) {
        const char* mode = getenv("AGB_SHELL_OPS");

        g_active = (mode && !_stricmp(mode, "mock")) ? &g_mock_ops : &g_win32_ops;
    }

    return g_active;
}

void shellOpsSet(const shell_ops_t* ops)
{
    fillTables();
    g_active = ops ? ops : &g_win32_ops;

    if (ops)
        return;

    {
        const char* mode = getenv("AGB_SHELL_OPS");

        if (mode && !_stricmp(mode, "mock"))
            g_active = &g_mock_ops;
    }
}

void shellOpsReset(void)
{
    g_active = NULL;
}

int shellOpsIsMock(void)
{
    return shellOps() == &g_mock_ops;
}

shell_ops_t* shellOpsMock(void)
{
    fillTables();
    return &g_mock_ops;
}
