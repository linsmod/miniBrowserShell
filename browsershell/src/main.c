/*
 * main.c - Agave BrowserShell, built on the AgaveWebView control.
 *
 * The shell used to drive macross_* directly (its own engine thread, DIB and
 * input translation). It now only owns UI: every tab is an AgaveWebView child
 * window, so the control gets exercised the way an embedding host would.
 *
 * Threading: the control owns the engine thread; this file never touches the
 * engine directly and only uses the control's C API (queued, thread safe).
 */

#include <windows.h>
#include <shellapi.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* The probe writes PNG through WIC (shot_png.c): this project does not carry
 * agave's vendored libpng. */
#include "shot_png.h"

#include "agave_webview.h"
#include "agb_log.h"
#include "build_info.h"
#include "crash_page.h"
#include "crash_report.h"
#include "shell_ops.h"
#include "ui_chrome.h"
#include "ui_favicon.h"
#include "ui_inspect.h"
#include "ui_menu.h"
#include "ui_menu_host.h"
#include "ui_tabsearch.h"

#define MAX_TABS        UI_MAX_TABS
#define MAX_PATH_CHARS  1024

/* The internal page a new tab starts on (Ctrl+T, +, --new-tab without a URL). */
#define NEW_TAB_URL     "minishell://newtab"

/* app.rc: the page half of the F12 element marker (assets/inspect.js). */
#define IDR_INSPECT_JS  201

/* The engine's context-menu item ids (mb.h: mbMenuItemId). Editing and image
 * commands are handed back to miniblink instead of being emulated here, so they
 * target the right frame and keep the engine's own state in sync. */
#define MB_MENU_SELECT_ALL   (1 << 1)
#define MB_MENU_COPY_TEXT    (1 << 2)
#define MB_MENU_UNDO         (1 << 3)
#define MB_MENU_COPY_IMAGE   (1 << 4)
#define MB_MENU_INSPECT      (1 << 5)
#define MB_MENU_CUT          (1 << 6)
#define MB_MENU_PASTE        (1 << 7)
#define MB_MENU_PRINT        (1 << 8)
#define MB_MENU_GO_FORWARD   (1 << 9)
#define MB_MENU_GO_BACK      (1 << 10)
#define MB_MENU_RELOAD       (1 << 11)
#define MB_MENU_SAVE_IMAGE   (1 << 12)

/* The command IDs live in ui_chrome.h (UI_CMD_*): the custom-drawn chrome and
 * the old WM_COMMAND switch must keep speaking the same numbers. */
#define ID_BACK       UI_CMD_BACK
#define ID_FORWARD    UI_CMD_FORWARD
#define ID_RELOAD     UI_CMD_RELOAD
#define ID_STOP       UI_CMD_STOP
#define ID_ADDRESS    UI_CMD_ADDRESS
#define ID_GO         UI_CMD_GO
#define ID_NEWTAB     UI_CMD_NEWTAB
#define ID_MENU       UI_CMD_MENU

#define ID_FIND_EDIT  UI_CMD_FIND_EDIT
#define ID_FIND_NEXT  UI_CMD_FIND_NEXT
#define ID_FIND_PREV  UI_CMD_FIND_PREV
#define ID_FIND_CLOSE UI_CMD_FIND_CLOSE

/* Matches app.rc: the shell / executable icon. */
#define IDI_APPICON   101

/* Posted (not called) by onUnresponsive so the "page unresponsive" prompt never
 * runs inside the control's window procedure. wParam = tab index. */
#define AGW_SHELL_HANG_PROMPT (WM_APP + 22)

/* A dropped command wrote its reason into tab->status on the engine thread; the
 * status bar is main-thread state, so the repaint is posted (see
 * onCommandDropped). */
#define AGW_SHELL_STATUS (WM_APP + 24)

/* Coalesced find update: the native EDIT delivers an IME word commit as several
 * back-to-back EN_CHANGEs (first '程', then '程度'), and searching each of them
 * jumped the page to a hit for the half-word. Posting one message makes the
 * whole burst collapse into a single search of the final text. */
#define AGW_FIND_UPDATE (WM_APP + 34)

/* Chrome metrics come from the active window's theme (DPI-scaled). */
#define CHROME        (&g_cur->chrome)
#define TAB_HEIGHT    (CHROME->theme.tabstrip_h)
#define TOOLBAR_H     (CHROME->theme.toolbar_h)
#define FIND_H        (CHROME->model.find_visible ? CHROME->theme.find_h : 0)
#define STATUS_H      (CHROME->theme.status_h)

struct ShellWindow;

typedef struct {
    AgaveWebView* view;
    HWND          hwnd;          /* the control's HWND */
    char          url[MAX_PATH_CHARS];
    char          title[256];
    char          status[512];
    int           loading;
    int           progress;
    int           used;
    /* The watchdog reported this tab's page as unresponsive (advisory: nothing
     * was killed). The dialog is only shown for the active tab; a background
     * tab is marked in the tab strip and prompts when the user switches to it. */
    int           hangPending;
    unsigned int  hangMs;
    char          hangStack[MAX_PATH_CHARS];
    /* The site's favicon, decoded by ui_favicon.c from the bytes the engine
     * delivered (on_favicon). Freed on navigation and on tab close; NULL means
     * the strip paints the letter chip. */
    UiFavicon*    icon;
    /* Favicon download state: the page URL the icon belongs to, the candidate
     * list the page reported (newline separated, /favicon.ico last) and how far
     * the chain has got. One request at a time; see requestFavicon(). */
    char          iconPage[MAX_PATH_CHARS];
    char          iconCandidates[768];
    int           iconNext;
    int           iconTried;
    int           iconFetching;
    int           iconCached;      /* came from the disk cache: refresh after load */
    /* Last Page Visibility value handed to the control (-1 = never sent), so a
     * resize/minimize does not queue a visibility command per WM_SIZE. */
    int           visibilitySent;
    /* Callbacks arrive with the Tab* as their user pointer and no window; the
     * back pointer is how they find the window whose UI they may touch. */
    struct ShellWindow* window;
    /* Navigation guard (see armNavGuard): when the engine was asked for the
     * current URL, and how many times a view that never answered was rebuilt.
     * A miniblink control that drops its *first* navigation stays dead forever
     * (no URL callback, no loadingFinish) while the rest of the engine keeps
     * working, so the only recovery is a new control. */
    unsigned long long navStartMs;
    int                navRespawns;
    /* For the tab-search popup's "2h ago" column: when this tab was last the
     * active one (shell tick). */
    unsigned long long activeMs;
} Tab;

/* Closed tabs, so ctrl+shift+T can bring the last one back. */
#define MAX_CLOSED_TABS UI_MAX_TABS
typedef struct {
    char url[MAX_PATH_CHARS];
    char title[256];
    unsigned long long closedMs;   /* shell tick, for the tab-search popup */
} ClosedTab;

/* One top-level shell window. The shell used to keep all of this in file-scope
 * globals; one process may now own several windows (Chrome's
 * "--new-window hands off, the browser process opens it"), so every window
 * has its own copy and the rest of this file reaches it through the macros
 * below. */
#define MAX_SHELL_WINDOWS 8

typedef struct ShellWindow {
    int           used;
    HWND          hwnd;
    HWND          address, findEdit;
    HWND          findBar;       /* find overlay: child window above the view */
    HWND          statusBar;     /* status bubble: bottom-left overlay */
    UiChrome      chrome;
    Tab           tabs[MAX_TABS];
    int           activeTab;
    int           zoom;
    int           findVisible;
    int           minimized;     /* this window is minimized */
    char          status[1024];
    ClosedTab     closed[MAX_CLOSED_TABS];
    int           closedCount;
} ShellWindow;

static HINSTANCE    g_instance;
static ShellWindow  g_windows[MAX_SHELL_WINDOWS];
static ShellWindow* g_cur;          /* window the UI thread is acting on */
static ShellWindow* g_lastActive;   /* handoff target = last active window */
static int          g_shellWindows; /* used entries in g_windows */

#define g_hwnd        (g_cur->hwnd)
#define g_address     (g_cur->address)
#define g_findEdit    (g_cur->findEdit)
#define g_tabs        (g_cur->tabs)
#define g_activeTab   (g_cur->activeTab)
#define g_zoom        (g_cur->zoom)
#define g_findVisible (g_cur->findVisible)
#define g_minimized   (g_cur->minimized)
#define g_status      (g_cur->status)

/* Find match counter, reported by the page's find script
 * ("__agb_findcount__3/12"). Process-wide: one find bar at a time. */
static int  g_findIndex;
static int  g_findTotal;
static char g_findQuery[256];      /* text of the last searched query */
static int  g_findComposing;       /* an IME composition owns the query field */
static int  g_findUpdatePending;   /* an AGW_FIND_UPDATE is already queued */
#define g_closed      (g_cur->closed)
#define g_closedCount (g_cur->closedCount)

/* Window lookup helpers: a message's hwnd (or a view, in a control callback)
 * names the window whose state may be touched, and also selects the context. */
static ShellWindow* windowForHwnd(HWND hwnd)
{
    HWND root;
    ShellWindow* win;

    if (!hwnd)
        return NULL;

    root = GetAncestor(hwnd, GA_ROOT);

    if (!root)
        root = hwnd;

    win = (ShellWindow*)GetWindowLongPtrA(root, GWLP_USERDATA);

    return (win && win->used) ? win : NULL;
}

static ShellWindow* windowForView(AgaveWebView* view)
{
    int i, t;

    for (i = 0; i < MAX_SHELL_WINDOWS; ++i) {
        ShellWindow* win = &g_windows[i];

        if (!win->used)
            continue;

        for (t = 0; t < MAX_TABS; ++t) {
            if (win->tabs[t].used && win->tabs[t].view == view)
                return win;
        }
    }

    return NULL;
}

static ShellWindow* firstUsedWindow(void)
{
    int i;

    for (i = 0; i < MAX_SHELL_WINDOWS; ++i) {
        if (g_windows[i].used)
            return &g_windows[i];
    }

    return NULL;
}

/* Control callbacks run on the UI thread but outside shellProc, so they carry
 * a Tab* and must switch the context themselves. */
static void enterTabWindow(Tab* tab)
{
    if (tab && tab->window)
        g_cur = (ShellWindow*)tab->window;
}

/* Drop a tab's decoded favicon (navigation, close, view rebuild). The model
 * snapshot borrows the pointer, so it is rebuilt by the next syncChrome(). */
static void tabClearIcon(Tab* tab)
{
    if (!tab || !tab->icon)
        return;

    uiFaviconFree(tab->icon);
    tab->icon = NULL;
    tab->iconCached = 0;
}

static void tabLoadCachedIcon(Tab* tab);

/* Who owns the keyboard. The page is a separate child window, so the shell has
 * to remember this: window activation restores the keyboard to whoever had it,
 * and clicking outside the page has to take it away from the page instead of
 * leaving two parties believing they own it. */
enum { FOCUS_PAGE = 0, FOCUS_ADDRESS, FOCUS_FIND, FOCUS_CHROME };

static int g_focusOwner = FOCUS_PAGE;

/* Host-side inspect marker (see ui_inspect.c): the page reports the target rect,
 * the shell paints it. Declared here because layoutAll/setActiveTab touch it. */
static UiInspect g_inspect;
static int       g_inspectOn;
static int       g_inspectReady;
static char      g_inspectLabel[256];
static char      g_inspectSelector[512];
static int       g_inspectCss[4];
static unsigned long g_inspectProbeTick;

/* Host-side inspect marker (see ui_inspect.c): the page reports the target rect,
 * the shell paints it. Declared here because layoutAll/setActiveTab touch it. */
static void focusPage(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    g_focusOwner = FOCUS_PAGE;

    if (tab->used && tab->view)
        agave_webview_set_focus(tab->view);
    else
        SetFocus(g_hwnd);
}

/* The shell is taking the keyboard (a chrome click, a caption press, the
 * address bar): tell the page so its caret/IME state stops believing it is
 * focused. */
static void releasePageFocus(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (tab->used && tab->view)
        agave_webview_kill_focus(tab->view);
}

#define AGW_MAX_BOOKMARKS 64
#define AGW_MAX_HISTORY   200

typedef struct { char url[MAX_PATH_CHARS]; char title[256]; } Bookmark;

static Bookmark g_bookmarks[AGW_MAX_BOOKMARKS];
static int      g_bookmarkCount;
static char     g_history[AGW_MAX_HISTORY][MAX_PATH_CHARS];
static int      g_historyCount;
static char     g_dataDir[MAX_PATH_CHARS];

/* ------------------------------------------------------------------------- */
/* data directory + persisted state                                          */
/* ------------------------------------------------------------------------- */

/* $AGB_USERDATA (if set) is the single profile root: bookmarks/session here and
 * cookies/localStorage/module cache in the engine. The headless probe points it
 * at its own directory so test runs never touch the user's profile. */
static void createDirTree(char* path)
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

static void dataDirInit(void)
{
    g_dataDir[0] = 0;

    /* The profile layout (AGB_USERDATA / APPDATA / LOCALAPPDATA / exe-relative
     * fallback) is an OS service now: shell_ops.c owns it and the mock table
     * gives tests a path that is not the developer's profile. */
    if (!shellOps()->profile_root(NULL, g_dataDir, sizeof(g_dataDir)) || !g_dataDir[0]) {
        const char* temp = getenv("TEMP");

        if (temp && temp[0]) {
            _snprintf(g_dataDir, sizeof(g_dataDir), "%s\\MiniShellData", temp);
            g_dataDir[sizeof(g_dataDir) - 1] = 0;
            createDirTree(g_dataDir);
        }
    }

    agbLog(AGB_LOG_SHELL, "profile root=%s (%s)",
           g_dataDir[0] ? g_dataDir : "(none)", shellOps()->name);

    agave_webview_set_profile_dir(g_dataDir);
}

/* Headless runs must not share the GUI profile: cookies, localStorage, the
 * module/HTTP caches and the shell state all live under $AGB_USERDATA, so the
 * probe gets its own root. AGB_USERDATA (env or --userdata) always wins, which
 * also lets parallel probes use disjoint directories. */
static void probeUserDataInit(void)
{
    char dir[MAX_PATH_CHARS];
    char env[MAX_PATH_CHARS + 32];
    const char* existing = getenv("AGB_USERDATA");

    dir[0] = 0;

    if (existing && existing[0]) {
        _snprintf(dir, sizeof(dir), "%s", existing);
    } else {
        const char* base = getenv("LOCALAPPDATA");

        if (!base || !base[0])
            base = getenv("TEMP");

        if (base && base[0])
            _snprintf(dir, sizeof(dir), "%s\\MiniShell\\probe", base);
    }

    if (dir[0]) {
        createDirTree(dir);
    }

    if (dir[0]) {
        _snprintf(env, sizeof(env), "AGB_USERDATA=%s", dir);
        putenv(env);
    }

    /* Shell state (bookmarks/session) follows the same root; nothing writes it
     * in probe mode, but a stray path must not land in the user profile. */
    if (dir[0]) {
        _snprintf(g_dataDir, sizeof(g_dataDir), "%s", dir);
        g_dataDir[sizeof(g_dataDir) - 1] = 0;
    }

    agbLog(AGB_LOG_PROBE, "userdata=%s", dir[0] ? dir : "(default: not isolated!)");
    agave_webview_set_profile_dir(g_dataDir);
    fflush(stderr);
}

static void statePath(char* out, size_t size, const char* name)
{
    _snprintf(out, size, "%s\\%s", g_dataDir, name);
    out[size - 1] = 0;
}

static void loadBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    char line[1400];
    FILE* file;

    g_bookmarkCount = 0;
    statePath(path, sizeof(path), "bookmarks.txt");
    file = fopen(path, "rb");

    if (!file)
        return;

    while (g_bookmarkCount < AGW_MAX_BOOKMARKS && fgets(line, sizeof(line), file)) {
        char* tab = strchr(line, '\t');
        size_t n;

        if (!tab)
            continue;

        *tab = 0;
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;

        strncpy(g_bookmarks[g_bookmarkCount].url, line, MAX_PATH_CHARS - 1);
        strncpy(g_bookmarks[g_bookmarkCount].title, tab + 1, sizeof(g_bookmarks[0].title) - 1);

        {
            char* title = g_bookmarks[g_bookmarkCount].title;
            size_t m = strlen(title);

            while (m && (title[m - 1] == '\n' || title[m - 1] == '\r'))
                title[--m] = 0;
        }

        ++g_bookmarkCount;
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "bookmarks loaded: %d entr%s from %s",
           g_bookmarkCount, g_bookmarkCount == 1 ? "y" : "ies", path);
}

/* Read the history back. It used to be append-only: recordHistory() wrote the
 * file but nothing ever loaded it, so the History menu only ever showed the
 * current session and every restart looked like "history lost". */
static void loadHistory(void)
{
    char path[MAX_PATH_CHARS];
    char line[1400];
    FILE* file;

    g_historyCount = 0;
    statePath(path, sizeof(path), "history.txt");
    file = fopen(path, "rb");

    if (!file)
        return;

    while (g_historyCount < AGW_MAX_HISTORY && fgets(line, sizeof(line), file)) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (len == 0)
            continue;
        if (g_historyCount > 0 && !strcmp(g_history[g_historyCount - 1], line))
            continue;

        strncpy(g_history[g_historyCount], line, MAX_PATH_CHARS - 1);
        g_history[g_historyCount][MAX_PATH_CHARS - 1] = 0;
        ++g_historyCount;
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "history loaded: %d entr%s from %s",
           g_historyCount, g_historyCount == 1 ? "y" : "ies", path);
}

/* ------------------------------------------------------------------------- */
/* theme preference                                                          */
/* ------------------------------------------------------------------------- */
/* Chrome's three appearance choices, switchable from the app menu at runtime:
 * "System" follows the OS (the shell already re-themes on WM_SETTINGCHANGE),
 * "Light"/"Dark" pin the chrome. The choice is remembered in
 * <profile>/theme.txt; AGB_SHELL_THEME still overrides it, so scripts and the
 * UI regression keep a deterministic shell. The PAGE's prefers-color-scheme is
 * not touched: it follows AGB_COLOR_SCHEME (see applySystemColorScheme), which
 * the engine reads once per document. */
enum { SHELL_THEME_SYSTEM = 0, SHELL_THEME_LIGHT, SHELL_THEME_DARK };

static int g_themePref = SHELL_THEME_SYSTEM;

static int themePrefFromName(const char* name, int* out)
{
    if (!name || !name[0])
        return 0;

    if (!_stricmp(name, "system") || !_stricmp(name, "auto")) {
        *out = SHELL_THEME_SYSTEM;
        return 1;
    }
    if (!_stricmp(name, "light")) {
        *out = SHELL_THEME_LIGHT;
        return 1;
    }
    if (!_stricmp(name, "dark")) {
        *out = SHELL_THEME_DARK;
        return 1;
    }

    return 0;
}

static const char* themePrefName(int pref)
{
    switch (pref) {
    case SHELL_THEME_LIGHT: return "light";
    case SHELL_THEME_DARK:  return "dark";
    default:                return "system";
    }
}

/* What the menu shows: the stored name is lowercase for the file, the label is
 * what a user reads. */
static const char* themePrefLabel(int pref)
{
    switch (pref) {
    case SHELL_THEME_LIGHT: return "Light";
    case SHELL_THEME_DARK:  return "Dark";
    default:                return "System";
    }
}

static void themePrefLoad(void)
{
    char path[MAX_PATH_CHARS];
    char line[64];
    char* end;
    int pref = SHELL_THEME_SYSTEM;
    const char* env;
    const char* source = "default";
    FILE* file;

    statePath(path, sizeof(path), "theme.txt");
    file = fopen(path, "rb");

    if (file) {
        if (fgets(line, sizeof(line), file)) {
            end = line + strlen(line);

            while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
                *--end = 0;

            if (themePrefFromName(line, &pref))
                source = "file";
            else
                agbLog(AGB_LOG_SHELL, "theme.txt: unknown value '%s' (using system)", line);
        }

        fclose(file);
    }

    /* The environment is the automation override and wins over the file. */
    env = getenv("AGB_SHELL_THEME");

    if (env && env[0]) {
        int fromEnv;

        if (themePrefFromName(env, &fromEnv)) {
            pref = fromEnv;
            source = "env";
        } else {
            agbLog(AGB_LOG_SHELL, "AGB_SHELL_THEME='%s' ignored (want system|light|dark)", env);
        }
    }

    g_themePref = pref;
    agbLog(AGB_LOG_SHELL, "theme pref=%s (%s)", themePrefName(g_themePref), source);
}

static void themePrefSave(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    statePath(path, sizeof(path), "theme.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "theme: cannot write %s", path);
        return;
    }

    fprintf(file, "%s\n", themePrefName(g_themePref));
    fclose(file);
}

/* ------------------------------------------------------------------------- */
/* preferences (the settings page)                                           */
/* ------------------------------------------------------------------------- */

/* Everything the settings page can change, persisted in <profile>/prefs.txt as
 * key=value lines. The theme keeps its own theme.txt (the app menu and the env
 * override already write there); this file holds the rest. */
typedef struct {
    int search;    /* 0 Baidu, 1 Bing, 2 Google, 3 DuckDuckGo */
    int zoom;      /* default page zoom for new tabs, percent */
    int restore;   /* 1 = reopen last session's tabs on startup */
} ShellPreferences;

static ShellPreferences g_prefs = { 0, 100, 1 };

static const char* kEngineNames[] = { "baidu", "bing", "google", "duckduckgo" };
static const char* kEngineLabels[] = { "Baidu", "Bing", "Google", "DuckDuckGo" };
static const char* kEngineSearch[] = {
    "https://www.baidu.com/s?wd=",
    "https://www.bing.com/search?q=",
    "https://www.google.com/search?q=",
    "https://duckduckgo.com/?q="
};

#define ENGINE_COUNT ((int)(sizeof(kEngineNames) / sizeof(kEngineNames[0])))

static void prefsSave(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    statePath(path, sizeof(path), "prefs.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "prefs: cannot write %s", path);
        return;
    }

    fprintf(file, "search=%s\nzoom=%d\nrestore=%d\n",
            kEngineNames[g_prefs.search], g_prefs.zoom, g_prefs.restore);
    fclose(file);
}

static void prefsLoad(void)
{
    char path[MAX_PATH_CHARS];
    char line[256];
    FILE* file;

    statePath(path, sizeof(path), "prefs.txt");
    file = fopen(path, "rb");

    if (!file)
        return;

    while (fgets(line, sizeof(line), file)) {
        char* value = strchr(line, '=');
        char* end;

        if (!value)
            continue;

        *value++ = 0;

        end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;

        if (!strcmp(line, "search")) {
            int i;

            for (i = 0; i < ENGINE_COUNT; ++i) {
                if (!_stricmp(value, kEngineNames[i])) {
                    g_prefs.search = i;
                    break;
                }
            }
        } else if (!strcmp(line, "zoom")) {
            int zoom = atoi(value);

            if (zoom >= 25 && zoom <= 400)
                g_prefs.zoom = zoom;
        } else if (!strcmp(line, "restore")) {
            g_prefs.restore = atoi(value) ? 1 : 0;
        }
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "prefs: search=%s zoom=%d restore=%d",
           kEngineNames[g_prefs.search], g_prefs.zoom, g_prefs.restore);
}

/* Profile-backed shell state. Both the GUI and the probe need it: the probe
 * reads history/bookmarks too (a probe run could otherwise not verify that the
 * files are loaded), and it must not re-implement the profile layout. */
static void shellStateInit(void)
{
    dataDirInit();
    themePrefLoad();
    prefsLoad();
    loadBookmarks();
    loadHistory();
}

static void saveBookmarks(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int i;

    statePath(path, sizeof(path), "bookmarks.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "bookmarks: cannot write %s", path);
        return;
    }

    for (i = 0; i < g_bookmarkCount; ++i)
        fprintf(file, "%s\t%s\n", g_bookmarks[i].url,
                g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url);

    fclose(file);
}

/* 1 when the bookmark was added, 0 when nothing changed (empty URL, duplicate,
 * table full). Callers have to tell the difference: the shell used to print
 * "Bookmark added" even for a URL that was already saved. */
static int addBookmark(const char* url, const char* title)
{
    int i;

    if (!url || !url[0] || g_bookmarkCount >= AGW_MAX_BOOKMARKS)
        return 0;

    for (i = 0; i < g_bookmarkCount; ++i) {
        if (!strcmp(g_bookmarks[i].url, url))
            return 0;
    }

    strncpy(g_bookmarks[g_bookmarkCount].url, url, MAX_PATH_CHARS - 1);
    g_bookmarks[g_bookmarkCount].url[MAX_PATH_CHARS - 1] = 0;
    strncpy(g_bookmarks[g_bookmarkCount].title, title ? title : "", sizeof(g_bookmarks[0].title) - 1);
    g_bookmarks[g_bookmarkCount].title[sizeof(g_bookmarks[0].title) - 1] = 0;
    ++g_bookmarkCount;
    saveBookmarks();
    return 1;
}

static void recordHistory(const char* url)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    if (!url || !url[0])
        return;

    /* Internal pages (new tab, settings, downloads) are not browsing history. */
    if (!_strnicmp(url, "minishell://", 12))
        return;

    if (g_historyCount > 0 && !strcmp(g_history[g_historyCount - 1], url))
        return;

    if (g_historyCount == AGW_MAX_HISTORY) {
        memmove(g_history[0], g_history[1], sizeof(g_history[0]) * (AGW_MAX_HISTORY - 1));
        --g_historyCount;
    }

    strncpy(g_history[g_historyCount], url, MAX_PATH_CHARS - 1);
    g_history[g_historyCount][MAX_PATH_CHARS - 1] = 0;
    ++g_historyCount;

    statePath(path, sizeof(path), "history.txt");
    file = fopen(path, "ab");

    if (file) {
        fprintf(file, "%s\n", url);
        fclose(file);
    } else {
        /* Silent before: "history keeps disappearing" started here. The errno
         * number (and its text) is the point: the path alone cannot tell a
         * locked/read-only profile from a full disk or a vanished directory. */
        int savedErrno = errno;

        agbLog(AGB_LOG_SHELL, "history: cannot append to %s (errno=%d: %s)",
               path, savedErrno, strerror(savedErrno));
    }
}

/* The settings page's "Clear history": empty the in-memory ring and the file.
 * recordHistory() only ever appends, so this is the one place that truncates. */
static void clearHistoryAll(void)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    g_historyCount = 0;

    statePath(path, sizeof(path), "history.txt");
    file = fopen(path, "wb");

    if (file) {
        fclose(file);
    } else {
        int savedErrno = errno;

        agbLog(AGB_LOG_SHELL, "history: cannot truncate %s (errno=%d: %s)",
               path, savedErrno, strerror(savedErrno));
    }
}

static void saveSession(void)
{    char path[MAX_PATH_CHARS];
    FILE* file;
    int i, w;
    int saved = 0;

    statePath(path, sizeof(path), "session.txt");
    file = fopen(path, "wb");

    if (!file) {
        agbLog(AGB_LOG_SHELL, "session: cannot write %s", path);
        return;
    }

    /* Flat on purpose: every window's tabs go into one list, so a restart
     * restores one window with all of them (the window structure itself is not
     * persisted yet; see design-shell-singleton.md). */
    for (w = 0; w < MAX_SHELL_WINDOWS; ++w) {
        ShellWindow* win = &g_windows[w];

        if (!win->used)
            continue;

        for (i = 0; i < MAX_TABS; ++i) {
            if (win->tabs[i].used && win->tabs[i].url[0]) {
                fprintf(file, "%s\n", win->tabs[i].url);
                ++saved;
            }
        }
    }

    fclose(file);
    agbLog(AGB_LOG_SHELL, "session saved tabs=%d -> %s", saved, path);
}

static int loadSession(char urls[][MAX_PATH_CHARS], int max)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    int count = 0;

    statePath(path, sizeof(path), "session.txt");
    file = fopen(path, "rb");

    if (!file)
        return 0;

    while (count < max && fgets(urls[count], MAX_PATH_CHARS, file)) {
        size_t n = strlen(urls[count]);

        while (n && (urls[count][n - 1] == '\n' || urls[count][n - 1] == '\r'))
            urls[count][--n] = 0;

        if (urls[count][0])
            ++count;
    }

    fclose(file);
    return count;
}

/* ------------------------------------------------------------------------- */
/* layout                                                                    */
/* ------------------------------------------------------------------------- */

/* The engine speaks UTF-8; Win32 controls and GDI speak UTF-16. Everything that
 * shows page-provided text goes through these helpers (the tab used to print
 * UTF-8 bytes with DrawTextA -> mojibake). */
static void utf8ToWide(const char* source, wchar_t* target, int count)
{
    if (count <= 0)
        return;

    if (!source) {
        target[0] = 0;
        return;
    }

    if (!MultiByteToWideChar(CP_UTF8, 0, source, -1, target, count))
        target[0] = 0;

    target[count - 1] = 0;
}

static void wideToUtf8(const wchar_t* source, char* target, int count)
{
    if (count <= 0)
        return;

    if (!source) {
        target[0] = 0;
        return;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, source, -1, target, count, NULL, NULL))
        target[0] = 0;

    target[count - 1] = 0;
}

/* Reads a control back as UTF-8. The address bar is a Unicode edit control
 * (CreateWindowExW), so GetWindowTextA would push paths through the ANSI code
 * page and lose every character outside it. */
static void readControlText(HWND control, char* out, size_t outSize)
{
    wchar_t wide[MAX_PATH_CHARS];

    if (!outSize)
        return;

    wide[0] = 0;
    GetWindowTextW(control, wide, MAX_PATH_CHARS);
    wideToUtf8(wide, out, (int)outSize);
}

static int nextFreeTab(void)
{
    int i;

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            return i;
    }

    return -1;
}

/* One place that turns ShellWindow state into the chrome's value snapshot.
 * Components never look at Tab/ShellWindow; the UI test feeds this struct
 * directly. Called from paint/status/layout paths, so it must stay cheap. */
static void syncChrome(void)
{
    UiChromeModel* model;
    Tab* tab;
    int i;

    if (!g_cur)
        return;

    model = &g_cur->chrome.model;
    memset(model, 0, sizeof(*model));

    tab = &g_tabs[g_activeTab];
    model->active = g_activeTab;

    if (tab->used && tab->view) {
        model->can_back = agave_webview_can_back(tab->view) ? 1 : 0;
        model->can_forward = agave_webview_can_forward(tab->view) ? 1 : 0;
        model->loading = tab->loading;
        model->progress = tab->progress;

        /* The new tab page is an empty omnibox, like every browser's: showing
         * "minishell://newtab" there reads like a web address. */
        if (!strcmp(tab->url, NEW_TAB_URL))
            model->address[0] = 0;
        else
            _snprintf(model->address, sizeof(model->address), "%s", tab->url);
    }

    _snprintf(model->status, sizeof(model->status), "%s", g_status);
    model->zoom = g_zoom;
    model->find_visible = g_findVisible;
    model->find_index = g_findIndex;
    model->find_total = g_findTotal;
    model->maximized = g_hwnd && IsZoomed(g_hwnd) ? 1 : 0;

    if (g_findVisible && g_findEdit)
        readControlText(g_findEdit, model->find_text, sizeof(model->find_text));

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        model->tabs[i].used = 1;
        _snprintf(model->tabs[i].title, sizeof(model->tabs[i].title), "%s",
                  g_tabs[i].title[0] ? g_tabs[i].title : "New tab");
        _snprintf(model->tabs[i].url, sizeof(model->tabs[i].url), "%s", g_tabs[i].url);
        model->tabs[i].hung = g_tabs[i].hangPending;
        model->tabs[i].icon = g_tabs[i].icon;
    }

    model->address[sizeof(model->address) - 1] = 0;
    model->status[sizeof(model->status) - 1] = 0;

    uiChromeSync(&g_cur->chrome);
}

static void layoutTab(int index)
{
    UiRect content;

    if (!g_cur || !g_tabs[index].used || !g_tabs[index].view)
        return;

    content = g_cur->chrome.content;

    if (content.w < 1)
        content.w = 1;
    if (content.h < 1)
        content.h = 1;

    agave_webview_set_rect(g_tabs[index].view, content.x, content.y,
                           content.w, content.h);
}

/* Windows DPI as "percent of 96" - the engine's device pixels per CSS pixel.
 * Rounded, not truncated: 120 DPI is 125%, 144 is 150%. */
static int deviceScalePercent(unsigned dpi)
{
    if (dpi < 48)
        dpi = 96;

    return (int)((dpi * 100u + 48u) / 96u);
}

/* The page half of the window's DPI, pushed to every tab of the active window.
 * A GUI window has always scaled its own chrome; until this existed the page
 * stayed at 1 CSS px = 1 device px, so at 150% the toolbar was 1.5x and the
 * page 0.67x, and window.devicePixelRatio lied (1). The engine multiplies this
 * with the user zoom, so Ctrl+= stays a zoom (see
 * docs/design-hidpi.md). Called on tab create and on every DPI change. */
static void syncTabsDeviceScale(void)
{
    int percent;
    int i;

    if (!g_cur)
        return;

    percent = deviceScalePercent(g_cur->chrome.theme.dpi);

    /* Once per call, not per tab: "why is this page 1.25x" is otherwise
     * invisible in the log (the engine has no line for it either). */
    if (percent != 100)
        agbLog(AGB_LOG_SHELL, "page device scale %d%% (window dpi %u)",
               percent, g_cur->chrome.theme.dpi);

    for (i = 0; i < MAX_TABS; ++i) {
        Tab* tab = &g_tabs[i];

        if (tab->used && tab->view && tab->window == g_cur)
            agave_webview_set_device_scale(tab->view, percent);
    }
}

/* Move a child only when it actually has to move. MoveWindow(..., TRUE) repaints
 * the control every time, and the toolbar is laid out on every WM_SIZE - a load
 * that makes the scrollbar appear/disappear fires those repeatedly, which is why
 * the "<" / ">" captions appeared to flicker during reloads. */
static void placeChild(HWND child, int x, int y, int w, int h)
{
    RECT r;
    POINT origin = { 0, 0 };

    if (!child)
        return;

    if (!GetWindowRect(child, &r))
        return;

    MapWindowPoints(NULL, g_hwnd, (POINT*)&r, 2);
    origin.x = r.left;
    origin.y = r.top;

    if (origin.x == x && origin.y == y &&
        (r.right - r.left) == w && (r.bottom - r.top) == h)
        return;

    MoveWindow(child, x, y, w, h, TRUE);
}

static HWND createFindOverlay(ShellWindow* win);
static void invalidateFindOverlay(void);

/* Status bubble: only while there is a message; "Ready" is not a message, and
 * the page keeps its full height either way. Called from layoutAll() and whenever
 * the status text changes. */
static void syncStatusOverlay(void);
static HWND createStatusOverlay(ShellWindow* win);
static void syncTabSearch(void);
static void toggleTabSearchPopup(void);

static void layoutAll(void)
{
    RECT client;
    int i;
    UiRect edit;

    if (!g_cur || !g_hwnd)
        return;

    GetClientRect(g_hwnd, &client);

    syncChrome();
    uiChromeLayout(CHROME, client.right, client.bottom);

    edit = CHROME->address_edit;
    placeChild(g_address, edit.x, edit.y, edit.w, edit.h);

    /* The find bar floats over the page: its own child window sits at
     * chrome->findbar.rect, the native EDIT inside it stays a child of the
     * shell (WM_CTLCOLOREDIT keeps working) and is raised above the overlay. */
    if (g_findVisible) {
        if (!g_cur->findBar)
            g_cur->findBar = createFindOverlay(g_cur);

        if (g_cur->findBar) {
            const UiRect* card = &CHROME->findbar.rect;

            placeChild(g_cur->findBar, card->x, card->y, card->w, card->h);
            ShowWindow(g_cur->findBar, SW_SHOW);
            SetWindowPos(g_cur->findBar, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            InvalidateRect(g_cur->findBar, NULL, FALSE);
        }

        edit = CHROME->find_edit;
        ShowWindow(g_findEdit, SW_SHOW);
        placeChild(g_findEdit, edit.x, edit.y, edit.w, edit.h);
        SetWindowPos(g_findEdit, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        if (g_cur->findBar)
            ShowWindow(g_cur->findBar, SW_HIDE);
        ShowWindow(g_findEdit, SW_HIDE);
    }

    syncStatusOverlay();

    if (g_inspectReady)
        uiInspectSetContentRect(&g_inspect, CHROME->content.x, CHROME->content.y,
                                CHROME->content.w, CHROME->content.h);

    for (i = 0; i < MAX_TABS; ++i)
        layoutTab(i);
}

/* The "Loading <url>... 42%" bubble was kept but is no longer shown: the URL just
 * echoed the address bar and the percent comes from the engine's coarse progress
 * callback, so it read as a fake progress display. The code stays behind this
 * flag instead of being deleted - a real loading state may come back. */
#define STATUS_SHOW_LOADING 0

static void updateStatus(void)
{
    Tab* tab = &g_tabs[g_activeTab];
    char text[1024];

    /* The bubble only exists while it has something to say: a message (hover
     * link / download / error / bookmark / inspect target). The plain URL is not
     * a message - a bubble that never hides is just the old status bar wearing a
     * costume. */
    text[0] = 0;

    if (tab->used) {
        if (STATUS_SHOW_LOADING && tab->loading)
            _snprintf(text, sizeof(text), "Loading %s... %d%%", tab->url, tab->progress);
        else if (tab->status[0])
            _snprintf(text, sizeof(text), "%s", tab->status);
    }

    text[sizeof(text) - 1] = 0;

    /* Hovering a link and a loading page both fire this on every event, and
     * every call used to re-sync the model, invalidate the whole window and
     * re-place the bubble. The bubble is identical when the text is - skip
     * then, so a mouse move over the same link costs nothing. */
    if (strcmp(text, g_status) == 0)
        return;

    strncpy(g_status, text, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = 0;

    if (!g_hwnd) {
        syncChrome();
        return;
    }

    /* The bubble's rect comes from the chrome layout, and layoutAll() is the only
     * place that recomputes it. Skipping it here left the previous message's box
     * in place: an emptied status kept an empty pill on screen, and a longer
     * message stayed clipped to the old width until the window was resized. */
    layoutAll();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void applyZoomToActive(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (tab->used && tab->view)
        agave_webview_set_zoom(tab->view, g_zoom);
}

/* ------------------------------------------------------------------------- */
/* tabs                                                                      */
/* ------------------------------------------------------------------------- */

static void setActiveTab(int index);
static void newTabSendTheme(AgaveWebView* view);
static void switchTabWarm(int index);
static int  createTabIn(int index, const char* url);
static void createTab(const char* url);
static void armNavGuard(void);
static void navGuardWatch(Tab* tab, const char* url);
static void navGuardSettled(Tab* tab);
static void toggleInspect(void);
static void inspectStop(const char* source);
static void inspectOnMove(void* user, int x, int y);
static void inspectOnClick(void* user, int x, int y, int button);
static void updateWindowTitle(void);
static void onCloseWindow(AgaveWebView* view, void* user);
static void onActivateView(AgaveWebView* view, void* user);
static void probeCrashPage(AgaveWebView* view, const char* html, const char* base,
                            int code, const char* report);
/* Set when --probe was given: defined with the probe harness further down, used
 * by the callbacks above. */
static int g_probeMode;

/* The control delivers state callbacks (progress/title/url/status) on the thread
 * that owns its window; the probe checks that once so a regression fails a suite
 * instead of showing up as an intermittent cross-thread AV (R185). */
static DWORD g_probeUiThread;
static int   g_probeCallbackThreadChecked;
static int   g_probeCallbackThreadBad;

/* Page Visibility for the whole shell: exactly the active tab is visible, and
 * nothing is when the window is minimized (Chrome does the same, and a minimized
 * window's views must stop laying out and painting). Sent only on a change: the
 * control's engine loop keys the paint skip off it. */
static void syncTabVisibility(void)
{
    int i;

    for (i = 0; i < MAX_TABS; ++i) {
        int want;

        if (!g_tabs[i].used || !g_tabs[i].view)
            continue;

        want = (i == g_activeTab && !g_minimized) ? 1 : 0;

        if (g_tabs[i].visibilitySent != want) {
            g_tabs[i].visibilitySent = want;
            agave_webview_set_visible(g_tabs[i].view, want);
        }
    }
}

/* Set while closeTab() activates the tab that replaces the closed one: that is
 * part of the close, not a user switch, so it must not drop the slide. */
static int g_closingTab;

static void setActiveTab(int index)
{
    int i;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    /* A user tab switch ends the close slide: its frozen active width belongs to
     * the tab that is no longer active. The activation closeTab() performs is
     * part of the close itself and keeps it. */
    if (!g_closingTab && g_cur)
        uiChromeEndCloseSlide(CHROME);

    if (g_inspectOn && index != g_activeTab)
        inspectStop("tab switch");

    g_tabs[index].activeMs = shellOps()->tick_ms(NULL);

    if (index != g_activeTab || !g_tabs[index].hwnd)
        agbLog(AGB_LOG_SHELL, "tab activate index=%d (was %d) url=%s",
               index, g_activeTab, g_tabs[index].url);

    g_activeTab = index;

    syncTabVisibility();

    /* Lay the view out before showing it: a view shown at its previous rect for
     * one frame paints a page that covers only part of the content area (and
     * the rest is the shell's background) - that snap is the "torn page area"
     * when a tab is opened. */
    layoutTab(index);

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && g_tabs[i].hwnd)
            ShowWindow(g_tabs[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
    }

    applyZoomToActive();

    /* The new tab page follows the chrome's light/dark choice. The page also
     * asks on boot; this catches a tab that was inactive while the theme
     * changed. */
    if (g_tabs[index].used && g_tabs[index].view &&
        !strcmp(g_tabs[index].url, NEW_TAB_URL))
        newTabSendTheme(g_tabs[index].view);

    if (g_tabs[index].view) {
        wchar_t wideAddress[MAX_PATH_CHARS];

        /* Same as syncChrome(): the new tab page is an empty omnibox. */
        if (!strcmp(g_tabs[index].url, NEW_TAB_URL)) {
            wideAddress[0] = 0;
        } else {
            utf8ToWide(g_tabs[index].url, wideAddress, MAX_PATH_CHARS);
        }
        SetWindowTextW(g_address, wideAddress);
    }

    /* Keyboard ownership follows the tab: a page that had the keyboard hands it
     * to the new tab's page, while a focused address/find field keeps it (so
     * Ctrl+Tab in the middle of typing does not steal the text field). */
    if (g_focusOwner == FOCUS_ADDRESS && g_address)
        SetFocus(g_address);
    else if (g_focusOwner == FOCUS_FIND && g_findEdit && g_findVisible)
        SetFocus(g_findEdit);
    else if (g_focusOwner == FOCUS_PAGE && g_tabs[index].view)
        agave_webview_set_focus(g_tabs[index].view);

    updateStatus();
    updateWindowTitle();

    /* The tab strip's slot rects change when a tab is added/removed, and hit
     * testing reads those rects. Refresh them before the next mouse message;
     * without this a newly created first tab is painted nowhere and its
     * client area is treated as caption. */
    layoutAll();
    InvalidateRect(g_hwnd, NULL, FALSE);

    /* A tab that hung while it was in the background asks now that the user is
     * looking at it. Posted so the prompt never runs inside setActiveTab
     * (which is called from tab creation and from the tab strip). */
    if (g_tabs[index].hangPending && index == g_activeTab)
        PostMessageA(g_hwnd, AGW_SHELL_HANG_PROMPT, (WPARAM)index, 0);
}

/* --- cold tab switches: render a frame, then switch ------------------------
 *
 * Showing a tab paints its last frame immediately. After a resize while the tab
 * was in the background its surface was rebuilt and the engine, which skips
 * layout+paint for hidden views, had no frame to publish - so the switch showed
 * an empty client for a frame or two (a flash). Instead of showing the empty
 * surface, ask the engine for one frame and finish the switch when it lands
 * (one or two engine ticks, ~6-12 ms), leaving the old tab on screen meanwhile.
 * A target that already has a frame switches immediately, which is the common
 * case; a page that cannot produce one (hung, crashed) still switches after a
 * short timeout so a click never feels stuck. */
#define WARM_TAB_TIMER      1
#define WARM_TAB_TIMEOUT_MS 120
#define WARM_TAB_POLL_MS    5

static int           g_warmTab = -1;
static unsigned long g_warmSince;

static void cancelWarmSwitch(void)
{
    g_warmTab = -1;

    if (g_hwnd)
        KillTimer(g_hwnd, WARM_TAB_TIMER);
}

static void finishWarmSwitch(void)
{
    int index = g_warmTab;

    if (index >= 0)
        agbLog(AGB_LOG_SHELL, "tab switch warm settled index=%d waited=%lums",
               index, (unsigned long)(GetTickCount() - g_warmSince));

    cancelWarmSwitch();

    if (index >= 0 && index < MAX_TABS && g_tabs[index].used && index != g_activeTab)
        setActiveTab(index);
}

/* User-initiated switch (tab click, Ctrl+Tab). Internal flows (new tab, close,
 * session restore, window.open) keep calling setActiveTab directly: they own
 * the ordering and are not about to show a stale surface. */
static void switchTabWarm(int index)
{
    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used || index == g_activeTab)
        return;

    /* A newer click supersedes a pending switch. */
    cancelWarmSwitch();

    if (!g_tabs[index].view || agave_webview_has_frame(g_tabs[index].view)) {
        setActiveTab(index);
        return;
    }

    /* The surface may still be the pre-resize one; layoutTab posts the resize,
     * request_frame asks for the frame, and the engine applies both in the same
     * iteration. */
    layoutTab(index);
    agave_webview_request_frame(g_tabs[index].view);

    g_warmTab = index;
    g_warmSince = GetTickCount();

    agbLog(AGB_LOG_SHELL, "tab switch warm index=%d (no frame yet)", index);

    if (g_hwnd)
        SetTimer(g_hwnd, WARM_TAB_TIMER, WARM_TAB_POLL_MS, NULL);
}

static int anyTabOpen(void)
{
    int i;

    for (i = 0; i < MAX_TABS; ++i)
        if (g_tabs[i].used)
            return 1;

    return 0;
}

static int countUsedTabs(void)
{
    int i;
    int count = 0;

    for (i = 0; i < MAX_TABS; ++i)
        if (g_tabs[i].used)
            ++count;

    return count;
}

static int nextUsedTab(int index)
{
    int i;

    for (i = index + 1; i < MAX_TABS; ++i)
        if (g_tabs[i].used)
            return i;

    return -1;
}

/* Test-only cursor: AGENTS forbids touching the real pointer, so the headless
 * close-slide checks substitute the client position here. */
static int   g_testCursorSet;
static POINT g_testCursor;
static int   g_testCloseLeft;   /* repeated fake X-clicks still to do */

static int shellCursorClient(HWND hwnd, POINT* point)
{
    if (g_testCursorSet) {
        *point = g_testCursor;
        return 1;
    }

    if (!GetCursorPos(point))
        return 0;
    return ScreenToClient(hwnd, point) ? 1 : 0;
}

/* 1 when the pointer is over a tab's close chip right now. The close slide is
 * armed from this, not from the command that closed the tab: a middle click on
 * the tab body or Ctrl+W must keep the normal relayout. */
static int pointerOnCloseChip(int index)
{
    POINT point;

    if (!g_cur || index < 0 || index >= MAX_TABS)
        return 0;
    if (CHROME->tabstrip.close[index].w <= 0)
        return 0;
    if (!shellCursorClient(g_hwnd, &point))
        return 0;

    return uiRectContains(CHROME->tabstrip.close[index], point.x, point.y);
}

/* Hover is set by mouse moves; right after a slide the pointer did not move, so
 * the chip that landed under it would look idle until the next move. */
static void refreshTabHoverFromCursor(void)
{
    UiTabStrip* strip;
    POINT point;
    int i;

    if (!g_cur)
        return;

    strip = &CHROME->tabstrip;
    strip->hover_tab = -1;
    strip->hover_close = -1;

    if (!shellCursorClient(g_hwnd, &point))
        return;

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];

        if (uiRectContains(strip->close[index], point.x, point.y)) {
            strip->hover_tab = index;
            strip->hover_close = index;
            return;
        }

        if (uiRectContains(strip->tab[index], point.x, point.y)) {
            strip->hover_tab = index;
            return;
        }
    }
}

static void closeTab(int index)
{
    int i;
    int wasActive;
    int slide;
    int fill;
    char url[MAX_PATH_CHARS];

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    wasActive = (index == g_activeTab);
    strncpy(url, g_tabs[index].url, sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;

    /* Clicked that tab's own close chip: remember the strip's widths so the
     * hole closes by sliding the tabs to the right one slot left. The next tab
     * (promoted if this one was active) then sits exactly under the pointer
     * with its chip, so the same click can close it too. Captured before the
     * teardown: the strip still has the pre-close geometry. */
    fill = nextUsedTab(index);

    if (uiChromeCloseSliding(CHROME) ||
        (pointerOnCloseChip(index) && fill >= 0 &&
         CHROME->tabstrip.count == CHROME->tabstrip.total)) {
        /* An active slide keeps going (even when this close leaves nothing on
         * the right) until the pointer leaves the tab host; a fresh one only
         * starts on a close-chip click with a tab to slide into the hole. */
        uiChromeBeginCloseSlide(CHROME, index, fill);
        slide = uiChromeCloseSliding(CHROME);
    } else {
        uiChromeEndCloseSlide(CHROME);
        slide = 0;
    }

    tabClearIcon(&g_tabs[index]);

    /* Remember the page so ctrl+shift+T can reopen it. */
    if (g_tabs[index].url[0] && strcmp(g_tabs[index].url, "about:blank") != 0) {
        if (g_closedCount == MAX_CLOSED_TABS) {
            memmove(&g_closed[0], &g_closed[1], sizeof(ClosedTab) * (MAX_CLOSED_TABS - 1));
            g_closedCount--;
        }
        strncpy(g_closed[g_closedCount].url, g_tabs[index].url, MAX_PATH_CHARS - 1);
        g_closed[g_closedCount].url[MAX_PATH_CHARS - 1] = 0;
        strncpy(g_closed[g_closedCount].title, g_tabs[index].title, sizeof(g_closed[0].title) - 1);
        g_closed[g_closedCount].title[sizeof(g_closed[0].title) - 1] = 0;
        g_closed[g_closedCount].closedMs = shellOps()->tick_ms(NULL);
        g_closedCount++;
    }

    if (g_tabs[index].view)
        agave_webview_destroy(g_tabs[index].view);

    memset(&g_tabs[index], 0, sizeof(Tab));

    if (g_activeTab == index) {
        int prev = -1;

        /* Browsers activate the tab to the right; only a closed rightmost tab
         * falls back to the one on its left. */
        for (i = 0; i < index; ++i) {
            if (g_tabs[i].used)
                prev = i;
        }

        g_closingTab = 1;
        if (fill >= 0)
            setActiveTab(fill);
        else if (prev >= 0)
            setActiveTab(prev);
        g_closingTab = 0;

        if (fill < 0 && prev < 0) {
            g_activeTab = 0;
            updateStatus();
        }
    }

    agbLog(AGB_LOG_SHELL, "tab close index=%d url=%s active=%d closed-saved=%d",
           index, url, wasActive, g_closedCount);

    /* Last tab in this window: close the window, the way every browser does
     * (with one window, that ends the process). Posted, not DestroyWindow():
     * closeTab runs inside mouse/paint handlers and inside loops such as
     * closeTabsToRight, so the window must go after the handler returns. */
    if (!anyTabOpen()) {
        agbLog(AGB_LOG_SHELL, "tab close: no tabs left, closing the window (pid=%lu)",
               (unsigned long)GetCurrentProcessId());
        PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    }

    layoutAll();

    /* The pointer did not move, so the chip that just landed under it has no
     * hover from a mouse message: set it here. */
    if (slide) {
        refreshTabHoverFromCursor();
        agbLog(AGB_LOG_SHELL, "close slide: widths frozen, next tab under pointer (pid=%lu)",
               (unsigned long)GetCurrentProcessId());
        InvalidateRect(g_hwnd, &CHROME->tabstrip.rect, FALSE);
    }

    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* The close slide lasts while the pointer stays inside the tab host. Any other
 * layout change (resize, DPI) drops it too. */
static void shellCheckCloseSlide(HWND hwnd)
{
    POINT point;
    UiRect host;

    if (!g_cur || !uiChromeCloseSliding(CHROME))
        return;

    host = CHROME->tabstrip.rect;

    if (shellCursorClient(hwnd, &point) && uiRectContains(host, point.x, point.y))
        return;

    uiChromeEndCloseSlide(CHROME);
    agbLog(AGB_LOG_SHELL, "close slide: pointer left the tab strip, relayout (pid=%lu)",
           (unsigned long)GetCurrentProcessId());
    layoutAll();
    InvalidateRect(hwnd, &CHROME->tabstrip.rect, FALSE);
}

/* Reopen a specific recently-closed entry (the tab-search popup indexes into
 * the same array the snapshot handed out). */
static void reopenClosedAt(int slot);

/* ctrl+shift+T: reopen the tab that was closed last. */
static void reopenClosedTab(void)
{
    reopenClosedAt(g_closedCount - 1);
}

static void reopenClosedAt(int slot)
{
    ClosedTab closed;
    int i;

    if (slot < 0 || slot >= g_closedCount) {
        agbLog(AGB_LOG_SHELL, "tab reopen: nothing was closed");
        return;
    }

    closed = g_closed[slot];
    for (i = slot; i < g_closedCount - 1; ++i)
        g_closed[i] = g_closed[i + 1];
    g_closedCount--;

    agbLog(AGB_LOG_SHELL, "tab reopen url=%s left=%d", closed.url, g_closedCount);
    createTab(closed.url);
}

/* ctrl+tab / ctrl+shift+tab: walk through the open tabs. */
static void switchTab(int direction)
{
    int i, count = 0, index = -1, slot = 0;

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        if (i == g_activeTab)
            index = slot;

        ++slot;
        ++count;
    }

    if (count < 2) {
        agbLog(AGB_LOG_SHELL, "tab switch: only %d tab(s) open", count);
        return;
    }

    index = (index + direction + count) % count;

    slot = 0;
    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        if (slot == index) {
            switchTabWarm(i);
            return;
        }

        ++slot;
    }
}

/* ------------------------------------------------------------------------- */
/* control callbacks (engine thread)                                         */
/* ------------------------------------------------------------------------- */

static void onTitle(AgaveWebView* view, const char* title, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    enterTabWindow(tab);

    strncpy(tab->title, title ? title : "", sizeof(tab->title) - 1);
    tab->title[sizeof(tab->title) - 1] = 0;

    if (tab == &g_tabs[g_activeTab])
        updateWindowTitle();

    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void onUrl(AgaveWebView* view, const char* url, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab || !url)
        return;

    enterTabWindow(tab);

    /* A new document drops the previous site's icon: the old one must not sit
     * on a tab while the new page loads. The engine sends the new icon once it
     * has it (on_favicon) or requestFavicon() fetches it after the load. */
    {
        int changed = strcmp(tab->url, url) != 0;

        if (changed) {
            tabClearIcon(tab);
            tab->iconPage[0] = 0;
            tab->iconCandidates[0] = 0;
            tab->iconNext = 0;
            tab->iconTried = 0;
            tab->iconFetching = 0;
        }

        strncpy(tab->url, url, MAX_PATH_CHARS - 1);
        tab->url[MAX_PATH_CHARS - 1] = 0;

        /* A page visited before shows its cached icon immediately; the load
         * then refreshes it in the background. */
        if (changed) {
            tabLoadCachedIcon(tab);

            if (tab->icon) {
                syncChrome();
                InvalidateRect(g_hwnd, &g_cur->chrome.tabstrip.rect, FALSE);
            }
        }
    }

    /* The engine committed this document: the navigation guard can stand down. */
    navGuardSettled(tab);

    if (tab == &g_tabs[g_activeTab]) {
        if (GetFocus() != g_address) {
            wchar_t wideUrl[MAX_PATH_CHARS];

            utf8ToWide(tab->url, wideUrl, MAX_PATH_CHARS);
            SetWindowTextW(g_address, wideUrl);
        }

        recordHistory(tab->url);
    }
}

static void requestFavicon(Tab* tab);
static void startFaviconFetch(Tab* tab, const char* pageCandidates);

static void onProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    enterTabWindow(tab);

    tab->progress = percent;
    tab->loading = finished ? 0 : 1;

    /* 10% is the "URL committed" step mb_view reports, 100% the load end:
     * either one proves the engine is working on this navigation. */
    if (finished || percent >= 10)
        navGuardSettled(tab);

    /* The document is complete: it can report its icons now. */
    if (finished)
        requestFavicon(tab);

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

/* The page reports that Tab traversal left it (see the prelude listener): hand
 * the keyboard back to the address bar, the way Chrome does when Tab walks out
 * of the last focusable element. */
static void settingsSendAll(AgaveWebView* view);
static void settingsApply(const char* key, const char* value);
static void settingsAction(AgaveWebView* view, const char* action);

/* Defined with the tab code; the new tab page's search box navigates through
 * exactly the same path as the address bar. */
static void navigateActive(const char* text, int newTab, const char* source);

static void onPageMessage(AgaveWebView* view, const char* message, void* user)
{
    Tab* tab = (Tab*)user;

    if (!tab || !message || !tab->used)
        return;

    /* Read-only state requests belong to the tab that asked, not to the tab the
     * user happens to be looking at. A restored minishell://settings page asks
     * once while it loads; when it was a background tab on startup the old
     * active-tab gate dropped that request and the page stayed blank. */
    if (!strncmp(message, "__agb_settings_get__", 20)) {
        ShellWindow* previous = g_cur;

        if (tab != &g_tabs[g_activeTab])
            agbLog(AGB_LOG_SHELL, "settings: background tab requested values (pid=%lu)",
                   (unsigned long)GetCurrentProcessId());

        enterTabWindow(tab);
        settingsSendAll(view);
        g_cur = previous;
        return;
    }

    /* minishell://newtab asks for the chrome theme when it boots (and when the
     * tab is activated). Read-only, so a background tab gets it too. */
    if (!strncmp(message, "__agb_newtab_get__", 18)) {
        newTabSendTheme(view);
        return;
    }

    /* The page reported its <link rel=icon> URLs after the load finished (see
     * requestFavicon). Read-only and answered for the tab that asked. */
    if (!strncmp(message, "__agb_favicon__", 15)) {
        startFaviconFetch(tab, message + 15);
        return;
    }

    /* Everything below is an action on what the user is looking at. */
    if (tab != &g_tabs[g_activeTab])
        return;

    enterTabWindow(tab);

    /* minishell://newtab: the search box submits its text, and the shell runs
     * it through normalizeUrl - default engine, attribution parameters, or a
     * plain URL when the text looks like one. */
    if (!strncmp(message, "__agb_newtab_search__", 21)) {
        const char* query = message + 21;

        if (query[0])
            navigateActive(query, 0, "newtab");
        return;
    }

    /* minishell://settings: the page pushes one "key=value" per change and runs
     * a few actions. The initial read was handled above, before the active-tab
     * gate. See loadSettingsPage() in mb_view.c for the page half. */
    if (!strncmp(message, "__agb_settings_set__", 20)) {
        const char* pair = message + 20;
        const char* equals = strchr(pair, '=');
        char key[64];

        if (equals) {
            size_t length = (size_t)(equals - pair);

            if (length >= sizeof(key))
                length = sizeof(key) - 1;
            memcpy(key, pair, length);
            key[length] = 0;

            settingsApply(key, equals + 1);
            settingsSendAll(view);
        }
        return;
    }

    if (!strncmp(message, "__agb_settings_do__", 19)) {
        settingsAction(view, message + 19);
        return;
    }

    /* Find counter from the page's find script: "3/12", or "0/0" when nothing
     * matched. Only the overlay is repainted, so typing is not disturbed. */
    if (!strncmp(message, "__agb_findcount__", 17) && g_findVisible) {
        int index = 0;
        int total = 0;

        if (sscanf(message + 17, "%d/%d", &index, &total) == 2) {
            agbLog(AGB_LOG_SHELL, "find count %d/%d (query='%s')", index, total,
                   g_findQuery);
            g_findIndex = index;
            g_findTotal = total;
            syncChrome();
            invalidateFindOverlay();
        }
        return;
    }

    /* Inspect probe: "<device x,y,w,h>|<css x,y,w,h>|<dpr>|<label>|<selector>".
     * The mark is placed in device pixels and the page already did that scaling
     * with its own devicePixelRatio: the shell's 125% guess is wrong on the first
     * frames (the engine reports a 1.0 ratio until the first resize), which put
     * the frame at 0.8x of the right place until the window was resized. */
    if (!strncmp(message, "__agb_inspect__", 15) && g_inspectOn) {
        const char* payload = message + 15;
        const char* p1 = strchr(payload, '|');
        const char* p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        const char* p3 = p2 ? strchr(p2 + 1, '|') : NULL;
        const char* p4 = p3 ? strchr(p3 + 1, '|') : NULL;
        int dx = 0, dy = 0, dw = 0, dh = 0;
        int cx = 0, cy = 0, cw = 0, ch = 0;
        double dpr = 0.0;

        if (p1 && p2 && p3 && p4 &&
            sscanf(payload, "%d,%d,%d,%d", &dx, &dy, &dw, &dh) == 4 &&
            sscanf(p1 + 1, "%d,%d,%d,%d", &cx, &cy, &cw, &ch) == 4) {
            size_t labelLen = (size_t)(p4 - (p3 + 1));

            sscanf(p2 + 1, "%lf", &dpr);

            if (labelLen >= sizeof(g_inspectLabel))
                labelLen = sizeof(g_inspectLabel) - 1;
            memcpy(g_inspectLabel, p3 + 1, labelLen);
            g_inspectLabel[labelLen] = 0;

            _snprintf(g_inspectSelector, sizeof(g_inspectSelector) - 1, "%s", p4 + 1);
            g_inspectSelector[sizeof(g_inspectSelector) - 1] = 0;

            /* The menu copies CSS pixels: that is what a page stylesheet and the
             * devtools element panel speak. */
            g_inspectCss[0] = cx;
            g_inspectCss[1] = cy;
            g_inspectCss[2] = cw;
            g_inspectCss[3] = ch;

            agbLog(AGB_LOG_NAV, "inspect mark dev=%d,%d %dx%d css=%d,%d %dx%d dpr=%.2f",
                   dx, dy, dw, dh, cx, cy, cw, ch, dpr);

            uiInspectSetMark(&g_inspect, dx, dy, dw, dh, g_inspectLabel);

            /* The info goes to the bottom-left status bubble instead of a chip
             * glued to the mark: next to the element, the label had to fight the
             * element's own size and a 2x2 target clipped it to nothing. The
             * bubble is sized for text and never clips a tiny target, and it can
             * be long - so the rect and the selector tail ride along. */
            if (g_inspectLabel[0]) {
                char info[320];
                const char* sel = g_inspectSelector;
                size_t selLen = strlen(sel);

                if (selLen > 96)
                    selLen = 96;

                _snprintf(info, sizeof(info) - 1, "%s   [%d,%d %dx%d]   %.*s%s",
                          g_inspectLabel, cx, cy, cw, ch,
                          (int)selLen, sel,
                          (strlen(g_inspectSelector) > selLen) ? " ..." : "");
                info[sizeof(info) - 1] = 0;

                _snprintf(tab->status, sizeof(tab->status) - 1, "%s", info);
                tab->status[sizeof(tab->status) - 1] = 0;
                updateStatus();
            }
        }
        return;
    }

    if (strncmp(message, "__agb_tab_end__", 15) != 0)
        return;

    agbLog(AGB_LOG_SHELL, "page handed the keyboard back (tab end%s)",
           strstr(message, "shift") ? ", shift" : "");

    /* The page is one node of the focus cycle that owns its own tab stops; when
     * its traversal runs out it hands the keyboard back to the next node: the
     * first chrome stop going forward, the last one going backward. */
    uiChromeFocusClear(CHROME);
    g_focusOwner = FOCUS_CHROME;
    SetFocus(g_hwnd);

    if (strstr(message, "shift"))
        uiChromeFocusStepFrom(CHROME, UI_PART_NONE, 0);
    else
        uiChromeFocusStepFrom(CHROME, UI_PART_NONE, 1);

    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* The control's HWND got the keyboard: the page owns it until something else
 * takes it (address bar, toolbar, caption, another tab). */
static void onViewFocus(AgaveWebView* view, int focused, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab || !focused)
        return;

    enterTabWindow(tab);
    g_focusOwner = FOCUS_PAGE;
    uiChromeFocusClear(CHROME);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void onStatus(AgaveWebView* view, const char* text, void* user)
{    Tab* tab = (Tab*)user;

    (void)view;

    if (!tab)
        return;

    enterTabWindow(tab);

    strncpy(tab->status, text ? text : "", sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

/* --- favicons ------------------------------------------------------------
 * This engine has no favicon downloader of its own: mbOnNetGetFavicon() exists
 * but is a stub in the shipped build (nothing ever calls the callback, and a
 * page load makes no request for the icon). The shell therefore asks the page
 * for its <link rel=icon> URLs and downloads the bytes through
 * agave_webview_fetch(), which uses the same network stack as a navigation
 * (proxy, cookies, TLS). /favicon.ico is the last candidate, which is what
 * browsers do for pages that declare nothing. */

/* Runs after the load finished: reports the icon URLs to the host (empty line
 * when the page declares none). */
static const char kFaviconProbeJs[] =
    "(function(){"
    "var out=[],l=document.querySelectorAll('link[rel]');"
    "for(var i=0;i<l.length;i++){"
    "var r=(l[i].getAttribute('rel')||'').toLowerCase();"
    "if(r.indexOf('icon')<0)continue;"
    "var h=l[i].href||'';"
    "if(!h||h.indexOf('data:')===0||/\\.svg(\\?|$)/i.test(h))continue;"
    "out.push(h);"
    "}"
    "try{agavePostMessage('__agb_favicon__'+out.join('\\n'));}catch(e){}"
    "})();";

/* "https://host/path?x" -> "https://host/favicon.ico" */
static int faviconFallbackUrl(const char* pageUrl, char* out, size_t cap)
{
    const char* authority;
    const char* end;
    size_t schemeLen;
    size_t prefix;

    if (!pageUrl || !out || cap < 16)
        return 0;

    if (!strncmp(pageUrl, "http://", 7))
        schemeLen = 7;
    else if (!strncmp(pageUrl, "https://", 8))
        schemeLen = 8;
    else
        return 0;

    authority = pageUrl + schemeLen;
    end = authority + strcspn(authority, "/?#");
    if (end == authority)
        return 0;

    prefix = (size_t)(end - pageUrl);
    if (prefix + 13 > cap)          /* "/favicon.ico" + NUL */
        return 0;

    memcpy(out, pageUrl, prefix);
    memcpy(out + prefix, "/favicon.ico", 13);
    return 1;
}

/* --- favicon disk cache --------------------------------------------------
 * A restored tab must show its icon immediately at startup, before (and even
 * without) a network: the encoded bytes that produced the last icon for a page
 * URL live in <profile>/favicons/<hash>.bin, keyed by the page URL because that
 * is all a restored session has. The cache is a hint, not the truth - the icon
 * is still refreshed after the load, and a file that no longer decodes is
 * dropped so the next load fetches again. */

#define FAVICON_CACHE_MAX (1024 * 1024)

static int faviconCachePath(const char* pageUrl, char* out, size_t cap)
{
    unsigned long long hash = 1469598103934665603ULL;
    const char* p;
    char name[64];
    char dir[MAX_PATH_CHARS];

    if (!pageUrl || !pageUrl[0] || !out || cap < 32)
        return 0;

    for (p = pageUrl; *p; ++p) {
        hash ^= (unsigned char)*p;
        hash *= 1099511628211ULL;
    }

    _snprintf(name, sizeof(name), "favicons\\%016llx.bin", hash);
    name[sizeof(name) - 1] = 0;
    statePath(out, cap, name);

    _snprintf(dir, sizeof(dir), "%s\\favicons", g_dataDir);
    dir[sizeof(dir) - 1] = 0;
    createDirTree(dir);
    return 1;
}

/* Shows the cached icon for the tab's current URL (no network). */
static void tabLoadCachedIcon(Tab* tab)
{
    char path[MAX_PATH_CHARS];
    FILE* file;
    long length;
    unsigned char* bytes;
    UiFavicon* icon;

    if (!tab || !tab->url[0] || !tab->view)
        return;
    if (strncmp(tab->url, "http://", 7) && strncmp(tab->url, "https://", 8))
        return;
    if (!faviconCachePath(tab->url, path, sizeof(path)))
        return;

    file = fopen(path, "rb");
    if (!file)
        return;

    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (length <= 0 || length > FAVICON_CACHE_MAX) {
        fclose(file);
        return;
    }

    bytes = (unsigned char*)malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return;
    }
    fclose(file);

    icon = uiFaviconDecode(bytes, (int)length);
    free(bytes);

    if (!icon) {
        remove(path);           /* corrupt or from an older decoder */
        return;
    }

    tabClearIcon(tab);
    tab->icon = icon;
    tab->iconCached = 1;        /* shown, but still worth refreshing */
    strncpy(tab->iconPage, tab->url, sizeof(tab->iconPage) - 1);
    tab->iconPage[sizeof(tab->iconPage) - 1] = 0;
    tab->iconTried = 0;

    agbLog(AGB_LOG_SHELL, "favicon: cached %dx%d for %s (pid=%lu)",
           uiFaviconWidth(icon), uiFaviconHeight(icon), tab->url,
           (unsigned long)GetCurrentProcessId());
}

static void faviconCacheStore(const char* pageUrl, const void* data, int size)
{
    char path[MAX_PATH_CHARS];
    FILE* file;

    if (!pageUrl || !data || size <= 0 || size > FAVICON_CACHE_MAX)
        return;
    if (!faviconCachePath(pageUrl, path, sizeof(path)))
        return;

    file = fopen(path, "wb");
    if (!file) {
        agbLog(AGB_LOG_SHELL, "favicon: cannot write %s", path);
        return;
    }

    if (fwrite(data, 1, (size_t)size, file) != (size_t)size)
        agbLog(AGB_LOG_SHELL, "favicon: short write to %s", path);

    fclose(file);
}

static void faviconNext(Tab* tab);

/* One candidate finished (UI thread). Decode it; on failure try the next. */
static void onFaviconFetched(AgaveWebView* view, void* user, const char* url,
                             const void* data, int size)
{
    Tab* tab = (Tab*)user;
    UiFavicon* icon;

    (void)view;

    if (!tab || !tab->used || !tab->view)
        return;

    tab->iconFetching = 0;

    /* The user navigated while the icon was downloading: the result belongs to
     * a page that is gone, and the new page will ask for its own icon. */
    if (!tab->iconPage[0] || strcmp(tab->iconPage, tab->url) != 0)
        return;

    if (data && size > 0) {
        icon = uiFaviconDecode(data, size);

        if (icon) {
            if (tab->icon)
                uiFaviconFree(tab->icon);
            tab->icon = icon;
            tab->iconCached = 0;

            /* Keep the encoded bytes: a restored tab shows them before the
             * network is even up. */
            faviconCacheStore(tab->iconPage, data, size);

            agbLog(AGB_LOG_SHELL, "favicon: %dx%d from %s (%d bytes, pid=%lu)",
                   uiFaviconWidth(icon), uiFaviconHeight(icon), url ? url : "",
                   size, (unsigned long)GetCurrentProcessId());

            syncChrome();
            InvalidateRect(g_hwnd, &g_cur->chrome.tabstrip.rect, FALSE);
            return;
        }

        agbLog(AGB_LOG_SHELL, "favicon: %d bytes from %s did not decode",
               size, url ? url : "");
    }

    ++tab->iconNext;
    faviconNext(tab);
}

/* Fetch the next candidate in tab->iconCandidates (offset tab->iconNext). */
static void faviconNext(Tab* tab)
{
    char url[MAX_PATH_CHARS];
    char* line;
    char* newline;
    size_t length;

    if (!tab || !tab->used || !tab->view || tab->iconFetching)
        return;

    line = tab->iconCandidates + tab->iconNext;
    if (!line[0])
        return;                     /* chain exhausted: keep the letter chip */

    newline = strchr(line, '\n');
    length = newline ? (size_t)(newline - line) : strlen(line);
    if (length >= sizeof(url))
        length = sizeof(url) - 1;

    memcpy(url, line, length);
    url[length] = 0;

    tab->iconNext += (int)length + (newline ? 1 : 0);
    tab->iconFetching = 1;

    if (!url[0] || !agave_webview_fetch(tab->view, url, onFaviconFetched, tab)) {
        tab->iconFetching = 0;
        faviconNext(tab);
    }
}

/* Starts the download chain: the page's own list, then /favicon.ico. */
static void startFaviconFetch(Tab* tab, const char* pageCandidates)
{
    size_t used;
    char fallback[MAX_PATH_CHARS];

    if (!tab || !tab->used || !tab->view || (tab->icon && !tab->iconCached))
        return;
    if (!tab->iconPage[0] || strcmp(tab->iconPage, tab->url) != 0)
        return;                     /* the page navigated away meanwhile */

    _snprintf(tab->iconCandidates, sizeof(tab->iconCandidates), "%s",
              pageCandidates ? pageCandidates : "");
    tab->iconCandidates[sizeof(tab->iconCandidates) - 1] = 0;

    if (faviconFallbackUrl(tab->url, fallback, sizeof(fallback))) {
        used = strlen(tab->iconCandidates);
        _snprintf(tab->iconCandidates + used, sizeof(tab->iconCandidates) - used,
                  "%s%s", used ? "\n" : "", fallback);
    }

    tab->iconNext = 0;
    tab->iconTried = 1;

    agbLog(AGB_LOG_SHELL, "favicon: fetching for %s (page icons=%s pid=%lu)",
           tab->url, pageCandidates && pageCandidates[0] ? "declared" : "fallback",
           (unsigned long)GetCurrentProcessId());
    faviconNext(tab);
}

/* A main-frame load finished: ask the page which icons it names. */
static void requestFavicon(Tab* tab)
{
    if (!tab || !tab->used || !tab->view)
        return;
    if (!tab->url[0])
        return;
    if (strncmp(tab->url, "http://", 7) && strncmp(tab->url, "https://", 8))
        return;                     /* internal pages keep the letter chip */
    if (tab->icon && !tab->iconCached && !strcmp(tab->iconPage, tab->url))
        return;
    if (tab->iconTried && !strcmp(tab->iconPage, tab->url))
        return;

    /* Called from loading-finish: the document exists, even when the engine
     * skipped mbOnDocumentReady (it does for a plain, fast page - docReady is
     * only set from that callback). Eval is safe here; queuing a script before
     * a document exists is what the has_document() guard protects against. */
    strncpy(tab->iconPage, tab->url, sizeof(tab->iconPage) - 1);
    tab->iconPage[sizeof(tab->iconPage) - 1] = 0;
    tab->iconTried = 0;

    agbLog(AGB_LOG_SHELL, "favicon: asking the page for icons (%s pid=%lu)",
           tab->url, (unsigned long)GetCurrentProcessId());
    agave_webview_eval(tab->view, kFaviconProbeJs);
}

/* The engine downloaded the page's icon: decode it here (UI thread, WIC) and
 * paint it in the tab strip. A page without a usable icon keeps the letter
 * chip, so a failed decode is not an error. */
static void onFavicon(AgaveWebView* view, const char* url, const void* data,
                      int size, void* user)
{
    Tab* tab = (Tab*)user;
    UiFavicon* icon;

    (void)view;

    if (!tab || !tab->used || !data || size <= 0)
        return;

    icon = uiFaviconDecode(data, size);

    if (!icon) {
        agbLog(AGB_LOG_SHELL, "favicon: %d bytes could not be decoded (%s pid=%lu)",
               size, url && url[0] ? url : "no url",
               (unsigned long)GetCurrentProcessId());
        return;
    }

    enterTabWindow(tab);

    if (tab->icon)
        uiFaviconFree(tab->icon);
    tab->icon = icon;

    /* Claim this icon for the current page so requestFavicon() does not fetch
     * it again. */
    strncpy(tab->iconPage, tab->url, sizeof(tab->iconPage) - 1);
    tab->iconPage[sizeof(tab->iconPage) - 1] = 0;
    tab->iconTried = 1;

    agbLog(AGB_LOG_SHELL, "favicon: %dx%d from %s (%d bytes pid=%lu)",
           uiFaviconWidth(icon), uiFaviconHeight(icon),
           url && url[0] ? url : "no url", size,
           (unsigned long)GetCurrentProcessId());

    /* Only the strip changed: that keeps the page undamaged even while it is
     * still painting its first frames. The model is a snapshot, so refresh it
     * first - painting reads the model, not the Tab array. */
    syncChrome();
    InvalidateRect(g_hwnd, &g_cur->chrome.tabstrip.rect, FALSE);
}

/* The engine process of this tab died. Leave a page the user can read and
 * reload: the control has already dropped the dead backend, so load_html
 * brings a fresh renderer up (S3 B2c). */
/* HTML-escape into `out` (bounded); a crash report contains '<' from C++
 * templates and '&' from URLs often enough to break the page without it. */
static void appendEscaped(char* out, size_t cap, size_t* used, const char* text)
{
    size_t n = *used;

    for (; text && *text && n + 8 < cap; ++text) {
        switch (*text) {
        case '<':  n += (size_t)_snprintf(out + n, cap - n, "&lt;");   break;
        case '>':  n += (size_t)_snprintf(out + n, cap - n, "&gt;");   break;
        case '&':  n += (size_t)_snprintf(out + n, cap - n, "&amp;");  break;
        case '"':  n += (size_t)_snprintf(out + n, cap - n, "&quot;"); break;
        case '\n': n += (size_t)_snprintf(out + n, cap - n, "<br>");   break;
        default:   out[n++] = *text; out[n] = 0;                      break;
        }
    }

    *used = n;
}

static void onCrashed(AgaveWebView* view, int code, const char* report, void* user)
{
    Tab* tab = (Tab*)user;
    static char html[32768];
    char base[MAX_PATH_CHARS];
    const char* target = "about:blank";
    size_t used;

    if (!view)
        return;

    enterTabWindow(tab);

    if (tab && tab->url[0])
        target = tab->url;

    snprintf(base, sizeof(base), "%s", target);

    used = (size_t)_snprintf(html, sizeof(html),
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
             "<title>Page crashed</title>"
             "<style>body{font:14px/1.5 \"Segoe UI\",sans-serif;margin:0;padding:32px 40px;"
             "background:#f6f7f9;color:#333}h1{font-size:20px;margin:0 0 8px}"
             "p{margin:6px 0;color:#555}code{background:#eee;padding:1px 4px;border-radius:3px}"
             "a{color:#0b62d6}pre{background:#fff;border:1px solid #d8dbe0;border-radius:6px;"
             "padding:12px 14px;overflow:auto;font:12px/1.45 Consolas,monospace;color:#222;"
             "max-height:70vh}</style></head><body>"
             "<h1>Page crashed</h1>"
             "<p>The tab's engine process stopped unexpectedly (exit code %d).</p>"
             "<p><code>%s</code></p>"
             "<p><a href=\"%s\">Reload this page</a></p>",
             code, base, base);
    html[used < sizeof(html) ? used : sizeof(html) - 1] = 0;

    if (report && report[0]) {
        used += (size_t)_snprintf(html + used, sizeof(html) - used,
                                  "<h2 style=\"font-size:15px;margin:18px 0 6px\">Crash report</h2>"
                                  "<pre>");
        appendEscaped(html, sizeof(html) - 32, &used, report);
        used += (size_t)_snprintf(html + used, sizeof(html) - used, "</pre>");
    } else {
        used += (size_t)_snprintf(html + used, sizeof(html) - used,
                                  "<p>The renderer left no crash report "
                                  "(clean exit, killed, or a build without symbol support).</p>");
    }

    used += (size_t)_snprintf(html + used, sizeof(html) - used, "</body></html>");
    html[used < sizeof(html) ? used : sizeof(html) - 1] = 0;

    agbLog(AGB_LOG_SHELL, "page crashed code=%d report_bytes=%lu",
           code, (unsigned long)(report ? strlen(report) : 0));

    /* Probe mode has no tab state: print the report so a suite can assert on
     * it, and still give the view the same page a user would see. The page's
     * text is read back (that is what proves the user actually sees the
     * report), then the probe exits through its normal message channel. */
    if (!tab) {
        probeCrashPage(view, html, base, code, report);
        return;
    }

    strncpy(tab->title, "Page crashed", sizeof(tab->title) - 1);
    tab->title[sizeof(tab->title) - 1] = 0;
    strncpy(tab->status, "Page crashed - reload to try again", sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;
    tab->loading = 0;

    agave_webview_load_html(view, html, base);

    if (tab == &g_tabs[g_activeTab]) {
        updateStatus();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void onError(AgaveWebView* view, int code, const char* url, void* user)
{
    Tab* tab = (Tab*)user;
    char text[512];

    (void)view;

    if (!tab)
        return;

    enterTabWindow(tab);

    _snprintf(text, sizeof(text), "Failed to load (%d): %s", code, url ? url : "");
    text[sizeof(text) - 1] = 0;
    strncpy(tab->status, text, sizeof(tab->status) - 1);
    tab->loading = 0;

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

static int onDialog(AgaveWebView* view, int type, const char* message,
                    const char* defaultValue, char* result, int resultSize, void* user)
{
    static const char* const names[] = { "alert", "confirm", "prompt" };
    int answer = 1;

    Tab* tab = (Tab*)user;

    (void)view;
    (void)user;

    enterTabWindow(tab);

    /* A modal box on the engine thread looks exactly like a hang in the log, so
     * say which page called what before opening it. */
    agbLog(AGB_LOG_SHELL, "%s msg='%s'", type >= 0 && type <= 2 ? names[type] : "dialog",
           message ? message : "");

    if (type == 0) {
        shellOps()->message_box(NULL, g_hwnd, message ? message : "", "Page says",
                                SHELL_MB_INFO);
        return 1;
    }

    if (type == 1) {
        answer = shellOps()->message_box(NULL, g_hwnd, message ? message : "", "Confirm",
                                         SHELL_MB_QUESTION | SHELL_MB_OKCANCEL);
        agbLog(AGB_LOG_SHELL, "confirm answered %s", answer ? "ok" : "cancel");
        return answer;
    }

    if (result && resultSize > 0) {
        _snprintf(result, resultSize, "%s", defaultValue ? defaultValue : "");
        result[resultSize - 1] = 0;
    }

    return 1;
}

static int onNewWindow(AgaveWebView* from, AgaveWebView* popup,
                       const char* url, void* user)
{
    Tab* tab = (Tab*)user;
    ShellWindow* win = tab ? (ShellWindow*)tab->window : windowForView(from);
    int index;

    if (!win)
        win = firstUsedWindow();

    if (!win) {
        agbLog(AGB_LOG_SHELL, "new window rejected (no shell window)");
        agave_webview_destroy(popup);
        return 0;
    }

    g_cur = win;
    index = nextFreeTab();

    if (index < 0 || !url || !url[0]) {
        agbLog(AGB_LOG_SHELL, "new window rejected (index=%d url=%s)", index,
               url ? url : "");
        agave_webview_destroy(popup);
        return 0;
    }

    agbLog(AGB_LOG_SHELL, "new window url=%s -> tab %d", url, index);

    g_tabs[index].used = 1;
    g_tabs[index].view = popup;
    g_tabs[index].window = win;
    g_tabs[index].hwnd = (HWND)agave_webview_hwnd(popup);
    strncpy(g_tabs[index].url, url, MAX_PATH_CHARS - 1);
    strncpy(g_tabs[index].title, url, sizeof(g_tabs[index].title) - 1);

    /* The popup inherited the callbacks and the state pointer of the view it
     * was opened from: without this the new tab's title/url/progress would be
     * written into the old tab. */
    agave_webview_set_user(popup, &g_tabs[index]);

    /* Re-parent into the shell so it behaves like a tab, not a floating popup. */
    SetParent(g_tabs[index].hwnd, g_hwnd);
    ShowWindow(g_tabs[index].hwnd, SW_HIDE);

    agave_webview_set_zoom(popup, g_zoom);
    setActiveTab(index);
    return 1;
}

/* Downloads are engine jobs; the control mirrors their state into the UI
 * thread. The shell only renders it - the file is already on disk. */
static void onDownload(AgaveWebView* view, const MC_DOWNLOAD_INFO* info, void* user)
{
    Tab* tab = (Tab*)user;
    const char* name = info && info->utf8_path ? info->utf8_path : "";
    const char* slash = strrchr(name, '\\');
    char text[512];

    (void)view;

    if (!tab || !info)
        return;

    enterTabWindow(tab);

    if (slash && slash[1])
        name = slash + 1;

    switch (info->state) {
    case MC_DL_STARTED:
    case MC_DL_PROGRESS: {
        char size[64];

        if (info->total)
            _snprintf(size, sizeof(size), "%d%% (%.1f/%.1f MB)",
                      (int)(info->received * 100 / info->total),
                      info->received / 1048576.0, info->total / 1048576.0);
        else
            _snprintf(size, sizeof(size), "%.1f MB", info->received / 1048576.0);
        size[sizeof(size) - 1] = 0;

        _snprintf(text, sizeof(text), "Downloading %s %s", name, size);
        break;
    }
    case MC_DL_FINISHED:
        _snprintf(text, sizeof(text), "Downloaded: %s", info->utf8_path ? info->utf8_path : "");
        break;
    case MC_DL_FAILED:
        _snprintf(text, sizeof(text), "Download failed (%d): %s", info->error,
                  info->utf8_url ? info->utf8_url : "");
        break;
    default:
        _snprintf(text, sizeof(text), "Download cancelled: %s", name);
        break;
    }

    text[sizeof(text) - 1] = 0;
    strncpy(tab->status, text, sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;

    /* A download navigation commits no document, so on_progress(finished) never
     * arrives and the tab would stay "loading" forever. */
    if (info->state != MC_DL_PROGRESS) {
        tab->loading = 0;
        tab->progress = 100;
    }

    if (tab == &g_tabs[g_activeTab])
        updateStatus();
}

/* ------------------------------------------------------------------------- */
/* "page unresponsive" prompt (advisory, per tab)                            */
/* ------------------------------------------------------------------------- */
/* Nothing was killed: the page script is still running and the user decides.
 * The prompt is tab-scoped - a background tab is only marked in the tab strip
 * and asks when it becomes active (a dialog for a tab the user is not looking
 * at would be noise, and with one engine thread it would still be honest to
 * mention it). Wording/behaviour follow the browser prompt: Wait or close the
 * page. */

#define ID_HANG_WAIT  2001
#define ID_HANG_CLOSE 2002

typedef struct {
    int result;   /* 0 = wait, 1 = close the tab */
} HangPrompt;

static LRESULT CALLBACK hangPromptProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    HangPrompt* prompt = (HangPrompt*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_HANG_WAIT || LOWORD(wParam) == ID_HANG_CLOSE) {
            if (prompt)
                prompt->result = (LOWORD(wParam) == ID_HANG_CLOSE) ? 1 : 0;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        if (prompt)
            prompt->result = 0;   /* dismissing the prompt means "keep waiting" */
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/* Returns 1 when the user chose to close the tab. Modal (its own message loop):
 * the engine thread keeps running the page meanwhile, which is the point. */
static int showHangPrompt(int tabIndex, unsigned int busyMs, const char* stackPath)
{
    static int registered;
    HWND dialog;
    HWND wait;
    HangPrompt prompt;
    wchar_t message[1400];
    wchar_t stack[600];
    RECT owner;
    int w = 520, h = 260;
    int cx, cy;
    MSG msg;

    if (!g_hwnd || tabIndex < 0 || tabIndex >= MAX_TABS || !g_tabs[tabIndex].used)
        return 0;

    agbLog(AGB_LOG_SHELL, "unresponsive prompt: show tab=%d busy=%ums", tabIndex, busyMs);

    if (!registered) {
        WNDCLASSEXW wc;

        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = hangPromptProc;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AgaveHangPrompt";
        RegisterClassExW(&wc);
        registered = 1;
    }

    memset(&prompt, 0, sizeof(prompt));

    utf8ToWide(stackPath && stackPath[0] ? stackPath : "(no JS stack captured)",
               stack, (int)(sizeof(stack) / sizeof(stack[0])));

    _snwprintf(message, sizeof(message) / sizeof(message[0]),
               L"The page has been unresponsive for %.1f s.\n\n"
               L"Its script has not returned. You can wait for the page to\n"
               L"become responsive, or close the page.\n\n"
               L"Full JS stack:\n%s",
               busyMs / 1000.0, stack);
    message[(sizeof(message) / sizeof(message[0])) - 1] = 0;

    GetWindowRect(g_hwnd, &owner);
    cx = (owner.left + owner.right) / 2;
    cy = (owner.top + owner.bottom) / 2;

    dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AgaveHangPrompt",
                             L"MiniShell - page unresponsive",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             cx - w / 2, cy - h / 2, w, h,
                             g_hwnd, NULL, g_instance, NULL);

    if (!dialog) {
        agbLog(AGB_LOG_SHELL, "unresponsive prompt: CreateWindowExW failed (%lu)",
               (unsigned long)GetLastError());
        return 0;
    }

    SetWindowLongPtrW(dialog, GWLP_USERDATA, (LONG_PTR)&prompt);

    CreateWindowExW(0, L"STATIC", message,
                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                    16, 12, w - 32, 150, dialog, NULL, g_instance, NULL);

    wait = CreateWindowExW(0, L"BUTTON", L"Wait",
                           WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                           w - 230, h - 58, 100, 30, dialog,
                           (HMENU)(INT_PTR)ID_HANG_WAIT, g_instance, NULL);

    CreateWindowExW(0, L"BUTTON", L"Close tab",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    w - 120, h - 58, 100, 30, dialog,
                    (HMENU)(INT_PTR)ID_HANG_CLOSE, g_instance, NULL);

    if (wait)
        SetFocus(wait);

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    EnableWindow(g_hwnd, FALSE);

    {
        /* The prompt's own loop dispatches messages for every window, so the
         * context must be put back before the caller touches this tab. */
        ShellWindow* previous = g_cur;

        for (;;) {
            BOOL got = GetMessageW(&msg, NULL, 0, 0);

            if (got <= 0) {
                /* The shell was asked to quit while the prompt was up: keep the
                 * quit request for the outer loop instead of swallowing it. */
                if (got == 0)
                    PostQuitMessage((int)msg.wParam);
                break;
            }

            if (!IsWindow(dialog))
                break;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        g_cur = previous;
    }

    EnableWindow(g_hwnd, TRUE);

    if (IsWindow(dialog))
        DestroyWindow(dialog);

    return prompt.result;
}

/* The watchdog reported an unresponsive page (advisory: see
 * AgaveWebView.c). Runs on the UI thread. Only the tab the user is looking at
 * prompts; a background tab is marked and prompts when it becomes active. */
/* Engine thread: a queued command never reached the page - the navigation the
 * user just started replaced the document, or there was nothing live to run it
 * on. Before this callback the command just never happened; the status line is
 * the least intrusive way to say so. */
static void onCommandDropped(AgaveWebView* view, const char* kind,
                             const char* reason, void* user)
{
    Tab* tab = (Tab*)user;

    (void)view;

    agbLog(AGB_LOG_SHELL, "dropped %s command: %s",
           kind ? kind : "?", reason ? reason : "?");

    if (tab) {
        enterTabWindow(tab);

        _snprintf(tab->status, sizeof(tab->status) - 1, "dropped %s: %s",
                  kind ? kind : "?", reason ? reason : "?");
        tab->status[sizeof(tab->status) - 1] = 0;

        /* Engine thread: only ask the main thread to repaint the bar. */
        if (g_hwnd && !g_probeMode)
            PostMessageA(g_hwnd, AGW_SHELL_STATUS, 0, 0);
    }
}

static void onUnresponsive(AgaveWebView* view, const char* reason,
                           unsigned int busy_ms, const char* stack_path, void* user)
{    Tab* tab = (Tab*)user;
    const char* show = getenv("AGB_HANG_POPUP");
    int index = -1;
    int i;

    (void)view;

    if (!tab)
        return;

    enterTabWindow(tab);

    for (i = 0; i < MAX_TABS; ++i) {
        if (&g_tabs[i] == tab) {
            index = i;
            break;
        }
    }

    strncpy(tab->status, reason ? reason : "", sizeof(tab->status) - 1);
    tab->status[sizeof(tab->status) - 1] = 0;

    tab->hangPending = 1;
    tab->hangMs = busy_ms;
    tab->hangStack[0] = 0;

    if (stack_path && stack_path[0]) {
        strncpy(tab->hangStack, stack_path, sizeof(tab->hangStack) - 1);
        tab->hangStack[sizeof(tab->hangStack) - 1] = 0;
    }

    agbLog(AGB_LOG_SHELL, "page unresponsive: tab=%d %s (busy=%ums) stack=%s",
           index, reason ? reason : "", busy_ms,
           tab->hangStack[0] ? tab->hangStack : "(none)");

    if (tab == &g_tabs[g_activeTab]) {
        if (!(show && show[0] == '0' && show[1] == 0))
            PostMessageA(g_hwnd, AGW_SHELL_HANG_PROMPT, (WPARAM)index, 0);

        updateStatus();
    } else {
        /* Background tab: no dialog; the tab strip marks it and the prompt
         * appears when the user switches to it. */
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* Right click: the control reports the hit-test; the shell owns the menu. */
static void navigateActive(const char* text, int newTab, const char* source);
static void addBookmarkCurrent(void);
static void reloadActive(const char* source);
static void backActive(const char* source);
static void forwardActive(const char* source);
static void stopActive(const char* source);

/* ------------------------------------------------------------------------- */
/* script injection (inspect.js / --inject files)                            */
/* ------------------------------------------------------------------------- */

/* Menu labels are UTF-8 (the shell speaks UTF-8 everywhere); the self-drawn
 * menu measures and paints them through ui_menu. */
static char* readFileUtf8(const wchar_t* path, size_t* lengthOut)
{
    FILE* file;
    long length;
    char* buffer;

    if (lengthOut)
        *lengthOut = 0;

    file = _wfopen(path, L"rb");
    if (!file)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    length = ftell(file);
    if (length < 0 || length > 16 * 1024 * 1024 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    buffer = (char*)malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[length] = 0;
    fclose(file);

    if (lengthOut)
        *lengthOut = (size_t)length;

    return buffer;
}

/* Reads a script compiled into the exe (app.rc RCDATA). The resource is not
 * NUL terminated, so copy it; the caller frees the result. */
static char* readEmbeddedScript(int id, size_t* lengthOut)
{
    HRSRC resource;
    HGLOBAL handle;
    DWORD size;
    const void* data;
    char* copy;

    if (lengthOut)
        *lengthOut = 0;

    resource = FindResourceA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(id), RT_RCDATA);
    if (!resource)
        return NULL;

    handle = LoadResource(NULL, resource);
    if (!handle)
        return NULL;

    size = SizeofResource(NULL, resource);
    data = LockResource(handle);
    if (!data || size == 0)
        return NULL;

    copy = (char*)malloc((size_t)size + 1);
    if (!copy)
        return NULL;

    memcpy(copy, data, size);
    copy[size] = 0;

    if (lengthOut)
        *lengthOut = size;

    return copy;
}

/* Runs a JS file in the page. The file is looked up next to minishell.exe
 * first, then in the current directory; when neither exists and `embeddedId`
 * is set, the copy compiled into the exe is used (that is how inspect.js works,
 * so F12 needs no files next to the exe). A file on disk still wins, which
 * keeps the script editable while developing it.
 *
 * `suffix` (optional) is appended after a newline and evaluated in the same
 * script: a UMD bundle needs its bootstrap to run only after the bundle has
 * registered itself, and one eval keeps that ordering independent of the
 * command queue. */
static int injectScriptAsset(AgaveWebView* view, const char* fileName,
                             const char* envName, const char* suffix,
                             int embeddedId)
{
    wchar_t path[MAX_PATH_CHARS];
    char* script = NULL;
    size_t length = 0;
    size_t fileLength = 0;
    int fromResource = 0;
    /* Probe mode has no tabs/window context: injection still has to work
     * (--inject is a probe flag), it just has no status bar to write to. */
    Tab* tab = g_cur ? &g_tabs[g_activeTab] : NULL;

    if (!view)
        return 0;

    path[0] = 0;

    if (envName) {
        const char* override = getenv(envName);

        if (override && override[0])
            utf8ToWide(override, path, MAX_PATH_CHARS);
    }

    if (!path[0]) {
        wchar_t module[MAX_PATH_CHARS];
        wchar_t name[MAX_PATH_CHARS];
        wchar_t* slash;

        if (!GetModuleFileNameW(NULL, module, MAX_PATH_CHARS))
            return 0;

        slash = wcsrchr(module, L'\\');
        if (slash)
            *(slash + 1) = 0;

        utf8ToWide(fileName, name, MAX_PATH_CHARS);

        wcsncpy(path, module, MAX_PATH_CHARS - 1);
        path[MAX_PATH_CHARS - 1] = 0;
        if (wcslen(path) + wcslen(name) < MAX_PATH_CHARS - 1)
            wcscat(path, name);
    }

    script = readFileUtf8(path, &length);

    if (!script) {
        /* Also accept a path relative to the working directory, which is how
         * the shell is usually launched while editing the repo. */
        utf8ToWide(fileName, path, MAX_PATH_CHARS);
        script = readFileUtf8(path, &length);
    }

    if (!script && embeddedId) {
        script = readEmbeddedScript(embeddedId, &length);
        if (script)
            fromResource = 1;
    }

    if (!script) {
        if (tab && tab->used) {
            _snprintf(tab->status, sizeof(tab->status),
                      "Cannot read %s (put it next to minishell.exe)", fileName);
            tab->status[sizeof(tab->status) - 1] = 0;
            updateStatus();
        }
        agbLog(AGB_LOG_SHELL, "inject: cannot read %s", fileName);
        return 0;
    }

    fileLength = length;

    if (suffix && suffix[0]) {
        size_t extra = strlen(suffix);
        char* grown = (char*)realloc(script, length + extra + 3);

        if (grown) {
            script = grown;
            /* Start a fresh statement: the asset may end without a semicolon,
             * and a suffix beginning with "(" would otherwise chain onto it. */
            script[length] = '\n';
            script[length + 1] = ';';
            memcpy(script + length + 2, suffix, extra);
            script[length + 2 + extra] = 0;
            length += extra + 2;
        }
    }

    agave_webview_eval(view, script);
    agave_webview_set_focus(view);
    free(script);

    agbLog(AGB_LOG_SHELL, "inject: %s (%lu bytes%s, eval %lu)",
           fileName, (unsigned long)fileLength, fromResource ? ", embedded" : "",
           (unsigned long)length);

    if (tab && tab->used && tab->view == view) {
        _snprintf(tab->status, sizeof(tab->status),
                  "Injected %s (%lu bytes)", fileName, (unsigned long)fileLength);
        tab->status[sizeof(tab->status) - 1] = 0;
        updateStatus();
    }

    return 1;
}

static int injectScriptFile(AgaveWebView* view, const char* fileName, const char* envName)
{
    /* inspect.js is the page half of the F12 marker and ships inside the exe;
     * any other name is a plain file on disk (--inject). */
    int embeddedId = !_stricmp(fileName, "inspect.js") ? IDR_INSPECT_JS : 0;

    return injectScriptAsset(view, fileName, envName, NULL, embeddedId);
}

/* ------------------------------------------------------------------------- */
/* host-side inspect marker                                                  */
/* ------------------------------------------------------------------------- */

/* The page half (inspect.js, embedded in the exe) only reports the target's
 * viewport rect; the box is painted by ui_inspect.c in a click-through child
 * window above the webview. Nothing is inserted into the page, so the
 * document's size - and therefore the scrollbars - cannot change. */
static void inspectEnsure(void)
{
    if (g_inspectReady || !g_hwnd)
        return;

    uiInspectInit(&g_inspect, g_hwnd, &CHROME->theme);
    g_inspectReady = g_inspect.hwnd != NULL;
    agbLog(AGB_LOG_SHELL, "inspect overlay: %s", g_inspectReady ? "ready" : "not created");
    if (g_inspectReady) {
        UiRect content = CHROME->content;

        uiInspectSetContentRect(&g_inspect, content.x, content.y, content.w, content.h);
    }
}

static void inspectStop(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!g_inspectOn)
        return;

    g_inspectOn = 0;
    agbLog(AGB_LOG_SHELL, "inspect off (%s)", source ? source : "?");

    /* Release the page's mouse capture before anything else: a stale capture
     * would keep the control swallowing input after inspect mode is gone. */
    if (tab->used && tab->view)
        agave_webview_set_input_capture(tab->view, 0, NULL, NULL);

    if (g_inspectReady) {
        uiInspectSetTarget(&g_inspect, NULL);
        uiInspectClear(&g_inspect);
        uiInspectShow(&g_inspect, 0);
    }

    if (tab->used && tab->view)
        agave_webview_eval(tab->view, "if(window.__mbInspect)window.__mbInspect.stop();");

    /* Drop the element info from the bubble: it described a mark that is gone. */
    if (tab->used) {
        tab->status[0] = 0;
        updateStatus();
    }
}

/* ------------------------------------------------------------------------- */
/* inspect input (control message path) and its own context menu             */
/* ------------------------------------------------------------------------- */

static void copyUtf8ToClipboard(const char* text)
{
    wchar_t wide[2048];
    int wideChars;
    int copied = 0;

    if (!text)
        return;

    wideChars = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide,
                                    (int)(sizeof(wide) / sizeof(wide[0])));
    if (wideChars <= 0)
        return;

    if (OpenClipboard(g_hwnd)) {
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wideChars * sizeof(wchar_t));

        if (memory) {
            void* target = GlobalLock(memory);

            if (target) {
                memcpy(target, wide, (size_t)wideChars * sizeof(wchar_t));
                GlobalUnlock(memory);
                EmptyClipboard();
                copied = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
                if (!copied)
                    GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    agbLog(AGB_LOG_SHELL, "clipboard copy ok=%d chars=%d", copied, wideChars);
}

/* The control swallows hover, so the page is asked for the element under a point
 * (it can still hit-test: elementFromPoint works without a real mouse event).
 * Coordinates go as device pixels; the page divides by its own
 * devicePixelRatio, which is the only reliable ratio (see inspect.js). */
static void inspectOnMove(void* user, int x, int y)
{
    Tab* tab = &g_tabs[g_activeTab];
    char script[160];

    (void)user;

    if (!g_inspectOn || !tab->used || !tab->view)
        return;
    if (GetTickCount() - g_inspectProbeTick < 40)     /* ~25 Hz is plenty */
        return;

    g_inspectProbeTick = GetTickCount();

    _snprintf(script, sizeof(script) - 1,
              "if(window.__mbInspect)window.__mbInspect.probe(%d,%d);", x, y);
    script[sizeof(script) - 1] = 0;
    agave_webview_eval(tab->view, script);
}

static void inspectMenuAt(Tab* tab, int x, int y)
{
    UiMenu menu;
    POINT point;
    ShellWindow* previous;
    int choice;

    uiMenuInit(&menu);
    uiMenuAdd(&menu, "Copy selector", NULL, 40, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Copy element info", NULL, 41, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Copy rect", NULL, 42, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&menu);
    uiMenuAdd(&menu, "Exit inspect", "Esc", 43, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuTrim(&menu);

    point.x = x;
    point.y = y;
    if (tab->hwnd)
        ClientToScreen(tab->hwnd, &point);

    previous = g_cur;
    choice = uiMenuHostRun(&CHROME->theme, &menu, point.x, point.y,
                           UI_MENU_ALIGN_LEFT, g_hwnd, NULL);
    g_cur = previous;

    switch (choice) {
    case 40:
        if (g_inspectSelector[0])
            copyUtf8ToClipboard(g_inspectSelector);
        break;
    case 41: {
        char info[1200];
        char url[512] = "";

        if (tab->used)
            _snprintf(url, sizeof(url) - 1, "%s", tab->url);
        url[sizeof(url) - 1] = 0;

        _snprintf(info, sizeof(info) - 1, "%s | rect=%d,%d %dx%d | selector=%s | url=%s",
                  g_inspectLabel[0] ? g_inspectLabel : "(none)",
                  g_inspectCss[0], g_inspectCss[1], g_inspectCss[2], g_inspectCss[3],
                  g_inspectSelector, url);
        info[sizeof(info) - 1] = 0;
        copyUtf8ToClipboard(info);
        break;
    }
    case 42: {
        char rect[96];

        _snprintf(rect, sizeof(rect) - 1, "%d,%d %dx%d",
                  g_inspectCss[0], g_inspectCss[1], g_inspectCss[2], g_inspectCss[3]);
        rect[sizeof(rect) - 1] = 0;
        copyUtf8ToClipboard(rect);
        break;
    }
    case 43:
        inspectStop("menu");
        break;
    default:
        break;
    }
}

static void inspectOnClick(void* user, int x, int y, int button)
{
    Tab* tab = &g_tabs[g_activeTab];

    (void)user;

    if (!g_inspectOn || !tab->used)
        return;

    if (button == 2) {
        /* Right click belongs to the inspector now: the page never sees it. */
        inspectMenuAt(tab, x, y);
        return;
    }

    /* Left click re-probes so a click without movement still selects. */
    g_inspectProbeTick = 0;
    inspectOnMove(NULL, x, y);
}

/* ------------------------------------------------------------------------- */
/* inspect input: low-level mouse hook                                       */
/* ------------------------------------------------------------------------- */

/* While inspecting, the page must not receive hover/clicks. Neither of the two
 * obvious mechanisms works here:
 *   - an overlay window above the webview (even an invisible layered child)
 *     makes the page stop repainting on resize;
 *   - a WH_MOUSE_LL hook that returns 1 for WM_MOUSEMOVE freezes the pointer
 *     itself (returning non-zero discards the event, movement included), which
 *     locks the user's mouse.
 * So the control swallows the input in its own window proc instead
 * (agave_webview_set_input_capture): the cursor moves normally, the page simply
 * never sees the message. kind: 0 = move, 1 = left, 2 = right. */

static void inspectInput(AgaveWebView* view, int kind, int x, int y, void* user)
{
    (void)view;
    (void)user;

    if (!g_inspectOn)
        return;

    if (kind == 0) {
        inspectOnMove(NULL, x, y);
        return;
    }

    inspectOnClick(NULL, x, y, kind);
}

static void inspectStart(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    inspectEnsure();
    if (!g_inspectReady)
        return;

    uiInspectSetTarget(&g_inspect, tab->hwnd);

    g_inspectOn = 1;
    uiInspectShow(&g_inspect, 1);
    injectScriptFile(tab->view, "inspect.js", NULL);

    /* The page must not see the mouse any more. The control swallows it in its
     * own window proc - no window above the page (that stops repainting on
     * resize) and no low-level hook (that freezes the cursor). */
    agave_webview_set_input_capture(tab->view, 1, inspectInput, NULL);

    agbLog(AGB_LOG_SHELL, "inspect on (host marker + control input capture)");

    /* Geometry snapshot: an offset that appears only before the first resize was
     * once a device-scale mismatch, so both sides are logged here. */
    agbLog(AGB_LOG_SHELL, "inspect geometry: content=%d,%d %dx%d scale=%d dpi=%u",
           CHROME->content.x, CHROME->content.y, CHROME->content.w, CHROME->content.h,
           agave_webview_device_scale(tab->view), (unsigned)CHROME->theme.dpi);

    /* Debug aid: a fixed mark proves the overlay's paint path without needing a
     * page report (probe runs have no pointer). */
    if (getenv("AGB_INSPECT_DEMO"))
        uiInspectSetMark(&g_inspect, 40, 40, 400, 120, "demo 400x120");

    /* Visible feedback: the marker itself only appears once the pointer is over
     * an element, which made F12 look dead. */
    strncpy(tab->status, "Inspect: hover an element, right click for the menu, Esc/F12 to exit",
            sizeof(tab->status) - 1);
    updateStatus();

    /* Pick up whatever is under the pointer right now. With the mouse still, the
     * capture swallows every WM_MOUSEMOVE, so no probe was ever issued and F12
     * looked like it had done nothing. Reading the cursor position is passive:
     * nothing is moved, clicked or foregrounded. */
    {
        POINT point;
        RECT client;

        if (GetCursorPos(&point) && ScreenToClient(tab->hwnd, &point) &&
            GetClientRect(tab->hwnd, &client) &&
            point.x >= 0 && point.y >= 0 &&
            point.x < client.right && point.y < client.bottom) {
            SetCursor(LoadCursorW(NULL, IDC_CROSS));
            g_inspectProbeTick = 0;
            inspectOnMove(NULL, point.x, point.y);
        }
    }
}

static void toggleInspect(void)
{
    static unsigned long last;

    /* The control delivers key messages more than once in some paths; an
     * undebounced toggle turns one F12 into on+off and looks dead. */
    if (GetTickCount() - last < 300)
        return;
    last = GetTickCount();

    if (g_inspectOn)
        inspectStop("toggle");
    else
        inspectStart();
}

static void onContextMenu(AgaveWebView* view, int x, int y,
                          const char* linkUrl, const char* imageUrl,
                          const char* text, int editable, void* user)
{
    UiMenu menu;
    POINT point;
    POINT reopen;
    Tab* tab = (Tab*)user;
    HWND viewHwnd = view ? (HWND)agave_webview_hwnd(view) : NULL;
    int choice;
    int hasSelection;
    int canBack;
    int canForward;

    if (!tab) {
        agbLog(AGB_LOG_SHELL, "context menu without a tab, ignored");
        return;
    }

    enterTabWindow(tab);

    agbLog(AGB_LOG_SHELL, "ctx request x=%d y=%d link=%s image=%s editable=%d text=%lu",
           x, y,
           (linkUrl && linkUrl[0]) ? linkUrl : "(none)",
           (imageUrl && imageUrl[0]) ? imageUrl : "(none)",
           editable, (unsigned long)(text ? strlen(text) : 0));

    /* Whether an editable is under the pointer comes from the page (the engine
     * has no query for it - mbGetCaretRect reports a caret even when nothing
     * editable is focused). The commands themselves run in the engine. */
    hasSelection = (text && text[0]) ? 1 : 0;
    canBack = tab->view && agave_webview_can_back(tab->view);
    canForward = tab->view && agave_webview_can_forward(tab->view);

    /* The ids are the historical TrackPopupMenu ids: logs and the switch below
     * stay exactly the same.
     *
     * No disabled rows and no "not implemented" entries: an item either works
     * here or is not in the menu. Grey rows only make sense for something that
     * can become enabled later (nothing here can), and the page menu is read at
     * a glance - a third of it being dead weight is worse than a shorter menu.
     * Blocks are added conditionally, so separators are only inserted in front
     * of a block that is actually present. */
    uiMenuInit(&menu);

    if (canBack)
        uiMenuAdd(&menu, "Back", "Alt+Left", 1, 0, UI_MENU_ICON_NONE, NULL);
    if (canForward)
        uiMenuAdd(&menu, "Forward", "Alt+Right", 2, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Reload", "Ctrl+R", 3, 0, UI_MENU_ICON_NONE, NULL);

    if (linkUrl && linkUrl[0]) {
        uiMenuAddSeparator(&menu);
        uiMenuAdd(&menu, "Open link in this tab", NULL, 4, 0,
                  UI_MENU_ICON_LINK, NULL);
        uiMenuAdd(&menu, "Open link in new tab", NULL, 5, 0,
                  UI_MENU_ICON_LINK, NULL);
        uiMenuAdd(&menu, "Copy link address", NULL, 6, 0,
                  UI_MENU_ICON_NONE, NULL);
    }

    if (hasSelection) {
        uiMenuAddSeparator(&menu);
        uiMenuAdd(&menu, "Copy", "Ctrl+C", 22, 0, UI_MENU_ICON_NONE, NULL);
    }

    if (editable || 1) {
        if (!hasSelection)
            uiMenuAddSeparator(&menu);
        uiMenuAdd(&menu, "Undo", "Ctrl+Z", 20, 0, UI_MENU_ICON_NONE, NULL);
        if (hasSelection)
            uiMenuAdd(&menu, "Cut", "Ctrl+X", 21, 0, UI_MENU_ICON_NONE, NULL);
        uiMenuAdd(&menu, "Paste", "Ctrl+V", 23, 0, UI_MENU_ICON_NONE, NULL);
    }

    uiMenuAddSeparator(&menu);
    uiMenuAdd(&menu, "Select all", "Ctrl+A", 24, 0, UI_MENU_ICON_NONE, NULL);

    if (imageUrl && imageUrl[0]) {
        uiMenuAddSeparator(&menu);
        uiMenuAdd(&menu, "Copy image", NULL, 30, 0, UI_MENU_ICON_NONE, NULL);
        uiMenuAdd(&menu, "Save image as...", NULL, 31, 0, UI_MENU_ICON_NONE, NULL);
    }

    uiMenuAddSeparator(&menu);
    uiMenuAdd(&menu, "Bookmark this page", "Ctrl+D", 8, 0,
              UI_MENU_ICON_NONE, NULL);

    uiMenuAddSeparator(&menu);
    uiMenuAdd(&menu, "Inspect page", "F12", 11, 0,
              g_inspectOn ? UI_MENU_ICON_CHECK : UI_MENU_ICON_NONE, NULL);

    uiMenuTrim(&menu);

    /* x,y are client coordinates of the control, not of the shell window: with
     * g_hwnd the menu was offset by the toolbar height, and on a multi-monitor
     * setup the popup could open on the wrong monitor entirely. */
    point.x = x;
    point.y = y;
    ClientToScreen(viewHwnd ? viewHwnd : g_hwnd, &point);
    reopen = point;

    {
        /* uiMenuHostRun runs a modal loop; restore the context so the menu
         * actions below always act on this window. */
        ShellWindow* previous = g_cur;

        choice = uiMenuHostRun(&CHROME->theme, &menu, point.x, point.y,
                               UI_MENU_ALIGN_LEFT, g_hwnd, &reopen);
        g_cur = previous;
    }

    /* choice==0 means the menu was dismissed (or the popup failed to open): the
     * one line that tells "the menu never opened" apart from "the item did
     * nothing". choice==UI_MENU_REOPEN means it was dismissed by a right click
     * and is about to be re-opened elsewhere. */
    agbLog(AGB_LOG_SHELL, "context menu at %d,%d link=%s image=%s choice=%d",
           x, y,
           (linkUrl && linkUrl[0]) ? linkUrl : "(none)",
           (imageUrl && imageUrl[0]) ? imageUrl : "(none)",
           choice);

    /* A second right click dismissed the menu (the popup had the mouse capture,
     * so the click never reached the page). Replay it where the pointer is and
     * let the engine decide what is under it now - link, image or plain page -
     * instead of leaving the user with "the menu just closed". */
    if (choice == UI_MENU_REOPEN) {
        HWND target = viewHwnd ? viewHwnd : g_hwnd;
        LPARAM at;

        ScreenToClient(target, &reopen);
        at = MAKELPARAM((short)reopen.x, (short)reopen.y);

        agbLog(AGB_LOG_SHELL, "context menu re-open at %d,%d", reopen.x, reopen.y);
        PostMessageA(target, WM_RBUTTONDOWN, MK_RBUTTON, at);
        PostMessageA(target, WM_RBUTTONUP, 0, at);
        return;
    }

    switch (choice) {
    case 1: if (canBack) backActive("context menu"); break;
    case 2: if (canForward) forwardActive("context menu"); break;
    case 3: reloadActive("context menu"); break;
    case 4: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 0, "context menu"); break;
    case 5: if (linkUrl && linkUrl[0]) navigateActive(linkUrl, 1, "context menu"); break;
    case 6:
        if (linkUrl && linkUrl[0]) {
            wchar_t wide[2048];
            int copied = 0;
            int wideBytes;

            /* CF_UNICODETEXT, not CF_TEXT: a UTF-8 URL pasted into an ANSI
             * clipboard came out as mojibake. */
            utf8ToWide(linkUrl, wide, 2048);
            wideBytes = (lstrlenW(wide) + 1) * (int)sizeof(wchar_t);

            if (OpenClipboard(g_hwnd)) {
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wideBytes);

                if (memory) {
                    void* target = GlobalLock(memory);

                    if (target) {
                        memcpy(target, wide, (size_t)wideBytes);
                        GlobalUnlock(memory);
                        EmptyClipboard();
                        copied = SetClipboardData(CF_UNICODETEXT, memory) != NULL;
                        if (!copied)
                            GlobalFree(memory);
                    } else {
                        GlobalFree(memory);
                    }
                }
                CloseClipboard();
            }

            agbLog(AGB_LOG_SHELL, "clipboard copy link ok=%d url=%s", copied, linkUrl);
        }
        break;
    case 8: addBookmarkCurrent(); break;
    case 11: toggleInspect(); break;
    case 20:
        /* Editing commands run through the engine's own context-menu path, so
         * they target the frame/element the menu was opened on and keep the
         * engine's undo stack, clipboard and caret consistent. */
        agbLog(AGB_LOG_SHELL, "context menu undo");
        agave_webview_context_command(view, MB_MENU_UNDO);
        break;
    case 21:
        agbLog(AGB_LOG_SHELL, "context menu cut");
        agave_webview_context_command(view, MB_MENU_CUT);
        break;
    case 22:
        agbLog(AGB_LOG_SHELL, "context menu copy selection=%lu",
               (unsigned long)(text ? strlen(text) : 0));
        agave_webview_context_command(view, MB_MENU_COPY_TEXT);
        break;
    case 23:
        agbLog(AGB_LOG_SHELL, "context menu paste");
        agave_webview_context_command(view, MB_MENU_PASTE);
        break;
    case 24:
        agave_webview_context_command(view, MB_MENU_SELECT_ALL);
        break;
    case 30:
        agbLog(AGB_LOG_SHELL, "context menu copy image");
        agave_webview_context_command(view, MB_MENU_COPY_IMAGE);
        break;
    case 31:
        agbLog(AGB_LOG_SHELL, "context menu save image");
        agave_webview_context_command(view, MB_MENU_SAVE_IMAGE);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* navigation helpers                                                        */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* address bar input -> URL                                                  */
/* ------------------------------------------------------------------------- */

static int startsWithNoCase(const char* text, const char* prefix)
{
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;

        if (!a)
            return 0;
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

/* "E:\dir", "E:/dir", "E:" (drive current directory). */
static int isDrivePath(const char* text)
{
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z')))
        return 0;
    if (text[1] != ':')
        return 0;
    return text[2] == '\\' || text[2] == '/' || text[2] == 0;
}

static int isUncPath(const char* text)
{
    return text[0] == '\\' && text[1] == '\\';
}

static int localPathExists(const char* text)
{
    wchar_t wide[MAX_PATH_CHARS];
    DWORD attributes;

    if (!text || !text[0])
        return 0;

    utf8ToWide(text, wide, MAX_PATH_CHARS);
    attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES;
}

/* Bytes that may appear verbatim in the path of a file: URL. Everything else
 * (space, %, #, ?, backslash and every non-ASCII UTF-8 byte) gets escaped -
 * the engine parses the URL, so an unescaped space or '#' truncates it. */
static int fileUrlByteIsSafe(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;

    switch (c) {
    case '-': case '_': case '.': case '~': case '/': case ':':
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=': case '@':
        return 1;
    default:
        return 0;
    }
}

static void appendUrlEncoded(char* out, size_t outSize, size_t* written, const char* text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = *written;

    while (*text && o + 4 < outSize) {
        unsigned char c = (unsigned char)*text++;

        if (fileUrlByteIsSafe(c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }

    out[o] = 0;
    *written = o;
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the %XX escapes of a URL path; invalid escapes stay literal. Needed
 * when a file: URL that is already encoded has to be rebuilt: treating it as a
 * raw filesystem path would encode its '%' again ("%20" -> "%2520") on every
 * pass, which is what made repeated Go clicks grow the address bar. */
static void decodeUrlPath(const char* text, char* out, size_t outSize)
{
    size_t o = 0;

    if (!outSize)
        return;

    if (!text) {
        out[0] = 0;
        return;
    }

    while (*text && o + 1 < outSize) {
        if (text[0] == '%' && text[1] && text[2]) {
            int hi = hexValue(text[1]);
            int lo = hexValue(text[2]);

            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                text += 3;
                continue;
            }
        }

        out[o++] = *text++;
    }

    out[o] = 0;
}

/* Splits "path?query#fragment" into the on-disk path and the URL suffix.
 * The suffix is appended to the file: URL verbatim - a query or fragment is
 * not part of the file name, and percent-encoding its '?' would make the path
 * point at a file called "x.html%3Ffocus=b" (which never exists). */
static void splitLocalSuffix(const char* text, char* pathOut, size_t pathSize,
                             const char** suffixOut)
{
    const char* mark = text ? strpbrk(text, "?#") : NULL;
    size_t len;

    if (!pathOut || !pathSize)
        return;

    if (!mark) {
        strncpy(pathOut, text ? text : "", pathSize - 1);
        pathOut[pathSize - 1] = 0;
        *suffixOut = "";
        return;
    }

    len = (size_t)(mark - text);
    if (len >= pathSize)
        len = pathSize - 1;
    memcpy(pathOut, text, len);
    pathOut[len] = 0;
    *suffixOut = mark;
}

/* Appends a query/fragment verbatim (no percent-encoding). */
static void appendLocalSuffix(char* out, size_t outSize, const char* suffix)
{
    size_t o = strlen(out);

    if (!suffix || !*suffix)
        return;

    while (*suffix && o + 1 < outSize)
        out[o++] = *suffix++;
    out[o] = 0;
}

/* "E:\deepseek-webui\index.html" (also with forward slashes, a UNC path or a
 * relative path) -> "file:///E:/deepseek-webui/index.html". GetFullPathNameW
 * normalizes separators, "." and ".." against the shell's working directory. */
static void fileUrlFromLocalPath(const char* input, char* out, size_t outSize)
{
    wchar_t wideInput[MAX_PATH_CHARS];
    wchar_t wideFull[MAX_PATH_CHARS];
    char full[MAX_PATH_CHARS];
    size_t written = 0;
    char* p;

    if (!outSize)
        return;

    out[0] = 0;
    if (outSize < 8)
        return;

    utf8ToWide(input, wideInput, MAX_PATH_CHARS);
    if (!GetFullPathNameW(wideInput, MAX_PATH_CHARS, wideFull, NULL)) {
        wcsncpy(wideFull, wideInput, MAX_PATH_CHARS - 1);
        wideFull[MAX_PATH_CHARS - 1] = 0;
    }
    wideToUtf8(wideFull, full, MAX_PATH_CHARS);

    for (p = full; *p; ++p) {
        if (*p == '\\')
            *p = '/';
    }

    if (full[0] == '/' && full[1] == '/') {
        /* UNC: the share host is the URL authority (file://host/share/x). */
        char* share = strchr(full + 2, '/');

        if (share) {
            char host[MAX_PATH_CHARS];
            size_t hostLen = (size_t)(share - (full + 2));

            if (hostLen >= sizeof(host))
                hostLen = sizeof(host) - 1;
            memcpy(host, full + 2, hostLen);
            host[hostLen] = 0;

            appendUrlEncoded(out, outSize, &written, "file://");
            appendUrlEncoded(out, outSize, &written, host);
            appendUrlEncoded(out, outSize, &written, share);
        } else {
            appendUrlEncoded(out, outSize, &written, "file:");
            appendUrlEncoded(out, outSize, &written, full);
        }
        return;
    }

    /* "C:/dir/x" -> file:///C:/dir/x. A drive path has no leading slash, so the
     * URL has to spell all three of them: with two ("file://C:/x") the third
     * rebuild below classified it as "file: + drive path" again and re-encoded
     * the path - the "%20" -> "%2520" growth on repeated Go clicks. */
    appendUrlEncoded(out, outSize, &written, "file:///");
    appendUrlEncoded(out, outSize, &written, full);
}

/* Percent-encodes UTF-8 bytes for a query component: RFC 3986 unreserved
 * characters stay, everything else (spaces, '+', CJK, '/') is escaped. */
static void urlEncodeComponent(const char* text, char* out, size_t outSize)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (!out || outSize == 0)
        return;

    for (; text && *text; ++text) {
        unsigned char c = (unsigned char)*text;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= outSize)
                break;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= outSize)
                break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }

    out[o] = 0;
}

/* Search URL for the default engine. Baidu and Google get an attribution value
 * built from the first 6 chars of the engine DLL's SHA-256 and the shell's git
 * version (tag or short commit, +dirty):
 *   baidu  ...&tn=minibrowsershell-<hash6>-<version>
 *   google ...&client=minibrowsershell-<hash6>-<version>
 * so a search can be traced back to this build. */
static void searchUrlFor(const char* query, char* out, size_t outSize)
{
    char encodedQuery[1024];
    char value[192];
    char encodedValue[640];
    int engine = g_prefs.search;

    if (engine < 0 || engine >= ENGINE_COUNT)
        engine = 0;

    urlEncodeComponent(query, encodedQuery, sizeof(encodedQuery));

    if (engine != 0 && engine != 2) {
        _snprintf(out, outSize, "%s%s", kEngineSearch[engine], encodedQuery);
        out[outSize - 1] = 0;
        return;
    }

    {
        const char* hash = buildInfoEngineSha256();
        char hash6[8];

        if (!hash || !hash[0] || !strcmp(hash, "-")) {
            strcpy(hash6, "unknown");
        } else {
            strncpy(hash6, hash, 6);
            hash6[6] = 0;
        }

        _snprintf(value, sizeof(value), "minibrowsershell-%s-%s",
                  hash6, buildInfoShellVersion());
        value[sizeof(value) - 1] = 0;
        urlEncodeComponent(value, encodedValue, sizeof(encodedValue));

        _snprintf(out, outSize, "%s%s&%s=%s", kEngineSearch[engine], encodedQuery,
                  engine == 0 ? "tn" : "client", encodedValue);
        out[outSize - 1] = 0;
    }
}

static void normalizeUrl(const char* input, char* out, size_t outSize)
{
    const char* text = input;
    const char* rest;

    if (!outSize)
        return;

    out[0] = 0;

    if (!text)
        return;

    while (*text == ' ')
        ++text;

    /* Explorer's "Copy as path" quotes the path; accept the pasted form. */
    if (text[0] == '"' && text[1]) {
        size_t len = strlen(text);

        if (len >= 2 && text[len - 1] == '"') {
            static char unquoted[MAX_PATH_CHARS];

            if (len - 1 < sizeof(unquoted)) {
                memcpy(unquoted, text + 1, len - 2);
                unquoted[len - 2] = 0;
                normalizeUrl(unquoted, out, outSize);
                return;
            }
        }
    }

    if (!*text) {
        strncpy(out, "about:blank", outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    /* Address-bar paths: Ctrl+L focuses the field without selecting its text,
     * so typing a path often lands behind the current scheme
     * ("http://E:\deepseek-webui\index.html"). A local path behind a scheme
     * wins over the scheme; otherwise the URL is taken as typed. */
    if (startsWithNoCase(text, "http://") || startsWithNoCase(text, "https://")) {
        rest = text + (startsWithNoCase(text, "https://") ? 8 : 7);

        if (isDrivePath(rest) || isUncPath(rest)) {
            char localPath[MAX_PATH_CHARS];
            const char* suffix;

            splitLocalSuffix(rest, localPath, sizeof(localPath), &suffix);
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    if (startsWithNoCase(text, "file:")) {
        const char* p = text + 5;
        const char* q;
        int slashes = 0;

        while (p[slashes] == '/')
            ++slashes;
        q = p + slashes;

        /* Windows separators, or a drive/UNC path without the third slash:
         * rebuild the canonical file:/// form (the engine needs it). A
         * ?query / #fragment rides along verbatim. The path may already be
         * percent-encoded (it came from a URL, not from a typed path): decode
         * first, or every rebuild would encode the '%' again. */
        if (strchr(p, '\\') || (slashes < 3 && (isDrivePath(q) || isUncPath(q)))) {
            char localPath[MAX_PATH_CHARS];
            char decoded[MAX_PATH_CHARS];
            const char* suffix;
            const char* candidate = (strchr(p, '\\') && slashes == 0) ? p : q;

            splitLocalSuffix(candidate, localPath, sizeof(localPath), &suffix);
            if (strchr(localPath, '\\')) {
                /* A typed Windows path: its '%' is a literal file-name byte,
                 * not an escape. Only URL paths get decoded. */
                strncpy(decoded, localPath, sizeof(decoded) - 1);
                decoded[sizeof(decoded) - 1] = 0;
            } else {
                decodeUrlPath(localPath, decoded, sizeof(decoded));
            }
            fileUrlFromLocalPath(decoded, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        if (slashes < 2 && q[0]) {
            /* file:C:/x */
            char localPath[MAX_PATH_CHARS];
            char decoded[MAX_PATH_CHARS];
            const char* suffix;

            splitLocalSuffix(q, localPath, sizeof(localPath), &suffix);
            decodeUrlPath(localPath, decoded, sizeof(decoded));
            fileUrlFromLocalPath(decoded, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    if (startsWithNoCase(text, "about:") || startsWithNoCase(text, "data:") ||
        startsWithNoCase(text, "blob:") || startsWithNoCase(text, "minishell:") ||
        startsWithNoCase(text, "javascript:") || startsWithNoCase(text, "chrome:")) {
        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }

    {
        /* A local path may carry a ?query / #fragment ("page.html?focus=b").
         * The suffix is not part of the file name: test and canonicalize the
         * path alone, then append the suffix verbatim. */
        char localPath[MAX_PATH_CHARS];
        const char* suffix;

        splitLocalSuffix(text, localPath, sizeof(localPath), &suffix);

        if (isDrivePath(localPath) || isUncPath(localPath)) {
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }

        /* Relative path that resolves to something on disk ("./x.html",
         * "debug/a.html"), optionally with a query. */
        if ((strchr(localPath, '\\') || strchr(localPath, '/')) && localPathExists(localPath)) {
            fileUrlFromLocalPath(localPath, out, outSize);
            appendLocalSuffix(out, outSize, suffix);
            return;
        }
    }

    /* "localhost:8971/x" has no dot but is a host, not a search term. */
    if (startsWithNoCase(text, "localhost") &&
        (text[9] == 0 || text[9] == ':' || text[9] == '/')) {
        _snprintf(out, outSize, "http://%s", text);
        out[outSize - 1] = 0;
        return;
    }

    if (strchr(text, ' ') || !strchr(text, '.')) {
        searchUrlFor(text, out, outSize);
        return;
    }

    _snprintf(out, outSize, "http://%s", text);
    out[outSize - 1] = 0;
}

/* Builds the tab in `index`, which the caller guarantees is free (createTab
 * picks it with nextFreeTab, the navigation guard reuses the slot it is
 * respawning).  Split out from createTab so a view the engine never started
 * can be replaced without inventing a second tab-construction path. */
static int createTabIn(int index, const char* url)
{
    AgaveWebViewCallback callbacks;
    Tab* tab;

    if (index < 0 || index >= MAX_TABS)
        return 0;

    tab = &g_tabs[index];
    tabClearIcon(tab);
    memset(tab, 0, sizeof(Tab));
    memset(&callbacks, 0, sizeof(callbacks));

    callbacks.on_title = onTitle;
    callbacks.on_url = onUrl;
    callbacks.on_progress = onProgress;
    callbacks.on_status = onStatus;
    callbacks.on_message = onPageMessage;
    callbacks.on_favicon = onFavicon;
    callbacks.on_focus = onViewFocus;
    callbacks.on_error = onError;
    callbacks.on_crashed = onCrashed;
    callbacks.on_dialog = onDialog;
    callbacks.on_new_window = onNewWindow;
    callbacks.on_close_window = onCloseWindow;
    callbacks.on_download = onDownload;
    callbacks.on_context_menu = onContextMenu;
    callbacks.on_unresponsive = onUnresponsive;
    callbacks.on_command_dropped = onCommandDropped;
    callbacks.on_activate_view = onActivateView;

    tab->used = 1;
    tab->window = g_cur;
    {
        /* Create the control at the rect layoutTab() will use. It used to be
         * created at a placeholder 800x400 and resized on activation, so a new
         * tab showed a page surface that covered only part of the content area
         * (the rest was the shell's white background) until the resize landed -
         * the "torn page area" when opening a tab. */
        UiRect content = CHROME->content;

        if (content.w < 1)
            content.w = 1;
        if (content.h < 1)
            content.h = 1;

        tab->view = agave_webview_create(g_hwnd, content.x, content.y,
                                         content.w, content.h, &callbacks, tab);
    }

    if (!tab->view) {
        agbLog(AGB_LOG_SHELL, "tab create failed: view=null url=%s", url ? url : "");
        tab->used = 0;
        return 0;
    }

    tab->hwnd = (HWND)agave_webview_hwnd(tab->view);
    strncpy(tab->url, url, MAX_PATH_CHARS - 1);
    ShowWindow(tab->hwnd, SW_HIDE);

    /* The page sees the window's DPI from its first layout on, not from the
     * next WM_DPICHANGED: a tab opened on a 150% monitor must not render at
     * 100% until the window happens to change monitors. */
    agave_webview_set_device_scale(tab->view, deviceScalePercent(CHROME->theme.dpi));

    agbLog(AGB_LOG_SHELL, "tab create index=%d dpi=%d%% url=%s", index,
           deviceScalePercent(CHROME->theme.dpi), tab->url);

    /* The GUI saves where a user expects to find downloads; the probe keeps the
     * engine default (<AGB_USERDATA>/downloads) so tests never touch this. */
    {
        char downloads[MAX_PATH_CHARS];
        static int logged;

        if (shellOps()->downloads_dir(NULL, downloads, sizeof(downloads))) {
            agave_webview_set_download_dir(tab->view, downloads);

            /* Once per process: the directory is per view, the message is not. */
            if (!logged) {
                logged = 1;
                agbLog(AGB_LOG_SHELL, "download dir=%s", downloads);
            }
        }
    }

    agave_webview_set_zoom(tab->view, g_zoom);
    setActiveTab(index);

    navGuardWatch(tab, url);
    agave_webview_navigate(tab->view, url);

    /* Restored sessions and startup URLs: show what the last visit's icon was
     * before the page (and its network) is anywhere near ready. */
    tabLoadCachedIcon(tab);
    if (tab->icon) {
        syncChrome();
        InvalidateRect(g_hwnd, &g_cur->chrome.tabstrip.rect, FALSE);
    }

    return 1;
}

static void createTab(const char* url)
{
    int index = nextFreeTab();

    if (index < 0) {
        agbLog(AGB_LOG_SHELL, "tab create failed: no free slot (max %d)", MAX_TABS);
        return;
    }

    createTabIn(index, url);
}

/* ------------------------------------------------------------------------- */
/* navigation guard                                                          */
/* ------------------------------------------------------------------------- */

/* miniblink can drop the *first* navigation of a fresh control without a word:
 * no mbOnURLChanged, no mbOnLoadingFinish, no error - the engine's other
 * threads stay healthy (a new control in the same process loads fine), so the
 * tab sits at 5% forever and not even mbLoadURL revives that view.  It is a
 * startup race (observed right after a previous instance was killed, and after
 * mbReload landed before the first commit), so the shell watches the first
 * seconds of every real navigation and rebuilds the one control that never
 * answered. */
#define NAV_GUARD_TIMER        2
/* Headless regression hooks (AGB_TEST_CLOSE_TAB_MS / AGB_TEST_CLOSE_CHIP): the
 * timers are handled with the other WM_TIMER cases below. */
#define TEST_CLOSE_TAB_TIMER   3
#define TEST_SLIDE_END_TIMER   4
#define NAV_GUARD_POLL_MS      250
#define NAV_GUARD_DEFAULT_MS   4000
#define NAV_GUARD_MAX_RESPAWNS 2

static long g_navGuardMs = -1;
static int  g_navGuardOn;

static long navGuardDeadMs(void)
{
    if (g_navGuardMs < 0) {
        const char* env = getenv("AGB_NAV_GUARD_MS");
        long value = (env && env[0]) ? atol(env) : 0;

        g_navGuardMs = value > 0 ? value : NAV_GUARD_DEFAULT_MS;
    }

    return g_navGuardMs;
}

/* Only a document-producing URL is expected to report a commit; about:blank,
 * minishell:// pages and javascript: never do, and guarding them would rebuild
 * a perfectly fine tab. */
static int urlExpectsCommit(const char* url)
{
    return url && (startsWithNoCase(url, "http://") ||
                   startsWithNoCase(url, "https://") ||
                   startsWithNoCase(url, "file://"));
}

static void armNavGuard(void)
{
    if (g_navGuardOn || !g_hwnd)
        return;

    g_navGuardOn = 1;
    SetTimer(g_hwnd, NAV_GUARD_TIMER, NAV_GUARD_POLL_MS, NULL);
}

static void navGuardSettled(Tab* tab)
{
    if (!tab)
        return;

    tab->navStartMs = 0;
    tab->navRespawns = 0;
}

/* Called right before a shell-initiated navigation: watch it when it is
 * supposed to produce a document, and let navGuardTick rebuild the control if
 * nothing arrives.  Reload of a committed document is not watched - mbReload
 * is reliable there, and its own finish callback settles the tab. */
static void navGuardWatch(Tab* tab, const char* url)
{
    if (!tab || !urlExpectsCommit(url))
        return;

    tab->navStartMs = shellOps()->tick_ms(NULL);
    armNavGuard();
}

/* Rebuild one tab's control in place, keeping the slot, the URL and (when the
 * dead tab was active) the selection.  The old view is destroyed first: a
 * control that never committed cannot be reused, but the engine can. */
static void respawnTabIn(int index, const char* why)
{
    Tab* tab = &g_tabs[index];
    char url[MAX_PATH_CHARS];
    int respawns;
    int wasActive;
    int oldActive = g_activeTab;

    if (!tab->used || !tab->view || !tab->url[0])
        return;

    strncpy(url, tab->url, sizeof(url) - 1);
    url[sizeof(url) - 1] = 0;
    respawns = tab->navRespawns + 1;
    wasActive = (index == g_activeTab);

    agbLog(AGB_LOG_SHELL, "nav guard: %s - rebuilding view index=%d try=%d url=%s",
           why, index, respawns, url);

    agave_webview_destroy(tab->view);
    tab->view = NULL;
    tab->hwnd = NULL;
    tab->used = 0;

    if (createTabIn(index, url)) {
        g_tabs[index].navRespawns = respawns;

        /* createTabIn() selects the tab it builds; a background tab must not
         * steal the selection back from the one the user is looking at. */
        if (!wasActive && oldActive != index &&
            oldActive >= 0 && oldActive < MAX_TABS && g_tabs[oldActive].used)
            setActiveTab(oldActive);
    }
}

static void navGuardTick(void)
{
    unsigned long long now = shellOps()->tick_ms(NULL);
    unsigned long long deadMs = (unsigned long long)navGuardDeadMs();
    int pending = 0;
    int i;

    for (i = 0; i < MAX_TABS; ++i) {
        Tab* tab = &g_tabs[i];

        if (!tab->used || !tab->view || !tab->navStartMs)
            continue;

        if (now - tab->navStartMs < deadMs) {
            pending = 1;
            continue;
        }

        if (tab->loading && tab->progress <= 5) {
            if (tab->navRespawns < NAV_GUARD_MAX_RESPAWNS) {
                respawnTabIn(i, "engine never committed");
                pending = 1;
                continue;
            }

            /* Two fresh controls and still nothing: stop pretending to load.
             * The page area keeps the placeholder, the status line says why. */
            tab->navStartMs = 0;
            tab->loading = 0;
            tab->progress = 100;
            strncpy(tab->status, "Page did not load (engine did not respond)",
                    sizeof(tab->status) - 1);
            agbLog(AGB_LOG_SHELL, "nav guard: giving up index=%d url=%s", i, tab->url);
            if (tab == &g_tabs[g_activeTab])
                updateStatus();
            continue;
        }

        tab->navStartMs = 0;
    }

    if (!pending && g_navGuardOn) {
        g_navGuardOn = 0;
        if (g_hwnd)
            KillTimer(g_hwnd, NAV_GUARD_TIMER);
    }
}

static void navigateActive(const char* text, int newTab, const char* source)
{
    char url[MAX_PATH_CHARS];

    /* Enter in an empty address bar is a no-op, not about:blank: on the new tab
     * page (whose omnibox is empty) it must not replace the page. */
    if (!text || !text[0])
        return;

    /* The marker is bound to the document it was started on. */
    if (g_inspectOn)
        inspectStop("navigate");

    normalizeUrl(text, url, sizeof(url));

    /* The one line that tells which entry point started the navigation: the
     * engine only logs [nav] open url, which every path shares. Without this
     * an address-bar Enter, a Go click and a history-menu pick look identical. */
    agbLog(AGB_LOG_SHELL, "navigate (%s) url=%s newtab=%d",
           source ? source : "?", url, newTab);

    if (newTab || !g_tabs[g_activeTab].used) {
        createTab(url);
        return;
    }

    strncpy(g_tabs[g_activeTab].url, url, MAX_PATH_CHARS - 1);
    recordHistory(url);
    navGuardWatch(&g_tabs[g_activeTab], url);

    {
        wchar_t wideUrl[MAX_PATH_CHARS];

        utf8ToWide(url, wideUrl, MAX_PATH_CHARS);
        SetWindowTextW(g_address, wideUrl);
    }

    agave_webview_navigate(g_tabs[g_activeTab].view, url);

    /* Hand the keyboard back to the page: with focus left in the address bar the
     * next keystroke edits the URL that was just typed. */
    focusPage();
}

/* Reload / back / forward / stop are each reachable from five places (toolbar,
 * F5 or ctrl+r, alt+arrow, main menu, context menu). The engine line they
 * produce ([nav] reload) only says *what* happened, never *what the user did* -
 * and "which refresh" is the first question in every white-page report. */
static void reloadActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view) {
        agbLog(AGB_LOG_SHELL, "reload (%s) ignored: no tab", source);
        return;
    }

    agbLog(AGB_LOG_SHELL, "reload (%s) url=%s", source, tab->url);
    agave_webview_reload(tab->view);
}

/* Ctrl+F5 / Ctrl+Shift+R. */
static void reloadHardActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view) {
        agbLog(AGB_LOG_SHELL, "reload hard (%s) ignored: no tab", source);
        return;
    }

    agbLog(AGB_LOG_SHELL, "reload hard (%s) url=%s", source, tab->url);
    agave_webview_reload_hard(tab->view);
}

static void backActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "back (%s) can=%d url=%s", source,
           agave_webview_can_back(tab->view) ? 1 : 0, tab->url);
    agave_webview_back(tab->view);
}

/* ------------------------------------------------------------------------- */
/* tab strip context menu                                                    */
/* ------------------------------------------------------------------------- */

/* Tabs live in fixed slots: a view keeps the Tab* it was created with, so the
 * array can never be shifted. "To the right" therefore means "the first free
 * slot after this one" - adjacent whenever the neighbour slot is free. */
static int freeSlotAfter(int index)
{
    int i;

    for (i = index + 1; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            return i;
    }

    return -1;
}

static void duplicateTab(int index)
{
    int slot;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    slot = freeSlotAfter(index);
    if (slot < 0)
        slot = nextFreeTab();
    if (slot < 0)
        return;

    agbLog(AGB_LOG_SHELL, "tab duplicate index=%d slot=%d url=%s", index, slot,
           g_tabs[index].url);
    createTabIn(slot, g_tabs[index].url);
}

static void reloadTab(int index)
{
    Tab* tab;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    tab = &g_tabs[index];
    if (!tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "reload (tab menu) index=%d url=%s", index, tab->url);
    agave_webview_reload(tab->view);
}

/* Closing walks downwards so the indices of the tabs still to close stay valid
 * (slots are sparse; closeTab only clears the one slot). */
static void closeOtherTabs(int keep)
{
    int i;

    if (keep < 0 || keep >= MAX_TABS || !g_tabs[keep].used)
        return;

    agbLog(AGB_LOG_SHELL, "tab close others keep=%d", keep);

    for (i = MAX_TABS - 1; i >= 0; --i) {
        if (i != keep && g_tabs[i].used)
            closeTab(i);
    }

    if (g_activeTab != keep)
        setActiveTab(keep);
}

static void closeTabsToRight(int from)
{
    int i;

    if (from < 0 || from >= MAX_TABS || !g_tabs[from].used)
        return;

    agbLog(AGB_LOG_SHELL, "tab close right of=%d", from);

    for (i = MAX_TABS - 1; i > from; --i) {
        if (g_tabs[i].used)
            closeTab(i);
    }

    if (!g_tabs[g_activeTab].used)
        setActiveTab(from);
}

static void showTabMenu(int index, int screenX, int screenY)
{
    UiMenu menu;
    ShellWindow* previous;
    int others = 0;
    int right = 0;
    int i;
    int choice;

    if (index < 0 || index >= MAX_TABS || !g_tabs[index].used)
        return;

    for (i = 0; i < MAX_TABS; ++i) {
        if (!g_tabs[i].used)
            continue;

        if (i != index)
            ++others;
        if (i > index)
            ++right;
    }

    uiMenuInit(&menu);
    uiMenuAdd(&menu, "New tab to the right", NULL, 50, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Reload", "Ctrl+R", 51, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&menu);
    uiMenuAdd(&menu, "Duplicate", NULL, 52, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Close", "Ctrl+W", 53, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Close other tabs", NULL,
              54, others ? 0 : UI_MENU_ITEM_DISABLED, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&menu, "Close tabs to the right", NULL,
              55, right ? 0 : UI_MENU_ITEM_DISABLED, UI_MENU_ICON_NONE, NULL);
    uiMenuTrim(&menu);

    agbLog(AGB_LOG_SHELL, "tab menu open index=%d others=%d right=%d", index, others, right);

    /* uiMenuHostRun runs a modal loop; the dispatch below must act on this
     * window even if the loop moved the focus. */
    previous = g_cur;

    {
        POINT anchor;

        anchor.x = screenX;
        anchor.y = screenY;
        choice = uiMenuHostRun(&CHROME->theme, &menu, anchor.x, anchor.y,
                               UI_MENU_ALIGN_LEFT, g_hwnd, NULL);
    }

    g_cur = previous;

    agbLog(AGB_LOG_SHELL, "tab menu choice=%d", choice);

    switch (choice) {
    case 50: {
        int slot = freeSlotAfter(index);

        if (slot >= 0)
            createTabIn(slot, NEW_TAB_URL);
        break;
    }
    case 51:
        reloadTab(index);
        break;
    case 52:
        duplicateTab(index);
        break;
    case 53:
        closeTab(index);
        break;
    case 54:
        closeOtherTabs(index);
        break;
    case 55:
        closeTabsToRight(index);
        break;
    default:
        break;
    }
}

static void forwardActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "forward (%s) can=%d url=%s", source,
           agave_webview_can_forward(tab->view) ? 1 : 0, tab->url);
    agave_webview_forward(tab->view);
}

static void stopActive(const char* source)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    agbLog(AGB_LOG_SHELL, "stop (%s) url=%s", source, tab->url);
    agave_webview_stop(tab->view);
}

static void addBookmarkCurrent(void)
{
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used)
        return;

    /* The status line has to match what really happened: a duplicate URL is a
     * no-op, and claiming "Bookmark added" made the menu look broken. */
    if (addBookmark(tab->url, tab->title)) {
        agbLog(AGB_LOG_SHELL, "bookmark add url=%s total=%d", tab->url, g_bookmarkCount);
        strncpy(tab->status, "Bookmark added", sizeof(tab->status) - 1);
    } else {
        agbLog(AGB_LOG_SHELL, "bookmark duplicate url=%s", tab->url);
        strncpy(tab->status, "Already bookmarked", sizeof(tab->status) - 1);
    }

    updateStatus();
}

static void findQueryChanged(void);

/* One search per burst of edits: an IME word commit arrives as several
 * EN_CHANGEs inside the same message, and each one used to run the page. The
 * queued message runs after the burst, so only the final text is searched. */
static void findPostUpdate(void)
{
    if (!g_hwnd || !g_findVisible || g_findUpdatePending)
        return;

    g_findUpdatePending = 1;

    if (!PostMessageW(g_hwnd, AGW_FIND_UPDATE, 0, 0))
        g_findUpdatePending = 0;
}

static void toggleFind(void)
{
    g_findVisible = !g_findVisible;

    agbLog(AGB_LOG_SHELL, "find bar %s", g_findVisible ? "on" : "off");

    /* The find ring dies with the bar (and a stale ring from the chrome ring
     * must not stay painted while the bar opens). */
    uiChromeFocusClear(CHROME);

    /* The counter belongs to the query that produced it: opening a bar or
     * closing it starts from "0/0" until the page answers. The query text is
     * re-searched on open so the bar never shows highlights without a count. */
    g_findIndex = 0;
    g_findTotal = 0;
    g_findQuery[0] = 0;
    g_findComposing = 0;

    if (!g_findVisible) {
        Tab* tab = &g_tabs[g_activeTab];

        /* Closing the bar has to clear the page: the marks live in the document,
         * and an empty query is the script's "remove everything" path. */
        if (tab->used && tab->view)
            agave_webview_search(tab->view, "", 1, 0);
    }

    /* The find bar is painted by UiChrome now; only the native EDIT is shown or
     * hidden, and the layout follows the model's find_visible flag. */
    ShowWindow(g_findEdit, g_findVisible ? SW_SHOW : SW_HIDE);

    syncChrome();
    layoutAll();

    if (g_findVisible) {
        releasePageFocus();
        g_focusOwner = FOCUS_FIND;
        SetFocus(g_findEdit);

        /* Re-run whatever is in the field: the marks in the page may belong to
         * a previous session, and the counter must match what is highlighted. */
        findQueryChanged();
    } else {
        focusPage();
    }

    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Ctrl+F / the app menu always *open* the bar and select the query; only the
 * close button and Esc take it away (Chrome). The old toggle made a second
 * Ctrl+F hide a bar the user was trying to get back to. */
static void openFind(void)
{
    if (!g_findVisible) {
        toggleFind();
        return;
    }

    if (g_findEdit) {
        SetFocus(g_findEdit);
        SendMessageW(g_findEdit, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
    }
}

static void doFind(int forward)
{
    char text[256];
    Tab* tab = &g_tabs[g_activeTab];

    if (!tab->used || !tab->view)
        return;

    readControlText(g_findEdit, text, sizeof(text));

    if (!text[0]) {
        agbLog(AGB_LOG_SHELL, "find: empty query, ignored");
        return;
    }

    strncpy(g_findQuery, text, sizeof(g_findQuery) - 1);
    g_findQuery[sizeof(g_findQuery) - 1] = 0;

    /* The engine gives no "hits" back; the page's find script reports the
     * counter over "__agb_findcount__" once it has counted the matches. */
    agbLog(AGB_LOG_SHELL, "find text='%s' forward=%d", text, forward);
    agave_webview_search(tab->view, text, forward, 0);
}

/* Search as you type, like Chrome: without it the counter and the highlights
 * only changed after pressing Enter, so the bar looked dead while typing. */
static void findQueryChanged(void)
{
    char text[256];
    Tab* tab = &g_tabs[g_activeTab];

    if (!g_findVisible || !tab->used || !tab->view)
        return;

    readControlText(g_findEdit, text, sizeof(text));
    agbLog(AGB_LOG_SHELL, "find query: current='%s' last='%s'", text, g_findQuery);

    if (strcmp(text, g_findQuery) == 0)
        return;

    strncpy(g_findQuery, text, sizeof(g_findQuery) - 1);
    g_findQuery[sizeof(g_findQuery) - 1] = 0;

    g_findIndex = 0;
    g_findTotal = 0;

    /* An empty query clears the highlights in the page. */
    agbLog(AGB_LOG_SHELL, "find search (typing) '%s'", text);
    agave_webview_search(tab->view, text, 1, 0);

    syncChrome();
    invalidateFindOverlay();
}

/* One place that clamps, applies and logs the zoom: it used to be four copies of
 * the same three lines, invisible in the log even though zoom changes vw/vh and
 * every layout width bug report depends on it. */
/* Chrome's zoom ladder. Stepping by a flat 25% made one Ctrl+plus jump straight
 * from 100% to 125% (and down to 75%), which reads as "the factor is
 * amplified"; the real browser takes small steps near 100% and larger ones at
 * the ends. */
static const int kZoomSteps[] = {
    25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400
};

/* Ctrl+wheel delta that has not made up a full notch yet. A mouse sends 120 per
 * notch; a precision touchpad sends smaller deltas and would otherwise zoom once
 * per event and shoot past the ladder. */
static int g_zoomWheelAccum;

static int zoomStep(int percent, int direction)
{
    int count = (int)(sizeof(kZoomSteps) / sizeof(kZoomSteps[0]));
    int i;

    if (direction > 0) {
        for (i = 0; i < count; ++i) {
            if (kZoomSteps[i] > percent)
                return kZoomSteps[i];
        }
        return kZoomSteps[count - 1];
    }

    for (i = count - 1; i >= 0; --i) {
        if (kZoomSteps[i] < percent)
            return kZoomSteps[i];
    }

    return kZoomSteps[0];
}

static void setZoom(int percent, const char* source)
{
    int before = g_zoom;

    if (percent < 25)
        percent = 25;
    if (percent > 400)
        percent = 400;

    g_zoom = percent;
    applyZoomToActive();
    updateStatus();

    if (before != g_zoom)
        agbLog(AGB_LOG_SHELL, "zoom %d%% -> %d%% (%s)", before, g_zoom, source);
}

static void updateToolbarState(void)
{
    syncChrome();
    InvalidateRect(g_hwnd, NULL, FALSE);
    syncTabSearch();
}

/* ------------------------------------------------------------------------- */
/* menu                                                                      */
/* ------------------------------------------------------------------------- */

static void rethemeShellWindow(ShellWindow* win);

/* The menu's Theme rows. Remember the choice, then re-theme every window (the
 * menu can be opened from any of them). A window already showing the resulting
 * kind is left alone, so picking Light while the OS is light changes nothing on
 * screen - but it is still the remembered preference, and rethemeShellWindow()
 * logs nothing in that case. */
static void applyThemePref(int pref)
{
    ShellWindow* previous = g_cur;
    int before = g_themePref;
    int i;

    /* The popup holds a pointer to the window's theme; the retheme frees it. */
    if (uiTabSearchIsOpen())
        uiTabSearchClose();

    g_themePref = pref;
    agbLog(AGB_LOG_SHELL, "theme %s -> %s (%s)",
           themePrefName(before), themePrefName(pref),
           before == pref ? "unchanged" : "menu");

    if (pref != before)
        themePrefSave();

    for (i = 0; i < MAX_SHELL_WINDOWS; ++i) {
        if (g_windows[i].used)
            rethemeShellWindow(&g_windows[i]);
    }

    /* A visible new tab page is a web document, not chrome: tell it the new
     * light/dark choice (the settings page learns it from its own values). */
    if (g_cur && g_tabs[g_activeTab].used && g_tabs[g_activeTab].view &&
        !strcmp(g_tabs[g_activeTab].url, NEW_TAB_URL))
        newTabSendTheme(g_tabs[g_activeTab].view);

    g_cur = previous;
}

/* ------------------------------------------------------------------------- */
/* settings page (minishell://settings)                                      */
/* ------------------------------------------------------------------------- */

/* Values that actually reached the page in the current settingsSendAll():
 * agave_webview_post_message() drops them until the view has a document. */
static int g_settingsDelivered;

/* One "key=value" per message: the page then needs no JSON parser, and a path
 * (spaces, backslashes) survives as-is because '=' is the only separator. */
static void settingsSend(AgaveWebView* view, const char* key, const char* value)
{
    char message[4096];
    char* p;

    if (!view || !key)
        return;

    _snprintf(message, sizeof(message) - 1, "__agb_settings__%s=%s", key,
              value ? value : "");
    message[sizeof(message) - 1] = 0;

    for (p = message; *p; ++p) {
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    }

    g_settingsDelivered += agave_webview_post_message(view, message);
}

static void settingsSendAll(AgaveWebView* view)
{
    char text[MAX_PATH_CHARS];
    char number[32];

    g_settingsDelivered = 0;

    settingsSend(view, "theme", themePrefName(g_themePref));
    settingsSend(view, "search", kEngineNames[g_prefs.search]);

    _snprintf(number, sizeof(number), "%d", g_prefs.zoom);
    settingsSend(view, "zoom", number);

    _snprintf(number, sizeof(number), "%d", g_prefs.restore);
    settingsSend(view, "restore", number);

    _snprintf(number, sizeof(number), "%d", g_historyCount);
    settingsSend(view, "history", number);

    _snprintf(number, sizeof(number), "%d", g_bookmarkCount);
    settingsSend(view, "bookmarks", number);

    if (shellOps()->downloads_dir(NULL, text, sizeof(text)))
        settingsSend(view, "downloads", text);

    if (shellOps()->profile_root(NULL, text, sizeof(text)))
        settingsSend(view, "profile", text);

    /* Read-only display facts: the Windows scaling the shell follows, and the
     * raw DPI it came from. A probe has no shell window, so it reports the
     * device scale it was started with. */
    {
        int scalePercent = 100;
        unsigned dpi = 96;

        if (g_cur) {
            dpi = g_cur->chrome.theme.dpi;
            scalePercent = deviceScalePercent(dpi);
        } else if (view) {
            scalePercent = agave_webview_device_scale(view);
            if (scalePercent <= 0)
                scalePercent = 100;
            dpi = (unsigned)MulDiv(96, scalePercent, 100);
        }

        _snprintf(text, sizeof(text), "%d%%", scalePercent);
        settingsSend(view, "scale", text);

        _snprintf(text, sizeof(text), "%u dpi", dpi);
        settingsSend(view, "dpi", text);
    }

    /* Shell only: the engine has its own rows right below (version, SHA-256).
     * "miniblink 132" used to be glued to the shell build, which read as one
     * product. */
    _snprintf(text, sizeof(text), "%s, built %s",
              buildInfoShellVersion(), buildInfoShellBuild());
    settingsSend(view, "version", text);

    /* Identification a bug report needs; the same facts go into the crash
     * report (build_info.c), and the engine hash is the build's fingerprint. */
    settingsSend(view, "project", buildInfoProjectUrl());
    settingsSend(view, "engine", buildInfoEngineVersion());
    settingsSend(view, "enginehash", buildInfoEngineSha256());
    settingsSend(view, "launch", buildInfoLaunchArgs());
    settingsSend(view, "env", buildInfoLaunchEnv());

    settingsSend(view, "done", "1");

    /* One line per answer: a restored background settings tab used to be
     * silently dropped by the active-tab gate, so this makes the round trip
     * observable from a headless run (and filters the shared log by pid).
     * delivered==0 means the view had no document and nothing reached it. */
    agbLog(AGB_LOG_SHELL, "settings: values sent delivered=%d (view=%p pid=%lu)",
           g_settingsDelivered, (void*)view, (unsigned long)GetCurrentProcessId());
}

static void settingsApply(const char* key, const char* value)
{
    if (!key || !value)
        return;

    if (!strcmp(key, "theme")) {
        int pref = SHELL_THEME_SYSTEM;

        if (themePrefFromName(value, &pref)) {
            agbLog(AGB_LOG_SHELL, "settings: theme -> %s", value);
            applyThemePref(pref);
        }
    } else if (!strcmp(key, "search")) {
        int i;

        for (i = 0; i < ENGINE_COUNT; ++i) {
            if (!_stricmp(value, kEngineNames[i])) {
                agbLog(AGB_LOG_SHELL, "settings: search -> %s", value);
                g_prefs.search = i;
                prefsSave();
                break;
            }
        }
    } else if (!strcmp(key, "zoom")) {
        int zoom = atoi(value);
        int w, t;

        if (zoom < 25)
            zoom = 25;
        if (zoom > 400)
            zoom = 400;

        agbLog(AGB_LOG_SHELL, "settings: zoom -> %d (all windows/tabs)", zoom);
        g_prefs.zoom = zoom;
        prefsSave();

        /* A default that only affects future tabs is invisible: apply it to every
         * tab that already exists, in every window, right away. */
        for (w = 0; w < MAX_SHELL_WINDOWS; ++w) {
            ShellWindow* win = &g_windows[w];

            if (!win->used)
                continue;

            win->zoom = zoom;

            for (t = 0; t < MAX_TABS; ++t) {
                if (win->tabs[t].used && win->tabs[t].view)
                    agave_webview_set_zoom(win->tabs[t].view, zoom);
            }
        }

        if (g_cur) {
            syncChrome();
            updateStatus();
            InvalidateRect(g_cur->hwnd, NULL, FALSE);
        }
    } else if (!strcmp(key, "restore")) {
        g_prefs.restore = atoi(value) ? 1 : 0;
        agbLog(AGB_LOG_SHELL, "settings: restore -> %d", g_prefs.restore);
        prefsSave();
    }
}

static void settingsAction(AgaveWebView* view, const char* action)
{
    if (action && !strcmp(action, "clear-history")) {
        clearHistoryAll();
        agbLog(AGB_LOG_SHELL, "settings: history cleared");
    } else if (action && !strcmp(action, "open-downloads")) {
        char dir[MAX_PATH_CHARS];

        if (shellOps()->downloads_dir(NULL, dir, sizeof(dir))) {
            agbLog(AGB_LOG_SHELL, "settings: open downloads %s", dir);
            ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
        }
    } else if (action && !strcmp(action, "open-profile")) {
        char dir[MAX_PATH_CHARS];

        if (shellOps()->profile_root(NULL, dir, sizeof(dir))) {
            agbLog(AGB_LOG_SHELL, "settings: open profile %s", dir);
            ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
        }
    }

    settingsSendAll(view);
}

static void showMainMenu(void)
{
    UiMenu root;
    UiMenu bookmarks;
    UiMenu history;
    UiMenu theme;
    POINT anchor;
    int choice;
    int i;

    /* Submenu labels borrow the bookmarks/history storage: it stays valid for
     * the whole modal call, which is all uiMenuHostRun needs. */
    uiMenuInit(&bookmarks);

    for (i = 0; i < g_bookmarkCount && i < 20; ++i)
        uiMenuAdd(&bookmarks,
                  g_bookmarks[i].title[0] ? g_bookmarks[i].title : g_bookmarks[i].url,
                  NULL, 100 + i, 0, UI_MENU_ICON_NONE, NULL);

    if (g_bookmarkCount == 0)
        uiMenuAdd(&bookmarks, "(empty)", NULL, 0, UI_MENU_ITEM_DISABLED,
                  UI_MENU_ICON_NONE, NULL);

    uiMenuInit(&history);

    for (i = g_historyCount - 1; i >= 0 && i >= g_historyCount - 20; --i)
        uiMenuAdd(&history, g_history[i], NULL,
                  200 + (g_historyCount - 1 - i), 0, UI_MENU_ICON_NONE, NULL);

    if (g_historyCount == 0)
        uiMenuAdd(&history, "(empty)", NULL, 0, UI_MENU_ITEM_DISABLED,
                  UI_MENU_ICON_NONE, NULL);

    /* Appearance: three exclusive rows, check mark on the active one. Ids
     * 20..22 sit below the bookmark (100+) and history (200+) ranges, like the
     * root's own commands. */
    uiMenuInit(&theme);
    uiMenuAdd(&theme, "System", NULL, 20, 0,
              g_themePref == SHELL_THEME_SYSTEM ? UI_MENU_ICON_CHECK : UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&theme, "Light", NULL, 21, 0,
              g_themePref == SHELL_THEME_LIGHT ? UI_MENU_ICON_CHECK : UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&theme, "Dark", NULL, 22, 0,
              g_themePref == SHELL_THEME_DARK ? UI_MENU_ICON_CHECK : UI_MENU_ICON_NONE, NULL);

    uiMenuInit(&root);
    uiMenuAdd(&root, "Add bookmark", "Ctrl+D", 1, 0, UI_MENU_ICON_NONE, NULL);    uiMenuAdd(&root, "Bookmarks", NULL, 0, 0, UI_MENU_ICON_NONE, &bookmarks);
    uiMenuAdd(&root, "History", NULL, 0, 0, UI_MENU_ICON_NONE, &history);
    uiMenuAdd(&root, "Downloads", "Ctrl+J", 10, 0, UI_MENU_ICON_DOWNLOAD, NULL);
    uiMenuAddSeparator(&root);
    uiMenuAdd(&root, "Zoom in", "Ctrl+=", 2, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&root, "Zoom out", "Ctrl+-", 3, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&root, "Reset zoom", "Ctrl+0", 4, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&root);
    uiMenuAdd(&root, "New tab", "Ctrl+T", 5, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&root, "Find", "Ctrl+F", 6, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&root, "Reload", "F5", 7, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&root);
    uiMenuAdd(&root, "Inspect page", "F12", 8, 0,
              g_inspectOn ? UI_MENU_ICON_CHECK : UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&root);
    /* The current choice rides on the row's right edge, like a shortcut: the
     * submenu itself is one click away and the check mark is only visible after
     * opening it. */
    uiMenuAdd(&root, "Theme", themePrefLabel(g_themePref), 0, 0, UI_MENU_ICON_NONE, &theme);
    /* Settings is a real page now (minishell://settings). */
    uiMenuAdd(&root, "Settings", NULL, 11, 0, UI_MENU_ICON_NONE, NULL);

    /* Chrome anchors the app menu under the button, right edges aligned. */
    {
        UiRect button = CHROME->toolbar.menu.rect;

        anchor.x = button.x + button.w;
        anchor.y = button.y + button.h + uiThemeScale(&CHROME->theme, 2);
    }
    ClientToScreen(g_hwnd, &anchor);

    /* Keep the 3-dot button lit while its menu is open, like Chrome. */
    CHROME->toolbar.menu.toggled = 1;
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);

    agbLog(AGB_LOG_SHELL, "app menu open");

    {
        /* uiMenuHostRun runs a modal loop; restore the context so the menu
         * actions above/below always act on this window. */
        ShellWindow* previous = g_cur;

        choice = uiMenuHostRun(&CHROME->theme, &root, anchor.x, anchor.y,
                               UI_MENU_ALIGN_RIGHT, g_hwnd, NULL);
        g_cur = previous;
    }

    /* The app menu has no "re-open where the pointer is" behaviour in Chrome:
     * a right click just puts it away (UI_MENU_REOPEN falls through the switch
     * below as a no-op). */
    agbLog(AGB_LOG_SHELL, "app menu choice=%d", choice);

    CHROME->toolbar.menu.toggled = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);

    if (choice >= 200) {
        int index = g_historyCount - 1 - (choice - 200);

        if (index >= 0 && index < g_historyCount)
            navigateActive(g_history[index], 0, "history menu");
        return;
    }

    if (choice >= 100) {
        int index = choice - 100;

        if (index >= 0 && index < g_bookmarkCount)
            navigateActive(g_bookmarks[index].url, 0, "bookmark menu");
        return;
    }

    switch (choice) {
    case 1: addBookmarkCurrent(); break;
    case 2: setZoom(zoomStep(g_zoom, 1), "menu zoom in"); break;
    case 3: setZoom(zoomStep(g_zoom, -1), "menu zoom out"); break;
    case 4: setZoom(100, "menu reset zoom"); break;
    case 5: navigateActive(NEW_TAB_URL, 1, "menu"); break;
    case 6: openFind(); break;
    case 7: reloadActive("menu"); break;
    case 8: toggleInspect(); break;
    /* Chrome opens both of these in a new tab; Ctrl+J does the same thing for
     * downloads, so the menu entry and the shortcut cannot drift apart. */
    case 10: navigateActive("minishell://downloads", 1, "menu downloads"); break;
    case 11: navigateActive("minishell://settings", 1, "menu settings"); break;
    case 20: applyThemePref(SHELL_THEME_SYSTEM); break;
    case 21: applyThemePref(SHELL_THEME_LIGHT); break;
    case 22: applyThemePref(SHELL_THEME_DARK); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* painting: UiChrome owns the pixels now                                     */
/* ------------------------------------------------------------------------- */

/* Double-buffered chrome paint: the tab strip, toolbar, find bar and status
 * bar are all GDI+ antialiased shapes, so painting straight to the window DC
 * would flash on every hover/resize. */

/* The backing store is kept across paints: a status-bubble text change
 * invalidates the whole window and a load produces several of them per second,
 * so allocating a 1100x800 DIB (and its DC) on every WM_PAINT was pure UI-thread
 * overhead. Recreated only when the client size or the window changes. */
static HDC     g_paintDc;
static HBITMAP g_paintBitmap;
static HGDIOBJ g_paintOld;
static HWND    g_paintHwnd;
static int     g_paintW, g_paintH;

static void paintCacheFree(void)
{
    if (g_paintDc) {
        if (g_paintOld)
            SelectObject(g_paintDc, g_paintOld);
        DeleteDC(g_paintDc);
    }
    if (g_paintBitmap)
        DeleteObject(g_paintBitmap);

    g_paintDc = NULL;
    g_paintBitmap = NULL;
    g_paintOld = NULL;
    g_paintHwnd = NULL;
    g_paintW = 0;
    g_paintH = 0;
}
static void paintShell(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc;

    hdc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &client);

    if (client.right <= 0 || client.bottom <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    syncChrome();

    if (g_paintHwnd != hwnd || g_paintW != client.right || g_paintH != client.bottom) {
        paintCacheFree();
        g_paintDc = CreateCompatibleDC(hdc);
        g_paintBitmap = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        if (g_paintDc && g_paintBitmap) {
            g_paintOld = SelectObject(g_paintDc, g_paintBitmap);
            g_paintHwnd = hwnd;
            g_paintW = client.right;
            g_paintH = client.bottom;
        } else {
            paintCacheFree();
        }
    }

    if (g_paintDc) {
        uiChromePaint(CHROME, g_paintDc);
        BitBlt(hdc, 0, 0, client.right, client.bottom, g_paintDc, 0, 0, SRCCOPY);
    }

    EndPaint(hwnd, &ps);
}

/* Repaint only the chrome parts the last mouse event changed. Called from
 * WM_MOUSEMOVE: the chrome lives in the HTCLIENT area, so Windows leaves the
 * window valid and hover feedback used to appear only when some other code
 * happened to invalidate it. */
static int dirtyHits(UiRect dirty, UiRect area)
{
    return dirty.w > 0 && dirty.h > 0 && area.w > 0 && area.h > 0 &&
           dirty.x < area.x + area.w && dirty.x + dirty.w > area.x &&
           dirty.y < area.y + area.h && dirty.y + dirty.h > area.y;
}

/* The native fields paint their own background with the pill's current fill
 * (WM_CTLCOLOREDIT), and child windows are not repainted by invalidating the
 * parent. So every pill state change - hover on/off and focus on/off alike -
 * has to invalidate the control, or the text keeps the old colour until
 * something else happens to touch it. */
static void syncFieldBackgrounds(void)
{
    if (g_address)
        InvalidateRect(g_address, NULL, FALSE);
    if (g_findEdit && IsWindowVisible(g_findEdit))
        InvalidateRect(g_findEdit, NULL, FALSE);

    /* The find bar's field is painted by its own overlay window, so it needs the
     * same treatment as the native controls: a state change has to reach it or
     * the pill and the EDIT end up in two different fills. */
    if (g_cur && g_cur->findBar)
        InvalidateRect(g_cur->findBar, NULL, FALSE);
}

static void invalidateChromeDirty(HWND hwnd)
{
    UiRect dirty;
    RECT rect;

    if (!g_cur || !uiChromeTakeDirty(CHROME, &dirty))
        return;

    rect.left = dirty.x;
    rect.top = dirty.y;
    rect.right = dirty.x + dirty.w;
    rect.bottom = dirty.y + dirty.h;

    InvalidateRect(hwnd, &rect, FALSE);

    if ((g_address && dirtyHits(dirty, CHROME->toolbar.address)) ||
        (g_findEdit && dirtyHits(dirty, CHROME->findbar.field)))
        syncFieldBackgrounds();
    if (g_cur->statusBar && dirtyHits(dirty, CHROME->statusbar.rect))
        InvalidateRect(g_cur->statusBar, NULL, FALSE);
}

/* The find bar's keyboard ring (field / prev / next / close). It is separate
 * from the chrome ring so Tab cannot walk from one into the other; the native
 * EDIT is the field stop and the three buttons are painted, so the shell takes
 * the keyboard when one of them has the ring. */
static int isFindRingPart(int part)
{
    return part == UI_PART_FIND_FIELD || part == UI_PART_FIND_PREV ||
           part == UI_PART_FIND_NEXT || part == UI_PART_FIND_CLOSE;
}

static void enterFindRingPart(int part)
{
    if (part == UI_PART_FIND_FIELD) {
        g_focusOwner = FOCUS_FIND;
        SetFocus(g_findEdit);
    } else {
        g_focusOwner = FOCUS_CHROME;
        SetFocus(g_hwnd);
    }

    agbLog(AGB_LOG_SHELL, "find ring -> part %d", part);

    invalidateChromeDirty(g_hwnd);
    invalidateFindOverlay();
}

/* Mouse tracking is per window: crossing from the chrome onto the native EDIT
 * (or back out of it) posts WM_MOUSELEAVE to the window being left even though
 * the pointer never left the pill. Clearing the hover there and re-setting it
 * from the other window's first WM_MOUSEMOVE repainted the pill twice (normal,
 * then hover), which showed up as a flash on the edge/inside boundary. So a
 * "leave" that is still inside the shell client is turned into a move at the
 * current cursor position - the hover then simply never changes. */
static int cursorInClient(HWND hwnd, int* x, int* y)
{
    POINT point;
    RECT client;

    if (!hwnd || !GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
        return 0;

    if (!GetClientRect(hwnd, &client))
        return 0;

    if (point.x < client.left || point.y < client.top ||
        point.x >= client.right || point.y >= client.bottom)
        return 0;

    if (x)
        *x = point.x;
    if (y)
        *y = point.y;

    return 1;
}

/* Same, but the whole window including the custom caption strip: the window
 * buttons live in the non-client area, so a capture-driven "leave" during a
 * press must be judged against the window rect, not the client rect. */
static int cursorInWindow(HWND hwnd, int* x, int* y)
{
    POINT point;
    RECT window;

    if (!hwnd || !GetCursorPos(&point) || !GetWindowRect(hwnd, &window))
        return 0;

    if (point.x < window.left || point.y < window.top ||
        point.x >= window.right || point.y >= window.bottom)
        return 0;

    if (ScreenToClient(hwnd, &point)) {
        if (x)
            *x = point.x;
        if (y)
            *y = point.y;
    }

    return 1;
}

/* The address and find fields are native EDITs: mouse moves over the text go to
 * the control, never to the shell, so the pill around it could not highlight.
 * Forward those moves into the chrome's own state machine in shell client
 * coordinates, and let the EDIT repaint when the fill changes. */
static LRESULT CALLBACK pillEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WNDPROC previous = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    ShellWindow* win = windowForHwnd(hwnd);

    (void)wParam;

    /* IME: while a composition is open the EDIT's text is the in-progress
     * candidate (pinyin letters, then the picked word), and EN_CHANGE fires for
     * every step. Searching then would run the query for "cheng", "chengdu" and
     * finally "程度" - three page scans and a scroll per keystroke. The final
     * commit always arrives before WM_IME_ENDCOMPOSITION, so the search is
     * deferred until then. */
    if (message == WM_IME_STARTCOMPOSITION) {
        g_findComposing = 1;
        agbLog(AGB_LOG_SHELL, "find ime: start (hwnd=%p)", (void*)hwnd);
    } else if (message == WM_IME_COMPOSITION) {
        char text[256];

        readControlText(hwnd, text, sizeof(text));
        agbLog(AGB_LOG_SHELL, "find ime: composition flags=0x%lx text='%s'",
               (unsigned long)lParam, text);
    } else if (message == WM_IME_ENDCOMPOSITION) {
        char text[256];

        g_findComposing = 0;
        readControlText(hwnd, text, sizeof(text));
        agbLog(AGB_LOG_SHELL, "find ime: end (hwnd=%p) text='%s'", (void*)hwnd, text);

        if (g_cur && hwnd == g_cur->findEdit)
            findPostUpdate();
    }

    if (win && (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE)) {
        UiMouseEvent event;
        UiRect dirty;
        POINT origin = { 0, 0 };
        ShellWindow* saved;
        int changed;

        event.x = (short)LOWORD(lParam);
        event.y = (short)HIWORD(lParam);
        ClientToScreen(hwnd, &origin);
        ScreenToClient(win->hwnd, &origin);
        event.x += origin.x;
        event.y += origin.y;

        if (message == WM_MOUSELEAVE) {
            int x = 0;
            int y = 0;

            /* Still inside the shell (the pointer went from the text back onto
             * the pill, or into another child): keep the hover instead of
             * clearing it and letting the shell's next move set it again. */
            if (cursorInClient(win->hwnd, &x, &y)) {
                event.type = UI_MOUSE_MOVE;
                event.x = x;
                event.y = y;
            } else {
                event.type = UI_MOUSE_LEAVE;
                event.x = 0;
                event.y = 0;
            }
        } else {
            event.type = UI_MOUSE_MOVE;
        }

        saved = g_cur;
        g_cur = win;
        uiChromeMouse(&win->chrome, &event);
        changed = uiChromeTakeDirty(&win->chrome, &dirty);
        g_cur = saved;

        if (changed) {
            RECT rect;

            rect.left = dirty.x;
            rect.top = dirty.y;
            rect.right = dirty.x + dirty.w;
            rect.bottom = dirty.y + dirty.h;

            InvalidateRect(win->hwnd, &rect, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);

            /* The hover belongs to the field, and the find bar's field is drawn
             * by its own overlay window - a sibling the shell's invalidation
             * never reaches. Without this the pill kept the idle fill while the
             * EDIT switched to the hover fill: two backgrounds inside one
             * field. (The address field has no separate window, which is why it
             * never showed this.) */
            if (win->findBar && dirtyHits(dirty, win->chrome.findbar.field))
                InvalidateRect(win->findBar, NULL, FALSE);
        }

        if (message == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT track;

            memset(&track, 0, sizeof(track));
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
        }
    }

    return CallWindowProcW(previous, hwnd, message, wParam, lParam);
}

static void installPillEditHandler(HWND edit)
{
    WNDPROC previous;

    if (!edit)
        return;

    previous = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)pillEditProc);

    if (previous)
        SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)previous);
}

/* ------------------------------------------------------------------------- */
/* window procedure                                                          */
/* ------------------------------------------------------------------------- */

/* window.close(): the engine thread asks for a tab to go away. The tab table
 * is main-thread state, so the actual close happens on the main thread. */
#define AGW_SHELL_CLOSE_TAB (WM_APP + 21)

static void onCloseWindow(AgaveWebView* view, void* user)
{
    Tab* tab = (Tab*)user;
    ShellWindow* win = tab ? (ShellWindow*)tab->window : windowForView(view);

    agbLog(AGB_LOG_SHELL, "window.close() -> close tab");

    if (!win)
        return;

    g_cur = win;

    if (g_hwnd)
        PostMessageA(g_hwnd, AGW_SHELL_CLOSE_TAB, (WPARAM)view, 0);
}

/* A named target (target="foo" / window.open(url,"foo")) navigated a tab the
 * shell already has: bring it forward. Same rule as close - the tab table is
 * main-thread state, so the switch happens there. */
#define AGW_SHELL_ACTIVATE_TAB (WM_APP + 23)

static void onActivateView(AgaveWebView* view, void* user)
{
    Tab* tab = (Tab*)user;
    ShellWindow* win = tab ? (ShellWindow*)tab->window : windowForView(view);

    agbLog(AGB_LOG_SHELL, "activate view (named window target)");

    if (g_probeMode) {
        printf("activate=1\n");
        fflush(stdout);
        return;
    }

    if (!win)
        return;

    g_cur = win;

    if (g_hwnd)
        PostMessageA(g_hwnd, AGW_SHELL_ACTIVATE_TAB, 0, (LPARAM)view);
}

/* Chrome action -> the same handlers the old WM_COMMAND switch used, so the
 * `[shell] back (button)` / `reload (button)` log sources stay meaningful. */
static void handleShellCommand(int id, const char* source)
{
    switch (id) {
    case ID_BACK:    backActive(source); break;
    case ID_FORWARD: forwardActive(source); break;
    case ID_RELOAD:  reloadActive(source); break;
    case ID_STOP:    stopActive(source); break;

    case ID_GO: {
        char text[MAX_PATH_CHARS];

        readControlText(g_address, text, sizeof(text));
        navigateActive(text, 0, source);
        break;
    }

    case ID_NEWTAB: navigateActive(NEW_TAB_URL, 1, source); break;
    case ID_MENU:   showMainMenu(); break;

    case ID_FIND_NEXT:  doFind(1); break;
    case ID_FIND_PREV:  doFind(-1); break;
    case ID_FIND_CLOSE: toggleFind(); break;

    case UI_CMD_MINIMIZE:
        ShowWindow(g_hwnd, SW_MINIMIZE);
        break;
    case UI_CMD_MAXIMIZE:
        ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case UI_CMD_CLOSE_WINDOW:
        PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
        break;
    default: break;
    }
}

static UiThemeKind shellThemeKind(void)
{
    if (g_themePref == SHELL_THEME_DARK)
        return UI_THEME_DARK;
    if (g_themePref == SHELL_THEME_LIGHT)
        return UI_THEME_LIGHT;

    return shellOps()->system_is_dark(NULL) ? UI_THEME_DARK : UI_THEME_LIGHT;
}

/* The new tab page is generated by the adapter and has no settings protocol of
 * its own; one message tells it the chrome's resolved theme. */
static void newTabSendTheme(AgaveWebView* view)
{
    char message[48];

    /* createTabIn() activates the tab before the first navigation, so this can
     * fire while the view has no document yet; the page asks again on boot. */
    if (!view || !agave_webview_has_document(view))
        return;

    _snprintf(message, sizeof(message), "__agb_newtab_theme__%s",
              shellThemeKind() == UI_THEME_DARK ? "dark" : "light");
    message[sizeof(message) - 1] = 0;

    agave_webview_post_message(view, message);
}

static void applyEditFonts(void)
{
    if (!g_cur)
        return;

    if (g_address)
        SendMessageW(g_address, WM_SETFONT, (WPARAM)CHROME->theme.hfont_address, TRUE);
    if (g_findEdit)
        SendMessageW(g_findEdit, WM_SETFONT, (WPARAM)CHROME->theme.hfont_ui, TRUE);

    /* Theme / DPI change: the palette behind WM_CTLCOLOREDIT just changed. */
    syncFieldBackgrounds();
}

/* Native window frame: keep WS_THICKFRAME | WS_CAPTION and let DWM draw the
 * shadow/rounded frame, while WM_NCCALCSIZE gives the client the whole window
 * rectangle. DwmExtendFrameIntoClientArea(1,1,1,1) keeps the shadow; the
 * Win11 corner preference rounds the top; DWMWA_BORDER_COLOR is set to "none"
 * so the light-mode 1px frame border cannot show as a white rim. No caption
 * dark/light color is forced here. */
static void applyWindowFrame(ShellWindow* win)
{
    int extended;

    if (!win || !win->hwnd)
        return;

    extended = shellOps()->prepare_frame(NULL, win->hwnd);

    agbLog(AGB_LOG_SHELL, "window frame extended=%d", extended);
}

/* Chromium's maximized custom frame also clips the window region to the
 * monitor work area (ResetWindowRegion). Maximized window rects overscan the
 * work area by the invisible frame thickness; the region keeps the DWM frame
 * and the 1px top border outside the visible surface. Restored windows have
 * no region (the full window rect is client). */
static void updateWindowRegion(HWND hwnd)
{
    RECT window;
    HMONITOR monitor;
    MONITORINFO info;
    RECT work;
    HRGN region;

    if (!hwnd)
        return;

    if (!IsZoomed(hwnd) || !GetWindowRect(hwnd, &window)) {
        SetWindowRgn(hwnd, NULL, TRUE);
        return;
    }

    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);

    if (!monitor || !GetMonitorInfoA(monitor, &info)) {
        SetWindowRgn(hwnd, NULL, TRUE);
        return;
    }

    work = info.rcWork;
    OffsetRect(&work, -window.left, -window.top);

    region = CreateRectRgnIndirect(&work);
    if (region)
        SetWindowRgn(hwnd, region, TRUE);   /* the window owns the region */
}

/* Resize hit-test thickness for this window's DPI (WM_NCHITTEST only; the
 * client itself covers the full window, including the DWM 1px frame). */
static int shellFrameBorder(HWND hwnd)
{
    unsigned dpi = shellOps()->dpi_for_window(NULL, hwnd);
    int frame = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);

    return MulDiv(frame, (int)dpi, 96);
}

static void updateWindowTitle(void)
{
    Tab* tab;
    wchar_t wide[300];

    if (!g_cur || !g_hwnd)
        return;

    tab = &g_tabs[g_activeTab];
    utf8ToWide(tab->used && tab->title[0] ? tab->title : "MiniShell - miniblink",
               wide, (int)(sizeof(wide) / sizeof(wide[0])));
    SetWindowTextW(g_hwnd, wide);
}

/* Follow the OS light/dark switch without restarting. The page's
 * prefers-color-scheme is deliberately not touched (see
 * applySystemColorScheme). */
static void rethemeShellWindow(ShellWindow* win)
{
    UiThemeKind kind;
    UiChromeCallbacks callbacks;
    unsigned dpi;
    HWND address;
    HWND find;
    int offscreen;

    if (!win || !win->hwnd)
        return;

    kind = shellThemeKind();

    if (kind == win->chrome.theme.kind)
        return;

    agbLog(AGB_LOG_SHELL, "retheme window kind=%s",
           kind == UI_THEME_DARK ? "dark" : "light");

    dpi = win->chrome.theme.dpi;
    callbacks = win->chrome.cb;
    address = win->address;
    find = win->findEdit;
    offscreen = win->chrome.offscreen;

    uiChromeFree(&win->chrome);
    uiChromeInit(&win->chrome, kind, dpi, offscreen, &callbacks);
    uiChromeSetNative(&win->chrome, win->hwnd, address, find);
    win->address = address;
    win->findEdit = find;

    g_cur = win;
    applyEditFonts();
    applyWindowFrame(win);
    layoutAll();

    /* The chrome object was rebuilt from scratch, so every pixel on the window is
     * stale - and layoutAll() alone does not invalidate anything. The menu path
     * happened to repaint because the closing menu invalidated the window, which
     * is why a theme change from the settings page (no menu to close) only
     * applied partially. The find overlay and the status bubble are re-rendered
     * by layoutAll() itself. */
    InvalidateRect(win->hwnd, NULL, FALSE);
}

static void onChromeCommand(void* user, int id)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;
    handleShellCommand(id, "button");
}

static void onChromeActivateTab(void* user, int index)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;
    switchTabWarm(index);
}

static void onChromeCloseTab(void* user, int index)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;
    closeTab(index);
}

static void onChromeNewTab(void* user)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;
    navigateActive(NEW_TAB_URL, 1, "button");
}

static void onChromeTabSearch(void* user)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;
    toggleTabSearchPopup();
}

static void onChromeFocusField(void* user, int field)
{
    ShellWindow* win = (ShellWindow*)user;

    if (!win)
        return;

    g_cur = win;

    if (field == 1 && g_address) {
        releasePageFocus();
        g_focusOwner = FOCUS_ADDRESS;
        SetFocus(g_address);
        SendMessageW(g_address, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
    } else if (field == 2 && g_findEdit) {
        releasePageFocus();
        g_focusOwner = FOCUS_FIND;
        SetFocus(g_findEdit);
    }
}

static LRESULT CALLBACK shellProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK shellProcInner(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case AGW_SHELL_CLOSE_TAB: {
        int i;

        for (i = 0; i < MAX_TABS; ++i) {
            if (g_tabs[i].used && g_tabs[i].view == (AgaveWebView*)wParam) {
                closeTab(i);
                break;
            }
        }
        return 0;
    }

    case AGW_SHELL_ACTIVATE_TAB: {
        int i;

        for (i = 0; i < MAX_TABS; ++i) {
            if (g_tabs[i].used && g_tabs[i].view == (AgaveWebView*)lParam) {
                if (i != g_activeTab)
                    setActiveTab(i);
                break;
            }
        }
        return 0;
    }

    case AGW_SHELL_STATUS:
        updateStatus();
        return 0;

    case AGW_FIND_UPDATE:
        g_findUpdatePending = 0;

        if (g_findVisible)
            findQueryChanged();
        return 0;

    case AGW_SHELL_HANG_PROMPT: {
        int index = (int)wParam;

        if (index >= 0 && index < MAX_TABS && g_tabs[index].used &&
            g_tabs[index].hangPending) {
            int closeIt = showHangPrompt(index, g_tabs[index].hangMs,
                                         g_tabs[index].hangStack);

            g_tabs[index].hangPending = 0;
            InvalidateRect(hwnd, NULL, FALSE);

            if (closeIt) {
                agbLog(AGB_LOG_SHELL, "unresponsive prompt: close tab index=%d", index);
                closeTab(index);
            } else {
                agbLog(AGB_LOG_SHELL, "unresponsive prompt: wait tab index=%d", index);
            }
        }
        return 0;
    }

    /* Chrome-style frame: the client area covers the whole window, so the tab
     * strip is the title bar. WS_THICKFRAME | WS_CAPTION stay (shadow, snap,
     * resize semantics); DwmExtendFrameIntoClientArea(1,1,1,1) keeps a 1px
     * frame margin for the shadow while WM_NCCALCSIZE paints over it. */
    case WM_NCCALCSIZE:
        if (wParam) {
            NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lParam;

            if (IsZoomed(hwnd)) {
                HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO info;

                memset(&info, 0, sizeof(info));
                info.cbSize = sizeof(info);

                if (monitor && GetMonitorInfoA(monitor, &info))
                    params->rgrc[0] = info.rcWork;
            }
            /* Normal window: rgrc[0] already is the full window rectangle. */

            return 0;
        } else {
            /* Win32 sends the RECT form (wParam == FALSE) during creation and
             * some resizes; leaving it to DefWindowProc would put the native
             * caption back (the shell would show two title rows). */
            RECT* client = (RECT*)lParam;
            RECT window;

            GetWindowRect(hwnd, &window);
            *client = window;
            return 0;
        }

    case WM_NCACTIVATE:
        /* No native caption to repaint; the chrome is the caption. */
        return TRUE;

    case WM_SETFOCUS:
        /* The frame itself got the focus: activation, Alt+Tab, or a caption
         * button press that called SetFocus(hwnd). Give it back to whoever owned
         * the keyboard before, instead of always stealing it for the page. A
         * child that asks for the focus itself (the address/find EDIT) gets
         * WM_SETFOCUS of its own, never this one. */
        if (g_focusOwner == FOCUS_ADDRESS && g_address) {
            SetFocus(g_address);
        } else if (g_focusOwner == FOCUS_FIND && g_findEdit && g_findVisible) {
            SetFocus(g_findEdit);
        } else if (g_focusOwner == FOCUS_PAGE) {
            focusPage();
        }
        return 0;

    case WM_NCHITTEST: {
        POINT screen = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        POINT client = screen;
        RECT window;
        int border;
        int x, y, w, h;

        GetWindowRect(hwnd, &window);
        border = shellFrameBorder(hwnd);
        x = screen.x - window.left;
        y = screen.y - window.top;
        w = window.right - window.left;
        h = window.bottom - window.top;

        if (!IsZoomed(hwnd)) {
            int left = x < border;
            int right = x >= w - border;
            int top = y < border;
            int bottom = y >= h - border;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }

        ScreenToClient(hwnd, &client);

        if (client.y >= 0 && client.y < CHROME->theme.tabstrip_h) {
            /* Same shape as Chromium's BrowserCaptionButtonContainer: the
             * buttons report system non-client codes (that is what gives
             * HTMAXBUTTON its Win11 Snap Layouts), but WM_NCLBUTTONDOWN is
             * intercepted below so DefWindowProc never paints the classic
             * pressed button over our glyph. Tabs stay HTCLIENT. */
            if (uiRectContains(CHROME->win_max.rect, client.x, client.y))
                return HTMAXBUTTON;
            if (uiRectContains(CHROME->win_min.rect, client.x, client.y))
                return HTMINBUTTON;
            if (uiRectContains(CHROME->win_close.rect, client.x, client.y))
                return HTCLOSE;

            /* Empty tab-strip space drags the window (and double-click
             * maximizes, right-click opens the system menu) like a caption. */
            if (uiChromeHitTest(CHROME, client.x, client.y) == UI_PART_NONE)
                return HTCAPTION;
        }

        return HTCLIENT;
    }

    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    /* Which window a handoff targets: Chrome appends tabs to the most recently
     * active window. Hidden test windows never activate, so creation and
     * handoff also set g_lastActive. */
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* The tab-search popup is a topmost layered window and never takes
             * focus, so losing activation must close it. But the engine's own
             * focus handling also sends a synthetic WA_INACTIVE while this
             * window is still the active one (seen ~3 s into a page load);
             * closing on that killed the popup for no user action. Only a real
             * activation change counts. */
            if (uiTabSearchIsOpen()) {
                HWND active = GetActiveWindow();

                if (active == hwnd) {
                    agbLog(AGB_LOG_SHELL, "tab search: ignore synthetic WA_INACTIVE (still active, pid=%lu)",
                           (unsigned long)GetCurrentProcessId());
                } else {
                    agbLog(AGB_LOG_SHELL, "tab search closed: shell deactivated (active=%p)",
                           (void*)active);
                    uiTabSearchClose();
                }
            }
        } else if (g_cur) {
            g_lastActive = g_cur;
        }
        break;

    case WM_ACTIVATEAPP:
        if (!wParam && uiTabSearchIsOpen()) {
            agbLog(AGB_LOG_SHELL, "tab search closed: application deactivated");
            uiTabSearchClose();
        }
        break;

    case WM_MOVE:
        /* Owned popups are positioned in screen coordinates, so moving the
         * shell has to re-place the inspect mark and the status bubble. */
        if (g_inspectReady && g_inspectOn)
            uiInspectRefresh(&g_inspect);
        if (g_cur && g_cur->statusBar)
            syncStatusOverlay();
        break;

    case WM_SIZE:
        /* A topmost popup would outlive the minimized shell. */
        if (wParam == SIZE_MINIMIZED && uiTabSearchIsOpen())
            uiTabSearchClose();

        /* The frozen tab widths belong to the old strip size. */
        if (g_cur)
            uiChromeEndCloseSlide(CHROME);

        /* Minimized: every view is hidden (Page Visibility + no layout/paint);
         * restored: only the active tab is. */
        g_minimized = (wParam == SIZE_MINIMIZED) ? 1 : 0;
        syncTabVisibility();

        /* Nothing is visible while minimized, and Windows reports a ~199x1
         * client: laying out would resize every view to 1x1 (DIB rebuilt, page
         * reflowed at 1x1) only to undo it on restore - a full surface rebuild
         * and reflow per minimize/restore. Restore always reports a different
         * size, so it lays out then. */
        if (g_minimized)
            return 0;

        layoutAll();
        updateWindowRegion(hwnd);
        updateToolbarState();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        /* A cold tab switch is waiting for its warm-up frame (see
         * switchTabWarm): finish as soon as it lands, or give up after the
         * timeout so a hung page cannot swallow the click. */
        if (wParam == WARM_TAB_TIMER) {
            int index = g_warmTab;

            if (index < 0 || index >= MAX_TABS || !g_tabs[index].used) {
                cancelWarmSwitch();
                return 0;
            }

            if ((g_tabs[index].view &&
                 agave_webview_has_frame(g_tabs[index].view)) ||
                GetTickCount() - g_warmSince > WARM_TAB_TIMEOUT_MS)
                finishWarmSwitch();

            return 0;
        }

        /* A navigation the engine never picked up: rebuild the control. */
        if (wParam == NAV_GUARD_TIMER) {
            navGuardTick();
            return 0;
        }

        if (wParam == TEST_CLOSE_TAB_TIMER) {
            const char* chip = getenv("AGB_TEST_CLOSE_CHIP");

            KillTimer(hwnd, TEST_CLOSE_TAB_TIMER);

            if (chip && chip[0]) {
                /* Synthetic "click on the active tab's X", repeatable: the slide
                 * must survive several closes in a row. The real pointer is
                 * never moved. */
                int left = g_testCloseLeft > 0 ? g_testCloseLeft : atoi(chip);

                if (left < 1)
                    left = 1;

                if (CHROME->tabstrip.close[g_activeTab].w > 0) {
                    UiRect chipRect = CHROME->tabstrip.close[g_activeTab];

                    g_testCursor.x = chipRect.x + chipRect.w / 2;
                    g_testCursor.y = chipRect.y + chipRect.h / 2;
                    g_testCursorSet = 1;
                    agbLog(AGB_LOG_SHELL, "test: fake cursor on close chip %d,%d",
                           g_testCursor.x, g_testCursor.y);
                }

                agbLog(AGB_LOG_SHELL, "test: closing tab %d from its X", g_activeTab);
                closeTab(g_activeTab);

                g_testCloseLeft = --left;

                if (left > 0 && countUsedTabs() > 1)
                    SetTimer(hwnd, TEST_CLOSE_TAB_TIMER, 400, NULL);
                else
                    SetTimer(hwnd, TEST_SLIDE_END_TIMER, 400, NULL);
                return 0;
            }

            agbLog(AGB_LOG_SHELL, "test: closing the active tab (fetch may be in flight)");
            closeTab(g_activeTab);

            if (g_testCursorSet)
                SetTimer(hwnd, TEST_SLIDE_END_TIMER, 400, NULL);
            return 0;
        }

        if (wParam == TEST_SLIDE_END_TIMER) {
            UiRect host = CHROME->tabstrip.rect;

            KillTimer(hwnd, TEST_SLIDE_END_TIMER);
            g_testCursor.x = host.x + 20;
            g_testCursor.y = host.y + host.h + 30;
            agbLog(AGB_LOG_SHELL, "test: fake cursor leaves the tab strip %d,%d",
                   g_testCursor.x, g_testCursor.y);
            PostMessageA(hwnd, WM_MOUSEMOVE, 0,
                         MAKELPARAM(g_testCursor.x, g_testCursor.y));
            return 0;
        }
        break;

    case WM_PAINT:
        paintShell(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = (MINMAXINFO*)lParam;

        if (info) {
            info->ptMinTrackSize.x = uiThemeScale(&CHROME->theme, 680);
            info->ptMinTrackSize.y = uiThemeScale(&CHROME->theme, 460);
        }

        /* Leave ptMaxSize/ptMaxPosition alone (DefWindowProc's default): a
         * normal maximized window overscans the work area by the invisible
         * frame thickness, which keeps the DWM 1px frame off-screen. Pinning
         * the maximized window to the exact work area exposed that frame as a
         * white line along the top edge. WM_NCCALCSIZE still sets the client
         * to the monitor work area, so the taskbar is not covered. */
        break;
    }

    case WM_CTLCOLOREDIT: {
        HDC control = (HDC)wParam;
        HWND edit = (HWND)lParam;

        if (edit == g_address || edit == g_findEdit) {
            /* The EDIT paints its own background, so it has to be exactly the
             * fill the chrome painted under and around it. Ask the chrome
             * instead of guessing from the part: hovering the Go button (part
             * of the pill, but not the pill's own part) highlights the pill just
             * the same, and the text used to sit in a differently-coloured
             * rectangle whenever it did. */
            int state = uiChromeFieldState(CHROME, edit == g_address ? 1 : 2);

            SetTextColor(control, CHROME->theme.text_color);
            SetBkColor(control, state == 2 ? CHROME->theme.field_focus_color
                              : state == 1 ? CHROME->theme.field_hover_color
                                           : CHROME->theme.field_color);
            return (LRESULT)(state == 2 ? CHROME->theme.field_focus_brush
                            : state == 1 ? CHROME->theme.field_hover_brush
                                         : CHROME->theme.field_brush);
        }
        break;
    }

    case WM_MOUSEMOVE: {
        UiMouseEvent event;
        TRACKMOUSEEVENT track;

        shellCheckCloseSlide(hwnd);

        event.type = UI_MOUSE_MOVE;
        event.x = (short)LOWORD(lParam);
        event.y = (short)HIWORD(lParam);
        uiChromeMouse(CHROME, &event);
        invalidateChromeDirty(hwnd);

        memset(&track, 0, sizeof(track));
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);

        return 0;
    }

    case WM_MOUSELEAVE: {
        UiMouseEvent event;

        shellCheckCloseSlide(hwnd);

        /* The pointer may only have moved onto a child (the native EDIT keeps
         * its pill hovered). If the cursor is still inside the shell, report a
         * move at that point instead of clearing the hover: clearing it here
         * and letting the EDIT's forwarded move set it again repainted the pill
         * twice (normal, then hover) - the edge/inside flash. */
        if (cursorInClient(hwnd, &event.x, &event.y)) {
            event.type = UI_MOUSE_MOVE;
        } else {
            event.type = UI_MOUSE_LEAVE;
            event.x = -1;
            event.y = -1;
        }

        uiChromeMouse(CHROME, &event);
        invalidateChromeDirty(hwnd);
        /* Leaving the window clears the hover; the native field keeps its own
         * background, so it needs the repaint even when the dirty region does
         * not reach the pill. */
        syncFieldBackgrounds();
        return 0;
    }

    case WM_NCMOUSEMOVE: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        UiMouseEvent event;
        TRACKMOUSEEVENT track;

        ScreenToClient(hwnd, &point);
        event.type = UI_MOUSE_MOVE;
        event.x = point.x;
        event.y = point.y;
        uiChromeMouse(CHROME, &event);
        invalidateChromeDirty(hwnd);

        memset(&track, 0, sizeof(track));
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE | TME_NONCLIENT;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);

        return 0;
    }

    case WM_NCMOUSELEAVE: {
        UiMouseEvent event;

        /* A caption-button press is turned into a client press with capture
         * (see WM_NCLBUTTONDOWN).  While captured, Windows reports the pointer
         * leaving the *non-client* area as soon as it moves a single pixel inside
         * the button - and a LEAVE here resets every pressed window button, so
         * "press, nudge the mouse, release" never fired.  While the capture is
         * ours and the cursor is still over the window, report a move at the
         * current point; only a real exit is a leave. */
        if (GetCapture() == hwnd && cursorInWindow(hwnd, &event.x, &event.y)) {
            event.type = UI_MOUSE_MOVE;
        } else {
            event.type = UI_MOUSE_LEAVE;
            event.x = -1;
            event.y = -1;
        }

        uiChromeMouse(CHROME, &event);
        invalidateChromeDirty(hwnd);
        return 0;
    }

    case WM_NCLBUTTONDOWN: {
        /* Chromium's HWNDMessageHandler::OnNCLButtonDown does exactly this for
         * FrameMode::CUSTOM_DRAWN: turn the non-client press into a client
         * press so our button owns capture and the subsequent WM_LBUTTONUP.
         * Letting DefWindowProc handle it paints the classic pressed caption
         * button over our glyph (the offset white square in the bug report). */
        if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE) {
            POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            UiMouseEvent event;

            /* Pressing a caption button is a click outside the page. */
            releasePageFocus();
            g_focusOwner = FOCUS_CHROME;
            uiChromeFocusClear(CHROME);

            ScreenToClient(hwnd, &point);
            event.type = UI_MOUSE_DOWN;
            event.x = point.x;
            event.y = point.y;
            uiChromeMouse(CHROME, &event);

            SetCapture(hwnd);
            SetFocus(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_NCLBUTTONDBLCLK:
        /* Double-click on our buttons must not fall through to DefWindowProc
         * (it would paint the classic pressed caption button again). */
        if (wParam == HTMINBUTTON || wParam == HTMAXBUTTON || wParam == HTCLOSE)
            return 0;
        break;

    case WM_LBUTTONDOWN: {
        UiMouseEvent event;

        /* A click takes the keyboard back from the tab-stop ring and from the
         * page: only one of them may believe it owns the keyboard. */
        uiChromeFocusClear(CHROME);
        releasePageFocus();
        g_focusOwner = FOCUS_CHROME;

        event.type = UI_MOUSE_DOWN;
        event.x = (short)LOWORD(lParam);
        event.y = (short)HIWORD(lParam);

        if (uiChromeMouse(CHROME, &event)) {
            invalidateChromeDirty(hwnd);
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP: {
        UiMouseEvent event;

        event.type = UI_MOUSE_UP;
        event.x = (short)LOWORD(lParam);
        event.y = (short)HIWORD(lParam);

        if (GetCapture() == hwnd)
            ReleaseCapture();

        uiChromeMouse(CHROME, &event);
        invalidateChromeDirty(hwnd);
        return 0;
    }

    /* Middle click closes the tab under the pointer, like Chrome. The engine
     * only ever sees clicks that land on the page, so this lives in the shell's
     * own tab strip (uiChromeTabAt also answers for the close chip). */
    case WM_MBUTTONUP: {
        int index = uiChromeTabAt(CHROME, (short)LOWORD(lParam), (short)HIWORD(lParam));

        if (index >= 0) {
            agbLog(AGB_LOG_SHELL, "middle click closes tab index=%d", index);
            closeTab(index);
            return 0;
        }
        break;
    }

    /* Right click on a tab opens the tab strip's own menu. The page's menu never
     * applies to the chrome, and a right click anywhere else in the chrome keeps
     * whatever the default handler does. The menu acts on the tab under the
     * pointer, which is not necessarily the active one (Chrome behaves the same
     * way: the row you right clicked is the row the commands hit). */
    case WM_RBUTTONUP: {
        int index = uiChromeTabAt(CHROME, (short)LOWORD(lParam), (short)HIWORD(lParam));

        if (index >= 0) {
            POINT point;

            point.x = (short)LOWORD(lParam);
            point.y = (short)HIWORD(lParam);
            ClientToScreen(hwnd, &point);

            agbLog(AGB_LOG_SHELL, "right click on tab index=%d", index);
            showTabMenu(index, point.x, point.y);
            return 0;
        }
        break;
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_cur) {            POINT point;
            UiPart part;

            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            part = uiChromeHitTest(CHROME, point.x, point.y);

            /* Chrome's chrome is not a web page: toolbar buttons and tabs keep
             * the arrow. Only the two text fields ask for a caret - a hand
             * cursor over an input read as a link. */
            if (part == UI_PART_ADDRESS || part == UI_PART_FIND_FIELD) {
                SetCursor(LoadCursorW(NULL, IDC_IBEAM));
                return TRUE;
            }

            if (part != UI_PART_NONE) {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
                return TRUE;
            }
        }
        break;

    case WM_DPICHANGED: {
        unsigned dpi = HIWORD(wParam) ? (unsigned)HIWORD(wParam) : (unsigned)LOWORD(wParam);
        RECT* suggested = (RECT*)lParam;

        uiChromeEndCloseSlide(CHROME);
        uiChromeSetDpi(CHROME, dpi);
        applyEditFonts();
        /* The page half: chrome metrics just scaled, and every tab of this
         * window renders its page at the new device scale (text is laid out and
         * rasterized larger, not stretched). */
        syncTabsDeviceScale();

        /* The settings page shows the system scaling read-only: refresh it when
         * the window lands on a monitor with a different DPI. */
        if (g_tabs[g_activeTab].used && g_tabs[g_activeTab].view &&
            !strncmp(g_tabs[g_activeTab].url, "minishell://settings", 20))
            settingsSendAll(g_tabs[g_activeTab].view);

        if (suggested) {
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        applyWindowFrame(g_cur);
        layoutAll();
        updateWindowRegion(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (g_cur) {
            rethemeShellWindow(g_cur);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int note = HIWORD(wParam);

        if (note == EN_SETFOCUS || note == EN_KILLFOCUS) {            HWND control = (HWND)lParam;

            if (control == g_address)
                uiChromeSetFocusField(CHROME, note == EN_SETFOCUS ? 1 : 0);
            else if (control == g_findEdit)
                uiChromeSetFocusField(CHROME, note == EN_SETFOCUS ? 2 : 0);

            InvalidateRect(hwnd, NULL, FALSE);
            /* Focus changes the pill fill; the native field does not repaint
             * with the parent, so it has to be invalidated explicitly. */
            syncFieldBackgrounds();
            /* The find bar paints its own focus ring in the overlay window. */
            invalidateFindOverlay();
            return 0;
        }

        /* Search as you type: the query field is the only EDIT with a live
         * search behind it. An open IME composition is skipped - the final
         * commit is handled when the composition ends. */
        if (id == ID_FIND_EDIT && note == EN_CHANGE) {
            char text[256];

            readControlText(g_findEdit, text, sizeof(text));
            agbLog(AGB_LOG_SHELL, "find edit: change text='%s' composing=%d",
                   text, g_findComposing);

            if (!g_findComposing)
                findPostUpdate();
            return 0;
        }

        handleShellCommand(id, "button");
        return 0;
    }

    case WM_DESTROY: {
        /* Several windows share the process now: only the last one ends it.
         * Session state is flat (all windows' tabs), so a restart restores one
         * window with every tab; noted in the design doc. */
        int last = (g_shellWindows <= 1);

        /* The popup is a top-level window of its own; a shell going away must
         * not leave it (and its capture) behind. */
        uiTabSearchClose();

        if (last)
            saveSession();

        uiChromeFree(CHROME);
        if (g_inspectReady) {
            uiInspectDestroy(&g_inspect);
            g_inspectReady = 0;
            g_inspectOn = 0;
        }
        paintCacheFree();
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        g_cur->used = 0;

        if (g_lastActive == g_cur)
            g_lastActive = NULL;

        if (g_shellWindows > 0)
            --g_shellWindows;

        if (last)
            PostQuitMessage(0);
        else
            saveSession();

        return 0;
    }

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* Every shell message selects the window it belongs to and restores the
 * previous context afterwards: handlers (and modal loops inside them) may
 * dispatch messages for another window. */
static LRESULT CALLBACK shellProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ShellWindow* previous = g_cur;
    ShellWindow* win;

    if (message == WM_NCCREATE) {
        CREATESTRUCTA* create = (CREATESTRUCTA*)lParam;

        win = create ? (ShellWindow*)create->lpCreateParams : NULL;

        if (win)
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)win);
    } else {
        win = (ShellWindow*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    }

    if (win) {
        g_cur = win;
        win->hwnd = hwnd;
    }

    {
        LRESULT result = shellProcInner(hwnd, message, wParam, lParam);

        g_cur = previous;
        return result;
    }
}

/* ------------------------------------------------------------------------- */
/* entry point                                                               */
/* ------------------------------------------------------------------------- */

/* Exit forensics: if some library calls exit() from inside a callback, the
 * message loop never returns and we never see why. This handler runs on exit()
 * and prints who did it. */
/* Set when --probe was given: headless runs must not pop dialogs (their stderr
 * already carries the same information, and a modal box would hang CI). */
static int g_probeMode;

/* Console-only crash reporting: probe runs, offscreen chrome renders, the crash
 * page host and tests launched with AGB_SHELL_HIDDEN have nobody to dismiss a
 * dialog. The report still goes to stderr and to agb_crash.txt. */
static int g_crashConsoleOnly;

/* The control delivers state callbacks (progress/title/url/status) on the thread
 * that owns its window; the probe checks that once so a regression fails a suite
 * instead of showing up as an intermittent cross-thread AV (R185). */
static DWORD g_probeUiThread;
static int   g_probeCallbackThreadChecked;
static int   g_probeCallbackThreadBad;
/* --interrupt-after <ms>: abort the page script once, mid-load (hang recovery). */
static int g_probeInterruptAfterMs;
/* --nav-after <ms>: navigate while the page is still busy (choke-point test). */
static int g_probeNavAfterMs;
static char g_probeNavUrl[MAX_PATH_CHARS];
/* --hide-after/--show-after <ms>: Page Visibility changes without a GUI tab
 * switch (the control's set_visible path). */
static int g_probeHideAfterMs;
static int g_probeShowAfterMs;
/* --reload-every <ms> --reload-count <n>: hammer Reload regardless of load state. */
static int g_probeReloadEveryMs;
static int g_probeReloadCount;
/* --eval-early <js>: run a script immediately after navigate() returns, while
 * the document swap is still in flight. That is the window where a host command
 * used to vanish without a trace; the run must either execute it or report
 * on_command_dropped, and the suite asserts exactly that. */
static const char* g_probeEarlyEval;
/* --find <text>: run the find-in-page path once before --eval, so a probe can
 * assert on the highlight DOM it builds. */
static const char* g_probeFindText;
/* --eval-before-nav <js>: eval before the first navigate(), i.e. a view with no
 * page at all. The control has nowhere to run it: the run must report
 * on_command_dropped ("no page yet"), deterministically. */
static const char* g_probeBeforeNavEval;
/* GUI runs only: open the crash report page in a fresh process.
 *
 * The report text is the artifact; the page is what makes it usable (scrollable,
 * selectable, one Copy button) without depending on the crashed process still
 * being able to draw. The file name is relative to the process CWD, so the
 * fallback dialog shows the absolute path - the shell can be started anywhere. */
static void shellAnnounceCrashFile(const char* file)
{
    const char* opt = getenv("AGB_CRASH_POPUP");
    static volatile LONG announced;
    int consoleOnly;
    char full[MAX_PATH];
    char msg[768];
    DWORD n;

    if (InterlockedExchange(&announced, 1) != 0) return;

    consoleOnly = g_crashConsoleOnly || g_probeMode ||
                  (opt && opt[0] && strcmp(opt, "0") == 0);

    n = GetFullPathNameA(file, (DWORD)sizeof(full), full, NULL);
    if (n == 0 || n >= (DWORD)sizeof(full))
        lstrcpynA(full, file, (int)sizeof(full));

    /* Headless/automation: the stack is already on stderr; say where the report
     * is and never create a window the caller cannot dismiss. */
    if (consoleOnly) {
        fprintf(stderr, "[crash] report: %s\n", full);
        fflush(stderr);
        return;
    }

    if (crashPageLaunch(file))
        return;

    snprintf(msg, sizeof(msg),
             "MiniShell crashed and wrote the crash report here:\n\n%s\n\n"
             "The crash page could not be started. Attach that file when you "
             "report the problem.\n"
             "Set AGB_CRASH_POPUP=0 to disable this dialog.",
             full);

    MessageBoxA(NULL, msg, "MiniShell crash", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}
static void shellPrintFrames(int skip, int depth)
{
    void* frames[64];
    unsigned short count = CaptureStackBackTrace((DWORD)skip, (DWORD)depth, frames, NULL);
    int i;

    for (i = 0; i < (int)count; ++i) {
        unsigned long long a = (unsigned long long)frames[i];
        const char* mod = "?";
        unsigned long long off = a;
        unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
        unsigned long long av  = (unsigned long long)GetModuleHandleA("libagave.dll");
        unsigned long long ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");

        if (av && a >= av && a < av + 0x2000000ULL) { mod = "libagave"; off = a - av; }
        else if (ab && a >= ab && a < ab + 0x2000000ULL) { mod = "antjsb"; off = a - ab; }
        else if (exe && a >= exe && a < exe + 0x100000ULL) { mod = "exe"; off = a - exe; }

        fprintf(stderr, "  #%02d %s+0x%llx\n", i, mod, off);
    }
}

/* The crash report file this process writes.
 *
 * User runs and headless/test runs must never share a file: a suite crash would
 * otherwise overwrite the report a user wants to attach to a bug. The timestamp
 * then keeps two crashes in the same folder apart (a crash loop, two windows).
 *
 * Fixed buffers and a manual clock read: the handlers run with the process
 * possibly half dead, and the vectored handler and the unhandled filter must
 * name the same file, so it is built once and cached. */
static const char* shellCrashReportName(void)
{
    static char name[64];

    if (!name[0]) {
        SYSTEMTIME now;
        const char* base = g_crashConsoleOnly ? "agb_crash_test" : "agb_crash";
        char* p = name;
        const char* s;
        unsigned stamp[6];
        int widths[6] = { 4, 2, 2, 2, 2, 2 };
        int i, d;

        GetLocalTime(&now);
        stamp[0] = now.wYear;
        stamp[1] = now.wMonth;
        stamp[2] = now.wDay;
        stamp[3] = now.wHour;
        stamp[4] = now.wMinute;
        stamp[5] = now.wSecond;

        for (s = base; *s; ++s)
            *p++ = *s;
        *p++ = '_';

        /* yyyymmdd_hhmmss, zero padded, most significant digit first. */
        for (i = 0; i < 6; ++i) {
            unsigned div = 1;

            if (i == 3)
                *p++ = '_';

            for (d = 1; d < widths[i]; ++d)
                div *= 10;

            for (d = 0; d < widths[i]; ++d) {
                *p++ = (char)('0' + (stamp[i] / div) % 10);
                div /= 10;
            }
        }

        *p++ = '.';
        *p++ = 't';
        *p++ = 'x';
        *p++ = 't';
        *p = 0;
    }

    return name;
}

/* One report per process: the vectored handler usually gets here first and the
 * unhandled filter may follow for the same exception, so the second writer must
 * not overwrite the file. The dialog, on the other hand, belongs to the
 * *unhandled* filter only: first-chance access violations are common inside
 * GDI+ (it uses SEH for text/font fallbacks), and popping a crash page for one
 * of those interrupted the user for an exception the process survived. */
static void shellWriteCrashReport(EXCEPTION_RECORD* rec)
{
    static volatile LONG reported;

    if (InterlockedExchange(&reported, 1) != 0)
        return;

    /* Raw Win32 I/O and fixed buffers: the CRT can be what broke. */
    crashReportWrite(shellCrashReportName(), rec);
}

/* First-chance dump: the unhandled-exception filter can be bypassed - ant
 * installs its own handler and only chains to the previous filter once per
 * process, and its own StackWalk can fault - so an AV on a worker thread can
 * kill the process without any output. A vectored handler runs first, on every
 * thread, before SEH/filters, and writes the faulting stack to a file with raw
 * Win32 I/O (the CRT may be mid-crash). It never handles the exception. */
static LONG WINAPI shellVehHandler(EXCEPTION_POINTERS* ep)
{
    static volatile LONG dumped;
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;
    DWORD code = rec ? rec->ExceptionCode : 0;

    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedExchange(&dumped, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "[crash-firstchance] thread=%lu code=0x%08lx addr=0x%llx",
            (unsigned long)GetCurrentThreadId(), (unsigned long)code,
            (unsigned long long)(rec ? (unsigned long long)rec->ExceptionAddress : 0));

    if (rec && rec->NumberParameters >= 2)
        fprintf(stderr, " info0=%llu info1=0x%llx",
                (unsigned long long)rec->ExceptionInformation[0],
                (unsigned long long)rec->ExceptionInformation[1]);

    fprintf(stderr, "\n");
    shellPrintFrames(1, 48);
    fflush(stderr);

    shellWriteCrashReport(rec);

    /* Ask ant for the JS stack: without it the native frames only say
     * "JIT code". Resolved dynamically - the shell does not link antjsb. */
    {
        HMODULE ant = GetModuleHandleA("antjsb.dll");

        if (ant) {
            typedef void (*DumpJsStackFn)(void);
            DumpJsStackFn dump = (DumpJsStackFn)GetProcAddress(ant, "antb_print_last_js_stack");

            if (dump)
                dump();
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* Crash forensics: an AV that only happens outside the debugger (timing
 * sensitive UAFs) cannot be caught with cdb - attaching changes the schedule
 * and the bug disappears. The filter still prints the faulting address and the
 * stack to stderr, then writes the same privacy-free report the page shows. */
static LONG WINAPI shellCrashFilter(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;

    fprintf(stderr, "[crash] thread=%lu code=0x%08lx addr=0x%llx",
            (unsigned long)GetCurrentThreadId(),
            (unsigned long)(rec ? rec->ExceptionCode : 0),
            (unsigned long long)(rec ? (unsigned long long)rec->ExceptionAddress : 0));

    if (rec && (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
        fprintf(stderr, " %s 0x%llx",
                rec->ExceptionInformation[0] == 0 ? "read" :
                rec->ExceptionInformation[0] == 1 ? "write" : "exec",
                (unsigned long long)rec->ExceptionInformation[1]);
    }

    fprintf(stderr, "\n");
    fflush(stderr);

    shellPrintFrames(1, 48);
    fflush(stderr);
    shellWriteCrashReport(rec);
    shellAnnounceCrashFile(shellCrashReportName());

    /* Keep the OS behaviour (exit code 0xC0000005 etc.): let the default
     * handler / WER terminate the process. */
    return EXCEPTION_CONTINUE_SEARCH;
}

static int g_exitTraceSuppressed;   /* the hand-off client is a utility exit */

static void shellExitTrace(void)
{
    unsigned long long exe;
    unsigned long long av;
    unsigned long long ab;

    if (g_exitTraceSuppressed)
        return;

    exe = (unsigned long long)GetModuleHandleA(NULL);
    av  = (unsigned long long)GetModuleHandleA("libagave.dll");
    ab  = (unsigned long long)GetModuleHandleA("antjsb.dll");

    agbLog(AGB_LOG_SHELL, "atexit fired, bases exe=%llx agave=%llx ant=%llx", exe, av, ab);
    shellPrintFrames(1, 32);
    fflush(stderr);
}

/* ------------------------------------------------------------------------- */
/* probe mode: headless load + JS evaluation, for page debugging             */
/* ------------------------------------------------------------------------- */

#define PROBE_TIMER_EVAL    1
#define PROBE_TIMER_SETTLE  2
#define PROBE_TIMER_TIMEOUT 3
#define PROBE_TIMER_POLL    4   /* repeating: poll new document after reload */
#define PROBE_TIMER_INTERRUPT 5 /* one-shot: --interrupt-after (hang recovery) */
#define PROBE_TIMER_NAV     6   /* one-shot: --nav-after (navigation aborts a stuck page) */
#define PROBE_TIMER_RELOAD  7   /* repeating: --reload-every (hammer Reload) */
#define PROBE_TIMER_HIDE    8   /* one-shot: --hide-after (Page Visibility) */
#define PROBE_TIMER_SHOW    9   /* one-shot: --show-after (Page Visibility) */

/* Engine-thread callbacks post this to the probe window (main thread) to arm
 * the eval timer: window timers must be installed by the owning thread. */
#define AGW_PROBE_MSG (WM_APP + 77)
/* Engine thread -> main thread: the --eval script has finished. */
#define AGW_PROBE_EVALDONE (WM_APP + 78)

/* Probe exit codes (process exit status): scripts and CI can tell failure
 * classes apart without parsing the log.
 *   0  eval ran and the page called probeExit()
 *   1  usage error (missing URL etc.)
 *   2  timeout: the load never finished
 *   3  timeout: load finished but no [exit] arrived
 *   4  load finished and eval ran, but no [exit] arrived (script error?)
 *   5  the navigation itself failed (on_error), no [exit]            */
#define PROBE_EXIT_OK       0
#define PROBE_EXIT_USAGE    1
#define PROBE_EXIT_TIMEOUT_LOAD  2
#define PROBE_EXIT_TIMEOUT_NOEXIT 3
#define PROBE_EXIT_NOEXIT   4
#define PROBE_EXIT_LOADERROR 5
#define PROBE_EXIT_SANITY   6   /* eval ran against an empty document */

static HWND          g_probeWnd;
static AgaveWebView* g_probeView;
static AgaveWebView* g_probeNewView;   /* window opened by the page (target=_blank) */
static char          g_probeScript[8192] =
    "agavePostMessage('[dump] ' + document.documentElement.outerHTML)";
/* --eval-file keeps the file's bytes on the heap: scripts (asset bundles,
 * generated probes) are routinely larger than the inline --eval buffer, and a
 * silent cut at 8 KiB used to break them before the backend ever saw them. */
static char*         g_probeScriptFile;
static int g_probeWaitMs    = 3000;   /* after load finished, before eval   */
static int g_probeWaitGiven;          /* --wait was explicit (see --wait-dcl) */
static int g_probeWaitDcl;            /* arm --eval at DOMContentLoaded      */
static int g_probeSettleMs  = 1000;   /* after eval, before exit            */
static int g_probeTimeoutMs = 60000;  /* hard cap from start                */
static int g_probeFinished;
static int g_probeLoadArmed;   /* the load-finished signal already scheduled --eval */
static int g_probeLoadNoted;   /* wait-dcl: load finished logged, DCL arms the eval */
static int g_probeEvalDone;
/* --eval-retry: a script click/navigation can replace the document between
 * arming the eval timer and the eval reaching the engine; the control then
 * drops the command ("document replaced by a navigation"). Without this flag
 * the probe runs to --timeout with no payload. With it, the eval is retried
 * against the new document (bounded). Used by the nav/window suites, whose
 * whole point is a navigation triggered by the input. */
static int g_probeEvalRetry;
static int g_probeEvalRetryCount;
static int g_probeGotExit;
static int g_probeSawError;
static int g_probeRespawned;   /* AGB_PROBE_RESPAWN: at most one respawn */
static int g_probeSawDownload;   /* a download finished: that IS the outcome */
static int g_probeClosed;        /* window.close() reached the host */
static int g_probeTimedOut;
static int g_probeSanityFailed;
static int g_probeReloadsLeft;
static int g_probeReloadMode;    /* --reload was given                     */
static int g_probeRound      = 1; /* current round (1 = first document)      */
static int g_probeTotalRounds = 1; /* 1 + number of reloads                 */
static int g_probePollCount;      /* readyState polls in the current round  */
static int g_probeRoundsDone;     /* every round finished -> OK exit code   */
static char g_probeToken[64];     /* per-reload marker for the NEW document */
static int  g_probeVisible;       /* AGB_PROBE_SHOW: the window is on screen  */
static char g_probeShot[512];     /* --shot: capture here, then exit        */
static int  g_probeShotRect[4];   /* --shot-rect x,y,w,h (client pixels)     */
static int  g_probeShotRectSet;   /* a rect was given (else the whole view) */
static int  g_probeShotSettled;   /* one frame elapsed since --eval          */
static char g_probeKeys[512];     /* --keys: typed into the page before eval */
static int  g_probeKeysDone;      /* the --keys list was typed out             */
#define PROBE_MAX_CLICKS 8
static int  g_probeClickList[PROBE_MAX_CLICKS][2]; /* --click: viewport points  */
static int  g_probeClickCount;
static int  g_probeClickIndex;    /* next --click to deliver                   */
/* --drag x1,y1,x2,y2: press at the first point, move, release - the real
 * WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP path, which is how text selection
 * and drag gestures are produced. */
static int  g_probeDragList[PROBE_MAX_CLICKS][4];
static int  g_probeDragCount;
static int  g_probeDragIndex;
/* --shift-click x,y: one click with Shift held (extend the selection from the
 * caret). Delivered with SendMessage so the probe modifier can be set and
 * cleared around the single event, unlike --mod which holds for a whole run. */
static int  g_probeShiftList[PROBE_MAX_CLICKS][2];
static int  g_probeShiftCount;
static int  g_probeShiftIndex;
/* --right-click x,y: the same mouse path with the right button, so the
 * context-menu chain (renderer -> event -> control -> host) can be checked
 * headlessly. */
static int  g_probeRightList[PROBE_MAX_CLICKS][2];
static int  g_probeRightCount;
static int  g_probeRightIndex;
static char g_probePreScript[2048]; /* --pre-eval: runs once before --keys     */
static char g_probeEditor[64];      /* --editor: Editor::execCommand name      */
static int  g_probeEditorDone;      /* the editor command was issued           */
/* --mod ctrl|shift|alt: holds a modifier down while --click/--drag/--keys run.
 * The control reads modifiers with GetKeyState() in a GUI run, so a posted
 * WM_KEYDOWN cannot fake them; in probe mode the control also consults the
 * simulated state set by agave_webview_set_probe_modifiers(). */
static char g_probeMod[16];
static unsigned g_probeModMask;     /* parsed from --mod, 0 = none        */
static int  g_probeModHeld;
static char g_probeIme[512];        /* --ime: posted as WM_IME_CHAR           */
static int  g_probePreDone;       /* --pre-eval was executed                   */
static char g_probeInject[512];   /* --inject: script asset to run before eval */
static int  g_probeInjectDone;    /* --inject already ran for this document    */
static int  g_probeResizeW;       /* --resize WxH: control size before --eval   */
static int  g_probeResizeH;
static int  g_probeResizeDone;    /* the probe window was resized already      */
static int  g_probeZoom;          /* --zoom <percent>: page zoom before --eval  */
static int  g_probeZoomDone;
static int  g_probeDeviceScale;   /* --device-scale <percent>: display DPI/96   */

/* The script --eval/--eval-file asked for: the heap copy when the script came
 * from a file, otherwise the inline buffer. */
static const char* probeScriptText(void)
{
    return g_probeScriptFile ? g_probeScriptFile : g_probeScript;
}

/* --mod: tell the control which modifiers are "held", so every mouse and key
 * event it turns into an engine command carries them. A headless run has no
 * keyboard to press and a posted WM_KEYDOWN cannot change GetKeyState(), so
 * this is the probe's equivalent of holding Ctrl/Shift/Alt. Accepts one name
 * or a '+'-separated combination ("ctrl", "ctrl+shift"). */
static unsigned probeModifierMask(void)
{
    unsigned mask = 0;
    const char* p = g_probeMod;

    while (*p) {
        if (!strncmp(p, "ctrl", 4)) {
            mask |= AGW_MOD_CTRL;
            p += 4;
        } else if (!strncmp(p, "shift", 5)) {
            mask |= AGW_MOD_SHIFT;
            p += 5;
        } else if (!strncmp(p, "alt", 3)) {
            mask |= AGW_MOD_ALT;
            p += 3;
        } else {
            return 0;
        }

        if (*p == '+')
            ++p;
    }

    return mask;
}

static void probeModifierDown(void)
{
    if (!g_probeMod[0] || g_probeModHeld)
        return;

    if (!g_probeModMask)
        return;

    g_probeModHeld = 1;
    agave_webview_set_probe_modifiers(g_probeModMask);
}

static void probeModifierUp(void)
{
    if (!g_probeModHeld)
        return;

    g_probeModHeld = 0;
    agave_webview_set_probe_modifiers(0);
}

/* Offscreen capture. A hidden window gets no WM_PAINT, so PrintWindow has
 * nothing to render; the control keeps its own mirror buffer, and
 * agave_webview_paint_to() blits that into our DIB. Nothing is ever shown, so
 * a screenshot cannot disturb the user (iron law 7).
 *
 * The extension picks the encoding: .bmp is the raw 32-bit bottom-up DIB that
 * pixel_check.py has always read, .png goes straight out through the libpng
 * copy the engine already links (dist/) - no post-processing step needed.
 * --shot-rect crops the grab to a client-pixel rect, so one element can be
 * captured without cropping the image afterwards. */

#define PROBE_SHOT_BMP     0
#define PROBE_SHOT_PNG     1
#define PROBE_SHOT_UNKNOWN (-1)

static int probeShotFormat(const char* path)
{
    const char* dot = strrchr(path, '.');
    const char* slash = strrchr(path, '\\');
    const char* fwd = strrchr(path, '/');

    if (fwd && (!slash || fwd > slash))
        slash = fwd;

    /* A dot in a directory name is not an extension; no extension at all is
     * still an unambiguous "give me an image", so it means PNG. */
    if (!dot || (slash && dot < slash))
        return PROBE_SHOT_PNG;

    if (!_stricmp(dot, ".bmp"))
        return PROBE_SHOT_BMP;
    if (!_stricmp(dot, ".png"))
        return PROBE_SHOT_PNG;

    return PROBE_SHOT_UNKNOWN;
}

/* Writes the (rx,ry,rw,rh) crop of a bottom-up 32-bit BGRA frame as a
 * top-down RGB PNG. Alpha is dropped on purpose: the DIB section keeps none
 * (BI_RGB, alpha byte unused), and a screenshot with a transparent background
 * would be worse than one without an alpha channel. */
static int probeWritePng(const char* path, const unsigned char* bits,
                         int w, int h, int stride,
                         int rx, int ry, int rw, int rh)
{
    unsigned char* image;
    int y, ok;

    if (!path || !bits || rw <= 0 || rh <= 0)
        return 0;

    image = (unsigned char*)malloc((size_t)rw * (size_t)rh * 4);
    if (!image)
        return 0;

    /* DIB rows are bottom-up; WIC wants top-down BGRA rows. */
    for (y = 0; y < rh; ++y) {
        const unsigned char* src = bits
            + (size_t)(h - 1 - (ry + y)) * (size_t)stride + (size_t)rx * 4;
        memcpy(image + (size_t)y * (size_t)rw * 4, src, (size_t)rw * 4);
    }

    ok = shotWritePng(path, image, rw, rh, rw * 4);
    free(image);
    if (!ok)
        agbLog(AGB_LOG_PROBE, "shot: WIC could not write %s", path);
    return ok;
}

/* BMP stays exactly what it always was (bottom-up, 32-bit): pixel_check.py and
 * the pixel-assertion suites read it. The crop is written bottom-up too, so a
 * full-frame capture is byte-identical to the old output. */
static int probeWriteBmp(const char* path, const unsigned char* bits,
                         int w, int h, int stride,
                         int rx, int ry, int rw, int rh)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER bh;
    int rowBytes = rw * 4;
    int y, ok;
    FILE* f;

    f = fopen(path, "wb");
    if (!f) {
        agbLog(AGB_LOG_PROBE, "shot: cannot open %s (errno %d)", path, errno);
        return 0;
    }

    memset(&bh, 0, sizeof(bh));
    bh.biSize = sizeof(BITMAPINFOHEADER);
    bh.biWidth = rw;
    bh.biHeight = rh;               /* bottom-up: the BMP order */
    bh.biPlanes = 1;
    bh.biBitCount = 32;
    bh.biCompression = BI_RGB;

    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;             /* 'BM' */
    fh.bfOffBits = (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER));
    fh.bfSize = fh.bfOffBits + (DWORD)((size_t)rowBytes * (size_t)rh);

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bh, sizeof(BITMAPINFOHEADER), 1, f);
    for (y = 0; y < rh; ++y) {
        /* Bottom-up crop row y is client row (ry + rh - 1 - y); the DIB is
         * bottom-up as well, so its row index is (h - ry - rh + y). */
        const unsigned char* src = bits
            + (size_t)(h - ry - rh + y) * (size_t)stride + (size_t)rx * 4;
        fwrite(src, (size_t)rowBytes, 1, f);
    }

    ok = ferror(f) ? 0 : 1;
    fclose(f);
    if (!ok)
        remove(path);
    return ok;
}

/* CSS px -> device px at the probe's display scale, rounding half away from zero.
 * getBoundingClientRect() is CSS px and a --shot-rect value comes from it; page
 * zoom is a CSS zoom, so its effect is already in the rect (a 40px box reports
 * 50 at 125%) and only the display scale is left to apply. */
static int probeRectCssToClient(int value, int deviceScalePercent)
{
    long v = (long)value * deviceScalePercent;

    return (int)((v >= 0 ? v + 50 : v - 50) / 100);
}

static int probeCapture(const char* path)
{
    HWND target = (HWND)agave_webview_hwnd(g_probeView);
    RECT rc;
    HDC mdc;
    HBITMAP dib;
    BITMAPINFO bi;
    unsigned char* bits = NULL;
    int w, h, stride, fmt, ok = 0;
    int rx = 0, ry = 0, rw, rh;

    if (!target || !GetClientRect(target, &rc))
        return 0;

    w = (int)(rc.right - rc.left);
    h = (int)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0)
        return 0;

    fmt = probeShotFormat(path);
    if (fmt == PROBE_SHOT_UNKNOWN) {
        agbLog(AGB_LOG_PROBE, "shot: unknown extension in %s (use .png or .bmp)", path);
        return 0;
    }

    /* --shot-rect is in CSS px, origin at the viewport's top-left: exactly what
     * getBoundingClientRect() reports for an element scrolled into view. Both
     * edges are converted before subtracting so a fractional display scale cannot
     * shrink the crop by a pixel. Clamp instead of failing: a stale rect still
     * yields the part that is on screen, and a rect fully outside is reported as
     * a failure, not a blank file that looks like a rendering bug. */
    rw = w;
    rh = h;
    if (g_probeShotRectSet) {
        int deviceScale = g_probeDeviceScale > 0 ? g_probeDeviceScale : 100;
        int x0 = probeRectCssToClient(g_probeShotRect[0], deviceScale);
        int y0 = probeRectCssToClient(g_probeShotRect[1], deviceScale);
        int x1 = probeRectCssToClient(g_probeShotRect[0] + g_probeShotRect[2], deviceScale);
        int y1 = probeRectCssToClient(g_probeShotRect[1] + g_probeShotRect[3], deviceScale);

        rx = x0;
        ry = y0;
        rw = x1 - x0;
        rh = y1 - y0;

        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }

        if (rx >= w || ry >= h || rw <= 0 || rh <= 0) {
            int total = deviceScale > 0 ? deviceScale : 100;

            agbLog(AGB_LOG_PROBE, "shot: rect %d,%d,%d,%d (css px) is outside the %dx%d view",
                   g_probeShotRect[0], g_probeShotRect[1],
                   g_probeShotRect[2], g_probeShotRect[3],
                   (w * 100 + total / 2) / total, (h * 100 + total / 2) / total);
            return 0;
        }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;      /* bottom-up: the BMP order */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    mdc = CreateCompatibleDC(NULL);
    if (!mdc)
        return 0;
    dib = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mdc);
        return 0;
    }
    SelectObject(mdc, dib);

    agave_webview_paint_to(g_probeView, mdc, w, h);

    stride = w * 4;
    if (fmt == PROBE_SHOT_BMP)
        ok = probeWriteBmp(path, bits, w, h, stride, rx, ry, rw, rh);
    else
        ok = probeWritePng(path, bits, w, h, stride, rx, ry, rw, rh);

    if (ok)
        agbLog(AGB_LOG_PROBE, "shot %dx%d at %d,%d %s -> %s",
               rw, rh, rx, ry, fmt == PROBE_SHOT_BMP ? "bmp" : "png", path);

    DeleteObject(dib);
    DeleteDC(mdc);
    return ok;
}

/* Command line, parsed before any window exists: a --probe run must never
 * create the browser window (it used to be shown, then left on screen for the
 * whole capture because probeRun runs its own message loop). */
static int  g_showUsage;                        /* --help                     */
static int  g_argError;                         /* --probe without a URL       */
static char g_startupUrl[MAX_PATH_CHARS];       /* normalized, may be empty   */

/* --crash-page <report>: show a crash report in its own minimal window. This
 * process is started by the exception handler, so nothing below the report
 * (profile, session, single instance, engine state) is touched. */
static char g_crashPagePath[MAX_PATH_CHARS];

/* --ui-shot: headless chrome render (no window, no engine, no profile). */
static char g_uiShotPath[MAX_PATH_CHARS];
static char g_uiShotTheme[16];
static int  g_uiShotWidth = 1100;
static int  g_uiShotHeight = 760;
static unsigned g_uiShotDpi = 96;
static int  g_uiShotMenu;          /* --ui-menu: paint the sample app menu */
static int  g_uiShotFocus;         /* --ui-focus: address pill owns focus */
static int  g_uiShotSlide;         /* --ui-close-slide: close-slide geometry checks */
static char g_uiShotFaviconPath[MAX_PATH_CHARS];  /* --ui-favicon: tab icon fixture */

static void probePrint(const char* tag, const char* text)
{
    printf("[%9.1fms] %s %s\n", agbLogElapsedMs(), tag, text ? text : "");
    fflush(stdout);
}

static void probeOnMessage(AgaveWebView* view, const char* message, void* user)
{
    (void)view;
    (void)user;

    /* probeExit(result) sends this sentinel: print the payload and leave at
     * once instead of waiting for the settle timer. */
    if (message && !strncmp(message, "__agb_exit__", 12)) {
        g_probeGotExit = 1;
        probePrint("[exit]", message + 12);
        PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        return;
    }

    /* Host-marker reports (inspect.js, embedded). Headless runs use this line as
     * the regression signal: the page half is provable without any window. */
    if (message && !strncmp(message, "__agb_inspect__", 15)) {
        probePrint("[inspect]", message + 15);
        return;
    }

    /* Empty-document sanity failure: report and leave with the SANITY code. */
    if (message && !strncmp(message, "__agb_sanity__", 14)) {
        g_probeSanityFailed = 1;
        probePrint("[sanity]", message + 14);
        PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        return;
    }

    /* The page's own window load event: the proper moment to run --eval.
     * Route through the main thread (PostMessage) - never SetTimer from the
     * engine thread. */
    if (message && !strncmp(message, "__agb_loaded__", 14)) {
        const char* href = message + 14;

        if (!g_probeFinished) {
            char line[768];

            _snprintf(line, sizeof(line), "window load event url=%s", href);
            line[sizeof(line) - 1] = 0;
            probePrint("[probe]", line);
            PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
        }
        return;
    }

    /* DOMContentLoaded, reported by prelude.js (registered at document bind,
     * before the page's own scripts). Only --wait-dcl consumes it: other probes
     * ignore the message, so the opt-in stays invisible in their output. */
    if (message && !strncmp(message, "__agb_dcl__", 11)) {
        const char* href = message + 11;

        if (!g_probeWaitDcl)
            return;

        if (!g_probeFinished && !g_probeLoadArmed) {
            char line[768];

            g_probeLoadArmed = 1;
            _snprintf(line, sizeof(line), "domcontentloaded url=%s",
                      href && href[0] ? href : "(unknown)");
            line[sizeof(line) - 1] = 0;
            probePrint("[probe]", line);
            PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
        }
        return;
    }

    /* The --eval script has returned (the probe posts a follow-up eval that
     * sends this). Only now may --settle start counting: it used to be armed
     * when the eval was *posted*, so any script slower than --settle had the
     * view destroyed underneath it - the 0xC0000005 that looked like a JIT
     * crash (R61). Still a timer on the main thread: the message arrives from
     * the engine thread. */
    if (message && !strncmp(message, "__agb_evaldone__", 16)) {
        PostMessageA(g_probeWnd, AGW_PROBE_EVALDONE, 0, 0);
        return;
    }

    /* minishell://settings asks for its values; the probe answers with the same
     * code the shell uses, so the page can be verified headlessly. */
    if (message && !strncmp(message, "__agb_settings_get__", 20)) {
        settingsSendAll(g_probeView);
        return;
    }

    /* New tab page asks for its theme; AGB_NEWTAB_THEME lets a probe exercise
     * the dark palette without a shell window. */
    if (message && !strncmp(message, "__agb_newtab_get__", 18)) {
        const char* theme = getenv("AGB_NEWTAB_THEME");
        char reply[48];

        _snprintf(reply, sizeof(reply), "__agb_newtab_theme__%s",
                  (theme && !_stricmp(theme, "dark")) ? "dark" : "light");
        reply[sizeof(reply) - 1] = 0;
        agave_webview_post_message(g_probeView, reply);
        return;
    }

    /* The new tab page's search box: print the URL the shell would navigate to
     * (the same builder the address bar uses), so the attribution parameters
     * are testable headlessly. */
    if (message && !strncmp(message, "__agb_newtab_search__", 21)) {
        char url[MAX_PATH_CHARS];

        normalizeUrl(message + 21, url, sizeof(url));
        probePrint("[search]", url);
        return;
    }

    probePrint("[msg]", message);
}

/* AGB_PROBE_RESPAWN=1: load a normal page into the view whose renderer just
 * died, proving the control made it respawn-ready. Returns 1 when it did. */
static int probeRespawnInto(AgaveWebView* view, const char* url)
{
    static const char respawn[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>respawned</title></head><body>respawned</body></html>";

    if (!getenv("AGB_PROBE_RESPAWN") || !view || g_probeRespawned)
        return 0;

    g_probeRespawned = 1;
    agave_webview_load_html(view, respawn, url ? url : "about:blank");
    agbLog(AGB_LOG_PROBE, "respawn: loading a page into the crashed view");
    return 1;
}

static void probeOnError(AgaveWebView* view, int code, const char* url, void* user)
{
    (void)user;

    g_probeSawError = 1;
    agbLog(AGB_LOG_PROBE, "load error code=%d url=%s", code, url ? url : "");

    /* AGB_PROBE_RESPAWN=1: after a renderer crash the control has dropped the
     * dead backend, so a load_html brings a fresh one up. The crash regression
     * asserts exactly that (the probe then evals the new document). */
    if (probeRespawnInto(view, url)) {
        g_probeSawError = 0;
        g_probeLoadArmed = 0;
        g_probeLoadNoted = 0;
    }

    /* A failed navigation commits no document, so on_progress(finished) never
     * arrives. Arm the eval here or the run would sit until --timeout; the
     * eval then reports the empty document and the run ends with the load
     * error code. A respawned run skips this: its page loads normally. */
    if (!g_probeFinished && !g_probeReloadMode && !g_probeLoadArmed && !g_probeRespawned) {
        g_probeLoadArmed = 1;
        PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);

        /* A dead engine (the process backend's renderer crashed, or a realm
         * that never came up) never reports eval-done: bound the run so an
         * error is an error and not a --timeout. A healthy run's eval re-arms
         * this timer in AGW_PROBE_EVALDONE. */
        SetTimer(g_probeWnd, PROBE_TIMER_SETTLE,
                 (UINT)(g_probeWaitMs + g_probeSettleMs + 600), NULL);
    }
}

static int probeOnNewWindow(AgaveWebView* view, AgaveWebView* newView,
                            const char* url, void* user)
{
    (void)view;
    (void)user;

    /* Keep it: the probe reports the URL the new window ended up on, which is
     * what tells a working new-window path from a blank one. */
    g_probeNewView = newView;
    agbLog(AGB_LOG_PROBE, "new window requested url=%s", url ? url : "");
    fflush(stderr);
    return 1;
}

static int probeOnDialog(AgaveWebView* view, int type, const char* message,
                         const char* defaultValue, char* result, int resultSize,
                         void* user)
{
    (void)view;
    (void)type;
    (void)defaultValue;
    (void)result;
    (void)resultSize;
    (void)user;

    /* A beforeunload guard blocks the navigation; everything else (page
     * alert/confirm in other tests) auto-accepts. AGB_PROBE_ALLOW_UNLOAD=1
     * flips the guard answer, to check that allowing really does navigate. */
    agbLog(AGB_LOG_PROBE, "dialog type=%d msg=%s", type, message ? message : "");
    fflush(stderr);

    if (message && strstr(message, "LEAVE-GUARD")) {
        const char* allow = getenv("AGB_PROBE_ALLOW_UNLOAD");

        return (allow && *allow && *allow != '0') ? 1 : 0;
    }

    return 1;
}

/* The watchdog's notice, on the UI thread. The probe has no window to show it
 * in, so it prints one line plus the busy-stack file: that is what makes "the
 * engine reported a stuck page and told the host" testable headlessly
 * (AGB_UNRESPONSIVE_MS=2000 on a spinner). */
/* The probe has no tab to show a crashed page on: print the report (a suite
 * asserts on it), keep the exact page the GUI would load when asked, and leave
 * with the normal OK code - a renderer that crashed on request is not a probe
 * failure. */
static void probeCrashPage(AgaveWebView* view, const char* html, const char* base,
                           int code, const char* report)
{
    const char* pagePath = getenv("AGB_PROBE_CRASH_PAGE");
    size_t reportChars = report ? strlen(report) : 0;

    printf("[probe] crash code=%d report_bytes=%lu page_bytes=%lu\n",
           code, (unsigned long)reportChars, (unsigned long)strlen(html));

    if (reportChars)
        printf("%s\n", report);
    fflush(stdout);

    if (pagePath && pagePath[0]) {
        FILE* f = fopen(pagePath, "wb");

        if (f) {
            fwrite(html, 1, strlen(html), f);
            fclose(f);
        }
    }

    agave_webview_load_html(view, html, base);

    /* A respawn run wants the view usable again, not a report: load a normal
     * page into it and let its own load event drive --eval (the run's exit code
     * is then the respawned page's, not the crash's). */
    if (probeRespawnInto(view, base))
        return;

    /* Regression hook for P1-ac: eval right after load_html, i.e. while the new
     * document has not been committed yet. The command must be dropped with a
     * reason (on_command_dropped), never fall into the legacy interpreter. */
    if (getenv("AGB_PROBE_CRASH_EVAL")) {
        agave_webview_eval(view, "agavePostMessage('__agb_exit__crasheval-ran')");
        PostMessageA(g_probeWnd, AGW_PROBE_EVALDONE, 0, 0);
        return;
    }

    g_probeGotExit = 1;
    PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
}

static void probeOnUnresponsive(AgaveWebView* view, const char* reason,
                                unsigned int busy_ms, const char* stack_path,
                                void* user)
{
    (void)view;
    (void)user;

    probePrint("[probe] unresponsive", reason ? reason : "");
    fprintf(stderr, "[probe] unresponsive busy=%ums stack=%s\n",
            busy_ms, (stack_path && stack_path[0]) ? stack_path : "(none)");
}

/* A command the host issued could not be delivered: print it and count it, so a
 * probe run can prove "either it ran or the host heard about it" from stdout. */
static volatile LONG g_probeDropped;

static void probeOnCommandDropped(AgaveWebView* view, const char* kind,
                                  const char* reason, void* user)
{
    (void)view;
    (void)user;

    InterlockedIncrement(&g_probeDropped);
    printf("dropped=%ld kind=%s reason=%s\n",
           (long)g_probeDropped, kind ? kind : "?", reason ? reason : "?");
    fflush(stdout);
    agbLog(AGB_LOG_PROBE, "dropped command kind=%s reason=%s",
           kind ? kind : "?", reason ? reason : "?");

    /* The eval lost the race with a navigation the probe's own click started:
     * the new document is committing right now, so arm the eval again in a
     * moment instead of letting the run sit until --timeout. Bounded: a page
     * that navigates forever still ends at --timeout. */
    if (g_probeEvalRetry && !g_probeGotExit && g_probeEvalRetryCount < 8 &&
        kind && !strcmp(kind, "eval")) {
        ++g_probeEvalRetryCount;
        agbLog(AGB_LOG_PROBE, "retrying eval after navigation (%d)", g_probeEvalRetryCount);
        /* The eval was already dispatched (g_probeEvalDone is set before the
         * queue sees it), so clear both gates before arming it again. */
        g_probeEvalDone = 0;
        g_probeFinished = 0;
        PostMessageA(g_probeWnd, AGW_PROBE_MSG, 600, 0);
    }
}

/* Right click reaches the host on the UI thread (the control copies the payload
 * and posts it there; a menu cannot be built from the engine thread). The probe
 * only records it - no menu, so the run stays headless - which is what makes the
 * path testable without a human at the mouse. */
static void probeOnContextMenu(AgaveWebView* view, int x, int y,
                               const char* linkUrl, const char* imageUrl,
                               const char* text, int editable, void* user)
{
    (void)view;
    (void)user;

    agbLog(AGB_LOG_PROBE, "context menu at %d,%d link=%s image=%s editable=%d",
           x, y, linkUrl && linkUrl[0] ? linkUrl : "(none)",
           imageUrl && imageUrl[0] ? imageUrl : "(none)", editable);

    /* Stable stdout line: the process backend's context-menu chain (renderer ->
     * event -> control -> host) is checked against it. */
    printf("[ctx] menu x=%d y=%d link=%s image=%s editable=%d selection=%lu\n",
           x, y,
           linkUrl && linkUrl[0] ? linkUrl : "(none)",
           imageUrl && imageUrl[0] ? imageUrl : "(none)",
           editable,
           (unsigned long)(text ? strlen(text) : 0));
    fflush(stdout);
}

static void probeOnCloseWindow(AgaveWebView* view, void* user)
{
    (void)view;
    (void)user;

    /* window.close() reached the host: report and leave (a success exit, so
     * a script can assert the whole chain worked). */
    g_probeClosed = 1;
    g_probeGotExit = 1;
    probePrint("[close]", "window.close() reached the host");
    PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
}

/* A download is a successful navigation outcome with no document behind it:
 * nothing commits, so on_progress(finished) never fires and there is nothing to
 * eval. Report the file and leave with the OK exit code. */
static void probeOnDownload(AgaveWebView* view, const MC_DOWNLOAD_INFO* info, void* user)
{
    (void)view;
    (void)user;

    if (!info)
        return;

    if (info->state == MC_DL_FINISHED) {
        /* Only a download that *is* the navigation counts as the outcome: if a
         * document already committed, this was a side effect of the page (an
         * iframe, a link) and the run must end the way it normally does. */
        if (!g_probeFinished) {
            g_probeSawDownload = 1;

            printf("[%9.1fms] [probe] download path=%s bytes=%llu total=%llu\n",
                   agbLogElapsedMs(), info->utf8_path ? info->utf8_path : "",
                   info->received, info->total);
            fflush(stdout);

            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
        } else {
            agbLog(AGB_LOG_PROBE, "download finished path=%s bytes=%llu",
                   info->utf8_path ? info->utf8_path : "", info->received);
        }
    } else if (info->state == MC_DL_FAILED) {
        g_probeSawError = 1;

        printf("[%9.1fms] [probe] download failed code=%d url=%s\n",
               agbLogElapsedMs(), info->error, info->utf8_url ? info->utf8_url : "");
        fflush(stdout);

        if (!g_probeFinished)
            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
    }
}

static void probeOnProgress(AgaveWebView* view, int percent, int finished, void* user)
{
    (void)view;
    (void)percent;
    (void)user;

    if (!g_probeCallbackThreadChecked) {
        g_probeCallbackThreadChecked = 1;
        if (g_probeUiThread && GetCurrentThreadId() != g_probeUiThread) {
            g_probeCallbackThreadBad = 1;
            probePrint("[probe]", "callback thread violation: progress arrived on the engine thread");
        } else {
            probePrint("[probe]", "callback thread ok: progress arrived on the ui thread");
        }
    }

    /* Engine-thread callback: never SetTimer here (the probe window is owned
     * by the main thread; cross-thread window timers are undefined). Post the
     * request to the main loop instead.
     *
     * "progress finished" is the engine's own "the document loaded" signal and
     * fires exactly once, for the requested document - a frame now gets its one
     * document from its first commit, so there is no initial empty document to
     * report early. --wait is therefore measured from the real load, and no JS
     * hook or readyState poll is needed. */
    if (finished && !g_probeFinished && !g_probeLoadArmed) {
        if (g_probeWaitDcl) {
            /* --wait-dcl: DOMContentLoaded (reported by prelude.js) owns the
             * arming. Log the load for the record, once, and do not start the
             * test at a later lifecycle point than the caller asked for. */
            if (!g_probeLoadNoted) {
                g_probeLoadNoted = 1;
                probePrint("[probe]", "progress finished (waiting for domcontentloaded)");
            }
        } else {
            g_probeLoadArmed = 1;
            probePrint("[probe]", g_probeSawError ? "progress finished (with error)" : "progress finished");
            PostMessageA(g_probeWnd, AGW_PROBE_MSG, g_probeWaitMs, 0);
        }
    }
}

static LRESULT CALLBACK probeWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == AGW_PROBE_MSG) {
        /* Engine thread asks for the eval timer; wParam carries the delay.
         * Also stops the post-reload readyState poll: the first
         * __agb_loaded__ means the new document is done. */
        KillTimer(g_probeWnd, PROBE_TIMER_POLL);

        if (!g_probeFinished) {
            g_probeFinished = 1;
            /* Reload rounds: the document is already parsed, the extra
             * wait only exists for pages that paint after load. */
            SetTimer(g_probeWnd, PROBE_TIMER_EVAL,
                     (UINT)(g_probeRound > 1 ? 150 : wParam), NULL);
        }
        return 0;
    }

    if (message == AGW_PROBE_EVALDONE) {
        /* The eval finished. A probeExit()/sanity abort already closed the
         * window; otherwise the settle delay starts here. */
        if (!g_probeGotExit && !g_probeSanityFailed) {
            KillTimer(g_probeWnd, PROBE_TIMER_SETTLE);
            SetTimer(g_probeWnd, PROBE_TIMER_SETTLE, g_probeSettleMs, NULL);
        }
        return 0;
    }

    /* The message loop must end when the window goes away: GetMessage(NULL)
     * only returns 0 on WM_QUIT, never because the last window was destroyed. */
    if (message == WM_DESTROY)
        PostQuitMessage(0);

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

/* --keys: replay real typing through the control's own window procedure, so a
 * headless probe exercises the same path a keyboard does (WM_KEYDOWN, WM_CHAR,
 * WM_KEYUP) instead of a DOM-level shortcut that would skip the key handling
 * under test.
 *
 * {backspace} {enter} {tab} {esc} {delete} {left} {right} {up} {down} {home}
 * {end} name a non-printable key; everything else is typed as UTF-8.
 *
 * Keys go out one per timer tick: TranslateMessage appends the WM_CHAR to the
 * end of the message queue, so a burst of posted keydowns would deliver every
 * character after all of them - a real keyboard never does that, and the page
 * would see the keys in the wrong order. */
static int probeKeyIsExtended(unsigned vk)
{
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT:
    case VK_PRIOR: case VK_NEXT:
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    unsigned vk;    /* virtual key for WM_KEYDOWN, 0 = text only */
    unsigned ch;    /* the character the key produces, 0 = none */
    unsigned ime;   /* 1 = post WM_IME_CHAR (what an IME delivers) */
} ProbeKey;

#define PROBE_MAX_KEYS 128
static ProbeKey g_probeKeyList[PROBE_MAX_KEYS];
static int g_probeKeyCount;
static int g_probeKeyIndex;

static void probeParseKeys(void)
{
    const unsigned char* p = (const unsigned char*)g_probeKeys;

    g_probeKeyCount = 0;

    while (*p && g_probeKeyCount < PROBE_MAX_KEYS) {
        unsigned vk = 0;
        unsigned ch = 0;
        unsigned cp;

        if (*p == '{') {
            const char* name = (const char*)p + 1;
            const char* close = strchr(name, '}');
            size_t len = close ? (size_t)(close - name) : 0;

            if (!close) {
                ++p;            /* stray '{': type it literally */
                ch = '{';
            } else {
                if (len == 9 && !strncmp(name, "backspace", 9))      { vk = VK_BACK;   ch = 8; }
                else if (len == 5 && !strncmp(name, "enter", 5))     { vk = VK_RETURN; ch = 13; }
                else if (len == 3 && !strncmp(name, "tab", 3))       { vk = VK_TAB;    ch = 9; }
                else if (len == 3 && !strncmp(name, "esc", 3))       { vk = VK_ESCAPE; }
                else if (len == 6 && !strncmp(name, "delete", 6))    { vk = VK_DELETE; }
                else if (len == 4 && !strncmp(name, "left", 4))      { vk = VK_LEFT; }
                else if (len == 5 && !strncmp(name, "right", 5))     { vk = VK_RIGHT; }
                else if (len == 2 && !strncmp(name, "up", 2))        { vk = VK_UP; }
                else if (len == 4 && !strncmp(name, "down", 4))      { vk = VK_DOWN; }
                else if (len == 4 && !strncmp(name, "home", 4))      { vk = VK_HOME; }
                else if (len == 3 && !strncmp(name, "end", 3))       { vk = VK_END; }
                p = (const unsigned char*)close + 1;
                if (!vk && !ch)
                    continue;   /* unknown token: skip it */
            }
        } else {
            /* Decode one UTF-8 code point (the control re-encodes it). */
            if (p[0] < 0x80) {
                cp = p[0];
                p += 1;
            } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x1F) << 6) | (unsigned)(p[1] & 0x3F);
                p += 2;
            } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x0F) << 12) |
                     ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
                p += 3;
            } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
                       (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                cp = ((unsigned)(p[0] & 0x07) << 18) | ((unsigned)(p[1] & 0x3F) << 12) |
                     ((unsigned)(p[2] & 0x3F) << 6) | (unsigned)(p[3] & 0x3F);
                p += 4;
            } else {
                ++p;            /* invalid byte: skip */
                continue;
            }

            if (cp >= 0x10000) {
                /* WM_CHAR carries one UTF-16 code unit: an astral character is
                 * a surrogate pair, i.e. two messages. */
                unsigned int pair = cp - 0x10000;
                unsigned int hi = 0xD800 + (pair >> 10);
                unsigned int lo = 0xDC00 + (pair & 0x3FF);

                if (g_probeKeyCount + 1 < PROBE_MAX_KEYS) {
                    g_probeKeyList[g_probeKeyCount].vk = 0;
                    g_probeKeyList[g_probeKeyCount].ch = hi;
                    g_probeKeyList[g_probeKeyCount].ime = 0;
                    g_probeKeyCount++;
                    g_probeKeyList[g_probeKeyCount].vk = 0;
                    g_probeKeyList[g_probeKeyCount].ch = lo;
                    g_probeKeyList[g_probeKeyCount].ime = 0;
                    g_probeKeyCount++;
                }
                continue;
            }

            ch = cp;
            /* The virtual key a real keyboard reports: letters arrive as
             * VK_A..VK_Z, digits as their ASCII code. Non-ASCII text has no vk. */
            if (cp >= 'a' && cp <= 'z')
                vk = cp - 'a' + 'A';
            else if (cp < 0x80)
                vk = cp;
        }

        g_probeKeyList[g_probeKeyCount].vk = vk;
        g_probeKeyList[g_probeKeyCount].ch = ch;
        g_probeKeyCount++;
    }
}

/* --ime: what a real IME delivers. Each character arrives as WM_IME_CHAR with
 * one UTF-16 code unit (a character outside the BMP arrives as two of them, the
 * surrogate pair), not as WM_KEYDOWN + WM_CHAR. The text is appended to the
 * key list so it shares the one-per-tick pacing. */
static void probeParseIme(void)
{
    WCHAR wide[PROBE_MAX_KEYS];
    int count;
    int i;

    if (!g_probeIme[0])
        return;

    count = MultiByteToWideChar(CP_UTF8, 0, g_probeIme, -1, wide, PROBE_MAX_KEYS - 1);
    if (count <= 0)
        return;

    for (i = 0; i < count && g_probeKeyCount < PROBE_MAX_KEYS; ++i) {
        if (!wide[i])
            break;
        g_probeKeyList[g_probeKeyCount].vk = 0;
        g_probeKeyList[g_probeKeyCount].ch = (unsigned)wide[i];
        g_probeKeyList[g_probeKeyCount].ime = 1;
        g_probeKeyCount++;
    }
}

/* Sends the next key; returns 0 when the list is exhausted. */
static int probeSendNextKey(void)
{
    HWND target = (HWND)agave_webview_hwnd(g_probeView);
    ProbeKey key;

    if (!target || g_probeKeyIndex >= g_probeKeyCount)
        return 0;

    key = g_probeKeyList[g_probeKeyIndex++];

    /* A posted WM_KEYDOWN is turned into WM_CHAR by the shell's own
     * TranslateMessage, exactly like a keyboard message; posting WM_CHAR as
     * well would deliver every character twice. Only text without a virtual
     * key (non-ASCII) has to be posted directly.
     *
     * The lParam has to look like a real keyboard message: TranslateMessage
     * derives WM_CHAR from it, and a zeroed lParam makes the WM_KEYUP look
     * like a key press too, so every character arrived twice. */
    if (key.ime) {
        agbLog(AGB_LOG_PROBE, "ime u+%04X", key.ch);
        PostMessageW(target, WM_IME_CHAR, (WPARAM)key.ch, 1);
    } else if (key.vk) {
        unsigned scan = MapVirtualKeyA(key.vk, MAPVK_VK_TO_VSC);
        LPARAM down = (LPARAM)(1 | (scan << 16) | (probeKeyIsExtended(key.vk) ? (1 << 24) : 0));
        LPARAM up = (LPARAM)(down | (1 << 30) | (1 << 31));

        agbLog(AGB_LOG_PROBE, "key vk=%u ch=%u", key.vk, key.ch);
        PostMessageA(target, WM_KEYDOWN, (WPARAM)key.vk, down);
        PostMessageA(target, WM_KEYUP, (WPARAM)key.vk, up);
    } else if (key.ch) {
        agbLog(AGB_LOG_PROBE, "char cp=%u", key.ch);
        PostMessageW(target, WM_CHAR, (WPARAM)key.ch, 1);
    }

    return 1;
}

static int probeRun(const char* url)
{
    /* Own profile root before any engine exists (cookies/localStorage/caches). */
    probeUserDataInit();
    shellStateInit();

    WNDCLASSA wc;
    AgaveWebViewCallback callbacks;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = probeWndProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = "AgaveProbeWindow";
    RegisterClassA(&wc);

    /* Hidden popup: the control only needs a parent HWND. No frame, no caption
     * and WS_EX_TOOLWINDOW, so a probe never shows a taskbar entry or steals
     * the foreground - the capture is headless unless AGB_PROBE_SHOW is set. */
    g_probeWnd = CreateWindowExA(WS_EX_TOOLWINDOW, "AgaveProbeWindow", "MiniShell probe",
                                 WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1100, 800, NULL, NULL, g_instance, NULL);
    g_probeUiThread = GetCurrentThreadId();
    if (!g_probeWnd) {
        agbLog(AGB_LOG_PROBE, "CreateWindowEx failed: %lu", (unsigned long)GetLastError());
        return PROBE_EXIT_USAGE;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_message = probeOnMessage;
    callbacks.on_progress = probeOnProgress;
    callbacks.on_error = probeOnError;
    callbacks.on_download = probeOnDownload;
    callbacks.on_new_window = probeOnNewWindow;
    callbacks.on_close_window = probeOnCloseWindow;
    callbacks.on_dialog = probeOnDialog;
    callbacks.on_context_menu = probeOnContextMenu;
    callbacks.on_unresponsive = probeOnUnresponsive;
    callbacks.on_command_dropped = probeOnCommandDropped;
    callbacks.on_activate_view = onActivateView;
    callbacks.on_crashed = onCrashed;

    g_probeView = agave_webview_create(g_probeWnd, 0, 0, 1100, 800, &callbacks, NULL);
    if (!g_probeView) {
        agbLog(AGB_LOG_PROBE, "agave_webview_create failed");
        return PROBE_EXIT_USAGE;
    }

    /* --device-scale: a headless run has no monitor to ask, so the DPI the GUI
     * would push through WM_DPICHANGED comes from the command line. It only
     * describes the environment (the settings page / logs / --shot-rect report
     * it); page zoom is CSS zoom and does not change devicePixelRatio. */
    if (g_probeDeviceScale > 100)
        agave_webview_set_device_scale(g_probeView, g_probeDeviceScale);

    /* Match the GUI: createTabIn() applies the tab's page zoom before the first
     * navigation, so a probe without --zoom must still pin 100% instead of
     * leaving the engine's default (which follows the process DPI). */
    /* Only a real --zoom touches the engine; with no flag the view keeps the
     * engine's high-DPI default, like the GUI (see viewApplyEngineScale). */
    if (g_probeZoom > 0) {
        agave_webview_set_zoom(g_probeView, g_probeZoom);
        g_probeZoomDone = 1;
    }

    /* AGB_PROBE_SHOW=1 shows the window; debugging aid for load paths that
     * behave differently when the control is never visible. A hidden window
     * gets no WM_PAINT, so this is also what pixel checks need. */
    g_probeVisible = getenv("AGB_PROBE_SHOW") != NULL;
    if (g_probeVisible)
        ShowWindow(g_probeWnd, SW_SHOWNOACTIVATE);

    /* A capture needs pixels from the final layout, and the engine loop skips
     * layout+paint for a view the host reported hidden. Real pages finish their
     * CSS/JS after the load event, so a skipped paint left the mirror holding
     * the first, unstyled frame (github.com came out as a white page with default
     * link blue while every stylesheet had applied in the DOM). Marking the page
     * visible makes the loop paint every iteration and also gives the page
     * document.visibilityState/requestAnimationFrame of a foreground tab - both
     * true for a capture - while the window itself stays hidden. */
    if (g_probeShot[0])
        agave_webview_set_visible(g_probeView, 1);

    agbLog(AGB_LOG_PROBE, "headless %s (pid %lu)",
           g_probeVisible ? "window shown (AGB_PROBE_SHOW)" : "window hidden, AGB_PROBE_SHOW=1 to show",
           (unsigned long)GetCurrentProcessId());

    SetTimer(g_probeWnd, PROBE_TIMER_TIMEOUT, g_probeTimeoutMs, NULL);
    if (g_probeInterruptAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_INTERRUPT, (UINT)g_probeInterruptAfterMs, NULL);
    if (g_probeNavAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_NAV, (UINT)g_probeNavAfterMs, NULL);
    if (g_probeHideAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_HIDE, (UINT)g_probeHideAfterMs, NULL);
    if (g_probeShowAfterMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_SHOW, (UINT)g_probeShowAfterMs, NULL);
    if (g_probeReloadEveryMs > 0)
        SetTimer(g_probeWnd, PROBE_TIMER_RELOAD, (UINT)g_probeReloadEveryMs, NULL);

    /* Focus only makes sense for a visible window; a hidden control that grabs
     * focus activates a window nobody can see. */
    if (g_probeVisible)
        agave_webview_set_focus(g_probeView);

    if (g_probeBeforeNavEval && g_probeBeforeNavEval[0])
        agave_webview_eval(g_probeView, g_probeBeforeNavEval);

    agave_webview_navigate(g_probeView, url);

    /* --eval-early: right after navigate() returns, i.e. while the old document
     * may still be on screen. Either this runs or it is reported as dropped -
     * never both missing (see probeOnCommandDropped). */
    if (g_probeEarlyEval && g_probeEarlyEval[0])
        agave_webview_eval(g_probeView, g_probeEarlyEval);

    /* Nothing is injected here any more: the engine's on_progress(finished)
     * callback is the single "the requested document loaded" signal, and --wait
     * is measured from it. */

    while (GetMessageW(&msg, NULL, 0, 0)) {
        /* Only our own window's timers: the webview control runs internal
         * SetTimer(id=1, ~10ms) frame-pump timers on its own child window,
         * and their WM_TIMER (same message, same wParam=1) used to fire the
         * EVAL branch instantly. Filter by hwnd. */
        if (msg.message == WM_TIMER && msg.hwnd == g_probeWnd) {
            if (msg.wParam == PROBE_TIMER_TIMEOUT) {
                g_probeTimedOut = 1;
                probePrint("[probe]", g_probeFinished ? "timeout, exiting" : "timeout, load never finished");
                break;
            }

            if (msg.wParam == PROBE_TIMER_RELOAD) {
                if (g_probeReloadCount-- <= 0) {
                    KillTimer(g_probeWnd, PROBE_TIMER_RELOAD);
                } else if (g_probeView) {
                    probePrint("[probe]", "reload-every: posting reload");
                    agbLog(AGB_LOG_PROBE, "reload (hammer) %d left", g_probeReloadCount);
                    agave_webview_reload(g_probeView);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_NAV) {
                KillTimer(g_probeWnd, PROBE_TIMER_NAV);
                if (g_probeView) {
                    probePrint("[probe]", "nav-after: navigating while the page is busy");
                    agave_webview_navigate(g_probeView,
                                           g_probeNavUrl[0] ? g_probeNavUrl : "about:blank");

                    /* Same race as the start-of-run --eval-early, but with a
                     * live document on screen: the command is issued for the
                     * page that is about to be replaced. */
                    if (g_probeEarlyEval && g_probeEarlyEval[0])
                        agave_webview_eval(g_probeView, g_probeEarlyEval);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_HIDE) {
                KillTimer(g_probeWnd, PROBE_TIMER_HIDE);
                if (g_probeView) {
                    probePrint("[probe]", "hide-after: tab hidden");
                    agave_webview_set_visible(g_probeView, 0);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_SHOW) {
                KillTimer(g_probeWnd, PROBE_TIMER_SHOW);
                if (g_probeView) {
                    probePrint("[probe]", "show-after: tab visible again");
                    agave_webview_set_visible(g_probeView, 1);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_INTERRUPT) {
                KillTimer(g_probeWnd, PROBE_TIMER_INTERRUPT);
                if (g_probeView) {
                    probePrint("[probe]", "interrupt-after: aborting the page script");
                    agave_webview_interrupt(g_probeView);
                }
                continue;
            }

            if (msg.wParam == PROBE_TIMER_EVAL) {
                KillTimer(g_probeWnd, PROBE_TIMER_EVAL);

                /* agave_webview_eval pumps the control's queue, which can
                 * re-enter this message loop while the EVAL timer message is
                 * still pending; without the flag the branch re-runs and the
                 * script gets evaluated once per nesting level. */
                if (g_probeEvalDone)
                    continue;

                /* --inject: run a script asset exactly the way the shell menus
                 * do, before --keys/--eval. The queue is FIFO, so waiting one
                 * tick is enough for the injected script to have run. */
                if (g_probeInject[0] && !g_probeInjectDone) {
                    g_probeInjectDone = 1;
                    probePrint("[probe]", "injecting script");
                    /* inspect.js resolves to the copy embedded in the exe; any
                     * other name is a file next to the exe or in the cwd. */
                    injectScriptFile(g_probeView, g_probeInject, NULL);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 250, NULL);
                    continue;
                }

                /* --click x,y: one real click per tick before --keys/--eval.
                 * Posted to the control window so it takes the same WndProc ->
                 * engine command path as a user's click (that is what opens a
                 * <select> menu: mousedown on the control). */
                if (g_probeClickIndex < g_probeClickCount) {
                    HWND target = (HWND)agave_webview_hwnd(g_probeView);
                    int cx = g_probeClickList[g_probeClickIndex][0];
                    int cy = g_probeClickList[g_probeClickIndex][1];

                    ++g_probeClickIndex;
                    probeModifierDown();
                    if (target) {
                        LPARAM at = MAKELPARAM(cx, cy);

                        agbLog(AGB_LOG_PROBE, "click %d,%d", cx, cy);
                        PostMessageA(target, WM_MOUSEMOVE, 0, at);
                        PostMessageA(target, WM_LBUTTONDOWN, MK_LBUTTON, at);
                        PostMessageA(target, WM_LBUTTONUP, 0, at);
                    }
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --drag x1,y1,x2,y2: press, move in steps, release. The
                 * intermediate moves matter: the engine only extends the
                 * selection on a mouse move, and a mouse-up handler without a
                 * preceding move leaves a caret (no range). */
                if (g_probeDragIndex < g_probeDragCount) {
                    HWND target = (HWND)agave_webview_hwnd(g_probeView);
                    int x1 = g_probeDragList[g_probeDragIndex][0];
                    int y1 = g_probeDragList[g_probeDragIndex][1];
                    int x2 = g_probeDragList[g_probeDragIndex][2];
                    int y2 = g_probeDragList[g_probeDragIndex][3];
                    int step;

                    ++g_probeDragIndex;
                    probeModifierDown();
                    if (target) {
                        agbLog(AGB_LOG_PROBE, "drag %d,%d -> %d,%d", x1, y1, x2, y2);
                        PostMessageA(target, WM_MOUSEMOVE, 0, MAKELPARAM(x1, y1));
                        PostMessageA(target, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
                        /* Eight moves: enough for the engine to cross a few
                         * nodes/glyphs, few enough that the command queue does
                         * not spend a whole frame draining them. */
                        for (step = 1; step <= 8; ++step) {
                            int mx = x1 + (x2 - x1) * step / 8;
                            int my = y1 + (y2 - y1) * step / 8;

                            PostMessageA(target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(mx, my));
                        }
                        PostMessageA(target, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
                    }
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --shift-click x,y: extend the current selection to that point,
                 * like a real Shift+click. SendMessage runs the control's window
                 * procedure synchronously, which lets the simulated Shift be set
                 * for exactly this click and cleared again afterwards. */
                if (g_probeShiftIndex < g_probeShiftCount) {
                    HWND target = (HWND)agave_webview_hwnd(g_probeView);
                    int sx = g_probeShiftList[g_probeShiftIndex][0];
                    int sy = g_probeShiftList[g_probeShiftIndex][1];

                    ++g_probeShiftIndex;
                    if (target) {
                        LPARAM at = MAKELPARAM(sx, sy);

                        agbLog(AGB_LOG_PROBE, "shift click %d,%d", sx, sy);
                        agave_webview_set_probe_modifiers(AGW_MOD_SHIFT);
                        SendMessageA(target, WM_MOUSEMOVE, 0, at);
                        SendMessageA(target, WM_LBUTTONDOWN, MK_LBUTTON, at);
                        SendMessageA(target, WM_LBUTTONUP, 0, at);
                        agave_webview_set_probe_modifiers(0);
                    }
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --editor <name>: issue an Editor::execCommand command through
                 * the control API the shell's context menu uses (Copy, SelectAll,
                 * Cut, Paste, ...). Runs after the drag/click that builds the
                 * selection, before the right click so a following
                 * --right-click sees the selection this command created. */
                if (g_probeEditor[0] && !g_probeEditorDone) {
                    g_probeEditorDone = 1;
                    agbLog(AGB_LOG_PROBE, "editor command %s", g_probeEditor);
                    if (g_probeView)
                        agave_webview_editor_command(g_probeView, g_probeEditor);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 500, NULL);
                    continue;
                }

                if (g_probeRightIndex < g_probeRightCount) {
                    HWND target = (HWND)agave_webview_hwnd(g_probeView);
                    int rx = g_probeRightList[g_probeRightIndex][0];
                    int ry = g_probeRightList[g_probeRightIndex][1];

                    ++g_probeRightIndex;
                    probeModifierDown();
                    if (target) {
                        LPARAM at = MAKELPARAM(rx, ry);

                        agbLog(AGB_LOG_PROBE, "right click %d,%d", rx, ry);
                        PostMessageA(target, WM_MOUSEMOVE, 0, at);
                        PostMessageA(target, WM_RBUTTONDOWN, MK_RBUTTON, at);
                        PostMessageA(target, WM_RBUTTONUP, 0, at);
                    }
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --keys / --ime run before the eval: one key per tick so the
                 * character of a key is processed before the next key, then a
                 * short pause for the last one, then the eval. */
                if ((g_probeKeys[0] || g_probeIme[0]) && !g_probeKeysDone) {
                    if (!g_probeKeyCount) {
                        probeParseKeys();
                        probeParseIme();
                    }
                    probeModifierDown();
                    /* --pre-eval: focus the field / dismiss a dialog before the
                     * first key. Without it a headless run can only type into
                     * whatever the page focused by itself. */
                    if (!g_probeKeyIndex && !g_probePreDone) {
                        g_probePreDone = 1;
                        agave_webview_set_focus(g_probeView);
                        if (g_probePreScript[0]) {
                            agave_webview_eval(g_probeView, g_probePreScript);
                            SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 200, NULL);
                            continue;
                        }
                    }
                    if (probeSendNextKey()) {
                        SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 60, NULL);
                        continue;
                    }
                    g_probeKeysDone = 1;
                    /* Give the engine time to drain the posted key messages
                     * before the eval reads the value. 300 ms was enough on an
                     * idle machine but flaked under load (grayscale text AA
                     * made the first paints slower): a key case would
                     * occasionally report a short/empty value. */
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 800, NULL);
                    continue;
                }

                /* --zoom <percent>: apply page zoom before --eval (order with
                 * --resize does not matter, both are settled before the eval
                 * reads the viewport). */
                if (g_probeZoom > 0 && !g_probeZoomDone) {
                    g_probeZoomDone = 1;
                    probePrint("[probe]", "applying zoom");
                    agbLog(AGB_LOG_PROBE, "zoom %d%%", g_probeZoom);
                    agave_webview_set_zoom(g_probeView, g_probeZoom);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                /* --resize WxH: resize the control once before --eval, so a run
                 * can measure what the page does when the window changes size
                 * (relayout, viewport units, media queries, resize event). The
                 * parent popup is resized too, so the control's own WM_SIZE is
                 * what drives the engine - the same path a dragged window takes,
                 * minus the visible window. */
                if (g_probeResizeW > 0 && g_probeResizeH > 0 && !g_probeResizeDone) {
                    g_probeResizeDone = 1;
                    probePrint("[probe]", "resizing control");
                    SetWindowPos(g_probeWnd, NULL, 0, 0,
                                 g_probeResizeW, g_probeResizeH,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    agave_webview_set_rect(g_probeView, 0, 0,
                                           g_probeResizeW, g_probeResizeH);
                    SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 400, NULL);
                    continue;
                }

                g_probeEvalDone = 1;

                /* --shot without --eval: capture what the page painted and
                 * leave. The window is never shown, so this is the headless
                 * screenshot path. With an eval script the capture has to wait
                 * for the script and its --settle delay (see PROBE_TIMER_SETTLE
                 * below), otherwise the shot would be the pre-eval frame. */
                if (g_probeShot[0] && !probeScriptText()[0]) {
                    /* Let one more frame run: an --eval that scrolled or mutated
                     * the DOM has not been painted yet, and the control's mirror
                     * still holds the pre-eval pixels. */
                    if (!g_probeShotSettled) {
                        g_probeShotSettled = 1;
                        g_probeEvalDone = 0;
                        SetTimer(g_probeWnd, PROBE_TIMER_EVAL, 300, NULL);
                        continue;
                    }
                    if (probeCapture(g_probeShot))
                        g_probeGotExit = 1;
                    else
                        agbLog(AGB_LOG_PROBE, "shot failed: %s", g_probeShot);
                    break;
                }

                /* Commit check first: a failed/stalled navigation still
                 * reports progress finished=1, and the eval would then run
                 * against the initial about:blank document. Require the
                 * control's current URL to carry the requested host before
                 * trusting the document. */
                {
                    char host[256] = "";
                    const char* hs = strstr(url, "://");

                    if (hs) {
                        const char* he;
                        size_t n;

                        hs += 3;
                        he = strchr(hs, '/');
                        n = he ? (size_t)(he - hs) : strlen(hs);
                        if (n >= sizeof(host))
                            n = sizeof(host) - 1;
                        memcpy(host, hs, n);
                    }

                    if (host[0]) {
                        const char* cur = agave_webview_url(g_probeView);

                        if (!cur || !strstr(cur, host)) {
                            g_probeSanityFailed = 1;
                            printf("[sanity] load never committed: document url=%s, wanted host %s\n",
                                   cur && cur[0] ? cur : "(none)", host);
                            fflush(stdout);
                            PostMessageA(g_probeWnd, WM_CLOSE, 0, 0);
                            continue;
                        }
                    }
                }

                probeModifierUp();

                if (g_probeFindText && g_probeFindText[0]) {
                    agbLog(AGB_LOG_PROBE, "find %s", g_probeFindText);
                    agave_webview_search(g_probeView, g_probeFindText, 1, 0);
                }

                probePrint("[probe]", "running eval");
                /* Sanity first: a failed navigation still reports progress
                 * finished=1, and the eval would then run against an empty
                 * document. Install probeExit() only when the document really
                 * exists; otherwise probeExit() becomes a sanity reporter and
                 * the run ends with the SANITY exit code instead of a
                 * misleading OK. */
                agave_webview_eval(g_probeView,
                    /* A failed navigation can commit a document that never got
                     * parsed. Judge it by the document itself: it must carry a
                     * URL String, be a real document tree, and have something
                     * in it. Counting element children under <body> was wrong:
                     * a page whose body holds only text is a perfectly good
                     * document and was reported as "empty". */
                    "window.__agbDocOk = ((typeof document.URL === 'string') "
                    "&& (document.URL.length > 0) "
                    "&& (document.URL !== 'about:blank') "
                    "&& (document.documentElement !== null) "
                    "&& (document.documentElement.firstChild !== null));"
                    "if (window.__agbDocOk) {"
                    " window.probeExit = function (result) {"
                    "  agavePostMessage('__agb_exit__' + (result === undefined ? '' : String(result)));"
                    " };"
                    "} else {"
                    " agavePostMessage('__agb_sanity__ document is empty (page never parsed?) url=' + document.URL);"
                    " window.probeExit = function (result) {"
                    "  agavePostMessage('__agb_sanity__ skipped eval: ' + (result === undefined ? '' : String(result)));"
                    " };"
                    "}");
                /* Tag the document with the round number: the document itself
                 * is replaced on reload, so nothing else survives to tell
                 * the rounds apart. Sized from the script: --eval-file may be
                 * an asset bundle far larger than the old 8 KiB stack buffer. */
                {
                    const char* source = probeScriptText();
                    size_t need = strlen(source) + 128;
                    char* script = (char*)malloc(need);

                    if (script) {
                        snprintf(script, need,
                                 "window.__agbRound=%d;window.__agbRounds=%d;%s",
                                 g_probeRound, g_probeTotalRounds, source);
                        agave_webview_eval(g_probeView, script);
                        free(script);
                    }
                }
                /* Eval-completion signal. The engine drains commands in order,
                 * so this marker runs right after the script returns - and it
                 * still runs when the script threw or failed to compile, which
                 * is why it is a second eval instead of a wrapper around the
                 * first one (a syntax error in the user script would take the
                 * wrapper down with it). --settle starts counting only from
                 * here: it used to be armed when the eval was posted, so a
                 * script slower than --settle had its view destroyed while it
                 * was still running (R61). */
                agave_webview_eval(g_probeView,
                    "if (typeof agavePostMessage === 'function')"
                    " agavePostMessage('__agb_evaldone__');");
                continue;
            }

            if (msg.wParam == PROBE_TIMER_SETTLE) {
                KillTimer(g_probeWnd, PROBE_TIMER_SETTLE);

                /* --reload: start the next round; its load reports through
                 * on_progress(finished) like any other load. */
                if (g_probeReloadsLeft > 0) {
                    g_probeReloadsLeft--;
                    g_probeRound++;
                    g_probeFinished = 0;
                    g_probeLoadArmed = 0;
                    g_probeLoadNoted = 0;
                    g_probeEvalDone = 0;
                    g_probePollCount = 0;
                    g_probeInjectDone = 0;
                    probePrint("[probe]", "reloading");
                    agbLog(AGB_LOG_PROBE, "reload (--reload) round=%d of %d",
                           g_probeRound, g_probeTotalRounds);

                    /* The reloaded document reports its own load through
                     * on_progress(finished), exactly like the first one: no
                     * marker or readyState poll is needed, and a stale
                     * document cannot pass for the new one because the
                     * signal comes from the engine per load. */
                    agave_webview_reload(g_probeView);
                    continue;
                }

                if (g_probeReloadMode)
                    g_probeRoundsDone = 1;

                /* --eval + --shot: the script has returned and --settle has
                 * elapsed. Give the engine one more frame before capturing:
                 * the control's mirror still holds the pre-eval pixels (R82). */
                if (g_probeShot[0]) {
                    if (!g_probeShotSettled) {
                        g_probeShotSettled = 1;
                        SetTimer(g_probeWnd, PROBE_TIMER_SETTLE, 300, NULL);
                        continue;
                    }
                    if (probeCapture(g_probeShot))
                        g_probeGotExit = 1;
                    else
                        agbLog(AGB_LOG_PROBE, "shot failed: %s", g_probeShot);
                }

                break;
            }

            if (msg.wParam == PROBE_TIMER_POLL) {
                char poll[512];

                g_probePollCount++;

                /* Give up waiting for a new document: arm the eval anyway so
                 * the round still reports what the frame holds. */
                if (g_probePollCount > 30) {
                    KillTimer(g_probeWnd, PROBE_TIMER_POLL);
                    probePrint("[probe]", "poll exhausted, arming eval anyway");
                    PostMessageA(g_probeWnd, AGW_PROBE_MSG, 0, 0);
                    continue;
                }

                snprintf(poll, sizeof(poll),
                         "var m = typeof window.__agbPreReload;"
                         "if (document.readyState === 'complete' && m !== '%s') {"
                         " agavePostMessage('__agb_loaded__' + location.href);"
                         "} else {"
                         " agavePostMessage('[poll] rs=' + document.readyState"
                         " + ' marker=' + m + ' id=' + window.__docId);"
                         "}",
                         g_probeToken);
                agave_webview_eval(g_probeView, poll);
                continue;
            }
        }

        /* A simulated Ctrl cannot change what TranslateMessage produces: it
         * would turn the posted keydown into the plain letter, so Ctrl+V
         * pasted and then left a "v" behind. A real Ctrl+letter produces a
         * control character, which WebView drops (no editing command ever
         * wants it), so skip the translation entirely while Ctrl is held. */
        if (!(g_probeModHeld && (g_probeModMask & AGW_MOD_CTRL)))
            TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_probeNewView) {
        const char* u;
        const char* t;
        int waited;

        /* The new tab loads asynchronously (another renderer under the process
         * backend): give it a moment to reach its own title before reporting,
         * so the suite can assert what the window ended up showing. The message
         * loop below is gone at this point, so pump by hand. */
        for (waited = 0; waited < 150; ++waited) {
            MSG msg;

            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            t = agave_webview_title(g_probeNewView);
            if (t && t[0])
                break;

            Sleep(20);
        }

        u = agave_webview_url(g_probeNewView);
        t = agave_webview_title(g_probeNewView);

        printf("[%9.1fms] [probe] new window url=%s title=%s\n", agbLogElapsedMs(),
               u && u[0] ? u : "(none)", t && t[0] ? t : "(none)");
        fflush(stdout);
        agave_webview_destroy(g_probeNewView);
        g_probeNewView = NULL;
    }

    {
        const char* u = agave_webview_url(g_probeView);

        printf("[%9.1fms] [probe] main url=%s\n", agbLogElapsedMs(), u && u[0] ? u : "(none)");
        fflush(stdout);
    }

    /* Never leave a synthetic modifier pressed, whatever path ended the run. */
    probeModifierUp();

    agave_webview_destroy(g_probeView);

    {
        int code;

        if (g_probeSawError)
            code = PROBE_EXIT_LOADERROR;   /* nothing was committed: say why */
        else if (g_probeSawDownload)
            code = PROBE_EXIT_OK;          /* a download IS the outcome */
        else if (g_probeSanityFailed)
            code = PROBE_EXIT_SANITY;
        else if (g_probeGotExit)
            code = PROBE_EXIT_OK;
        else if (g_probeReloadMode && g_probeRoundsDone)
            code = PROBE_EXIT_OK;   /* every reload round ran to the end */
        else if (g_probeTimedOut && !g_probeFinished)
            code = PROBE_EXIT_TIMEOUT_LOAD;
        else if (g_probeTimedOut)
            code = PROBE_EXIT_TIMEOUT_NOEXIT;
        else
            code = PROBE_EXIT_NOEXIT;

        agbLog(AGB_LOG_PROBE, "exit code %d", code);
        agave_webview_shutdown();
        fflush(stderr);
        return code;
    }
}

static void probeUsage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  minishell.exe [--backend local|process] [url]\n"
            "  --backend local|process  where the engine runs: in this process (default)\n"
            "            or one agave_render_host.exe per view (S3; the renderer must sit\n"
            "            next to minishell.exe). Also usable with --probe.\n"
            "  minishell.exe --probe <url> [--eval <js> | --eval-file <path>]\n"
            "                  [--wait <ms>] [--settle <ms>] [--timeout <ms>]\n"
            "  --eval    script to run after the load finished (default dumps outerHTML\n"
            "            via agavePostMessage; page console goes to stderr)\n"
            "            the script may call probeExit(result) to leave immediately\n"
            "  --eval-file  same as --eval, but read the script from a file: the\n"
            "            command line parser treats \" and \\ specially, so any JS\n"
            "            containing string literals belongs in a file\n"
            "  --inject <path>  run a script file before --eval (next to the exe\n"
            "            first, then the cwd); \"inspect.js\" uses the copy built\n"
            "            into the exe\n"
            "  --wait    settle time after 'load finished' before eval (default 3000)\n"
            "  --wait-dcl  arm --eval at DOMContentLoaded instead of load, and count\n"
            "            --wait from there (a mark from prelude.js; without an explicit\n"
            "            --wait it becomes 0). Starts the test before subresources are\n"
            "            done; state seen by the eval depends on how fast the page\n"
            "            finishes loading (record DCL state from page JS to be exact)\n"
            "  --settle  time after the eval script returns before exit (default 1000;\n"
            "            a script slower than settle is still allowed to finish)\n"
            "  --timeout hard cap from start (default 60000)\n"
            "  --nav-after <ms> [--nav-url <url>]  navigate while the page is busy\n"
            "  --eval-before-nav <js>  eval before the first navigate (no page yet:\n"
            "            must be reported as dropped, never silently ignored)\n"
            "  --eval-early <js>  run a script right after navigate() returns, i.e.\n"
            "            while the document swap is still in flight; it must either\n"
            "            run or be reported as dropped (\"dropped=1 kind=eval ...\")\n"
            "  --hide-after <ms> / --show-after <ms>  Page Visibility change\n"
            "            (document.hidden + visibilitychange) without a GUI tab switch\n"
            "  --shot <path>  capture what the page painted and exit; the extension\n"
            "            picks the format: .png (written directly, no conversion\n"
            "            step) or .bmp (raw 32-bit bottom-up, what pixel_check.py\n"
            "            reads); any other extension is refused. The window stays\n"
            "            hidden, so this is the headless screenshot path (no focus\n"
            "            stealing, nothing on screen). Combine it with\n"
            "            --eval/--eval-file to capture what the script changed\n"
            "            (the capture waits for the script and --settle)\n"
            "  --ui-shot <path>  render the browser chrome offscreen (no window, no\n"
            "            engine, no profile) and run layout/hit/pixel checks. Files:\n"
            "            .png or .bmp. --ui-theme light|dark|auto, --ui-dpi 72..288,\n"
            "            --ui-size WxH, --ui-menu (paint and assert the self-drawn\n"
            "            app menu, including its Bookmarks submenu), --ui-focus\n"
            "            (address pill focused: ring and fill assertions).\n"
            "            --ui-favicon <file> decodes a real icon (PNG/ICO) and\n"
            "            asserts the first tab painted it. --ui-close-slide runs\n"
            "            the close-slide geometry checks (widths frozen, next tab\n"
            "            under the pointer, relayout after the pointer leaves).\n"
            "            Exit 0 only when every check passes (UI_SHOT PASS).\n"
            "  --shot-rect <x,y,w,h>  crop the capture to a CSS-pixel rect\n"
            "            (origin at the viewport's top-left, i.e. the values\n"
            "            getBoundingClientRect() reports for an element scrolled\n"
            "            into view): --shot-rect 10,10,320,200 captures one\n"
            "            element. The file stays device pixels, so with --zoom 125\n"
            "            a 100px rect comes out 125px wide. Parts outside the\n"
            "            viewport are clamped to what is on screen\n"
            "  --keys <text>  type into the page before --eval, through the real\n"
            "            WM_KEYDOWN/WM_CHAR/WM_KEYUP path (the page must focus its\n"
            "            own control; {backspace} {enter} {tab} {left} {right}\n"
            "            {up} {down} {home} {end} {delete} {esc} name a key)\n"
            "  --click <x,y>  click at a CSS pixel of the viewport before --eval,\n"
            "            through the real WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP\n"
            "            path (what opens a <select> menu, focuses a field, ...)\n"
            "  --drag <x1,y1,x2,y2>  press, move, release before --eval, through\n"
            "            the same real mouse path (text selection, drag gestures)\n"
            "  --shift-click <x,y>  one click with Shift held (extend the selection)\n"
            "  --pre-eval <js>  short snippet run once before --keys (focus a\n"
            "            field: \"document.querySelector('input').focus()\")\n"
            "  --editor <name>  run an Editor command through the control API the\n"
            "            context menu uses (Copy, SelectAll, Cut, Paste, ...)\n"
            "  --mod ctrl|shift|alt  hold the modifier for every --click/--drag/\n"
            "            --keys (also ctrl+shift style combinations); a headless run\n"
            "            has no keyboard, so this is how Ctrl+A/C and Shift+arrows\n"
            "            are exercised\n"
            "  --ime <text>  deliver text the way an IME does: WM_IME_CHAR per\n"
            "            UTF-16 code unit (surrogate pairs included), no keydown\n"
            "  --resize <WxH>  resize the control (and its hidden parent) once\n"
            "            after the page settled, then wait 400ms so the engine can\n"
            "            relayout before --eval reads the new viewport\n"
            "  --zoom <percent>  page zoom before --eval (10..500, default 100)\n"
            "  --device-scale <percent>  display DPI the shell reports (100..500,\n"
            "            default 100). The engine paints at the real window DPI; the\n"
            "            value only feeds the settings page and the logs\n"
            "  --reload <n>  reload the page n times and run the eval after\n"
            "                every round (page lifecycle comparison)\n"
            "  --offline module fetches are served from the cache only\n"
            "            (no network; warm the cache with a normal run first)\n"
            "  --userdata <dir>  profile root for this run (cookies, localStorage,\n"
            "            module/HTTP caches, shell session). --probe defaults to\n"
            "            %%LOCALAPPDATA%%\\MiniShell\\probe so tests never touch the GUI profile\n"
            "  --new-tab [URL]   GUI only, with a shell already running on this\n"
            "            profile: open URL (or a blank tab) in a new tab and exit;\n"
            "            a bare URL already means this (Chrome's default)\n"
            "  --new-window [URL]  same, but the running shell opens a new window in\n"
            "            its own process (no second browser process is started)\n"
            "  --crash-page <report>  show a crash report in its own window (this is\n"
            "            what the crash handler starts; it reads no profile)\n"
            "\n"
            "GUI: one shell per profile root. A second launch\n"
            "hands its URL to the running shell and exits; give a different\n"
            "--userdata to run two instances side by side.\n");
}

/* ------------------------------------------------------------------------- */
/* headless UI render (--ui-shot)                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    int last_command;
    int command_count;
    int activated;
    int closed;
    int new_tabs;
} UiShotLog;

static UiShotLog g_uiShotLog;
static int g_uiChecks;
static int g_uiFails;

static void uiCheck(const char* name, int ok)
{
    ++g_uiChecks;

    if (!ok)
        ++g_uiFails;

    printf("ui-check %-24s ok=%d\n", name, ok ? 1 : 0);
    fflush(stdout);
}

static const char* uiPartName(UiPart part)
{
    switch (part) {
    case UI_PART_TAB: return "tab";
    case UI_PART_TAB_CLOSE: return "tab-close";
    case UI_PART_TAB_NEW: return "tab-new";
    case UI_PART_BACK: return "back";
    case UI_PART_FORWARD: return "forward";
    case UI_PART_RELOAD: return "reload";
    case UI_PART_GO: return "go";
    case UI_PART_MENU: return "menu";
    case UI_PART_ADDRESS: return "address";
    case UI_PART_FIND_FIELD: return "find-field";
    case UI_PART_FIND_PREV: return "find-prev";
    case UI_PART_FIND_NEXT: return "find-next";
    case UI_PART_FIND_CLOSE: return "find-close";
    case UI_PART_STATUS: return "status";
    case UI_PART_WINDOW_MIN: return "win-min";
    case UI_PART_WINDOW_MAX: return "win-max";
    case UI_PART_WINDOW_CLOSE: return "win-close";
    default: return "none";
    }
}

static void shotCommand(void* user, int id)
{
    (void)user;
    g_uiShotLog.last_command = id;
    ++g_uiShotLog.command_count;
}

static void shotActivate(void* user, int index)
{
    (void)user;
    g_uiShotLog.activated = index;
}

static void shotClose(void* user, int index)
{
    (void)user;
    g_uiShotLog.closed = index;
}

static void shotNewTab(void* user)
{
    (void)user;
    ++g_uiShotLog.new_tabs;
}

static void shotFocus(void* user, int field)
{
    (void)user;
    (void)field;
}

static int shotPartAt(UiChrome* chrome, UiRect rect)
{
    return uiChromeHitTest(chrome, rect.x + rect.w / 2, rect.y + rect.h / 2);
}

static void shotClick(UiChrome* chrome, int x, int y)
{
    UiMouseEvent event;

    event.x = x;
    event.y = y;
    event.type = UI_MOUSE_DOWN;
    uiChromeMouse(chrome, &event);
    event.type = UI_MOUSE_UP;
    uiChromeMouse(chrome, &event);
}

static unsigned shotPixel(const unsigned char* bits, int width, int height, int x, int y)
{
    const unsigned char* p = bits + (size_t)(height - 1 - y) * (size_t)width * 4 + (size_t)x * 4;

    return UI_RGB(p[2], p[1], p[0]);
}

/* Alpha of a pixel in the same bottom-up DIB. A layered surface has to keep the
 * rounded corners transparent, which is what a window region cannot do. */
static unsigned shotAlpha(const unsigned char* bits, int width, int height, int x, int y)
{
    return bits[(size_t)(height - 1 - y) * (size_t)width * 4 + (size_t)x * 4 + 3];
}

/* Pixels inside a rect that are painted with an exact glyph colour. Used to tell
 * an enabled icon (theme->icon) from a disabled one (theme->text_faint) without
 * depending on where a stroke happens to land. */
static int shotCountColor(const unsigned char* bits, int width, int height,
                          const UiRect* r, unsigned color, int tolerance)
{
    int count = 0;
    int x, y;

    for (y = r->y; y < r->y + r->h; ++y) {
        for (x = r->x; x < r->x + r->w; ++x) {
            unsigned c = shotPixel(bits, width, height, x, y);
            int dr = (int)((c >> 16) & 0xFF) - (int)((color >> 16) & 0xFF);
            int dg = (int)((c >> 8) & 0xFF) - (int)((color >> 8) & 0xFF);
            int db = (int)(c & 0xFF) - (int)(color & 0xFF);

            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;

            if (dr <= tolerance && dg <= tolerance && db <= tolerance)
                ++count;
        }
    }

    return count;
}

static int sameColor(unsigned got, unsigned want)
{
    return (got & 0x00FFFFFFu) == (want & 0x00FFFFFFu);
}

/* Squared RGB distance: antialiased arc pixels are blends, so "which of the two
 * colours is this pixel closer to" is the only robust question to ask there. */
static int colorDistance(unsigned a, unsigned b)
{
    int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
    int dg = (int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF);
    int db = (int)(a & 0xFF) - (int)(b & 0xFF);

    return dr * dr + dg * dg + db * db;
}

static int shotLuma(unsigned argb)
{
    return (int)(((argb >> 16) & 0xFF) * 30 + ((argb >> 8) & 0xFF) * 59 +
                 (argb & 0xFF) * 11) / 100;
}

static void shotFillModel(UiChromeModel* model)
{
    memset(model, 0, sizeof(*model));

    model->tabs[0].used = 1;
    strcpy(model->tabs[0].title, "MiniShell - miniblink");
    strcpy(model->tabs[0].url, "https://agave.local/");
    model->tabs[0].hung = 0;

    model->tabs[1].used = 1;
    strcpy(model->tabs[1].title, "DeepSeek");
    strcpy(model->tabs[1].url, "https://chat.deepseek.com/");

    model->tabs[2].used = 1;
    strcpy(model->tabs[2].title, "GitHub - onecoolx/agave");
    strcpy(model->tabs[2].url, "https://github.com/onecoolx/agave");

    model->tabs[3].used = 1;
    strcpy(model->tabs[3].title, "MDN Web Docs");
    strcpy(model->tabs[3].url, "https://developer.mozilla.org/");

    model->tabs[4].used = 1;
    strcpy(model->tabs[4].title, "New tab");
    strcpy(model->tabs[4].url, "about:blank");

    model->active = 1;
    model->can_back = 1;
    model->can_forward = 0;
    model->loading = 1;
    model->progress = 42;
    strcpy(model->address, "https://chat.deepseek.com/");
    strcpy(model->status, "Loading https://chat.deepseek.com/ ... 42%");
    model->zoom = 110;
    model->find_visible = 1;
    strcpy(model->find_text, "webkit");
}

/* --- --ui-menu sample ----------------------------------------------------
 * Static storage keeps the borrowed label pointers alive while the session
 * assertions below run. The hover starts on "Bookmarks" so the PNG shows an
 * open submenu next to a highlighted row, exactly the state Chrome users see
 * when they move onto a submenu parent. */
static UiMenu g_shotMenuRoot;
static UiMenu g_shotMenuBookmarks;
static UiMenu g_shotMenuHistory;
static UiMenu g_shotMenuTheme;
static UiMenuSession g_shotMenu;
static int g_shotMenuHoverItem = -1;
static int g_shotMenuThemeItem = -1;

static void shotBuildMenu(UiChrome* chrome, HDC dc, int width, int height)
{
    UiPainter painter;
    UiRect work = uiRectMake(0, 0, width, height);
    UiRect button = chrome->toolbar.menu.rect;
    const UiMenuPanel* root;
    int ax = button.x + button.w;
    int ay = button.y + button.h + uiThemeScale(&chrome->theme, 2);
    int hx;
    int hy;

    uiMenuInit(&g_shotMenuRoot);
    uiMenuInit(&g_shotMenuBookmarks);
    uiMenuInit(&g_shotMenuHistory);

    uiMenuAdd(&g_shotMenuBookmarks, "MiniShell - miniblink", NULL, 100, 0,
              UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuBookmarks, "DeepSeek", NULL, 101, 0,
              UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuBookmarks, "GitHub - onecoolx/agave", NULL, 102, 0,
              UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuBookmarks, "MDN Web Docs", NULL, 103, 0,
              UI_MENU_ICON_NONE, NULL);

    uiMenuAdd(&g_shotMenuHistory, "https://chat.deepseek.com/", NULL, 200, 0,
              UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuHistory, "https://github.com/onecoolx/agave", NULL, 201,
              0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuHistory, "https://developer.mozilla.org/", NULL, 202, 0,
              UI_MENU_ICON_NONE, NULL);

    uiMenuAdd(&g_shotMenuRoot, "Add bookmark", "Ctrl+D", 1, 0,
              UI_MENU_ICON_NONE, NULL);
    g_shotMenuHoverItem = uiMenuAdd(&g_shotMenuRoot, "Bookmarks", NULL, 0, 0,
                                    UI_MENU_ICON_NONE, &g_shotMenuBookmarks);
    uiMenuAdd(&g_shotMenuRoot, "History", NULL, 0, 0, UI_MENU_ICON_NONE,
              &g_shotMenuHistory);
    uiMenuAdd(&g_shotMenuRoot, "Downloads", "Ctrl+J", 10, 0,
              UI_MENU_ICON_DOWNLOAD, NULL);
    uiMenuAddSeparator(&g_shotMenuRoot);
    uiMenuAdd(&g_shotMenuRoot, "Zoom in", "Ctrl+=", 2, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuRoot, "Zoom out", "Ctrl+-", 3, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuRoot, "Reset zoom", "Ctrl+0", 4, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&g_shotMenuRoot);
    uiMenuAdd(&g_shotMenuRoot, "New tab", "Ctrl+T", 5, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuRoot, "Find", "Ctrl+F", 6, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuRoot, "Reload", "F5", 7, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&g_shotMenuRoot);
    uiMenuAdd(&g_shotMenuRoot, "Inspect page", "F12", 8, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAddSeparator(&g_shotMenuRoot);

    /* Theme submenu: exclusive rows with a check mark on the active one (the
     * offscreen run has no profile, so "System" is the choice). */
    uiMenuInit(&g_shotMenuTheme);
    uiMenuAdd(&g_shotMenuTheme, "System", NULL, 20, 0, UI_MENU_ICON_CHECK, NULL);
    uiMenuAdd(&g_shotMenuTheme, "Light", NULL, 21, 0, UI_MENU_ICON_NONE, NULL);
    uiMenuAdd(&g_shotMenuTheme, "Dark", NULL, 22, 0, UI_MENU_ICON_NONE, NULL);

    g_shotMenuThemeItem = uiMenuAdd(&g_shotMenuRoot, "Theme", "System", 0, 0,
                                    UI_MENU_ICON_NONE, &g_shotMenuTheme);
    uiMenuAdd(&g_shotMenuRoot, "Settings", NULL, 11, 0, UI_MENU_ICON_NONE, NULL);

    uiMenuSessionBegin(&g_shotMenu, &g_shotMenuRoot, &chrome->theme, ax, ay,
                       UI_MENU_ALIGN_RIGHT, &work);

    root = uiMenuSessionPanel(&g_shotMenu, 0);

    if (root && g_shotMenuHoverItem >= 0) {
        hx = root->rect.x + root->rect.w - uiThemeScale(&chrome->theme, 30);
        hy = (root->item_top[g_shotMenuHoverItem] +
              root->item_bottom[g_shotMenuHoverItem]) / 2;
        uiMenuSessionMove(&g_shotMenu, &chrome->theme, hx, hy);
    }

    if (uiPainterBegin(&painter, dc, &chrome->theme, width, height)) {
        uiMenuSessionPaint(&g_shotMenu, &chrome->theme, &painter, 0, 0);
        uiPainterEnd(&painter);
    }
}

static int uiShotMain(void)
{
    UiChrome chrome;
    UiChromeCallbacks callbacks;
    UiThemeKind kind = UI_THEME_LIGHT;
    BITMAPINFO info;
    UiFavicon* favicon = NULL;
    HDC screen;
    HDC memory;
    HBITMAP bitmap;
    HGDIOBJ old;
    void* bits = NULL;
    int width = g_uiShotWidth;
    int height = g_uiShotHeight;
    int stride;
    int ok = 1;
    int index = -1;
    UiPart part;

    if (!uiSpaceInit()) {
        fprintf(stderr, "[ui-shot] GDI+ startup failed\n");
        return 5;
    }

    if (!_stricmp(g_uiShotTheme, "dark"))
        kind = UI_THEME_DARK;
    else if (!_stricmp(g_uiShotTheme, "auto") || !g_uiShotTheme[0])
        kind = shellOps()->system_is_dark(NULL) ? UI_THEME_DARK : UI_THEME_LIGHT;

    memset(&g_uiShotLog, 0, sizeof(g_uiShotLog));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.command = shotCommand;
    callbacks.activate_tab = shotActivate;
    callbacks.close_tab = shotClose;
    callbacks.new_tab = shotNewTab;
    callbacks.focus_field = shotFocus;

    uiChromeInit(&chrome, kind, g_uiShotDpi, 1, &callbacks);
    shotFillModel(&chrome.model);

    /* --ui-favicon: decode a real icon file and put it on the first tab. This
     * exercises the same decoder/painter the shell uses when the engine hands
     * over a page's icon, and the pixel check below proves it reached the tab. */
    if (g_uiShotFaviconPath[0]) {
        FILE* file = fopen(g_uiShotFaviconPath, "rb");

        if (file) {
            unsigned char* bytes;
            long length;

            fseek(file, 0, SEEK_END);
            length = ftell(file);
            fseek(file, 0, SEEK_SET);

            if (length > 0 && length <= 1024 * 1024) {
                bytes = (unsigned char*)malloc((size_t)length);

                if (bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length)
                    favicon = uiFaviconDecode(bytes, (int)length);

                free(bytes);
            }
            fclose(file);
        }

        if (favicon) {
            chrome.model.tabs[0].icon = favicon;
            printf("ui-favicon decoded %dx%d from %s\n", uiFaviconWidth(favicon),
                   uiFaviconHeight(favicon), g_uiShotFaviconPath);
        } else {
            printf("ui-favicon could not decode %s\n", g_uiShotFaviconPath);
            uiCheck("tab-favicon-decoded", 0);
        }
    }

    /* Buttons take their enabled state from the model (can_back / can_forward);
     * without this sync every button drew as enabled, so the sample could never
     * show a dead arrow next to a live one. */
    uiChromeSync(&chrome);

    /* The real shell lays the chrome out at least once *before* the find bar is
     * opened (startup, resizes). That hidden layout used to memset the whole
     * find bar, disarming its buttons, so the offscreen shot (which only ever
     * saw it visible) disagreed with the window. Lay it out closed first. */
    chrome.model.find_visible = 0;
    uiChromeLayout(&chrome, width, height);
    chrome.model.find_visible = 1;

    uiChromeLayout(&chrome, width, height);

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;      /* bottom-up: probeWritePng's order */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    memory = CreateCompatibleDC(screen);
    bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, NULL, 0);

    if (!bitmap || !bits) {
        fprintf(stderr, "[ui-shot] cannot create the offscreen DIB\n");
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
        uiChromeFree(&chrome);
        return 5;
    }

    old = SelectObject(memory, bitmap);

    if (g_uiShotFocus)
        uiChromeSetFocusField(&chrome, 1);

    if (g_uiShotMenu) {
        /* The sample shot also shows Chrome's circular toolbar hover: park the
         * pointer on Reload before the first paint. */
        UiMouseEvent move;

        move.type = UI_MOUSE_MOVE;
        move.x = chrome.toolbar.reload.rect.x + chrome.toolbar.reload.rect.w / 2;
        move.y = chrome.toolbar.reload.rect.y + chrome.toolbar.reload.rect.h / 2;
        uiChromeMouse(&chrome, &move);

        /* The menu is "open", so the 3-dot button stays lit. */
        chrome.toolbar.menu.toggled = 1;
    }

    uiChromePaint(&chrome, memory);

    if (g_uiShotMenu)
        shotBuildMenu(&chrome, memory, width, height);

    GdiFlush();

    /* Enabled vs disabled glyphs: the enabled arrow must be painted with
     * theme->icon, the disabled one must not (it uses text_faint). This is the
     * "can I go back?" cue the light theme used to lose. */
    uiCheck("icon-enabled-vs-disabled",
            shotCountColor((const unsigned char*)bits, width, height,
                           &chrome.toolbar.back.rect, chrome.theme.icon, 6) > 0 &&
            shotCountColor((const unsigned char*)bits, width, height,
                           &chrome.toolbar.forward.rect, chrome.theme.icon, 6) == 0);

    if (favicon) {
        unsigned rgb = 0;
        int pixels = 0;

        if (uiFaviconPixel(favicon, uiFaviconWidth(favicon) / 2,
                           uiFaviconHeight(favicon) / 2, &rgb))
            pixels = shotCountColor((const unsigned char*)bits, width, height,
                                    &chrome.tabstrip.tab[0], rgb, 8);

        uiCheck("tab-favicon-painted", pixels > 30);
        printf("ui-favicon tab0=%d,%d %dx%d centre=#%06x pixels=%d\n",
               chrome.tabstrip.tab[0].x, chrome.tabstrip.tab[0].y,
               chrome.tabstrip.tab[0].w, chrome.tabstrip.tab[0].h, rgb, pixels);
    }

    stride = width * 4;
    {
        int format = probeShotFormat(g_uiShotPath);

        if (format == PROBE_SHOT_BMP)
            ok = probeWriteBmp(g_uiShotPath, (const unsigned char*)bits, width, height,
                               stride, 0, 0, width, height);
        else if (format == PROBE_SHOT_PNG)
            ok = probeWritePng(g_uiShotPath, (const unsigned char*)bits, width, height,
                               stride, 0, 0, width, height);
        else
            ok = 0;
    }

    uiCheck("shot-written", ok);
    printf("ui-shot file=%s size=%dx%d theme=%s dpi=%u bytes=%d\n",
           g_uiShotPath, width, height, kind == UI_THEME_DARK ? "dark" : "light",
           g_uiShotDpi, bitmap ? 1 : 0);

    /* --- geometry / hit testing ------------------------------------------- */
    part = shotPartAt(&chrome, chrome.toolbar.back.rect);
    uiCheck("hit-back", part == UI_PART_BACK);
    printf("ui-part back got=%s\n", uiPartName(part));

    part = shotPartAt(&chrome, chrome.toolbar.menu.rect);
    uiCheck("hit-menu", part == UI_PART_MENU);
    printf("ui-part menu got=%s\n", uiPartName(part));

    part = uiChromeHitTest(&chrome, chrome.toolbar.address.x + 6,
                           chrome.toolbar.address.y + chrome.toolbar.address.h / 2);
    uiCheck("hit-address", part == UI_PART_ADDRESS);
    printf("ui-part address got=%s\n", uiPartName(part));

    part = uiTabStripHit(&chrome.tabstrip,
                         chrome.tabstrip.tab[0].x + chrome.tabstrip.tab[0].w / 2,
                         chrome.tabstrip.tab[0].y + chrome.tabstrip.tab[0].h / 2,
                         &index);
    uiCheck("hit-tab", part == UI_PART_TAB && index == 0);
    uiCheck("tab-at", uiChromeTabAt(&chrome,
                                   chrome.tabstrip.tab[0].x + chrome.tabstrip.tab[0].w / 2,
                                   chrome.tabstrip.tab[0].y + chrome.tabstrip.tab[0].h / 2) == 0);

    /* A squeezed strip (less than half the natural tab width) hides the chip,
     * so the close assertions only run when the model actually has one. */
    if (chrome.tabstrip.close[1].w > 0) {
        part = shotPartAt(&chrome, chrome.tabstrip.close[1]);
        uiCheck("hit-tab-close", part == UI_PART_TAB_CLOSE);
        printf("ui-part tab-close got=%s\n", uiPartName(part));
    }

    part = shotPartAt(&chrome, chrome.tabstrip.new_tab);
    uiCheck("hit-tab-new", part == UI_PART_TAB_NEW);

    part = shotPartAt(&chrome, chrome.findbar.prev.rect);
    uiCheck("hit-find-prev", part == UI_PART_FIND_PREV);

    part = shotPartAt(&chrome, chrome.win_min.rect);
    uiCheck("hit-win-min", part == UI_PART_WINDOW_MIN);
    part = shotPartAt(&chrome, chrome.win_max.rect);
    uiCheck("hit-win-max", part == UI_PART_WINDOW_MAX);
    part = shotPartAt(&chrome, chrome.win_close.rect);
    uiCheck("hit-win-close", part == UI_PART_WINDOW_CLOSE);

    /* --- events ----------------------------------------------------------- */
    g_uiShotLog.last_command = -1;
    shotClick(&chrome, chrome.toolbar.back.rect.x + chrome.toolbar.back.rect.w / 2,
              chrome.toolbar.back.rect.y + chrome.toolbar.back.rect.h / 2);
    uiCheck("click-back", g_uiShotLog.last_command == UI_CMD_BACK);

    shotClick(&chrome, chrome.tabstrip.tab[2].x + chrome.tabstrip.tab[2].w / 2,
              chrome.tabstrip.tab[2].y + chrome.tabstrip.tab[2].h / 2);
    uiCheck("click-tab", g_uiShotLog.activated == 2);

    /* The press switches the tab; the release must not switch it again. */
    {
        UiMouseEvent press;

        press.x = chrome.tabstrip.tab[3].x + chrome.tabstrip.tab[3].w / 2;
        press.y = chrome.tabstrip.tab[3].y + chrome.tabstrip.tab[3].h / 2;

        g_uiShotLog.activated = -1;
        press.type = UI_MOUSE_DOWN;
        uiChromeMouse(&chrome, &press);
        uiCheck("tab-activates-on-press", g_uiShotLog.activated == 3);

        g_uiShotLog.activated = -1;
        press.type = UI_MOUSE_UP;
        uiChromeMouse(&chrome, &press);
        uiCheck("tab-activates-once", g_uiShotLog.activated == -1);
    }

    if (chrome.tabstrip.close[2].w > 0) {
        shotClick(&chrome, chrome.tabstrip.close[2].x + chrome.tabstrip.close[2].w / 2,
                  chrome.tabstrip.close[2].y + chrome.tabstrip.close[2].h / 2);
        uiCheck("click-tab-close", g_uiShotLog.closed == 2);
    }

    shotClick(&chrome, chrome.tabstrip.new_tab.x + chrome.tabstrip.new_tab.w / 2,
              chrome.tabstrip.new_tab.y + chrome.tabstrip.new_tab.h / 2);
    uiCheck("click-tab-new", g_uiShotLog.new_tabs == 1);

    g_uiShotLog.last_command = -1;
    shotClick(&chrome, chrome.win_max.rect.x + chrome.win_max.rect.w / 2,
              chrome.win_max.rect.y + chrome.win_max.rect.h / 2);
    uiCheck("click-win-max", g_uiShotLog.last_command == UI_CMD_MAXIMIZE);

    g_uiShotLog.last_command = -1;
    shotClick(&chrome, chrome.win_close.rect.x + chrome.win_close.rect.w / 2,
              chrome.win_close.rect.y + chrome.win_close.rect.h / 2);
    uiCheck("click-win-close", g_uiShotLog.last_command == UI_CMD_CLOSE_WINDOW);

    {
        UiMouseEvent move;

        move.type = UI_MOUSE_MOVE;
        move.x = chrome.tabstrip.tab[0].x + chrome.tabstrip.tab[0].w / 2;
        move.y = chrome.tabstrip.tab[0].y + chrome.tabstrip.tab[0].h / 2;
        uiChromeMouse(&chrome, &move);
        uiCheck("hover-tab", chrome.tabstrip.hover_tab == 0);

        move.type = UI_MOUSE_LEAVE;
        move.x = -1;
        move.y = -1;
        uiChromeMouse(&chrome, &move);
    }

    if (chrome.model.progress != 42 || chrome.model.find_visible != 1)
        uiCheck("model-retained", 0);
    else
        uiCheck("model-retained", 1);

    /* --- pixels (solid fills must match the theme exactly) ---------------- */
    {
        UiRect active = chrome.tabstrip.tab[1];

        uiCheck("pixel-strip", sameColor(shotPixel((const unsigned char*)bits, width, height, 2, 2),
                                         chrome.theme.chrome));
        uiCheck("pixel-toolbar", sameColor(shotPixel((const unsigned char*)bits, width, height,
                                                     2, chrome.theme.tabstrip_h + 2),
                                           chrome.theme.toolbar));
        uiCheck("pixel-active-tab", sameColor(shotPixel((const unsigned char*)bits, width, height,
                                                        active.x + uiThemeScale(&chrome.theme, 3),
                                                        active.y + active.h -
                                                            uiThemeScale(&chrome.theme, 8)),
                                              chrome.theme.surface));
        /* A resting inactive tab paints no pill at all: the strip shows through.
         * The tab-strip hover indexes are model indices, so 0 (the memset
         * default) means "hover the first tab" - a chrome rebuilt by the theme
         * switch painted tab 0 hovered until the next mouse move. */
        {
            UiRect first = chrome.tabstrip.tab[0];

            uiCheck("pixel-first-tab-resting",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        first.x + uiThemeScale(&chrome.theme, 3),
                                        first.y + first.h / 2),
                              chrome.theme.chrome));
        }
        /* Chrome's active tab flares out where it meets the toolbar: one pixel
         * left of the tab and one above the base is tab colour, while the same
         * one pixel left, higher up, is still strip. */
        {
            unsigned low = shotPixel((const unsigned char*)bits, width, height,
                                     active.x - uiThemeScale(&chrome.theme, 1),
                                     active.y + active.h -
                                         uiThemeScale(&chrome.theme, 1));
            unsigned high = shotPixel((const unsigned char*)bits, width, height,
                                      active.x - uiThemeScale(&chrome.theme, 1),
                                      active.y + active.h -
                                          uiThemeScale(&chrome.theme, 12));

            uiCheck("pixel-tab-flare",
                    colorDistance(low, chrome.theme.surface) <
                        colorDistance(low, chrome.theme.chrome));
            uiCheck("pixel-tab-no-outside-body",
                    sameColor(high, chrome.theme.chrome));
        }
        /* The status bar is a floating bubble now: the page keeps its full
         * height, so the bottom-left corner of the content is page, and the
         * bubble is checked in its own DIB below. */
        uiCheck("pixel-content", sameColor(shotPixel((const unsigned char*)bits, width, height,
                                                     width / 2, chrome.content.y + chrome.content.h / 2),
                                           chrome.theme.window));
        uiCheck("pixel-content-bottom",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    width / 2, chrome.content.y + chrome.content.h - 2),
                          chrome.theme.window));
        uiCheck("status-bubble-in-content",
                chrome.statusbar.rect.w > 0 &&
                chrome.statusbar.rect.y + chrome.statusbar.rect.h <=
                    chrome.content.y + chrome.content.h &&
                chrome.statusbar.rect.x >= chrome.content.x);
    }

    /* --- Chrome dark tint relationships ---------------------------------- */
    if (chrome.theme.kind == UI_THEME_DARK) {
        uiCheck("dark-strip-darker", shotLuma(chrome.theme.chrome) <
                                         shotLuma(chrome.theme.toolbar));
        uiCheck("dark-pill-darker", shotLuma(chrome.theme.field) <
                                        shotLuma(chrome.theme.toolbar));
        uiCheck("dark-hover-lighter", shotLuma(chrome.theme.tab_hover) >
                                          shotLuma(chrome.theme.surface));
        uiCheck("dark-focus-ring", (chrome.theme.focus_ring & 0xFFFFFF) == 0xA8C7FA);
    }

    /* --- self-drawn menu (--ui-menu) -------------------------------------- */
    if (g_uiShotMenu) {
        const UiMenuPanel* root = uiMenuSessionPanel(&g_shotMenu, 0);
        const UiMenuPanel* sub = NULL;
        int sub_index = -1;
        int p = -1;
        int i = -1;
        int chosen = 0;
        int guard;
        UiRect bounds;

        uiCheck("menu-root", root != NULL);
        uiCheck("menu-rows", root && g_shotMenuRoot.count >= 10 &&
                             root->menu->count == g_shotMenuRoot.count);

        if (root) {
            uiCheck("menu-width", root->rect.w >= chrome.theme.menu_min_w &&
                                  root->rect.w <= chrome.theme.menu_max_w);
            uiCheck("menu-inside", root->rect.x >= 0 && root->rect.y >= 0 &&
                                  root->rect.x + root->rect.w <= width &&
                                  root->rect.y + root->rect.h <= height);
            uiCheck("menu-anchor-right",
                    root->rect.x + root->rect.w ==
                        chrome.toolbar.menu.rect.x + chrome.toolbar.menu.rect.w);
        }

        for (i = 1; i < g_shotMenu.count; ++i) {
            if (g_shotMenu.visible[i]) {
                sub_index = i;
                break;
            }
        }

        sub = uiMenuSessionPanel(&g_shotMenu, sub_index);

        uiCheck("menu-submenu-open", uiMenuSessionVisibleCount(&g_shotMenu) == 2);
        uiCheck("menu-submenu-panel", sub && sub->parent == 0 &&
                                          sub->parent_item == g_shotMenuHoverItem);
        uiCheck("menu-submenu-inside", sub && sub->rect.x >= 0 && sub->rect.y >= 0 &&
                                           sub->rect.x + sub->rect.w <= width &&
                                           sub->rect.y + sub->rect.h <= height);
        uiCheck("menu-submenu-flipped", sub && root &&
                    sub->rect.x + sub->rect.w <= root->rect.x + uiThemeScale(&chrome.theme, 2) + 1);

        /* Hit: an item, a separator, and the gap between the two panels. */
        if (root) {
            p = -1;
            i = -1;
            uiCheck("menu-hit-item",
                    uiMenuSessionHit(&g_shotMenu, root->rect.x + 30,
                                     (root->item_top[0] + root->item_bottom[0]) / 2,
                                     &p, &i) && p == 0 && i == 0);
            p = -1;
            i = -1;
            uiCheck("menu-hit-separator",
                    uiMenuSessionHit(&g_shotMenu, root->rect.x + 30,
                                     (root->item_top[4] + root->item_bottom[4]) / 2,
                                     &p, &i) && p == 0 && i == 4 &&
                    (g_shotMenuRoot.items[4].flags & UI_MENU_ITEM_SEPARATOR));
        }

        /* Keyboard: Down starts at the first row and skips separators. */
        g_shotMenu.key_panel = -1;
        g_shotMenu.key_item = -1;
        uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_DOWN, &chosen);

        for (guard = 0; guard < 4; ++guard)
            uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_DOWN, &chosen);

        /* Down from Downloads (3) jumps the separator (4) onto Zoom in (5). */
        uiCheck("menu-key-skips-separator",
                g_shotMenu.key_panel == 0 && g_shotMenu.key_item == 5);

        /* Walk to Find (10): one more Down steps onto Reload (11) - the sample
         * menu has no disabled rows any more, the shell omits those entries
         * instead of greying them out. */
        for (guard = 0; guard < 8 && g_shotMenu.key_item != 10; ++guard)
            uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_DOWN, &chosen);

        uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_DOWN, &chosen);
        uiCheck("menu-key-steps",
                g_shotMenu.key_panel == 0 && g_shotMenu.key_item == 11);

        /* Right opens the focused submenu; Left closes it again. */
        while (g_shotMenu.key_item != g_shotMenuHoverItem)
            uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_UP, &chosen);

        uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_RIGHT, &chosen);
        uiCheck("menu-key-right", g_shotMenu.key_panel == sub_index &&
                                      g_shotMenu.key_item == 0);

        uiMenuSessionKey(&g_shotMenu, &chrome.theme, VK_LEFT, &chosen);
        uiCheck("menu-key-left", g_shotMenu.key_panel == 0 &&
                                     g_shotMenu.key_item == g_shotMenuHoverItem);

        /* Click an enabled row: it chooses and dismisses. */
        chosen = 0;
        uiCheck("menu-click-chooses",
                uiMenuSessionClick(&g_shotMenu, &chrome.theme,
                                   root->rect.x + 40,
                                   (root->item_top[5] + root->item_bottom[5]) / 2,
                                   &chosen) == 1 && chosen == 2);

        /* Clicking outside every panel cancels. */
        chosen = -1;
        uiCheck("menu-click-outside",
                uiMenuSessionClick(&g_shotMenu, &chrome.theme,
                                   root->rect.x - 40, root->rect.y - 40,
                                   &chosen) == 1 && chosen == 0);

        /* Pixel checks: the hovered row and the plain row must use the menu
         * tokens exactly (opaque fills; no AA in the sampled points). */
        if (root) {
            int hover_item = g_shotMenuHoverItem;
            int hx = root->rect.x + uiThemeScale(&chrome.theme, 6);
            int hy = (root->item_top[hover_item] + root->item_bottom[hover_item]) / 2;
            int bx = root->rect.x + root->rect.w - uiThemeScale(&chrome.theme, 7);
            int by = (root->item_top[0] + root->item_bottom[0]) / 2;

            uiCheck("pixel-menu-hover",
                    sameColor(shotPixel((const unsigned char*)bits, width, height, hx, hy),
                              chrome.theme.menu_hover));
            uiCheck("pixel-menu-bg",
                    sameColor(shotPixel((const unsigned char*)bits, width, height, bx, by),
                              chrome.theme.menu_bg));
        }

        /* Circular toolbar hover, sampled 3 px inside the circle's left edge
         * (the icon does not reach there). */
        uiCheck("pixel-hover-reload",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    chrome.toolbar.reload.rect.x + 3,
                                    chrome.toolbar.reload.rect.y +
                                        chrome.toolbar.reload.rect.h / 2),
                          chrome.theme.button_hover));

        /* --- theme submenu: exclusive rows, checked active one, choosing ----- */
        if (root) {
            const UiMenuItem* themeRow = &root->menu->items[g_shotMenuThemeItem];
            const UiMenu* themeMenu = themeRow->submenu;
            int checked = 0;
            int checkedItem = -1;
            int t;

            uiCheck("menu-theme-row", g_shotMenuThemeItem > 0 &&
                                      themeMenu == &g_shotMenuTheme &&
                                      themeRow->label && !strcmp(themeRow->label, "Theme"));

            for (t = 0; themeMenu && t < themeMenu->count; ++t) {
                if (themeMenu->items[t].icon == UI_MENU_ICON_CHECK) {
                    ++checked;
                    checkedItem = t;
                }
            }

            uiCheck("menu-theme-exclusive", themeMenu && themeMenu->count == 3 &&
                                            checked == 1 && checkedItem == 0);

            /* Hovering the row opens its panel; clicking "Dark" chooses id 22. */
            g_shotMenu.key_panel = -1;
            g_shotMenu.key_item = -1;
            uiMenuSessionMove(&g_shotMenu, &chrome.theme,
                              root->rect.x + 30,
                              (root->item_top[g_shotMenuThemeItem] +
                               root->item_bottom[g_shotMenuThemeItem]) / 2);

            {
                const UiMenuPanel* tp = NULL;
                int panel;

                for (panel = 1; panel < g_shotMenu.count; ++panel) {
                    if (g_shotMenu.visible[panel]) {
                        tp = uiMenuSessionPanel(&g_shotMenu, panel);
                        break;
                    }
                }

                uiCheck("menu-theme-opens", tp != NULL);

                if (tp) {
                    chosen = 0;
                    uiCheck("menu-theme-choose",
                            uiMenuSessionClick(&g_shotMenu, &chrome.theme,
                                               tp->rect.x + 30,
                                               (tp->item_top[2] + tp->item_bottom[2]) / 2,
                                               &chosen) == 1 && chosen == 22);
                }
            }
        }

        bounds = uiMenuSessionBounds(&g_shotMenu);
        uiCheck("menu-bounds", bounds.w > 0 && bounds.h > 0 &&
                                   bounds.x >= 0 && bounds.y >= 0);

        /* The popup host paints into a 32bppPARGB GDI+ bitmap, not an HDC.
         * Prove that path keeps alpha (opaque panel, transparent outside) so
         * the layered window really has rounded corners. */
        {
            enum { ALPHA_W = 220, ALPHA_H = 120 };
            unsigned char* buffer = (unsigned char*)calloc(
                (size_t)ALPHA_W * ALPHA_H, 4);
            GpBitmap* bitmap = NULL;
            GpGraphics* graphics = NULL;
            int ready = 0;

            if (buffer &&
                GdipCreateBitmapFromScan0(ALPHA_W, ALPHA_H, ALPHA_W * 4,
                                          PixelFormat32bppPARGB, buffer,
                                          &bitmap) == Ok &&
                GdipGetImageGraphicsContext(bitmap, &graphics) == Ok) {
                UiPainter alphaPainter;

                GdipGraphicsClear(graphics, 0);

                if (uiPainterBeginGraphics(&alphaPainter, graphics, &chrome.theme,
                                           ALPHA_W, ALPHA_H)) {
                    uiFillRoundRect(&alphaPainter, uiRectMake(10, 10, 140, 80), 8,
                                    chrome.theme.menu_bg);
                    uiText(&alphaPainter, "MiniShell", uiRectMake(20, 24, 100, 30),
                           chrome.theme.font_ui, chrome.theme.text, UI_ALIGN_LEFT);
                    uiPainterEnd(&alphaPainter);   /* deletes graphics */
                    graphics = NULL;
                    ready = 1;
                }
            }

            if (ready) {
                const unsigned char* inside = buffer + ((size_t)(10 + 6) * ALPHA_W +
                                                        (10 + 6)) * 4;
                const unsigned char* outside = buffer + ((size_t)2 * ALPHA_W + 2) * 4;
                unsigned insideColor = ((unsigned)inside[2] << 16) |
                                       ((unsigned)inside[1] << 8) | inside[0];

                uiCheck("menu-alpha-opaque", inside[3] == 255 &&
                                             insideColor ==
                                                 (chrome.theme.menu_bg & 0xFFFFFF));
                uiCheck("menu-alpha-empty", outside[3] == 0);
            } else {
                uiCheck("menu-alpha-opaque", 0);
                uiCheck("menu-alpha-empty", 0);
            }

            if (graphics)
                GdipDeleteGraphics(graphics);
            if (bitmap)
                GdipDisposeImage(bitmap);
            free(buffer);
        }
    }

    /* --- interaction states: dirty rects, hover pill, press, close glyph --- */
    {
        UiMouseEvent move;
        UiRect dirty;
        UiRect tab3 = chrome.tabstrip.tab[2];
        unsigned chip;
        int cx;
        int cy;
        int pad;

        pad = uiThemeScale(&chrome.theme, 3);

        /* Moving onto an inactive tab has to schedule a repaint, and that
         * repaint has to stay inside the tab strip: hover feedback used to be
         * painted only when some unrelated code invalidated the window. */
        uiChromeTakeDirty(&chrome, &dirty);   /* drop the shot's leftovers */
        move.type = UI_MOUSE_MOVE;
        move.x = tab3.x + tab3.w / 2;
        move.y = tab3.y + tab3.h / 2;
        uiChromeMouse(&chrome, &move);

        uiCheck("dirty-hover-tab", uiChromeTakeDirty(&chrome, &dirty) &&
                                   dirty.w > 0 && dirty.h > 0 &&
                                   dirty.y + dirty.h <= chrome.theme.tabstrip_h + 1);
        uiCheck("dirty-hover-tab-rect", dirty.x <= tab3.x &&
                                        dirty.x + dirty.w >= tab3.x + tab3.w);

        /* The separators that go away next to a hovered tab sit in the gaps, so
         * the dirty rect has to reach past both edges of the tab. */
        {
            int sepw = uiTabStripSeparatorWidth(&chrome.theme);
            int gapLeft = tab3.x - (chrome.theme.tab_gap + sepw) / 2;
            int gapRight = chrome.tabstrip.tab[3].x -   /* tab3 is index 2 */
                           (chrome.theme.tab_gap + sepw) / 2;

            uiCheck("dirty-hover-tab-seps", dirty.x <= gapLeft &&
                                            dirty.x + dirty.w >= gapRight + sepw);
        }

        /* A relayout (resize, theme switch) keeps the pointer's state: the
         * fields are model indices, so nothing has to be recomputed - they used
         * to be wiped here and only came back on the next mouse move. */
        uiChromeLayout(&chrome, width, height);
        uiCheck("layout-keeps-hover", chrome.tabstrip.hover_tab == 2);

        move.x = tab3.x + tab3.w / 2 + uiThemeScale(&chrome.theme, 2);
        uiChromeMouse(&chrome, &move);
        uiCheck("dirty-still", uiChromeTakeDirty(&chrome, &dirty) == 0);

        uiChromePaint(&chrome, memory);
        GdiFlush();

        /* The pill floats: filled just inside the top, strip at the bottom. */
        uiCheck("pixel-hover-pill",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    tab3.x + pad,
                                    tab3.y + tab3.h / 2),
                          chrome.theme.tab_hover));
        uiCheck("pixel-hover-pill-gap",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    tab3.x + tab3.w - uiThemeScale(&chrome.theme, 4),
                                    tab3.y + tab3.h - 1),
                          chrome.theme.chrome));

        /* The pill's bottom-left corner must survive the active tab next door:
         * that tab's base flare reaches into this rect, and stamping it after
         * the pill bit a notch out of it. */
        uiCheck("pixel-hover-pill-corner",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    tab3.x + uiThemeScale(&chrome.theme, 4),
                                    tab3.y + tab3.h - uiThemeScale(&chrome.theme, 8)),
                          chrome.theme.tab_hover));

        move.type = UI_MOUSE_DOWN;
        uiChromeMouse(&chrome, &move);
        uiChromePaint(&chrome, memory);
        GdiFlush();
        uiCheck("pixel-tab-pressed",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    tab3.x + pad, tab3.y + tab3.h / 2),
                          chrome.theme.tab_pressed));
        uiCheck("dirty-press",
                uiChromeTakeDirty(&chrome, &dirty) && dirty.w >= width);

        move.type = UI_MOUSE_UP;
        uiChromeMouse(&chrome, &move);

        /* Close chip: 20 px circle with a 16 px glyph. The old code left the
         * X 4x4 device pixels, so the point on the new diagonal was plain chip. */
        if (chrome.tabstrip.close[2].w > 0) {
            chip = chrome.theme.tab_close_hover;
            cx = chrome.tabstrip.close[2].x + chrome.tabstrip.close[2].w / 2;
            cy = chrome.tabstrip.close[2].y + chrome.tabstrip.close[2].h / 2;

            move.type = UI_MOUSE_MOVE;
            move.x = cx;
            move.y = cy;
            uiChromeMouse(&chrome, &move);
            uiChromePaint(&chrome, memory);
            GdiFlush();

            uiCheck("pixel-close-chip",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        cx + pad + 1, cy), chip));
            uiCheck("pixel-close-glyph",
                    !sameColor(shotPixel((const unsigned char*)bits, width, height,
                                         cx + pad, cy + pad), chip));
        }

        /* Hovering the address pill makes it one flat colour: the fill uses the
         * same token as the hover border. */
        move.type = UI_MOUSE_MOVE;
        move.x = chrome.toolbar.address.x + uiThemeScale(&chrome.theme, 5);
        move.y = chrome.toolbar.address.y + chrome.toolbar.address.h / 2;
        uiChromeMouse(&chrome, &move);
        uiChromePaint(&chrome, memory);
        GdiFlush();
        uiCheck("pixel-address-hover",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    chrome.toolbar.address.x +
                                        uiThemeScale(&chrome.theme, 5),
                                    chrome.toolbar.address.y +
                                        chrome.toolbar.address.h / 2),
                          /* focus wins over hover (--ui-focus case) */
                          chrome.focus_field == 1 ? chrome.theme.field_focus
                                                  : chrome.theme.border_strong));

        /* The Go button is part of the pill: hovering it highlights the pill, so
         * the native EDIT has to follow (it asks uiChromeFieldState for its
         * background - this is the "hovering Go brings the text background
         * back" case). */
        move.type = UI_MOUSE_MOVE;
        move.x = chrome.toolbar.go.rect.x + chrome.toolbar.go.rect.w / 2;
        move.y = chrome.toolbar.go.rect.y + chrome.toolbar.go.rect.h / 2;
        uiChromeMouse(&chrome, &move);
        uiChromePaint(&chrome, memory);
        GdiFlush();

        uiCheck("field-state-go-hover",
                uiChromeFieldState(&chrome, 1) == (chrome.focus_field == 1 ? 2 : 1));
        uiCheck("pixel-address-go-hover",
                sameColor(shotPixel((const unsigned char*)bits, width, height,
                                    chrome.toolbar.address.x +
                                        uiThemeScale(&chrome.theme, 5),
                                    chrome.toolbar.address.y +
                                        chrome.toolbar.address.h / 2),
                          chrome.focus_field == 1 ? chrome.theme.field_focus
                                                  : chrome.theme.field_hover));

        /* Park the pointer off the chrome again for the later samples. */
        move.type = UI_MOUSE_LEAVE;
        move.x = -1;
        move.y = -1;
        uiChromeMouse(&chrome, &move);
        uiChromeTakeDirty(&chrome, &dirty);
        uiChromePaint(&chrome, memory);
        GdiFlush();

        /* Cursor policy lives in WM_SETCURSOR (main.c); this is the part it
         * keys off: the two text fields must be the only IBeam parts. */
        uiCheck("cursor-ibeam-address",
                uiChromeHitTest(&chrome, chrome.toolbar.address.x + 4,
                                chrome.toolbar.address.y + 4) == UI_PART_ADDRESS);
        uiCheck("cursor-ibeam-find",
                chrome.model.find_visible &&
                uiChromeHitTest(&chrome, chrome.findbar.field.x + 4,
                                chrome.findbar.field.y + 4) == UI_PART_FIND_FIELD);

        /* --- find bar overlay (5.4) ---------------------------------------- */
        {
            const UiRect* card = &chrome.findbar.rect;
            const UiRect* findEdit = &chrome.find_edit;

            uiCheck("find-overlay-in-content",
                    chrome.model.find_visible &&
                    card->w > 0 && card->h > 0 &&
                    card->x >= chrome.content.x &&
                    card->y >= chrome.content.y &&
                    card->x + card->w <= chrome.content.x + chrome.content.w &&
                    card->y + card->h <= chrome.content.y + chrome.content.h);
            uiCheck("find-overlay-top-right",
                    card->x + card->w >= chrome.content.x + chrome.content.w -
                                        uiThemeScale(&chrome.theme, 6) - 1);
            uiCheck("find-overlay-edit-inside",
                    findEdit->x >= card->x && findEdit->y >= card->y &&
                    findEdit->x + findEdit->w <= card->x + card->w &&
                    findEdit->y + findEdit->h <= card->y + card->h);

            /* Same geometry rule as the address field: the EDIT is a box inside
             * the pill, never flush with it, so the pill's shape shows around
             * the text instead of being covered by a square block. */
            uiCheck("find-edit-inset",
                    findEdit->y > chrome.findbar.field.y &&
                    findEdit->y + findEdit->h <
                        chrome.findbar.field.y + chrome.findbar.field.h &&
                    findEdit->x > chrome.findbar.field.x &&
                    findEdit->x + findEdit->w <
                        chrome.findbar.field.x + chrome.findbar.field.w);
            uiCheck("find-overlay-below-toolbar",
                    card->y >= chrome.toolbar.rect.y + chrome.toolbar.rect.h);

            /* The three buttons must still be armed after the hidden layout:
             * they are the ones that disappeared from the real window. */
            uiCheck("find-buttons-armed",
                    chrome.findbar.prev.visible && chrome.findbar.next.visible &&
                    chrome.findbar.close.visible &&
                    chrome.findbar.prev.rect.w > 0 && chrome.findbar.next.rect.w > 0 &&
                    chrome.findbar.close.rect.w > 0);
            uiCheck("find-buttons-hit",
                    uiChromeHitTest(&chrome, chrome.findbar.prev.rect.x + 2,
                                    chrome.findbar.prev.rect.y + 2) == UI_PART_FIND_PREV &&
                    uiChromeHitTest(&chrome, chrome.findbar.next.rect.x + 2,
                                    chrome.findbar.next.rect.y + 2) == UI_PART_FIND_NEXT &&
                    uiChromeHitTest(&chrome, chrome.findbar.close.rect.x + 2,
                                    chrome.findbar.close.rect.y + 2) == UI_PART_FIND_CLOSE);

            /* Two rings that never meet: the chrome ring ends at the menu (the
             * find buttons are no longer part of it), and the find ring cycles
             * field -> prev -> next -> close -> field on its own. */
            uiChromeFocusClear(&chrome);
            uiCheck("focus-ring-separate",
                    uiChromeFocusStepFrom(&chrome, UI_PART_MENU, 1) == UI_PART_NONE);

            uiCheck("find-ring-forward",
                    uiChromeFindFocusStepFrom(&chrome, UI_PART_FIND_FIELD, 1) ==
                        UI_PART_FIND_PREV);
            uiCheck("find-ring-next", uiChromeFindFocusStep(&chrome, 1) == UI_PART_FIND_NEXT);
            uiCheck("find-ring-close", uiChromeFindFocusStep(&chrome, 1) == UI_PART_FIND_CLOSE);
            uiCheck("find-ring-wraps",
                    uiChromeFindFocusStep(&chrome, 1) == UI_PART_FIND_FIELD);
            uiCheck("find-ring-backward",
                    uiChromeFindFocusStep(&chrome, 0) == UI_PART_FIND_CLOSE);
            uiChromeFocusClear(&chrome);

            /* The card is painted into its own window, not into the chrome. */
            {
                BITMAPINFO cardInfo;
                HDC cardDc;
                HBITMAP cardBitmap;
                HGDIOBJ cardOld;
                void* cardBits = NULL;
                unsigned want;

                memset(&cardInfo, 0, sizeof(cardInfo));
                cardInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                cardInfo.bmiHeader.biWidth = card->w;
                cardInfo.bmiHeader.biHeight = card->h;   /* bottom-up: shotPixel */
                cardInfo.bmiHeader.biPlanes = 1;
                cardInfo.bmiHeader.biBitCount = 32;
                cardInfo.bmiHeader.biCompression = BI_RGB;

                cardDc = CreateCompatibleDC(screen);
                cardBitmap = CreateDIBSection(cardDc, &cardInfo, DIB_RGB_COLORS,
                                              &cardBits, NULL, 0);

                if (cardBitmap && cardBits) {
                    cardOld = SelectObject(cardDc, cardBitmap);
                    uiChromePaintFindOverlay(&chrome, cardDc, card->w, card->h);
                    GdiFlush();

                    want = chrome.focus_field == 2 ? chrome.theme.field_focus
                         : chrome.hover_part == UI_PART_FIND_FIELD ? chrome.theme.field_hover
                         : chrome.theme.field;

                    /* Chrome's shape: the card is the active-tab surface, the
                     * query pill inside it carries the field colour. */
                    uiCheck("pixel-find-card-bg",
                            sameColor(shotPixel((const unsigned char*)cardBits,
                                                card->w, card->h,
                                                card->w / 2, 3),
                                      chrome.theme.surface));
                    uiCheck("pixel-find-card",
                            sameColor(shotPixel((const unsigned char*)cardBits,
                                                card->w, card->h,
                                                chrome.findbar.field.x - card->x +
                                                    chrome.findbar.field.w / 2,
                                                chrome.findbar.field.y - card->y +
                                                    chrome.findbar.field.h / 2),
                                      want));
                    SelectObject(cardDc, cardOld);

                    /* Handy for eyeballing the bar without a GUI: a sibling PNG
                     * of the card, exactly as the overlay window gets it. */
                    if (probeShotFormat(g_uiShotPath) == PROBE_SHOT_PNG) {
                        char path[MAX_PATH];
                        int n;

                        n = _snprintf(path, sizeof(path) - 1, "%s-find.png", g_uiShotPath);
                        if (n > 0 && n < (int)sizeof(path)) {
                            path[sizeof(path) - 1] = 0;
                            probeWritePng(path, (const unsigned char*)cardBits,
                                          card->w, card->h, card->w * 4, 0, 0,
                                          card->w, card->h);
                        }
                    }
                } else {
                    uiCheck("pixel-find-card", 0);
                }

                if (cardBitmap)
                    DeleteObject(cardBitmap);
                if (cardDc)
                    DeleteDC(cardDc);
            }
        }

        /* Status bubble: painted into its own window too, so the chrome's own
         * pixels at the bottom of the content are page. */
        {
            const UiRect* bubble = &chrome.statusbar.rect;
            BITMAPINFO bubbleInfo;
            HDC bubbleDc;
            HBITMAP bubbleBitmap;
            HGDIOBJ bubbleOld;
            void* bubbleBits = NULL;

            memset(&bubbleInfo, 0, sizeof(bubbleInfo));
            bubbleInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bubbleInfo.bmiHeader.biWidth = bubble->w;
            bubbleInfo.bmiHeader.biHeight = bubble->h;   /* bottom-up: shotPixel */
            bubbleInfo.bmiHeader.biPlanes = 1;
            bubbleInfo.bmiHeader.biBitCount = 32;
            bubbleInfo.bmiHeader.biCompression = BI_RGB;

            bubbleDc = CreateCompatibleDC(screen);
            bubbleBitmap = CreateDIBSection(bubbleDc, &bubbleInfo, DIB_RGB_COLORS,
                                            &bubbleBits, NULL, 0);

            if (bubbleBitmap && bubbleBits) {
                bubbleOld = SelectObject(bubbleDc, bubbleBitmap);
                uiChromePaintStatusOverlay(&chrome, bubbleDc, bubble->w, bubble->h);
                GdiFlush();

                /* Sample the fill under the message (bottom padding of the
                 * card, away from any glyph). */
                uiCheck("pixel-status-bubble",
                        sameColor(shotPixel((const unsigned char*)bubbleBits,
                                            bubble->w, bubble->h,
                                            bubble->w / 2,
                                            bubble->h - 3),
                                  chrome.theme.surface));

                /* Layered presentation needs real alpha: the rounded corners must
                 * be transparent and the card opaque. A window region would give
                 * 255 at the corners - the stair-stepped edge this replaced. */
                uiCheck("pixel-status-alpha-corner",
                        shotAlpha((const unsigned char*)bubbleBits, bubble->w, bubble->h,
                                  0, 0) == 0 &&
                        shotAlpha((const unsigned char*)bubbleBits, bubble->w, bubble->h,
                                  bubble->w - 1, 0) == 0);
                uiCheck("pixel-status-alpha-fill",
                        shotAlpha((const unsigned char*)bubbleBits, bubble->w, bubble->h,
                                  bubble->w - uiThemeScale(&chrome.theme, 58),
                                  bubble->h / 2) == 255);
                SelectObject(bubbleDc, bubbleOld);
            } else {
                uiCheck("pixel-status-bubble", 0);
            }

            if (bubbleBitmap)
                DeleteObject(bubbleBitmap);
            if (bubbleDc)
                DeleteDC(bubbleDc);
        }

        /* Every tab carries its close chip at rest, not only when hovered, and
         * only loses it once the strip is squeezed below half the natural
         * width. Asserted as the rule so the check holds at any window size. */
        {
            int first = chrome.tabstrip.slot_of[0];

            uiCheck("tab-close-default",
                    chrome.tabstrip.tab[first].w > 0 &&
                    ((chrome.tabstrip.close[first].w > 0) ==
                     (chrome.tabstrip.tab[first].w * 2 >= chrome.theme.tab_max_w)));
            /* ...except the active tab, which keeps its chip at any width. */
            uiCheck("tab-close-active",
                    chrome.model.active >= 0 &&
                    chrome.tabstrip.close[chrome.model.active].w > 0);
        }

        /* Separators: one hairline between resting neighbours, none where a tab
         * merges with the strip (active) or opens a pill (hovered). */
        {
            int sep = uiTabStripSeparatorWidth(&chrome.theme);
            int sy = chrome.tabstrip.tab[3].y + chrome.tabstrip.tab[3].h / 2;
            int sx;

            sx = chrome.tabstrip.tab[3].x - (chrome.theme.tab_gap + sep) / 2;
            uiCheck("pixel-tab-sep",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        sx, sy), chrome.theme.tab_sep));

            /* tab 1 is active: no line on either side of it. */
            sx = chrome.tabstrip.tab[2].x - (chrome.theme.tab_gap + sep) / 2;
            uiCheck("pixel-tab-sep-active",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        sx, sy), chrome.theme.chrome));

            sx = chrome.tabstrip.tab[1].x - (chrome.theme.tab_gap + sep) / 2;
            uiCheck("pixel-tab-sep-active-left",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        sx, sy), chrome.theme.chrome));

            move.type = UI_MOUSE_MOVE;
            move.x = chrome.tabstrip.tab[3].x + chrome.tabstrip.tab[3].w / 2;
            move.y = sy;
            uiChromeMouse(&chrome, &move);
            uiChromePaint(&chrome, memory);
            GdiFlush();

            sx = chrome.tabstrip.tab[3].x - (chrome.theme.tab_gap + sep) / 2;
            uiCheck("pixel-tab-sep-hover-left",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        sx, sy), chrome.theme.chrome));

            sx = chrome.tabstrip.tab[4].x - (chrome.theme.tab_gap + sep) / 2;
            uiCheck("pixel-tab-sep-hover-right",
                    sameColor(shotPixel((const unsigned char*)bits, width, height,
                                        sx, sy), chrome.theme.chrome));

            move.type = UI_MOUSE_LEAVE;
            uiChromeMouse(&chrome, &move);
            uiChromeTakeDirty(&chrome, &dirty);
            uiChromePaint(&chrome, memory);
            GdiFlush();
        }
    }

    /* --- focused address pill (--ui-focus) -------------------------------- */
    if (g_uiShotFocus) {
        UiRect pill = chrome.toolbar.address;
        int cx = pill.x + pill.w / 2;
        int cy = pill.y + pill.h / 2;

        uiCheck("focus-field", chrome.focus_field == 1);
        uiCheck("focus-fill", sameColor(shotPixel((const unsigned char*)bits, width,
                                                  height, cx, pill.y + 6),
                                        chrome.theme.field_focus));
        uiCheck("focus-ring-left", sameColor(shotPixel((const unsigned char*)bits, width,
                                                       height, pill.x, cy),
                                             chrome.theme.focus_ring));
        uiCheck("focus-ring-top", sameColor(shotPixel((const unsigned char*)bits, width,
                                                      height, cx, pill.y),
                                            chrome.theme.focus_ring));
        uiCheck("focus-ring-token",
                chrome.theme.kind != UI_THEME_DARK ||
                    (chrome.theme.focus_ring & 0xFFFFFF) == 0xA8C7FA);
    }

    /* --- compact layout: the same model at the minimum window size -------- */
    {
        int compact_w = uiThemeScale(&chrome.theme, 700);
        int compact_h = uiThemeScale(&chrome.theme, 460);

        uiChromeLayout(&chrome, compact_w, compact_h);
        uiCheck("compact-content", chrome.content.h > uiThemeScale(&chrome.theme, 80));
        uiCheck("compact-address", chrome.toolbar.address.w >= uiThemeScale(&chrome.theme, 120));
        uiCheck("compact-menu-right", chrome.toolbar.menu.rect.x + chrome.toolbar.menu.rect.w <= compact_w);

        /* Squeezed below half the natural width: the close chip drops out (and
         * with it its hit target), the favicon stays. */
        uiCheck("compact-close-hidden",
                chrome.tabstrip.count > 0 &&
                chrome.tabstrip.close[chrome.tabstrip.slot_of[0]].w == 0);
        uiCheck("compact-close-active",
                chrome.model.active >= 0 &&
                chrome.tabstrip.close[chrome.model.active].w > 0);

        uiChromePaint(&chrome, memory);
        GdiFlush();

        if (chrome.tabstrip.count > 0) {
            const UiRect* t = &chrome.tabstrip.tab[chrome.tabstrip.slot_of[0]];
            int fx = t->x + uiThemeScale(&chrome.theme, 18);
            int fy = t->y + t->h / 2;

            uiCheck("compact-keeps-favicon",
                    !sameColor(shotPixel((const unsigned char*)bits, width, height, fx, fy),
                               chrome.theme.chrome));
        }
    }
    uiCheck("compact-back-inside", chrome.toolbar.back.rect.x >= 0 &&
                                  chrome.toolbar.back.rect.y + chrome.toolbar.back.rect.h <=
                                      chrome.toolbar.rect.y + chrome.toolbar.rect.h);
    if (chrome.tabstrip.count > 1) {
        int i;
        int overlap = 0;

        for (i = 0; i < chrome.tabstrip.count; ++i) {
            int ai = chrome.tabstrip.slot_of[i];
            int bi = chrome.tabstrip.slot_of[(i + 1) % chrome.tabstrip.count];

            if (i + 1 < chrome.tabstrip.count &&
                chrome.tabstrip.tab[ai].x + chrome.tabstrip.tab[ai].w >
                    chrome.tabstrip.tab[bi].x)
                overlap = 1;
        }

        uiCheck("compact-tabs-no-overlap", !overlap);
    }

    /* --- narrow strip: minimums hold, the tail hides, "+" does not move ----- */
    {
        int narrow_w = uiThemeScale(&chrome.theme, 330);
        int gap = chrome.theme.tab_gap;
        int item = chrome.theme.tab_min_w + gap;   /* one tab at its minimum */
        int extra = chrome.theme.tab_active_min_w - chrome.theme.tab_min_w;
        int avail;
        int i;
        int fit = 1;
        int bg_min = narrow_w;
        int full_icon = uiThemeScale(&chrome.theme, 16);
        int close_w = uiThemeScale(&chrome.theme, 24);

        uiChromeLayout(&chrome, narrow_w, uiThemeScale(&chrome.theme, 460));

        avail = chrome.tabstrip.rect.w - chrome.theme.strip_pad * 2 -
                chrome.theme.button_w * 2 - gap * 2 - chrome.tabstrip.right_reserved;

        /* Every laid-out tab meets its minimum and stays left of the "+"; the
         * tabs that do not fit are hidden instead of squeezed. */
        for (i = 0; i < chrome.tabstrip.count; ++i) {
            int index = chrome.tabstrip.slot_of[i];
            const UiRect* tab = &chrome.tabstrip.tab[index];

            if (tab->w <= 0 || tab->x + tab->w > chrome.tabstrip.new_tab.x - gap)
                fit = 0;
            if (index != chrome.model.active && tab->w < bg_min)
                bg_min = tab->w;
        }

        uiCheck("tab-shrink-fits", fit);
        uiCheck("tab-active-min",
                chrome.tabstrip.tab[chrome.model.active].w >=
                    chrome.theme.tab_active_min_w);
        uiCheck("tab-bg-min", bg_min >= chrome.theme.tab_min_w);
        uiCheck("tab-active-wider",
                chrome.tabstrip.tab[chrome.model.active].w > bg_min);
        uiCheck("tab-plus-inside",
                chrome.tabstrip.new_tab.x + chrome.tabstrip.new_tab.w <= narrow_w);

        /* Capacity is derived from the minimums: what fits fits, one more would
         * break the minimums (that is when hiding starts). */
        uiCheck("tab-capacity",
                chrome.tabstrip.count * item + extra <= avail &&
                (chrome.tabstrip.count + 1) * item + extra > avail);
        uiCheck("tab-hidden-tail",
                chrome.tabstrip.count > 0 &&
                chrome.tabstrip.count < chrome.tabstrip.total);

        fit = 1;
        for (i = chrome.tabstrip.count; i < chrome.tabstrip.total; ++i) {
            if (chrome.tabstrip.tab[chrome.tabstrip.slot_of[i]].w != 0 ||
                chrome.tabstrip.close[chrome.tabstrip.slot_of[i]].w != 0)
                fit = 0;
        }
        uiCheck("tab-hidden-no-rect", fit);

        /* The point of the exact fill: while compressed the "+" right edge is
         * the strip's right edge minus its padding and the window buttons. */
        uiCheck("tab-plus-right-fixed",
                chrome.tabstrip.new_tab.x + chrome.tabstrip.new_tab.w ==
                    narrow_w - chrome.theme.strip_pad -
                        chrome.tabstrip.right_reserved);

        /* The icon never shrinks; only the active tab may trade it for its
         * close chip at the 36 px minimum. */
        uiCheck("icon-never-shrinks",
                uiTabFaviconSize(&chrome.theme, chrome.theme.tab_min_w, 0, 0) ==
                        full_icon &&
                    uiTabFaviconSize(&chrome.theme, chrome.theme.tab_max_w,
                                     close_w, 0) == full_icon);
        uiCheck("active-drops-icon-for-chip",
                uiTabFaviconSize(&chrome.theme, chrome.theme.tab_active_min_w,
                                 close_w, 1) == 0 &&
                    uiTabFaviconSize(&chrome.theme, chrome.theme.tab_active_min_w,
                                     0, 1) == full_icon);

        /* All 128 slots: the tail is hidden and nothing overlaps the "+". */
        for (i = 5; i < UI_MAX_TABS; ++i) {
            chrome.model.tabs[i].used = 1;
            strcpy(chrome.model.tabs[i].title, "Tab");
            strcpy(chrome.model.tabs[i].url, "https://example.com/");
        }

        uiChromeLayout(&chrome, uiThemeScale(&chrome.theme, 700),
                       uiThemeScale(&chrome.theme, 460));

        uiCheck("slots-128", UI_MAX_TABS == 128 &&
                             chrome.tabstrip.total == UI_MAX_TABS);
        uiCheck("slots-128-hidden", chrome.tabstrip.count < chrome.tabstrip.total);

        fit = 1;
        for (i = 0; i < chrome.tabstrip.count; ++i) {
            const UiRect* tab = &chrome.tabstrip.tab[chrome.tabstrip.slot_of[i]];

            if (tab->w <= 0 ||
                tab->x + tab->w > chrome.tabstrip.new_tab.x - chrome.theme.tab_gap)
                fit = 0;
        }
        uiCheck("slots-128-fit", fit);

        /* Same strip width, 3 tabs instead of 128: the "+" must not have moved
         * (this is the "clicking + repeatedly hits the same spot" requirement). */
        {
            int right_128 = chrome.tabstrip.new_tab.x + chrome.tabstrip.new_tab.w;
            int right_3;

            for (i = 3; i < UI_MAX_TABS; ++i)
                chrome.model.tabs[i].used = 0;

            uiChromeLayout(&chrome, uiThemeScale(&chrome.theme, 700),
                           uiThemeScale(&chrome.theme, 460));

            right_3 = chrome.tabstrip.new_tab.x + chrome.tabstrip.new_tab.w;

            uiCheck("tab-plus-stable", right_128 == right_3);

            for (i = 3; i < 5; ++i)
                chrome.model.tabs[i].used = 1;
        }

        /* A minimized window reports a 1 px client (the shell logs 199x1), so
         * `available` goes negative. The layout must degrade to "one 1 px tab"
         * instead of dividing by a zero background-tab count (that used to
         * crash with 0xC0000094 when minimizing). */
        uiChromeLayout(&chrome, uiThemeScale(&chrome.theme, 199),
                       uiThemeScale(&chrome.theme, 1));
        uiCheck("degenerate-minimized-width",
                chrome.tabstrip.count >= 1 &&
                    chrome.tabstrip.tab[chrome.tabstrip.slot_of[0]].w >= 1 &&
                    chrome.tabstrip.tab[chrome.tabstrip.slot_of[0]].x >= 0);

        /* Restore the compact geometry the DPI block expects to start from. */
        uiChromeLayout(&chrome, uiThemeScale(&chrome.theme, 700),
                       uiThemeScale(&chrome.theme, 460));
    }

    /* --- 150% DPI: metrics scale, content still lands below the toolbar --- */
    uiChromeSetDpi(&chrome, 144);
    uiChromeLayout(&chrome, 1350, 1050);
    uiCheck("dpi-tabstrip", chrome.theme.tabstrip_h == 60);
    uiCheck("dpi-toolbar", chrome.theme.toolbar_h == 66);
    /* The find bar is an overlay: it must not push the page down (it used to add
     * its own row, which is what "searchbar 不要占页面空间" was about). */
    uiCheck("dpi-content-top", chrome.content.y == 60 + 66);
    uiCheck("dpi-menu-inside", chrome.toolbar.menu.rect.x + chrome.toolbar.menu.rect.w <= 1350);

    /* --- close slide (--ui-close-slide) ----------------------------------- */
    if (g_uiShotSlide) {
        int x0, w0, x1, w1, c1, x2_old, x3_old;

        shotFillModel(&chrome.model);
        chrome.model.find_visible = 0;

        /* The DPI block above left the theme at 150%; the slide is about the
         * strip's own geometry, so measure it at 100%. */
        uiChromeSetDpi(&chrome, 96);

        /* Narrow: the active tab's minimum is wider than a background tab. The
         * promoted tab must inherit the active width, or its close chip would
         * not land where the clicked one was. */
        uiChromeLayout(&chrome, 340, 300);

        uiCheck("close-slide-active-wider",
                chrome.tabstrip.tab[1].w > chrome.tabstrip.tab[0].w);

        x0 = chrome.tabstrip.tab[0].x;
        w0 = chrome.tabstrip.tab[0].w;
        x1 = chrome.tabstrip.tab[1].x;
        w1 = chrome.tabstrip.tab[1].w;
        c1 = chrome.tabstrip.close[1].x;
        x2_old = chrome.tabstrip.tab[2].x;
        x3_old = chrome.tabstrip.tab[3].x;

        uiChromeBeginCloseSlide(&chrome, 1, 2);
        chrome.model.tabs[1].used = 0;
        chrome.model.active = 2;
        uiChromeLayout(&chrome, 340, 300);

        uiCheck("close-slide-armed", uiChromeCloseSliding(&chrome) == 1);
        uiCheck("close-slide-next-x", chrome.tabstrip.tab[2].x == x1);
        uiCheck("close-slide-next-w", chrome.tabstrip.tab[2].w == w1);
        uiCheck("close-slide-next-chip", chrome.tabstrip.close[2].x == c1);
        uiCheck("close-slide-left-kept",
                chrome.tabstrip.tab[0].x == x0 && chrome.tabstrip.tab[0].w == w0);
        uiCheck("close-slide-right-shift",
                chrome.tabstrip.tab[3].x == x2_old &&
                chrome.tabstrip.tab[4].x == x3_old);

        /* The row is one slot shorter until the pointer leaves: the "+" follows
         * the last tab and leaves the gap on the right. */
        uiCheck("close-slide-gap",
                chrome.tabstrip.new_tab.x ==
                    chrome.tabstrip.tab[4].x + chrome.tabstrip.tab[4].w +
                        2 * chrome.theme.tab_gap);

        /* Pointer left the strip: normal compression is back. */
        uiChromeEndCloseSlide(&chrome);
        uiChromeLayout(&chrome, 340, 300);
        uiCheck("close-slide-ended", !uiChromeCloseSliding(&chrome));
        uiCheck("close-slide-refilled", chrome.tabstrip.tab[0].w > w0);

        /* Background close with roomy tabs: the row slides one slot left, every
         * width (including the untouched active tab's) stays the same. */
        {
            int bx, bw, bc, x2, x3;

            uiChromeLayout(&chrome, 1100, 760);

            bx = chrome.tabstrip.tab[0].x;
            bw = chrome.tabstrip.tab[0].w;
            bc = chrome.tabstrip.close[0].x;
            x2 = chrome.tabstrip.tab[2].x;
            x3 = chrome.tabstrip.tab[3].x;

            uiChromeBeginCloseSlide(&chrome, 0, 2);
            chrome.model.tabs[0].used = 0;
            uiChromeLayout(&chrome, 1100, 760);

            uiCheck("close-slide-bg-chip",
                    chrome.tabstrip.close[2].x == bc &&
                    chrome.tabstrip.tab[2].x == bx &&
                    chrome.tabstrip.tab[2].w == bw);
            uiCheck("close-slide-bg-shift",
                    (chrome.tabstrip.tab[3].x - x2 <= 1 &&
                     chrome.tabstrip.tab[3].x - x2 >= -1) &&
                    (chrome.tabstrip.tab[4].x - x3 <= 1 &&
                     chrome.tabstrip.tab[4].x - x3 >= -1));

            uiChromeEndCloseSlide(&chrome);
        }

        /* Right-edge close while the tabs are compressed (every tab still has a
         * chip): the row grows to where the closed tab ended, so the last
         * remaining tab's chip returns to the pointer and the next close walks
         * left. */
        {
            int cursor_x, before_w;

            shotFillModel(&chrome.model);
            uiChromeLayout(&chrome, 900, 300);

            cursor_x = chrome.tabstrip.close[4].x;
            before_w = chrome.tabstrip.tab[0].w;

            uiChromeBeginCloseSlide(&chrome, 4, -1);
            chrome.model.tabs[4].used = 0;
            uiChromeLayout(&chrome, 900, 300);

            uiCheck("close-slide-expand-chip",
                    uiChromeCloseSliding(&chrome) == 1 &&
                    chrome.tabstrip.close[3].x == cursor_x &&
                    chrome.tabstrip.tab[0].w > before_w);

            uiChromeBeginCloseSlide(&chrome, 3, -1);
            chrome.model.tabs[3].used = 0;
            uiChromeLayout(&chrome, 900, 300);

            /* Three tabs cannot reach the closed tab's edge within the natural
             * width, so they stop at the natural row end - the exact layout the
             * strip falls back to when the pointer leaves. */
            uiCheck("close-slide-grow-natural",
                    uiChromeCloseSliding(&chrome) == 1 &&
                    chrome.tabstrip.tab[0].w == chrome.theme.tab_max_w);

            uiChromeEndCloseSlide(&chrome);
            uiChromeLayout(&chrome, 900, 300);

            uiCheck("close-slide-natural-no-jump",
                    chrome.tabstrip.tab[0].w == chrome.theme.tab_max_w);
        }

        /* Same rule starting in the middle of the row: the growth target is
         * the closed tab's right edge, so the row stops there instead of
         * filling the strip. */
        {
            int cursor_x, target;

            shotFillModel(&chrome.model);
            uiChromeLayout(&chrome, 900, 300);

            cursor_x = chrome.tabstrip.close[2].x;

            uiChromeBeginCloseSlide(&chrome, 2, 3);
            chrome.model.tabs[2].used = 0;
            uiChromeLayout(&chrome, 900, 300);
            uiCheck("close-slide-mid-chip", chrome.tabstrip.close[3].x == cursor_x);

            uiChromeBeginCloseSlide(&chrome, 3, 4);
            chrome.model.tabs[3].used = 0;
            uiChromeLayout(&chrome, 900, 300);
            uiCheck("close-slide-mid-chain", chrome.tabstrip.close[4].x == cursor_x);

            target = chrome.tabstrip.tab[4].x + chrome.tabstrip.tab[4].w;

            uiChromeBeginCloseSlide(&chrome, 4, -1);
            chrome.model.tabs[4].used = 0;
            uiChromeLayout(&chrome, 900, 300);

            uiCheck("close-slide-mid-grow",
                    uiChromeCloseSliding(&chrome) == 1 &&
                    chrome.tabstrip.close[1].x == cursor_x &&
                    chrome.tabstrip.tab[1].x + chrome.tabstrip.tab[1].w == target);

            uiChromeEndCloseSlide(&chrome);
        }

        /* Right-edge close with natural-width tabs: nothing can grow, so the
         * row keeps its widths and the freeze simply holds. */
        {
            int i, w_before[4], x_before[4];

            shotFillModel(&chrome.model);
            uiChromeLayout(&chrome, 1600, 760);

            for (i = 0; i < 4; ++i) {
                w_before[i] = chrome.tabstrip.tab[i].w;
                x_before[i] = chrome.tabstrip.tab[i].x;
            }

            uiChromeBeginCloseSlide(&chrome, 4, -1);
            chrome.model.tabs[4].used = 0;
            uiChromeLayout(&chrome, 1600, 760);

            uiCheck("close-slide-last-kept",
                    uiChromeCloseSliding(&chrome) == 1 &&
                    chrome.tabstrip.tab[0].x == x_before[0] &&
                    chrome.tabstrip.tab[3].x == x_before[3] &&
                    chrome.tabstrip.tab[3].w == w_before[3]);

            uiChromeEndCloseSlide(&chrome);
        }
    }

    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(NULL, screen);

    /* The model only borrows the icon: clear it before the icon goes away, and
     * free after the last uiChromePaint() above. Freeing it next to the pixel
     * check left the model pointing at freed GDI+ memory for the hover/press
     * repaints and crashed inside gdiplus. */
    if (favicon) {
        chrome.model.tabs[0].icon = NULL;
        uiFaviconFree(favicon);
        favicon = NULL;
    }

    uiChromeFree(&chrome);

    printf("UI_SHOT %s checks=%d fails=%d file=%s\n",
           g_uiFails ? "FAIL" : "PASS", g_uiChecks, g_uiFails, g_uiShotPath);
    fflush(stdout);

    return g_uiFails ? 5 : 0;
}

/* Parses the raw command line into argv/argc in place, honoring double quotes.
 * GetCommandLineA keeps the executable name quoting, so the old single-token
 * skipper swallowed a quoted URL argument whenever the exe path was quoted
 * (Start-Process, drag&drop, Explorer) and silently fell back to the session. */
static int parseCommandLine(char* cmdline, char** argv, int maxArgs)
{
    int argc = 0;
    char* p = cmdline;

    while (*p && argc < maxArgs) {
        while (*p == ' ' || *p == '\t')
            ++p;

        if (!*p)
            break;

        if (*p == '"') {
            ++p;
            argv[argc++] = p;

            while (*p && *p != '"')
                ++p;
        } else {
            argv[argc++] = p;

            while (*p && *p != ' ' && *p != '\t')
                ++p;
        }

        if (*p)
            *p++ = 0;
    }

    return argc;
}

/* --backend <local|process> (S3). The control reads AGB_VIEW_BACKEND with
 * getenv(), and a Windows SetEnvironmentVariable() is not guaranteed to reach
 * the CRT's copy (R153), so the switch is put in through the CRT here. */
static char g_backendArg[64] = "local";
static int  g_backendArgSet;   /* --backend was given: only then override the env */

/* Second-instance intent (Chrome's --new-tab / --new-window). A bare URL means
 * --new-tab; with no URL at all the existing window is only activated. */
static int  g_newTabArg;
static int  g_newWindowArg;

static void applyBackendArg(void)
{
    char envbuf[96];
    const char* effective = g_backendArg;

    if (g_backendArgSet) {
        snprintf(envbuf, sizeof(envbuf), "AGB_VIEW_BACKEND=%s", g_backendArg);
        putenv(envbuf);   /* the CRT's getenv is what the control reads */
    } else {
        /* No flag: honour the environment (the test suites and the launcher set
         * it). Overwriting it with the default here silently broke every
         * env-driven run. */
        const char* env = getenv("AGB_VIEW_BACKEND");

        effective = (env && env[0]) ? env : "local";
    }

    if (!strcmp(effective, "process")) {
        char exe[MAX_PATH];
        char* slash;
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));

        if (n && n < sizeof(exe)) {
            slash = strrchr(exe, '\\');
            if (slash) {
                *(slash + 1) = 0;
                snprintf(exe + strlen(exe), sizeof(exe) - strlen(exe), "agave_render_host.exe");
                if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
                    fprintf(stderr, "shell: --backend process needs agave_render_host.exe next to minishell.exe (%s)\n", exe);
                    fflush(stderr);
                    ExitProcess(2);
                }
            }
        }
    }
}

/* Command line before any window exists: a --probe run must never create the
 * browser window. It used to be created and shown up front, so every headless
 * capture flashed a full browser and left it on screen for the whole run
 * (probeRun drives its own message loop on its own hidden window). */
static void parseShellArgv(void)
{
    static char cmdlineBuf[8192];
    char* argv[40];
    int argc = 0;
    const char* urlArg = NULL;
    const char* raw = GetCommandLineA();
    int i;

    if (raw && *raw) {
        strncpy(cmdlineBuf, raw, sizeof(cmdlineBuf) - 1);
        cmdlineBuf[sizeof(cmdlineBuf) - 1] = 0;
        argc = parseCommandLine(cmdlineBuf, argv, 40);
    }

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
            strncpy(g_backendArg, argv[++i], sizeof(g_backendArg) - 1);
            g_backendArg[sizeof(g_backendArg) - 1] = 0;
            g_backendArgSet = 1;
        } else if (!strncmp(argv[i], "--backend=", 10)) {
            strncpy(g_backendArg, argv[i] + 10, sizeof(g_backendArg) - 1);
            g_backendArg[sizeof(g_backendArg) - 1] = 0;
            g_backendArgSet = 1;
        } else if (!strcmp(argv[i], "--eval-retry")) {
            g_probeEvalRetry = 1;
        } else if (!strcmp(argv[i], "--new-tab")) {
            g_newTabArg = 1;
        } else if (!strcmp(argv[i], "--new-window")) {
            g_newWindowArg = 1;
        } else if (!strcmp(argv[i], "--crash-page") && i + 1 < argc) {
            strncpy(g_crashPagePath, argv[++i], sizeof(g_crashPagePath) - 1);
            g_crashPagePath[sizeof(g_crashPagePath) - 1] = 0;
            g_crashConsoleOnly = 1;
        } else if (!strcmp(argv[i], "--probe")) {
            g_probeMode = 1;
            g_crashConsoleOnly = 1;
        } else if (!strcmp(argv[i], "--ui-shot") && i + 1 < argc) {
            strncpy(g_uiShotPath, argv[++i], sizeof(g_uiShotPath) - 1);
            g_uiShotPath[sizeof(g_uiShotPath) - 1] = 0;
            g_crashConsoleOnly = 1;
        } else if (!strcmp(argv[i], "--ui-dpi") && i + 1 < argc) {
            int dpi = atoi(argv[++i]);

            if (dpi >= 72 && dpi <= 288)
                g_uiShotDpi = (unsigned)dpi;
            else {
                fprintf(stderr, "[shell] --ui-dpi wants 72..288, got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--ui-theme") && i + 1 < argc) {
            strncpy(g_uiShotTheme, argv[++i], sizeof(g_uiShotTheme) - 1);
            g_uiShotTheme[sizeof(g_uiShotTheme) - 1] = 0;

            if (_stricmp(g_uiShotTheme, "light") && _stricmp(g_uiShotTheme, "dark") &&
                _stricmp(g_uiShotTheme, "auto")) {
                fprintf(stderr, "[shell] --ui-theme wants light, dark or auto, got %s\n",
                        g_uiShotTheme);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--ui-size") && i + 1 < argc) {
            int uw = 0, uh = 0;

            if (sscanf(argv[++i], "%dx%d", &uw, &uh) == 2 && uw >= 480 && uh >= 360) {
                g_uiShotWidth = uw;
                g_uiShotHeight = uh;
            } else {
                fprintf(stderr, "[shell] --ui-size wants WxH >= 480x360, got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--ui-menu")) {
            g_uiShotMenu = 1;
        } else if (!strcmp(argv[i], "--ui-favicon") && i + 1 < argc) {
            strncpy(g_uiShotFaviconPath, argv[++i], sizeof(g_uiShotFaviconPath) - 1);
            g_uiShotFaviconPath[sizeof(g_uiShotFaviconPath) - 1] = 0;
        } else if (!strcmp(argv[i], "--ui-focus")) {
            g_uiShotFocus = 1;
        } else if (!strcmp(argv[i], "--ui-close-slide")) {
            g_uiShotSlide = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            g_showUsage = 1;
        } else if (!strcmp(argv[i], "--eval") && i + 1 < argc) {
            /* Last one wins: an --eval after --eval-file drops the file. */
            free(g_probeScriptFile);
            g_probeScriptFile = NULL;
            strncpy(g_probeScript, argv[++i], sizeof(g_probeScript) - 1);
            g_probeScript[sizeof(g_probeScript) - 1] = 0;
        } else if (!strcmp(argv[i], "--eval-file") && i + 1 < argc) {
            /* Same as --eval, but the script comes from a file. parseCommandLine
             * treats " and \ as syntax, so an inline script holding JS string
             * literals (or a regexp) arrives mangled; a file always survives.
             * The file is kept on the heap: assets and generated probes are
             * routinely larger than the inline buffer, and a silent cut here
             * would also cut what the backend is asked to run. */
            wchar_t widePath[MAX_PATH_CHARS];
            size_t got = 0;

            utf8ToWide(argv[++i], widePath, MAX_PATH_CHARS);
            free(g_probeScriptFile);
            g_probeScriptFile = readFileUtf8(widePath, &got);

            if (!g_probeScriptFile) {
                fprintf(stderr, "[shell] --eval-file: cannot open %s\n", argv[i]);
                g_argError = 1;
                return;
            }
            agbLog(AGB_LOG_PROBE, "eval-file %s (%lu bytes)", argv[i],
                   (unsigned long)got);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            strncpy(g_probeShot, argv[++i], sizeof(g_probeShot) - 1);
            g_probeShot[sizeof(g_probeShot) - 1] = 0;
        } else if (!strcmp(argv[i], "--shot-rect") && i + 1 < argc) {
            int rx = 0, ry = 0, rw = 0, rh = 0;

            /* x,y,w,h in client pixels (origin at the viewport's top-left). */
            if (sscanf(argv[++i], "%d,%d,%d,%d", &rx, &ry, &rw, &rh) == 4
                && rw > 0 && rh > 0) {
                g_probeShotRect[0] = rx;
                g_probeShotRect[1] = ry;
                g_probeShotRect[2] = rw;
                g_probeShotRect[3] = rh;
                g_probeShotRectSet = 1;
            } else {
                fprintf(stderr, "[shell] --shot-rect wants x,y,w,h (e.g. 10,10,320,200), got %s\n",
                        argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            strncpy(g_probeKeys, argv[++i], sizeof(g_probeKeys) - 1);
            g_probeKeys[sizeof(g_probeKeys) - 1] = 0;
        } else if (!strcmp(argv[i], "--right-click") && i + 1 < argc) {
            int rx = 0, ry = 0;

            if (sscanf(argv[++i], "%d,%d", &rx, &ry) == 2 && rx >= 0 && ry >= 0
                    && g_probeRightCount < PROBE_MAX_CLICKS) {
                g_probeRightList[g_probeRightCount][0] = rx;
                g_probeRightList[g_probeRightCount][1] = ry;
                ++g_probeRightCount;
            } else {
                fprintf(stderr, "[shell] --right-click wants x,y (e.g. 100,220), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--click") && i + 1 < argc) {
            int cx = 0, cy = 0;

            if (sscanf(argv[++i], "%d,%d", &cx, &cy) == 2 && cx >= 0 && cy >= 0
                    && g_probeClickCount < PROBE_MAX_CLICKS) {
                g_probeClickList[g_probeClickCount][0] = cx;
                g_probeClickList[g_probeClickCount][1] = cy;
                ++g_probeClickCount;
            } else {
                fprintf(stderr, "[shell] --click wants x,y (e.g. 100,220), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--shift-click") && i + 1 < argc) {
            int sx = 0, sy = 0;

            if (sscanf(argv[++i], "%d,%d", &sx, &sy) == 2 && sx >= 0 && sy >= 0
                    && g_probeShiftCount < PROBE_MAX_CLICKS) {
                g_probeShiftList[g_probeShiftCount][0] = sx;
                g_probeShiftList[g_probeShiftCount][1] = sy;
                ++g_probeShiftCount;
            } else {
                fprintf(stderr, "[shell] --shift-click wants x,y (e.g. 200,60), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--drag") && i + 1 < argc) {
            int x1 = 0, y1 = 0, x2 = 0, y2 = 0;

            if (sscanf(argv[++i], "%d,%d,%d,%d", &x1, &y1, &x2, &y2) == 4
                    && x1 >= 0 && y1 >= 0 && x2 >= 0 && y2 >= 0
                    && g_probeDragCount < PROBE_MAX_CLICKS) {
                g_probeDragList[g_probeDragCount][0] = x1;
                g_probeDragList[g_probeDragCount][1] = y1;
                g_probeDragList[g_probeDragCount][2] = x2;
                g_probeDragList[g_probeDragCount][3] = y2;
                ++g_probeDragCount;
            } else {
                fprintf(stderr, "[shell] --drag wants x1,y1,x2,y2 (e.g. 20,60,300,60), got %s\n",
                        argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--pre-eval") && i + 1 < argc) {
            strncpy(g_probePreScript, argv[++i], sizeof(g_probePreScript) - 1);
            g_probePreScript[sizeof(g_probePreScript) - 1] = 0;
        } else if (!strcmp(argv[i], "--editor") && i + 1 < argc) {
            strncpy(g_probeEditor, argv[++i], sizeof(g_probeEditor) - 1);
            g_probeEditor[sizeof(g_probeEditor) - 1] = 0;
        } else if (!strcmp(argv[i], "--mod") && i + 1 < argc) {
            strncpy(g_probeMod, argv[++i], sizeof(g_probeMod) - 1);
            g_probeMod[sizeof(g_probeMod) - 1] = 0;
            g_probeModMask = probeModifierMask();
            if (!g_probeModMask) {
                fprintf(stderr, "[shell] --mod wants ctrl/shift/alt or a combination "
                                "like ctrl+shift, got %s\n", g_probeMod);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--ime") && i + 1 < argc) {
            strncpy(g_probeIme, argv[++i], sizeof(g_probeIme) - 1);
            g_probeIme[sizeof(g_probeIme) - 1] = 0;
        } else if (!strcmp(argv[i], "--inject") && i + 1 < argc) {
            strncpy(g_probeInject, argv[++i], sizeof(g_probeInject) - 1);
            g_probeInject[sizeof(g_probeInject) - 1] = 0;
        } else if (!strcmp(argv[i], "--resize") && i + 1 < argc) {
            int rw = 0, rh = 0;

            /* WxH, the same spelling the desktop uses for window sizes. */
            if (sscanf(argv[++i], "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
                g_probeResizeW = rw;
                g_probeResizeH = rh;
            } else {
                fprintf(stderr, "[shell] --resize wants WxH (e.g. 500x400), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
            int percent = atoi(argv[++i]);

            if (percent >= 10 && percent <= 500)
                g_probeZoom = percent;
            else {
                fprintf(stderr, "[shell] --zoom wants 10..500 (percent), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--device-scale") && i + 1 < argc) {
            /* Display DPI for a headless run: the GUI gets it from
             * WM_DPICHANGED/GetDpiForWindow, a probe has no monitor to ask. */
            int percent = atoi(argv[++i]);

            if (percent >= 100 && percent <= 500)
                g_probeDeviceScale = percent;
            else {
                fprintf(stderr, "[shell] --device-scale wants 100..500 (percent), got %s\n", argv[i]);
                g_argError = 1;
                return;
            }
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            g_probeWaitMs = atoi(argv[++i]);
            g_probeWaitGiven = 1;
        } else if (!strcmp(argv[i], "--wait-dcl")) {
            /* Start --eval at DOMContentLoaded instead of after load. The
             * marker comes from prelude.js (top frame only). --wait then counts
             * from DCL; without an explicit --wait it defaults to 0, otherwise
             * the default 3 s would usually land after load anyway. */
            g_probeWaitDcl = 1;
        } else if (!strcmp(argv[i], "--settle") && i + 1 < argc) {
            g_probeSettleMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            g_probeTimeoutMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reload-every") && i + 1 < argc) {
            /* Hammer Reload like a user does: post it again every N ms even if
             * the previous load is still in flight. */
            g_probeReloadEveryMs = atoi(argv[++i]);
            if (!g_probeReloadCount) g_probeReloadCount = 10;
        } else if (!strcmp(argv[i], "--reload-count") && i + 1 < argc) {
            g_probeReloadCount = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--eval-before-nav") && i + 1 < argc) {
            g_probeBeforeNavEval = argv[++i];
        } else if (!strcmp(argv[i], "--find") && i + 1 < argc) {
            g_probeFindText = argv[++i];
        } else if (!strcmp(argv[i], "--eval-early") && i + 1 < argc) {
            /* Run a script while the navigation it was issued during is still
             * in flight: the case that used to swallow host commands. */
            g_probeEarlyEval = argv[++i];
        } else if (!strcmp(argv[i], "--nav-after") && i + 1 < argc) {
            /* Navigate N ms after the probe starts, while the page is still
             * busy: the control aborts the running script at agwPost(). */
            g_probeNavAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--hide-after") && i + 1 < argc) {
            g_probeHideAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--show-after") && i + 1 < argc) {
            g_probeShowAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--nav-url") && i + 1 < argc) {
            strncpy(g_probeNavUrl, argv[++i], sizeof(g_probeNavUrl) - 1);
            g_probeNavUrl[sizeof(g_probeNavUrl) - 1] = 0;
        } else if (!strcmp(argv[i], "--interrupt-after") && i + 1 < argc) {
            /* Abort the page's script N ms after the probe starts. A hung page
             * (or a page whose JS never returns) otherwise only ends at
             * --timeout; this exercises the same path Reload uses. */
            g_probeInterruptAfterMs = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--reload") && i + 1 < argc) {
            g_probeReloadsLeft = atoi(argv[++i]);
            g_probeReloadMode = 1;
            g_probeTotalRounds = g_probeReloadsLeft + 1;
        } else if (!strcmp(argv[i], "--userdata") && i + 1 < argc) {
            static char userdataEnv[MAX_PATH_CHARS + 32];

            /* Picked up by the shell and by the engine (cookies/localStorage/
             * module + HTTP caches). putenv, not SetEnvironmentVariableA: MinGW's
             * getenv reads the CRT environment. */
            _snprintf(userdataEnv, sizeof(userdataEnv), "AGB_USERDATA=%s", argv[++i]);
            userdataEnv[sizeof(userdataEnv) - 1] = 0;
            putenv(userdataEnv);
        } else if (!strcmp(argv[i], "--offline")) {
            /* Module fetches come from the cache only (read by the Ant runtime
             * as AGB_OFFLINE). Set before any view exists. putenv, not
             * SetEnvironmentVariable: MinGW's getenv reads the CRT _environ
             * copy, which SetEnvironmentVariableA misses. */
            putenv("AGB_OFFLINE=1");
        } else if (argv[i][0] != '-') {
            urlArg = argv[i];
        }
    }

    if (urlArg && urlArg[0]) {
        /* Same rules as typing the address: a local path becomes a file:// URL,
         * "localhost:port" stays a host instead of a search term. */
        normalizeUrl(urlArg, g_startupUrl, sizeof(g_startupUrl));
    }

    if (g_probeWaitDcl && !g_probeWaitGiven)
        g_probeWaitMs = 0;

    if (g_backendArgSet && strcmp(g_backendArg, "local") && strcmp(g_backendArg, "process")) {
        fprintf(stderr, "shell: --backend wants local or process, got '%s'\n", g_backendArg);
        g_argError = 1;
    } else {
        applyBackendArg();
    }

    if (g_probeMode && !g_startupUrl[0] && !g_showUsage &&
        !getenv("AGB_PROBE_HANDOFF")) {
        agbLog(AGB_LOG_PROBE, "missing URL");
        probeUsage();
        g_argError = 1;
    }
}

/* The engine answers `(prefers-color-scheme: dark)` from AGB_COLOR_SCHEME and
 * defaults to light. Following the Windows "app mode" here turned every page
 * with a `@media (prefers-color-scheme: dark)` block dark on a dark-mode
 * machine (css3test rendered its dark theme, which reads as "the viewport is
 * black"). The default is now light — the conventional canvas — and the OS
 * theme is only consulted when the host is asked for it explicitly with
 * `AGB_COLOR_SCHEME=auto`.
 *
 * Read once, before any view (and therefore any stylesheet) exists. Set it with
 * SetEnvironmentVariableA, not putenv: the shell and the engine DLL each link
 * their own static CRT, so a CRT-local environment would never reach the
 * engine. */
static void applySystemColorScheme(void)
{
    char setting[16];
    DWORD length = GetEnvironmentVariableA("AGB_COLOR_SCHEME", setting, sizeof(setting));
    int dark;

    /* An explicit dark/light wins; only `auto` follows the OS. */
    if (length == 0 || length >= sizeof(setting) || _stricmp(setting, "auto") != 0)
        return;

    /* The registry read lives in shell_ops.c now (the mock returns light). */
    dark = shellOps()->system_is_dark(NULL);

    SetEnvironmentVariableA("AGB_COLOR_SCHEME", dark ? "dark" : "light");
}

/* ------------------------------------------------------------------------- */
/* shell windows (one process, several windows)                               */
/* ------------------------------------------------------------------------- */

/* AGB_SHELL_HIDDEN=1 creates the windows without showing them and never steals
 * the foreground: automation needs the real UI thread and the real handoff
 * path, not a flashing window. */
static int g_shellHidden;

/* ------------------------------------------------------------------------- */
/* find bar overlay                                                          */
/* ------------------------------------------------------------------------- */

/* Chrome's find bar floats over the page and the page does not reflow. In Win32
 * the page *is* a child window, so the only way to be seen above it is another
 * child on top: this window holds the card, while the native EDIT stays a child
 * of the shell (so WM_CTLCOLOREDIT keeps working) and is raised above it.
 *
 * Every hover step, keystroke and button press repaints this window, so the card
 * is composed into a cached back buffer and blitted in one go: painting the
 * GDI+ shapes straight to the window DC flashed on every interaction. */

static HDC     g_findPaintDc;
static HBITMAP g_findPaintBitmap;
static HGDIOBJ g_findPaintOld;
static HWND    g_findPaintHwnd;
static int     g_findPaintW, g_findPaintH;

static void findPaintFree(void)
{
    if (g_findPaintDc) {
        if (g_findPaintOld)
            SelectObject(g_findPaintDc, g_findPaintOld);
        DeleteDC(g_findPaintDc);
    }
    if (g_findPaintBitmap)
        DeleteObject(g_findPaintBitmap);

    g_findPaintDc = NULL;
    g_findPaintBitmap = NULL;
    g_findPaintOld = NULL;
    g_findPaintHwnd = NULL;
    g_findPaintW = 0;
    g_findPaintH = 0;
}

static void findPaintDoubleBuffered(HWND hwnd, ShellWindow* win, HDC target, int width, int height)
{
    unsigned surface;
    HBRUSH background;
    RECT box;

    if (g_findPaintHwnd != hwnd || g_findPaintW != width || g_findPaintH != height) {
        findPaintFree();
        g_findPaintDc = CreateCompatibleDC(target);
        g_findPaintBitmap = CreateCompatibleBitmap(target, width, height);

        if (g_findPaintDc && g_findPaintBitmap) {
            g_findPaintOld = SelectObject(g_findPaintDc, g_findPaintBitmap);
            g_findPaintHwnd = hwnd;
            g_findPaintW = width;
            g_findPaintH = height;
        } else {
            findPaintFree();
        }
    }

    if (!g_findPaintDc) {
        uiChromePaintFindOverlay(&win->chrome, target, width, height);
        return;
    }

    /* The card's rounded edge is antialiased against whatever sits under it. In
     * the back buffer that is nothing, so prefill with the card colour: the
     * corners then blend into themselves instead of into black. */
    surface = win->chrome.theme.surface;
    background = CreateSolidBrush(RGB((surface >> 16) & 0xFF, (surface >> 8) & 0xFF,
                                      surface & 0xFF));

    box.left = 0;
    box.top = 0;
    box.right = width;
    box.bottom = height;

    if (background) {
        FillRect(g_findPaintDc, &box, background);
        DeleteObject(background);
    }

    uiChromePaintFindOverlay(&win->chrome, g_findPaintDc, width, height);
    BitBlt(target, 0, 0, width, height, g_findPaintDc, 0, 0, SRCCOPY);
}

static void findBarUpdateRegion(HWND bar)
{
    RECT client;
    HRGN region;
    int radius;
    ShellWindow* win = windowForHwnd(bar);

    if (!bar || !GetClientRect(bar, &client))
        return;

    if (client.right <= 0 || client.bottom <= 0)
        return;

    /* The card is drawn with an 8 DIP radius; a stadium region (corner diameter
     * = the full height) used to cut the ends into half-circles, and the right
     * end is exactly where the counter and the three buttons live. */
    radius = win ? uiThemeScale(&win->chrome.theme, 8) : 8;

    region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                radius * 2, radius * 2);

    if (region)
        SetWindowRgn(bar, region, TRUE);
}

static void invalidateFindOverlay(void)
{
    if (g_cur && g_cur->findBar)
        InvalidateRect(g_cur->findBar, NULL, FALSE);
}

/* Overlay client point -> shell client point, then the chrome's own state
 * machine: hover, pressed and the UI_CMD_FIND_* commands stay in one place. */
static void findBarMouse(HWND hwnd, ShellWindow* win, UiMouseEvent* event)
{
    UiRect dirty;
    ShellWindow* previous;

    if (!win)
        return;

    event->x += win->chrome.findbar.rect.x;
    event->y += win->chrome.findbar.rect.y;

    previous = g_cur;
    g_cur = win;
    uiChromeMouse(&win->chrome, event);

    if (uiChromeTakeDirty(&win->chrome, &dirty))
        InvalidateRect(hwnd, NULL, FALSE);

    g_cur = previous;
}

static LRESULT CALLBACK findBarProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ShellWindow* win = windowForHwnd(hwnd);

    (void)wParam;

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (win && GetClientRect(hwnd, &client) &&
            client.right > 0 && client.bottom > 0)
            findPaintDoubleBuffered(hwnd, win, hdc, client.right, client.bottom);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        findPaintFree();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        findBarUpdateRegion(hwnd);
        return 0;

    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        UiMouseEvent event;
        TRACKMOUSEEVENT track;

        event.x = (short)LOWORD(lParam);
        event.y = (short)HIWORD(lParam);

        if (message == WM_LBUTTONUP) {
            /* Deliver the click *before* dropping capture. ReleaseCapture()
             * sends WM_CAPTURECHANGED with lParam 0, and that handler sends a
             * LEAVE which clears the pressed state - so releasing first meant
             * the UP found nothing pressed and close / next / prev did nothing. */
            event.type = UI_MOUSE_UP;
            findBarMouse(hwnd, win, &event);

            if (GetCapture() == hwnd)
                ReleaseCapture();

            return 0;
        }

        if (message == WM_MOUSEMOVE) {
            event.type = UI_MOUSE_MOVE;
        } else if (message == WM_MOUSELEAVE) {
            int sx = 0;
            int sy = 0;

            /* The pointer may only have moved onto the native EDIT, which sits
             * above this window as a sibling: keep the hover instead of clearing
             * it and letting the EDIT's forwarded move set it again (the same
             * trick the shell's own leave handler uses). Clearing it here was
             * what left the pill idle-coloured while the EDIT used the hover
             * fill. */
            if (win && cursorInClient(win->hwnd, &sx, &sy)) {
                event.type = UI_MOUSE_MOVE;
                event.x = sx - win->chrome.findbar.rect.x;
                event.y = sy - win->chrome.findbar.rect.y;
            } else {
                event.type = UI_MOUSE_LEAVE;
                event.x = 0;
                event.y = 0;
            }
        } else {
            event.type = UI_MOUSE_DOWN;
            SetCapture(hwnd);
        }

        findBarMouse(hwnd, win, &event);

        if (message == WM_MOUSEMOVE) {
            memset(&track, 0, sizeof(track));
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
        }

        return 0;
    }

    /* Capture went elsewhere (the window is going away, another window grabbed
     * the mouse): a find button must not stay stuck in "pressed". */
    case WM_CAPTURECHANGED:
        if (win && (HWND)lParam != hwnd) {
            UiMouseEvent leave;

            leave.type = UI_MOUSE_LEAVE;
            leave.x = 0;
            leave.y = 0;
            findBarMouse(hwnd, win, &leave);
        }
        return 0;

    case WM_SETCURSOR: {
        POINT point;
        UiPart part;

        if (LOWORD(lParam) != HTCLIENT || !win)
            break;

        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        part = uiChromeHitTest(&win->chrome,
                               point.x + win->chrome.findbar.rect.x,
                               point.y + win->chrome.findbar.rect.y);

        /* Caret over the query, arrow over the three buttons. */
        SetCursor(LoadCursorW(NULL, (part == UI_PART_FIND_PREV ||
                                     part == UI_PART_FIND_NEXT ||
                                     part == UI_PART_FIND_CLOSE)
                                        ? IDC_ARROW : IDC_IBEAM));
        return TRUE;
    }

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static HWND createFindOverlay(ShellWindow* win)
{
    HWND bar;

    if (!win || !win->hwnd || !g_instance)
        return NULL;

    bar = CreateWindowExA(0, "MiniShellFindBar", "",
                          WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                          win->hwnd, NULL, g_instance, NULL);

    if (!bar) {
        agbLog(AGB_LOG_SHELL, "find overlay create failed: %lu",
               (unsigned long)GetLastError());
        return NULL;
    }

    return bar;
}

/* Status bubble: display only - no hover states, no native controls, so the
 * window proc is tiny.
 *
 * It is a layered popup, not a child with a window region. A region is a 1-bit
 * mask, so the rounded corners came out stair-stepped no matter how well the
 * card itself was antialiased. Here the card is drawn with GDI+ into a 32bpp DIB
 * and pushed with UpdateLayeredWindow, so the corner pixels carry real partial
 * alpha. A layered *popup* (not child) because UpdateLayeredWindow on a child is
 * refused on this system; it is owned by the shell, so it hides with it and
 * never reaches the taskbar. */
static int statusBarRender(HWND hwnd, ShellWindow* win, const UiRect* bubble)
{
    BITMAPINFO info;
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    void* bits = NULL;
    POINT origin;
    SIZE size;
    POINT src = { 0, 0 };
    BLENDFUNCTION blend;
    int ok = 0;

    if (!hwnd || !win || bubble->w <= 0 || bubble->h <= 0)
        return 0;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bubble->w;
    info.bmiHeader.biHeight = -bubble->h;        /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    if (!dc)
        return 0;

    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);

    if (bitmap && bits) {
        old = SelectObject(dc, bitmap);

        /* Start fully transparent so only the rounded card is visible. */
        memset(bits, 0, (size_t)bubble->w * (size_t)bubble->h * 4);

        uiChromePaintStatusOverlay(&win->chrome, dc, bubble->w, bubble->h);
        GdiFlush();

        origin.x = bubble->x;
        origin.y = bubble->y;
        ClientToScreen(win->hwnd, &origin);

        size.cx = bubble->w;
        size.cy = bubble->h;
        blend.BlendOp = AC_SRC_OVER;
        blend.BlendFlags = 0;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        /* The DIB has to stay selected while it is presented: deselecting it
         * first leaves the DC without a surface and UpdateLayeredWindow fails
         * with ERROR_GEN_FAILURE (31), which is how the bubble vanished. */
        ok = UpdateLayeredWindow(hwnd, NULL, &origin, &size, dc, &src, 0, &blend,
                                 ULW_ALPHA) != 0;

        SelectObject(dc, old);

        if (!ok) {
            static int logged;

            if (!logged) {
                logged = 1;
                agbLog(AGB_LOG_SHELL, "status overlay: UpdateLayeredWindow failed %lu",
                       (unsigned long)GetLastError());
            }
        }
    }

    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}

static LRESULT CALLBACK statusBarProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    (void)wParam;
    (void)lParam;

    switch (message) {
    case WM_NCHITTEST:
        /* Display only: the click belongs to the page underneath. */
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        /* Presented by UpdateLayeredWindow, never by GDI on this surface. */
        ValidateRect(hwnd, NULL);
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static void syncStatusOverlay(void)
{
    if (!g_cur || !g_hwnd)
        return;

    if (CHROME->statusbar.rect.w > 0) {
        const UiRect* bubble = &CHROME->statusbar.rect;

        if (!g_cur->statusBar)
            g_cur->statusBar = createStatusOverlay(g_cur);

        if (g_cur->statusBar) {
            if (statusBarRender(g_cur->statusBar, g_cur, bubble))
                ShowWindow(g_cur->statusBar, SW_SHOWNA);
            else
                ShowWindow(g_cur->statusBar, SW_HIDE);
        }
    } else if (g_cur->statusBar) {
        ShowWindow(g_cur->statusBar, SW_HIDE);
    }
}

static HWND createStatusOverlay(ShellWindow* win)
{
    HWND bar;

    if (!win || !win->hwnd || !g_instance)
        return NULL;

    bar = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                          "MiniShellStatusBar", "", WS_POPUP,
                          0, 0, 0, 0, win->hwnd, NULL, g_instance, NULL);

    if (!bar) {
        agbLog(AGB_LOG_SHELL, "status overlay create failed: %lu",
               (unsigned long)GetLastError());
        return NULL;
    }

    return bar;
}

/* ------------------------------------------------------------------------- */
/* tab search popup (Ctrl+Shift+A / the tab strip's v button)                */
/* ------------------------------------------------------------------------- */

static UiTabSearchItem g_tabSearchItems[UI_TABSEARCH_MAX_ITEMS];
static int g_tabSearchItemCount;

/* Snapshot of this window's open + recently closed tabs. The popup copies it,
 * so the indices the callbacks receive stay valid until the next refresh. */
static void fillTabSearchItems(void)
{
    unsigned long long now = shellOps()->tick_ms(NULL);
    int i;

    g_tabSearchItemCount = 0;

    for (i = 0; i < MAX_TABS && g_tabSearchItemCount < UI_TABSEARCH_MAX_ITEMS; ++i) {
        UiTabSearchItem* item = &g_tabSearchItems[g_tabSearchItemCount];

        if (!g_tabs[i].used || !g_tabs[i].url[0])
            continue;

        memset(item, 0, sizeof(*item));
        strncpy(item->title, g_tabs[i].title[0] ? g_tabs[i].title : g_tabs[i].url,
                sizeof(item->title) - 1);
        strncpy(item->url, g_tabs[i].url, sizeof(item->url) - 1);
        item->kind = UI_TABSEARCH_OPEN;
        item->age_ms = (int)(now - (g_tabs[i].activeMs ? g_tabs[i].activeMs : now));
        ++g_tabSearchItemCount;
    }

    for (i = 0; i < g_closedCount && g_tabSearchItemCount < UI_TABSEARCH_MAX_ITEMS; ++i) {
        UiTabSearchItem* item = &g_tabSearchItems[g_tabSearchItemCount];

        memset(item, 0, sizeof(*item));
        strncpy(item->title, g_closed[i].title[0] ? g_closed[i].title : g_closed[i].url,
                sizeof(item->title) - 1);
        strncpy(item->url, g_closed[i].url, sizeof(item->url) - 1);
        item->kind = UI_TABSEARCH_CLOSED;
        item->age_ms = (int)(now - (g_closed[i].closedMs ? g_closed[i].closedMs : now));
        ++g_tabSearchItemCount;
    }
}

static void syncTabSearch(void)
{
    if (!uiTabSearchIsOpen() || !g_cur)
        return;

    fillTabSearchItems();
    uiTabSearchRefresh(g_tabSearchItems, g_tabSearchItemCount);
}

static void onTabSearchActivate(void* user, int index)
{
    ShellWindow* win = (ShellWindow*)user;
    UiTabSearchItem* item;
    int i;

    if (!win || index < 0 || index >= g_tabSearchItemCount)
        return;

    item = &g_tabSearchItems[index];
    g_cur = win;

    if (item->kind == UI_TABSEARCH_OPEN) {
        for (i = 0; i < MAX_TABS; ++i) {
            if (g_tabs[i].used && strcmp(g_tabs[i].url, item->url) == 0) {
                setActiveTab(i);
                break;
            }
        }
    } else {
        for (i = 0; i < g_closedCount; ++i) {
            if (strcmp(g_closed[i].url, item->url) == 0) {
                reopenClosedAt(i);
                break;
            }
        }
    }
}

static void onTabSearchClose(void* user, int index)
{
    ShellWindow* win = (ShellWindow*)user;
    UiTabSearchItem* item;
    int i;

    if (!win || index < 0 || index >= g_tabSearchItemCount)
        return;

    item = &g_tabSearchItems[index];

    if (item->kind != UI_TABSEARCH_OPEN)
        return;

    g_cur = win;

    for (i = 0; i < MAX_TABS; ++i) {
        if (g_tabs[i].used && strcmp(g_tabs[i].url, item->url) == 0) {
            closeTab(i);
            break;
        }
    }
}

static void toggleTabSearchPopup(void)
{
    UiTabSearchCallbacks callbacks;

    if (uiTabSearchIsOpen()) {
        uiTabSearchClose();
        return;
    }

    if (!g_cur || !g_hwnd)
        return;

    fillTabSearchItems();

    if (g_tabSearchItemCount <= 0) {
        agbLog(AGB_LOG_SHELL, "tab search: no tabs to list");
        return;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.user = g_cur;
    callbacks.activate = onTabSearchActivate;
    callbacks.close = onTabSearchClose;

    {
        UiRect search = CHROME->tabstrip.search;
        POINT anchor;

        anchor.x = search.x;
        anchor.y = search.y + search.h + uiThemeScale(&CHROME->theme, 4);
        ClientToScreen(g_hwnd, &anchor);

        uiTabSearchOpen(g_hwnd, &CHROME->theme, g_tabSearchItems,
                        g_tabSearchItemCount, &callbacks, anchor.x, anchor.y);
    }
}

static void registerShellWindowClass(void)
{
    static int registered;
    WNDCLASSA wc;

    if (registered)
        return;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = shellProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hIcon = LoadIconA(g_instance, MAKEINTRESOURCEA(IDI_APPICON));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MiniShellWindow";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = findBarProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;   /* painted by uiChromePaintFindOverlay */
    wc.lpszClassName = "MiniShellFindBar";
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = statusBarProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;   /* painted by uiChromePaintStatusOverlay */
    wc.lpszClassName = "MiniShellStatusBar";
    RegisterClassA(&wc);

    registered = 1;
}

/* One top-level window with its toolbar and an empty tab strip. The caller
 * decides the first tab (URL, restored session) or leaves it empty when a
 * handoff is about to add one. */
static ShellWindow* createShellWindow(const char* title)
{
    ShellWindow* win = NULL;
    int i;

    for (i = 0; i < MAX_SHELL_WINDOWS; ++i) {
        if (!g_windows[i].used) {
            win = &g_windows[i];
            break;
        }
    }

    if (!win) {
        agbLog(AGB_LOG_SHELL, "new window refused: already %d windows", MAX_SHELL_WINDOWS);
        return NULL;
    }

    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->zoom = g_prefs.zoom > 0 ? g_prefs.zoom : 100;
    ++g_shellWindows;
    g_cur = win;

    /* Chrome exists before the window so WM_CREATE/WM_SIZE during creation can
     * already layout and paint. DPI is refined with the real window below. */
    {
        UiChromeCallbacks callbacks;

        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.user = win;
        callbacks.command = onChromeCommand;
        callbacks.activate_tab = onChromeActivateTab;
        callbacks.close_tab = onChromeCloseTab;
        callbacks.new_tab = onChromeNewTab;
        callbacks.tab_search = onChromeTabSearch;
        callbacks.focus_field = onChromeFocusField;

        uiChromeInit(&win->chrome, shellThemeKind(),
                     shellOps()->dpi_for_window(NULL, NULL), 0, &callbacks);
    }

    /* AGB_SHELL_FIND=1 opens the find bar with the first layout: the bar is an
     * overlay the automation cannot reach (Ctrl+F needs a real modifier), and
     * tools/check_shell_findbar.ps1 needs exactly that state. */
    if (getenv("AGB_SHELL_FIND"))
        win->findVisible = 1;

    /* The default window size is in DIPs: on a 125% display the window has to be
     * physically larger, or the client ends up 20% smaller than the intended
     * 1100x800 - which makes every page look zoomed in next to a normal browser
     * window (the page itself is already scaled by the DPI). */
    {
        unsigned dpi = shellOps()->dpi_for_window(NULL, NULL);
        int width = 1100;
        int height = 800;

        if (dpi >= 96) {
            width = MulDiv(width, (int)dpi, 96);
            height = MulDiv(height, (int)dpi, 96);
        }

        win->hwnd = CreateWindowExA(0, "MiniShellWindow",
                                    title ? title : "MiniShell - miniblink",
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                    CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                    NULL, NULL, g_instance, win);
    }

    if (!win->hwnd) {
        agbLog(AGB_LOG_SHELL, "CreateWindowEx failed: %lu", (unsigned long)GetLastError());
        uiChromeFree(&win->chrome);
        win->used = 0;
        --g_shellWindows;
        return NULL;
    }

    g_lastActive = win;

    /* The only native controls left: two Unicode EDITs. The chrome paints
     * their pills; WM_CTLCOLOREDIT makes the fields follow the theme. */
    win->address = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, win->hwnd, (HMENU)(INT_PTR)ID_ADDRESS,
                                   g_instance, NULL);

    win->findEdit = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, win->hwnd, (HMENU)(INT_PTR)ID_FIND_EDIT,
                                    g_instance, NULL);

    ShowWindow(win->findEdit, SW_HIDE);

    if (win->address)
        SendMessageW(win->address, EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    if (win->findEdit)
        SendMessageW(win->findEdit, EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));

    /* Hover over the text has to reach the pill as well (see pillEditProc). */
    installPillEditHandler(win->address);
    installPillEditHandler(win->findEdit);

    uiChromeSetNative(&win->chrome, win->hwnd, win->address, win->findEdit);
    uiChromeSetDpi(&win->chrome, shellOps()->dpi_for_window(NULL, win->hwnd));

    g_cur = win;
    applyEditFonts();
    applyWindowFrame(win);
    /* A tab may already exist (WM_CREATE opens the first one) and was scaled
     * with the pre-window system DPI; give it this window's real DPI. */
    syncTabsDeviceScale();

    /* A first layout before any tab: the toolbar/address EMPTY state is what a
     * handoff-created window shows for a few milliseconds. */
    uiChromeLayout(&win->chrome, 1100, 760);

    agbLog(AGB_LOG_SHELL, "window ready hwnd=%p (windows=%d theme=%s dpi=%u)",
           (void*)win->hwnd, g_shellWindows,
           win->chrome.theme.kind == UI_THEME_DARK ? "dark" : "light",
           win->chrome.theme.dpi);

    return win;
}

/* ------------------------------------------------------------------------- */
/* single instance per profile: lock + handoff                               */
/* ------------------------------------------------------------------------- */
/* Chrome's model: one browser process per user-data-dir. The second launch
 * does not open a window; it hands "open this URL (in a tab / new window)" to
 * the running process and exits. The lock is a named mutex (authoritative,
 * released by the OS when the process dies); the hand-off address is a hidden
 * window whose title carries the profile hash (FindWindow finds it). */

#define SHELL_HANDOFF_CLASS "MiniShellHandoff"
#define SHELL_HANDOFF_MAGIC 0x53484F31u   /* 'SHO1' */
#define SHELL_HANDOFF_VERSION 1
#define SHELL_ACK_BYTES 4096

typedef struct {
    DWORD size;
    DWORD version;
    DWORD op;                    /* 0 = focus, 1 = new tab, 2 = new window */
    DWORD pid;                   /* sender, for logs */
    char  ackName[64];
    char  url[MAX_PATH_CHARS];
} ShellHandoff;

static char   g_profileKey[32];      /* hex hash of the normalized profile root */
static char   g_handoffTitle[64];    /* window title = "agave-shell-<key>" */
static HWND   g_handoffWnd;
static int    g_handoffReady;        /* window created; requests may run */

/* FNV-1a 64 of the normalized profile root. The root is what --userdata /
 * AGB_USERDATA names, so "X", "X\" and "X\." all map to the same instance. */
static void profileKeyInit(void)
{
    char full[MAX_PATH_CHARS];
    char normalized[MAX_PATH_CHARS];
    const char* source = g_dataDir[0] ? g_dataDir : ".";
    DWORD got;
    unsigned long long hash = 1469598103934665603ULL;
    size_t i, n;

    got = GetFullPathNameA(source, (DWORD)sizeof(full), full, NULL);

    if (!got || got >= sizeof(full))
        _snprintf(full, sizeof(full), "%s", source);

    full[sizeof(full) - 1] = 0;

    n = strlen(full);

    while (n > 3 && (full[n - 1] == '\\' || full[n - 1] == '/'))
        full[--n] = 0;

    for (i = 0; i <= n && i < sizeof(normalized); ++i)
        normalized[i] = (char)tolower((unsigned char)full[i]);

    for (i = 0; normalized[i]; ++i) {
        hash ^= (unsigned char)normalized[i];
        hash *= 1099511628211ULL;
    }

    _snprintf(g_profileKey, sizeof(g_profileKey), "%016llx", hash);
    _snprintf(g_handoffTitle, sizeof(g_handoffTitle), "agave-shell-%s", g_profileKey);

    agbLog(AGB_LOG_SHELL, "profile key=%s root=%s", g_profileKey, normalized);
}

/* The sender creates a one-shot section and passes its name; the receiver
 * writes the result there. SendMessage is synchronous, so the sender reads it
 * as soon as WM_COPYDATA is handled. */
static void handoffWriteAck(const char* section, const char* text)
{
    HANDLE map;
    char* view;

    if (!section || !section[0] || !text)
        return;

    map = OpenFileMappingA(FILE_MAP_WRITE, FALSE, section);

    if (!map)
        return;

    view = (char*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, SHELL_ACK_BYTES);

    if (view) {
        size_t n = strlen(text);

        if (n >= SHELL_ACK_BYTES)
            n = SHELL_ACK_BYTES - 1;

        memcpy(view, text, n);
        view[n] = 0;
        UnmapViewOfFile(view);
    }

    CloseHandle(map);
}

static int handoffTabCount(ShellWindow* win)
{
    int i, count = 0;

    if (!win)
        return 0;

    for (i = 0; i < MAX_TABS; ++i) {
        if (win->tabs[i].used)
            ++count;
    }

    return count;
}

static void handoffDescribe(char* out, size_t cap, const char* state,
                            const char* op, ShellWindow* target)
{
    char line[600];
    int i;

    if (cap == 0)
        return;

    _snprintf(out, cap, "state=%s pid=%lu op=%s windows=%d target=0x%p tabs=%d\n",
              state, (unsigned long)GetCurrentProcessId(), op ? op : "?",
              g_shellWindows, target ? (void*)target->hwnd : (void*)0,
              handoffTabCount(target));
    out[cap - 1] = 0;

    for (i = 0; i < MAX_SHELL_WINDOWS; ++i) {
        ShellWindow* win = &g_windows[i];
        Tab* active;

        if (!win->used)
            continue;

        active = &win->tabs[win->activeTab];

        _snprintf(line, sizeof(line), "win%d hwnd=0x%p tabs=%d active=%s\n",
                  i, (void*)win->hwnd, handoffTabCount(win),
                  (active->used && active->url[0]) ? active->url : "(blank)");
        line[sizeof(line) - 1] = 0;
        strncat(out, line, cap - strlen(out) - 1);
    }
}

/* Runs on the shell's UI thread, inside SendMessage from the second process. */
static LRESULT CALLBACK handoffProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_COPYDATA) {
        const COPYDATASTRUCT* data = (const COPYDATASTRUCT*)lParam;
        ShellHandoff request;
        ShellWindow* target;
        char ack[SHELL_ACK_BYTES];
        const char* opName = "focus";

        if (!data || data->dwData != SHELL_HANDOFF_MAGIC ||
            data->cbData < sizeof(request) || !data->lpData)
            return 0;

        memcpy(&request, data->lpData, sizeof(request));

        if (request.size != sizeof(request) || request.version != SHELL_HANDOFF_VERSION) {
            handoffWriteAck(request.ackName, "state=error err=version\n");
            return 0;
        }

        if (!g_handoffReady) {
            handoffWriteAck(request.ackName, "state=starting\n");
            return 0;
        }

        if (request.op == 1)
            opName = "new-tab";
        else if (request.op == 2)
            opName = "new-window";

        target = (g_lastActive && g_lastActive->used) ? g_lastActive : firstUsedWindow();

        if (request.op == 2)
            target = createShellWindow(NULL);

        if (!target) {
            handoffWriteAck(request.ackName, "state=error err=no-window\n");
            return 0;
        }

        g_cur = target;

        if (request.op != 0) {
            const char* url = request.url[0] ? request.url : NEW_TAB_URL;

            createTab(url);
        }

        if (!g_shellHidden) {
            if (IsIconic(target->hwnd))
                ShowWindow(target->hwnd, SW_RESTORE);
            else
                ShowWindow(target->hwnd, SW_SHOW);

            SetForegroundWindow(target->hwnd);
        }

        g_lastActive = target;

        handoffDescribe(ack, sizeof(ack), "ok", opName, target);
        handoffWriteAck(request.ackName, ack);

        agbLog(AGB_LOG_SHELL, "handoff from pid=%lu: %s url=%s windows=%d tabs=%d",
               (unsigned long)request.pid, opName,
               request.url[0] ? request.url : "(none)",
               g_shellWindows, handoffTabCount(target));
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

static void createHandoffWindow(void)
{
    static int registered;
    WNDCLASSA wc;

    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = handoffProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = SHELL_HANDOFF_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }

    g_handoffWnd = CreateWindowExA(WS_EX_TOOLWINDOW, SHELL_HANDOFF_CLASS, g_handoffTitle,
                                   WS_POPUP, 0, 0, 0, 0,
                                   NULL, NULL, g_instance, NULL);

    if (!g_handoffWnd)
        agbLog(AGB_LOG_SHELL, "handoff window failed: %lu", (unsigned long)GetLastError());
}

/* Second instance: find the running shell, send the request, print the ack and
 * exit. Failure is reported, never silent (exit 3). */
static int handoffClient(void)
{
    char ackName[64];
    char ack[SHELL_ACK_BYTES];
    static int ackCounter;
    HANDLE map = NULL;
    char* view = NULL;
    HWND target = NULL;
    ShellHandoff request;
    COPYDATASTRUCT data;
    DWORD_PTR result = 0;
    ULONGLONG deadline;
    const char* opName;
    int op;

    if (g_newWindowArg)
        op = 2;
    else if (g_newTabArg || g_startupUrl[0])
        op = 1;
    else
        op = 0;

    opName = (op == 2) ? "new-window" : (op == 1) ? "new-tab" : "focus";

    /* A forwarder exits as a utility, not as a shell: the atexit forensics
     * stack dump would be pure noise on the console. */
    g_exitTraceSuppressed = 1;

    _snprintf(ackName, sizeof(ackName), "Local\\agave_shell_ack_%lu_%d",
              (unsigned long)GetCurrentProcessId(), ++ackCounter);
    ackName[sizeof(ackName) - 1] = 0;

    map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                             SHELL_ACK_BYTES, ackName);

    if (map)
        view = (char*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, SHELL_ACK_BYTES);

    if (view)
        view[0] = 0;

    deadline = GetTickCount64() + 15000;

    while (!target) {
        target = FindWindowA(SHELL_HANDOFF_CLASS, g_handoffTitle);

        if (target)
            break;

        if (GetTickCount64() >= deadline)
            break;

        Sleep(50);
    }

    if (!target) {
        fprintf(stderr, "[shell] another instance owns profile %s but is not answering\n",
                g_dataDir);
        if (view)
            UnmapViewOfFile(view);
        if (map)
            CloseHandle(map);
        return 3;
    }

    memset(&request, 0, sizeof(request));
    request.size = (DWORD)sizeof(request);
    request.version = SHELL_HANDOFF_VERSION;
    request.op = (DWORD)op;
    request.pid = GetCurrentProcessId();
    _snprintf(request.ackName, sizeof(request.ackName), "%s", ackName);

    if (g_startupUrl[0]) {
        strncpy(request.url, g_startupUrl, sizeof(request.url) - 1);
        request.url[sizeof(request.url) - 1] = 0;
    }

    memset(&data, 0, sizeof(data));
    data.dwData = SHELL_HANDOFF_MAGIC;
    data.cbData = (DWORD)sizeof(request);
    data.lpData = &request;

    agbLog(AGB_LOG_SHELL, "handoff %s url=%s -> hwnd=%p", opName,
           g_startupUrl[0] ? g_startupUrl : "(none)", (void*)target);

    for (;;) {
        if (view)
            view[0] = 0;

        if (!SendMessageTimeoutA(target, WM_COPYDATA, 0, (LPARAM)&data,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &result)) {
            fprintf(stderr, "[shell] handoff to pid %lu failed: error %lu\n",
                    (unsigned long)GetWindowThreadProcessId(target, NULL),
                    (unsigned long)GetLastError());
            if (view)
                UnmapViewOfFile(view);
            if (map)
                CloseHandle(map);
            return 3;
        }

        if (view) {
            _snprintf(ack, sizeof(ack), "%s", view);
            ack[sizeof(ack) - 1] = 0;
        } else {
            ack[0] = 0;
        }

        if (strncmp(ack, "state=starting", 14) != 0)
            break;

        if (GetTickCount64() >= deadline) {
            fprintf(stderr, "[shell] instance in profile %s is still starting\n", g_dataDir);
            UnmapViewOfFile(view);
            CloseHandle(map);
            return 3;
        }

        Sleep(100);
    }

    if (strncmp(ack, "state=error", 11) == 0) {
        fprintf(stderr, "[shell] handoff rejected: %s", ack);
        if (view)
            UnmapViewOfFile(view);
        if (map)
            CloseHandle(map);
        return 3;
    }

    fputs(ack, stdout);

    if (ack[0] && ack[strlen(ack) - 1] != '\n')
        fputc('\n', stdout);

    fflush(stdout);

    if (view)
        UnmapViewOfFile(view);
    if (map)
        CloseHandle(map);

    return 0;
}

/* Probe-mode logic check for the single-instance hand-off (chrome's singleton
 * handoff is process/UI behaviour, so its *logic* is tested headlessly here
 * instead of by launching a browser window):
 *
 *   AGB_PROBE_HANDOFF=1        run this check; print "[handoff] key=… op=… url=…"
 *   AGB_HANDOFF_MOCK_DIR=<d>   mock the running shell with files: the request
 *                              is written to <d>\request.txt and the answer is
 *                              read from <d>\ack.txt (the same trick as
 *                              AGB_CLIPBOARD_FILE — no Win32 window/transport)
 *   AGB_HANDOFF_WAIT_MS=<n>    ack wait cap (default 15000; tests use ~1 s)
 *
 * No window, mutex or WM_COPYDATA is involved. Exit 0 = ack printed, 3 = no
 * ack / rejected, so the tests can assert the decision and the ack handling. */
static int handoffProbeMain(void)
{
    const char* mockDir = getenv("AGB_HANDOFF_MOCK_DIR");
    const char* waitText = getenv("AGB_HANDOFF_WAIT_MS");
    int waitMs = (waitText && atoi(waitText) > 0) ? atoi(waitText) : 15000;
    int op = g_newWindowArg ? 2 : (g_newTabArg || g_startupUrl[0] ? 1 : 0);
    const char* opName = (op == 2) ? "new-window" : (op == 1) ? "new-tab" : "focus";
    char requestPath[MAX_PATH_CHARS];
    char ackPath[MAX_PATH_CHARS];
    char ack[512];
    ULONGLONG deadline;
    int gotAck = 0;

    probeUserDataInit();
    shellStateInit();
    profileKeyInit();

    /* A logic check exits as a utility: no atexit forensics noise. */
    g_exitTraceSuppressed = 1;

    if (!mockDir || !mockDir[0]) {
        printf("[handoff] key=%s title=%s op=%s url=%s\n",
               g_profileKey, g_handoffTitle, opName,
               g_startupUrl[0] ? g_startupUrl : "(none)");
        fflush(stdout);
        return 0;
    }

    _snprintf(requestPath, sizeof(requestPath), "%s\\request.txt", mockDir);
    requestPath[sizeof(requestPath) - 1] = 0;

    {
        FILE* file = fopen(requestPath, "wb");

        if (!file) {
            fprintf(stderr, "[handoff] cannot write %s\n", requestPath);
            return 3;
        }

        fprintf(file, "key=%s\ntitle=%s\nop=%d\nopname=%s\nurl=%s\npid=%lu\n",
                g_profileKey, g_handoffTitle, op, opName,
                g_startupUrl[0] ? g_startupUrl : "",
                (unsigned long)GetCurrentProcessId());
        fclose(file);
    }

    _snprintf(ackPath, sizeof(ackPath), "%s\\ack.txt", mockDir);
    ackPath[sizeof(ackPath) - 1] = 0;
    ack[0] = 0;

    deadline = GetTickCount64() + (ULONGLONG)waitMs;

    for (;;) {
        FILE* file = fopen(ackPath, "rb");

        if (file) {
            size_t n;

            ack[0] = 0;
            if (fgets(ack, sizeof(ack), file)) {
                n = strlen(ack);
                while (n && (ack[n - 1] == '\n' || ack[n - 1] == '\r'))
                    ack[--n] = 0;
            }
            fclose(file);
        }

        /* "starting" means the mock shell is not ready yet; retry, exactly
         * like the real client does (bounded by AGB_HANDOFF_WAIT_MS). */
        if (ack[0] && strncmp(ack, "state=starting", 14) != 0) {
            gotAck = 1;
            break;
        }

        if (GetTickCount64() >= deadline)
            break;

        Sleep(50);
    }

    if (!gotAck) {
        fprintf(stderr, "[handoff] no ack from the mock shell within %d ms\n", waitMs);
        return 3;
    }

    if (strncmp(ack, "state=error", 11) == 0) {
        fprintf(stderr, "[handoff] rejected: %s\n", ack);
        return 3;
    }

    printf("%s\n", ack);
    fflush(stdout);
    return 0;
}


int APIENTRY WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR commandLine, int show)
{
    MSG msg;
    ShellWindow* primary;
    char initial[MAX_PATH_CHARS];
    static char session[8][MAX_PATH_CHARS];
    int restored = 0;

    (void)previous;
    (void)show;
    (void)commandLine;

    /* Exception filter first: it only fires when no debugger is attached, which
     * is exactly the case where these crashes happen. */
    SetUnhandledExceptionFilter(shellCrashFilter);
    AddVectoredExceptionHandler(1, shellVehHandler);

    atexit(shellExitTrace);
    g_instance = instance;
    {
        char logPath[MAX_PATH];

        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        {
            char* slash = strrchr(logPath, '\\');
            if (slash)
                *slash = 0;
        }
        strncat(logPath, "\\shell.log", MAX_PATH - strlen(logPath) - 1);
        agbLogSetFile(logPath);
    }

    applySystemColorScheme();

    /* Headless first: no window, no toolbar, no session/bookmark files. */
    parseShellArgv();

    if (g_showUsage) {
        probeUsage();
        return 0;
    }

    if (g_argError)
        return PROBE_EXIT_USAGE;

    /* --crash-page is its own tiny host: no profile, no session, no single
     * instance, no restored tabs. It is started by the exception handler. */
    if (g_crashPagePath[0]) {
        g_exitTraceSuppressed = 1;   /* not a shell run: no exit stack trace */
        return crashPageRun(g_crashPagePath);
    }

    /* AGB_TEST_CRASH=1 proves the report/page round trip still works (the
     * handlers only matter when no debugger is attached). Kept out of the crash
     * page process, which inherits the environment of the crashed one. A test
     * crash also writes the test report name, never the user's. */
    if (getenv("AGB_TEST_CRASH")) {
        g_crashConsoleOnly = 1;
        *(volatile int*)0 = 1;
    }

    /* --ui-shot renders the chrome into a DIB and never creates a window. */
    if (g_uiShotPath[0])
        return uiShotMain();

    /* One line that answers "what was I started with": the options that succeed
     * used to leave no trace at all, so a probe run at --zoom 150 or a GUI run
     * with --userdata looked identical to a default start. */
    agbLog(AGB_LOG_SHELL, "argv url=%s probe=%d zoom=%d device-scale=%d resize=%dx%d offline=%d eval=%d bytes keys=%d",
           g_startupUrl[0] ? g_startupUrl : "(none)",
           g_probeMode,
           g_probeZoom,
           g_probeDeviceScale,
           g_probeResizeW, g_probeResizeH,
           getenv("AGB_OFFLINE") != NULL,
           (int)strlen(g_probeScript),
           g_probeKeys[0] ? 1 : 0);

    if (g_probeMode) {
        if (getenv("AGB_PROBE_HANDOFF"))
            return handoffProbeMain();

        return probeRun(g_startupUrl);
    }

    /* Per-monitor DPI is a GUI-only concern. Headless probes stay DPI-unaware:
     * the synthetic WM_* events posted by tools/send_ui_events.ps1 are
     * cross-process and Windows scales their coordinates when the target
     * process changes DPI awareness (the inspector suite asserts 120,140). */
    shellOps()->enable_dpi_awareness(NULL);

    /* The GUI shares the profile root with the engine (cookies, caches) and
     * with bookmarks/session; settle it before the lock, which is keyed on it. */
    shellStateInit();

    {
        char mutexName[96];
        HANDLE profileMutex;

        profileKeyInit();

        _snprintf(mutexName, sizeof(mutexName), "Local\\agave_shell_profile_%s", g_profileKey);
        mutexName[sizeof(mutexName) - 1] = 0;

        profileMutex = CreateMutexA(NULL, FALSE, mutexName);

        if (profileMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            int rc = handoffClient();

            CloseHandle(profileMutex);
            return rc;
        }

        /* Keep the handle for the life of the process: closing it would drop
         * the lock, and the OS releases it anyway when the process dies. */
        if (!profileMutex)
            agbLog(AGB_LOG_SHELL, "profile mutex failed (err %lu); single instance is not enforced",
                   (unsigned long)GetLastError());
    }

    {
        const char* hidden = getenv("AGB_SHELL_HIDDEN");

        g_shellHidden = hidden && hidden[0] && !(hidden[0] == '0' && hidden[1] == 0);

        /* A hidden test shell must never own a modal crash page either. */
        if (g_shellHidden)
            g_crashConsoleOnly = 1;
    }

    registerShellWindowClass();

    primary = createShellWindow("MiniShell - miniblink");

    if (!primary)
        return 1;

    /* Created before any tab: a second launch must be able to ask before the
     * first page has finished loading. Until g_handoffReady the receiver
     * answers "state=starting" and the client retries. */
    createHandoffWindow();
    g_handoffReady = 1;

    /* The command line was already parsed in parseShellArgv(): g_startupUrl
     * wins over the restored session. */
    if (g_startupUrl[0]) {
        strncpy(initial, g_startupUrl, sizeof(initial) - 1);
        initial[sizeof(initial) - 1] = 0;
        agbLog(AGB_LOG_SHELL, "startup url from argv: %s", initial);
    } else if (g_prefs.restore) {
        restored = loadSession(session, 8);
        strcpy(initial, restored > 0 ? session[0] : "http://www.163.com");
        agbLog(AGB_LOG_SHELL, "session restored tabs=%d first=%s", restored, initial);
    } else {
        strcpy(initial, "http://www.163.com");
        agbLog(AGB_LOG_SHELL, "session restore is off (settings), starting at %s", initial);
    }

    if (restored > 1) {
        int i;

        for (i = 0; i < restored; ++i)
            createTab(session[i]);

        setActiveTab(g_activeTab);
    } else {
        createTab(initial);
    }

    /* Headless hook: extra tabs keep the window alive while a test closes the
     * startup tab (see AGB_TEST_CLOSE_TAB_MS). The startup tab ends active, so
     * that is the one the tests close. */
    {
        const char* extra = getenv("AGB_TEST_EXTRA_TABS");

        if (extra && extra[0] && atoi(extra) > 0) {
            int keep = g_activeTab;
            int count = atoi(extra);
            int i;

            for (i = 0; i < count && i < 8; ++i)
                createTab(NEW_TAB_URL);

            setActiveTab(keep);
            agbLog(AGB_LOG_SHELL, "test: %d extra tab(s), startup tab stays active",
                   count);
        }
    }

    if (!g_shellHidden) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }

    /* Headless reproduction hook: open the tab search before the first page
     * has settled (AGB_SHELL_HIDDEN keeps the popup off the desktop). */
    if (getenv("AGB_TEST_TABSEARCH")) {
        agbLog(AGB_LOG_SHELL, "test: opening tab search at startup");
        toggleTabSearchPopup();
    }

    /* Headless reproduction hook: close the active tab after a delay, used to
     * close a tab while its favicon download is in flight. */
    {
        const char* closeMs = getenv("AGB_TEST_CLOSE_TAB_MS");

        if (closeMs && closeMs[0]) {
            int ms = atoi(closeMs);

            if (ms < 50)
                ms = 50;
            agbLog(AGB_LOG_SHELL, "test: closing the active tab in %d ms", ms);
            SetTimer(g_hwnd, TEST_CLOSE_TAB_TIMER, (UINT)ms, NULL);
        }
    }

    agbLog(AGB_LOG_SHELL, "entering message loop");

    while (GetMessageW(&msg, NULL, 0, 0)) {
        ShellWindow* win = windowForHwnd(msg.hwnd);

        /* Keyboard shortcuts below read the window's tab strip; every message
         * must select the window that owns the focused control. */
        if (win)
            g_cur = win;

        /* The tab-search popup owns the keyboard and the wheel while it is
         * open. A printable keydown is handled by the popup and then goes
         * through TranslateMessage, so the WM_CHAR produced for the focused
         * control is picked up on the next iteration and appended to the query
         * (the popup itself is never focused). */
        if (uiTabSearchIsOpen()) {
            if (msg.message == WM_CHAR || msg.message == WM_IME_CHAR) {
                uiTabSearchKey(WM_CHAR, msg.wParam, msg.lParam);
                continue;
            }

            if (msg.message == WM_MOUSEWHEEL) {
                uiTabSearchWheel(GET_WHEEL_DELTA_WPARAM(msg.wParam));
                continue;
            }

            if (msg.message == WM_KEYDOWN &&
                (!(GetKeyState(VK_CONTROL) & 0x8000) ||
                 msg.wParam == VK_UP || msg.wParam == VK_DOWN ||
                 msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT ||
                 msg.wParam == VK_HOME || msg.wParam == VK_END)) {
                uiTabSearchKey(WM_KEYDOWN, msg.wParam, msg.lParam);

                /* Enter/Esc/Tab end or consume the popup: no stray WM_CHAR
                 * for the page afterwards. */
                if (msg.wParam != VK_RETURN && msg.wParam != VK_ESCAPE &&
                    msg.wParam != VK_TAB)
                    TranslateMessage(&msg);
                continue;
            }
        }

        /* Ctrl+wheel is the other half of Ctrl+plus: the shell owns the wheel
         * while Ctrl is down, steps the same zoom ladder and swallows the event,
         * so the page never scrolls (or zooms on its own) under the pointer. */
        if (msg.message == WM_MOUSEWHEEL) {
            int delta = GET_WHEEL_DELTA_WPARAM(msg.wParam);

            if (GetKeyState(VK_CONTROL) & 0x8000) {
                g_zoomWheelAccum += delta;

                while (g_zoomWheelAccum >= WHEEL_DELTA) {
                    g_zoomWheelAccum -= WHEEL_DELTA;
                    setZoom(zoomStep(g_zoom, 1), "ctrl+wheel up");
                }

                while (g_zoomWheelAccum <= -WHEEL_DELTA) {
                    g_zoomWheelAccum += WHEEL_DELTA;
                    setZoom(zoomStep(g_zoom, -1), "ctrl+wheel down");
                }

                continue;
            }

            g_zoomWheelAccum = 0;
        }

        if (msg.message == WM_KEYDOWN) {
            if (msg.hwnd == g_address && msg.wParam == VK_RETURN) {
                char text[MAX_PATH_CHARS];

                readControlText(g_address, text, sizeof(text));
                navigateActive(text, 0, "enter");
                continue;
            }

            /* Escape in the address bar drops the edit: show the URL the tab is
             * really on and hand the keyboard back to the page. */
            if (msg.hwnd == g_address && msg.wParam == VK_ESCAPE) {
                Tab* escapeTab = &g_tabs[g_activeTab];

                if (escapeTab->used) {
                    wchar_t wideUrl[MAX_PATH_CHARS];

                    utf8ToWide(escapeTab->url, wideUrl, MAX_PATH_CHARS);
                    SetWindowTextW(g_address, wideUrl);
                    focusPage();
                }
                continue;
            }

            /* Escape takes the find bar away from anywhere - the EDIT, the page
             * or the chrome - and otherwise stops a load in progress, like
             * Chrome. Neither had a handler: the bar could only be closed with
             * the mouse, and the README's "Esc stops" was never wired up. */
            if (msg.wParam == VK_ESCAPE && g_cur) {
                if (g_inspectOn) {
                    inspectStop("esc");
                    continue;
                }

                if (g_findVisible) {
                    agbLog(AGB_LOG_SHELL, "esc closes find bar");
                    toggleFind();
                    continue;
                }

                if (g_tabs[g_activeTab].used && g_tabs[g_activeTab].loading) {
                    stopActive("esc");
                    continue;
                }
            }

            if (msg.hwnd == g_findEdit && msg.wParam == VK_RETURN) {
                doFind(1);
                continue;
            }

            /* Native single-line EDITs have no select-all, and Tab is a dead key
             * outside a dialog. The address bar is the shell's main input field,
             * so give both fields the two behaviours a browser user expects and
             * leave every other edit key (Ctrl+C/V/X/Z, Ctrl+Backspace,
             * Shift+arrows) to the control itself. */
            if (msg.hwnd == g_address || msg.hwnd == g_findEdit) {
                int fieldCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                int fieldShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (fieldCtrl && !fieldShift && msg.wParam == 'A') {
                    SendMessageW(msg.hwnd, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
                    continue;
                }

                if (!fieldCtrl && msg.wParam == VK_TAB) {
                    /* The find bar has its own ring: Tab from the query walks
                     * field -> prev -> next -> close and wraps, never into the
                     * toolbar ring. The address field keeps the chrome ring. */
                    if (msg.hwnd == g_findEdit) {
                        int next = uiChromeFindFocusStepFrom(CHROME, UI_PART_FIND_FIELD,
                                                             fieldShift ? 0 : 1);

                        if (next != UI_PART_NONE)
                            enterFindRingPart(next);
                        continue;
                    }

                    /* Tab walks the chrome's tab stops (see uiChromeFocusStep);
                     * Shift+Tab goes straight back to the page. Just moving to
                     * the page here would make every toolbar button unreachable
                     * from the keyboard. */
                    if (fieldShift) {
                        /* Shift+Tab walks left out of the address field. */
                        g_focusOwner = FOCUS_CHROME;
                        SetFocus(g_hwnd);
                        if (uiChromeFocusStepFrom(CHROME, UI_PART_ADDRESS, 0) == UI_PART_NONE)
                            focusPage();
                        agbLog(AGB_LOG_SHELL, "focus ring -> part %d (from address, backward)",
                               uiChromeFocusPart(CHROME));
                        invalidateChromeDirty(g_hwnd);
                    } else {
                        g_focusOwner = FOCUS_CHROME;
                        SetFocus(g_hwnd);
                        if (uiChromeFocusStepFrom(CHROME, UI_PART_ADDRESS, 1) == UI_PART_NONE)
                            focusPage();
                        agbLog(AGB_LOG_SHELL, "focus ring -> part %d (from address, forward)",
                               uiChromeFocusPart(CHROME));
                        invalidateChromeDirty(g_hwnd);
                    }
                    continue;                }
            }

            /* Keyboard focus ring over the painted toolbar buttons. While it is
             * active the shell itself has the focus, so these keys arrive here
             * and the chrome owns them until Tab leaves the ring again. */
            if (uiChromeFocusPart(CHROME) != UI_PART_NONE) {
                int ringCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                int ringShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (!ringCtrl && msg.wParam == VK_TAB) {
                    int next;

                    /* Two rings: while a find-bar button has the ring, Tab stays
                     * inside the find bar and wraps around to the query field. */
                    if (isFindRingPart(uiChromeFocusPart(CHROME))) {
                        next = uiChromeFindFocusStep(CHROME, ringShift ? 0 : 1);

                        if (next != UI_PART_NONE)
                            enterFindRingPart(next);
                        continue;
                    }

                    next = uiChromeFocusStep(CHROME, ringShift ? 0 : 1);

                    if (next == UI_PART_ADDRESS) {
                        /* The cycle passes through the native address field: it
                         * paints its own focus ring, so drop the chrome's. */
                        uiChromeFocusClear(CHROME);
                        g_focusOwner = FOCUS_ADDRESS;
                        SetFocus(g_address);
                        SendMessageW(g_address, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
                    } else if (next == UI_PART_NONE) {
                        /* End of the ring: the page is the next node in the
                         * cycle, in both directions. */
                        focusPage();
                    }

                    agbLog(AGB_LOG_SHELL, "focus ring -> part %d%s", next,
                           ringShift ? " (backward)" : "");
                    invalidateChromeDirty(g_hwnd);
                    continue;
                }

                if (msg.wParam == VK_RETURN || msg.wParam == VK_SPACE) {
                    if (uiChromeActivateFocus(CHROME)) {
                        invalidateChromeDirty(g_hwnd);
                        continue;
                    }
                }

                if (msg.wParam == VK_ESCAPE) {
                    uiChromeFocusClear(CHROME);
                    invalidateChromeDirty(g_hwnd);
                    focusPage();
                    continue;
                }
            }

            /* F6: the browser-wide "move between the address bar and the page"
             * key, which is also the way back out of the chrome ring. */
            if (msg.wParam == VK_F6 && g_cur) {
                if (GetFocus() == g_address) {
                    focusPage();
                } else {
                    uiChromeFocusClear(CHROME);
                    releasePageFocus();
                    g_focusOwner = FOCUS_ADDRESS;
                    SetFocus(g_address);
                    SendMessageW(g_address, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
                }
                invalidateChromeDirty(g_hwnd);
                continue;
            }

            if (GetKeyState(VK_CONTROL) & 0x8000) {
                int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (msg.wParam == 'D' && !shift) { addBookmarkCurrent(); continue; }
                if (msg.wParam == 'J' && !shift) { navigateActive("minishell://downloads", 1, "ctrl+j"); continue; }
                if (msg.wParam == 'F' && !shift) { openFind(); continue; }
                if (msg.wParam == 'T') {
                    /* ctrl+shift+T reopens the last closed tab, ctrl+T a new one. */
                    if (shift) reopenClosedTab();
                    else navigateActive(NEW_TAB_URL, 1, "ctrl+t");
                    continue;
                }
                if (msg.wParam == 'W' && !shift) { closeTab(g_activeTab); continue; }
                /* Ctrl+Shift+A: Chrome's "Search tabs". Toggling keeps it a
                 * one-key round trip, and it must run before the popup-open
                 * branch below swallows keys. */
                if (msg.wParam == 'A' && shift) { toggleTabSearchPopup(); continue; }
                if (msg.wParam == 'R') {
                    if (shift)
                        reloadHardActive("ctrl+shift+r");
                    else
                        reloadActive("ctrl+r");
                    continue;
                }
                /* Ctrl+Shift+I, like Chrome: the same host-side marker as F12. */
                if (msg.wParam == 'I' && shift && g_tabs[g_activeTab].used) {
                    toggleInspect();
                    continue;
                }
                if (msg.wParam == VK_TAB) {
                    if (uiTabSearchIsOpen())
                        uiTabSearchClose();
                    switchTab(shift ? -1 : 1);
                    continue;
                }
                /* Ctrl+L selects the current URL: typing then replaces it, which
                 * is also what stops "http://" + a typed path from being glued
                 * onto the old one. */
                if (msg.wParam == 'L' && !shift) {
                    SetFocus(g_address);
                    SendMessageW(g_address, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
                    continue;
                }
                if (msg.wParam == VK_ADD || msg.wParam == 0xBB) {
                    setZoom(zoomStep(g_zoom, 1), "ctrl+plus");
                    continue;
                }
                if (msg.wParam == VK_SUBTRACT || msg.wParam == 0xBD) {
                    setZoom(zoomStep(g_zoom, -1), "ctrl+minus");
                    continue;
                }
                if (msg.wParam == '0') {
                    setZoom(100, "ctrl+0");
                    continue;
                }
            }

            if (msg.wParam == VK_F5 && g_tabs[g_activeTab].used) {
                if (GetKeyState(VK_CONTROL) & 0x8000)
                    reloadHardActive("ctrl+f5");
                else
                    reloadActive("F5");
                continue;
            }

            /* F12 toggles the host-side element marker (see ui_inspect.c): the
             * page only reports rects, the box is painted by the shell. The app
             * menu and the page context menu expose the same toggle. */
            if (msg.wParam == VK_F12 && g_tabs[g_activeTab].used) {
                toggleInspect();
                continue;
            }

            /* alt+left / alt+right: back and forward. */
            if ((GetKeyState(VK_MENU) & 0x8000) && g_tabs[g_activeTab].used) {
                if (msg.wParam == VK_LEFT) { backActive("alt+left"); continue; }
                if (msg.wParam == VK_RIGHT) { forwardActive("alt+right"); continue; }
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    agbLog(AGB_LOG_SHELL, "message loop ended");
    agave_webview_shutdown();

    return 0;
}








