/*
 * mb_view.c - the agave_webview_* control API implemented on miniblink 132.
 *
 * One view == one MB_WINDOW_TYPE_CONTROL child window.  miniblink owns painting,
 * input, IME and focus routing inside that window; this file translates the
 * callbacks the shell UI already speaks (title/url/progress/dialog/download/...)
 * and forwards the host commands (navigate/eval/zoom/back/...).
 */

#include "agave_webview.h"
#include "agb_log.h"

#include <windows.h>
#include <windowsx.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

/* mb.h's own loader path is C++; we link the import library directly. */
#ifndef ENABLE_MB
#define ENABLE_MB 1
#endif
#include "mb.h"

/* Declared in the DLL but missing from mb.h's ITERATOR list. */
MB_EXTERN_C void MB_CALL_TYPE mbOnMouseOverUrlChanged(mbWebView webView,
                                                      mbMouseOverUrlChangedCallback callback,
                                                      void* param);

/* Context-menu executor: runs one mbMenuItemId through the engine's own menu
 * command path (see mbMenuItemId). Exported by the DLL, not in mb.h. */
MB_EXTERN_C void MB_CALL_TYPE mbExecuteContextMenu(mbWebView webView, int itemId);

/* Engine -> host context-menu notification: not used. Its argument layout is
 * undeclared and the values it hands over do not look like coordinates, and the
 * control's own WM_RBUTTONUP (below) is a cleaner trigger. The setter exists in
 * the export table if this ever needs revisiting. */

#define MBV_CTX_MESSAGE   (WM_APP + 0x210)
#define MBV_DL_MESSAGE    (WM_APP + 0x211)
#define MBV_CB_MESSAGE    (WM_APP + 0x212)
#define MBV_CTX_TIMER     2     /* id 1 is the engine's own frame-pump timer */

enum {
    MBV_EV_TITLE = 1,
    MBV_EV_URL,
    MBV_EV_PROGRESS,
    MBV_EV_STATUS,
    MBV_EV_ERROR,
    MBV_EV_DOCREADY,
    MBV_EV_MESSAGE,
    /* A popup the shell refused: destroyed later, never from inside the engine's
     * onCreateView callback (that crashed the next window.open). */
    MBV_EV_DESTROY
};

typedef struct MbEvent {
    int   type;
    char* a;
    char* b;
    int   n1;
    int   n2;
} MbEvent;

#define MBV_PROFILE_MAX   MAX_PATH

typedef struct MbView MbView;
typedef struct MbDownload MbDownload;

struct MbView {
    mbWebView            wv;
    HWND                 hwnd;
    HWND                 backdrop;   /* opaque fill until the first frame */
    HWND                 parent;
    WNDPROC              originalProc;
    AgaveWebViewCallback cb;
    void*                user;

    char                 url[4096];
    char                 title[1024];
    int                  canBack;
    int                  canForward;
    int                  loading;
    int                  progress;
    /* 1 once the control has committed a document (first mbOnURLChanged).
     * mbReload() needs one: called while the first navigation is still in
     * flight it drops that load and starts nothing. */
    int                  committed;
    /* Context menu: one request at a time (the control delivers bursts of
     * WM_RBUTTONUP), and none while our menu is on screen. The menu must open
     * even when the page cannot answer the element probe, so a timer opens it
     * with the basic items and the probe answer only *enriches* it. */
    int                  ctxPending;
    int                  ctxOpen;
    unsigned long        ctxTick;
    int                  ctxX, ctxY;
    int                  zoom;
    int                  visible;
    int                  destroying;
    int                  deviceScale;
    /* Element inspector: host owns the mouse while this is set (see
     * agave_webview_set_input_capture). */
    int                  inputCapture;
    AgaveWebViewInputFn  inputFn;
    void*                inputUser;

    LONG                 refs;
    DWORD                uiThread;

    RECT                 rect;
    char                 downloadDir[MBV_PROFILE_MAX];
    char                 profileDir[MBV_PROFILE_MAX];
};

struct MbDownload {
    MbDownload* next;
    MbView*     view;
    int         id;
    int         done;
    char        url[2048];
    char        path[MBV_PROFILE_MAX];   /* display path (resolved on finish) */
    char        dir[MBV_PROFILE_MAX];    /* directory handed to miniblink */
    char        stem[MAX_PATH];          /* base name without extension */
    double      total;
    double      received;
    int         error;
};

static void registerBackdropClass(void)
{
    static int registered;
    WNDCLASSEXW wc;

    if (registered)
        return;
    registered = 1;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = CreateSolidBrush(RGB(32, 33, 36));
    wc.lpszClassName = L"MiniShellViewBackdrop";
    RegisterClassExW(&wc);
}

static char        g_profileDir[MBV_PROFILE_MAX];
static mbSettings* g_mbSettings;
static LONG        g_mbInitOnce;
static LONG        g_nextDownloadId = 1;
static CRITICAL_SECTION g_dlLock;
static int         g_dlLockReady;
static MbDownload* g_downloads;

/* Windows' proxy is the only one the engine will use: miniblink does not read
 * the Internet Settings itself and has no PAC / bypass-list support, so on a
 * machine where the proxy is mandatory every navigation just hangs (no
 * response, no error, the tab stays at 5%).  The adapter therefore reads the
 * system proxy and hands it to the engine before the first navigation. */
static mbProxy g_systemProxy;
static int     g_systemProxySet;
static int     g_systemProxyLoaded;

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void wideFromUtf8(const char* src, wchar_t* dst, int cap)
{
    if (!dst || cap <= 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
    dst[cap - 1] = 0;
}

static void copyStr(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void ensureDownloadLock(void)
{
    if (!g_dlLockReady) {
        InitializeCriticalSection(&g_dlLock);
        g_dlLockReady = 1;
    }
}

/* Opt-in engine timeline: MBV_TRACE=1 writes one [nav] line per miniblink
 * call and callback, so a load that never commits can be compared against one
 * that does (which callback stopped arriving, and when). */
static int g_traceOn = -1;

/* Opt-in engine timeline (MBV_TRACE=1).  Logging every callback is cheap in
 * isolation but the log flushes per line, and a callback per console message
 * from an ad-heavy page is exactly the kind of UI-thread stall this shell does
 * not need by default. */
static void trace(const char* fmt, ...)
{
    char text[512];
    va_list args;

    if (g_traceOn < 0) {
        const char* env = getenv("MBV_TRACE");

        g_traceOn = (env && env[0] && !(env[0] == '0' && env[1] == 0)) ? 1 : 0;
    }

    if (!g_traceOn)
        return;

    va_start(args, fmt);
    _vsnprintf(text, sizeof(text) - 1, fmt ? fmt : "", args);
    va_end(args);
    text[sizeof(text) - 1] = 0;

    agbLog(AGB_LOG_NAV, "mbv[tid=%lu]: %s", (unsigned long)GetCurrentThreadId(), text);
}

static void trace(const char* fmt, ...);
static void stripJsonString(const char* json, const char* key, char* out, size_t cap);

/* ------------------------------------------------------------------------- */
/* system proxy                                                              */
/* ------------------------------------------------------------------------- */

static int proxyParseEntry(const char* entry, mbProxy* out)
{
    const char* host = entry;
    const char* colon;
    size_t n;

    while (*host == ' ' || *host == '\t')
        ++host;

    colon = strrchr(host, ':');
    if (!colon || colon == host)
        return 0;

    n = (size_t)(colon - host);
    if (n >= sizeof(out->hostname))
        n = sizeof(out->hostname) - 1;
    memcpy(out->hostname, host, n);
    out->hostname[n] = 0;
    out->port = (unsigned short)atoi(colon + 1);

    return out->port != 0;
}

/* "http=1.2.3.4:8080;https=1.2.3.4:8081" or "1.2.3.4:8080".  miniblink has one
 * proxy for every scheme, so https wins, then http, then a bare entry. */
static void proxyFromSpec(const char* spec, mbProxy* out)
{
    char buffer[512];
    char* entries[16];
    char* save = NULL;
    char* token;
    int count = 0;
    int i;

    memset(out, 0, sizeof(*out));
    out->type = MB_PROXY_HTTP;

    _snprintf(buffer, sizeof(buffer) - 1, "%s", spec ? spec : "");
    buffer[sizeof(buffer) - 1] = 0;

    token = strtok_s(buffer, ";", &save);
    while (token && count < (int)(sizeof(entries) / sizeof(entries[0]))) {
        entries[count++] = token;
        token = strtok_s(NULL, ";", &save);
    }

    for (i = 0; i < count; ++i)
        if (!_strnicmp(entries[i], "https=", 6) && proxyParseEntry(entries[i] + 6, out))
            return;

    for (i = 0; i < count; ++i)
        if (!_strnicmp(entries[i], "http=", 5) && proxyParseEntry(entries[i] + 5, out))
            return;

    for (i = 0; i < count; ++i)
        if (!_strnicmp(entries[i], "socks=", 6)) {
            out->type = MB_PROXY_SOCKS5;
            if (proxyParseEntry(entries[i] + 6, out))
                return;
        }

    for (i = 0; i < count; ++i)
        if (!strchr(entries[i], '=') && proxyParseEntry(entries[i], out))
            return;
}

/* Read the proxy once, before the first navigation.  `AGB_PROXY=host:port`
 * overrides it, `AGB_PROXY=none` forces direct connections. */
static void ensureSystemProxy(void)
{
    const char* env;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config;
    char spec[512];

    if (g_systemProxyLoaded)
        return;
    g_systemProxyLoaded = 1;

    env = getenv("AGB_PROXY");
    if (env && env[0]) {
        if (!_stricmp(env, "none") || !_stricmp(env, "off") || !strcmp(env, "0")) {
            agbLog(AGB_LOG_SHELL, "proxy: direct (AGB_PROXY)");
            return;
        }

        proxyFromSpec(env, &g_systemProxy);
        g_systemProxySet = g_systemProxy.port != 0;
        agbLog(AGB_LOG_SHELL, "proxy: %s:%u (AGB_PROXY)", g_systemProxy.hostname,
               (unsigned)g_systemProxy.port);
        return;
    }

    memset(&config, 0, sizeof(config));
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) {
        agbLog(AGB_LOG_SHELL, "proxy: cannot read the Windows settings (err %lu)",
               (unsigned long)GetLastError());
        return;
    }

    if (config.lpszProxy && config.lpszProxy[0]) {
        spec[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, config.lpszProxy, -1, spec, sizeof(spec), NULL, NULL);
        proxyFromSpec(spec, &g_systemProxy);
        g_systemProxySet = g_systemProxy.port != 0;
        agbLog(AGB_LOG_SHELL, "proxy: %s:%u (windows%s)", g_systemProxy.hostname,
               (unsigned)g_systemProxy.port,
               config.lpszProxyBypass ? ", bypass list ignored" : "");
    } else if (config.lpszAutoConfigUrl) {
        /* miniblink has no PAC support: a machine that only publishes a PAC
         * would hang on every request, so say so instead of loading nothing. */
        agbLog(AGB_LOG_SHELL, "proxy: Windows uses a PAC script; miniblink cannot use it "
               "(set AGB_PROXY=host:port)");
    } else {
        agbLog(AGB_LOG_SHELL, "proxy: direct (Windows has no proxy)");
    }

    if (config.lpszProxy)
        GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass)
        GlobalFree(config.lpszProxyBypass);
    if (config.lpszAutoConfigUrl)
        GlobalFree(config.lpszAutoConfigUrl);
}

static void ensureMbInit(void)
{
    if (InterlockedCompareExchange(&g_mbInitOnce, 1, 0) != 0)
        return;

    trace("mbInit begin");
    mbEnableHighDPISupport();
    mbInit(g_mbSettings);
    trace("mbInit done");

    ensureDownloadLock();
}

static MbView* viewFromHwnd(HWND hwnd)
{
    return (MbView*)GetPropW(hwnd, L"mbshell_view");
}

static void viewAddRef(MbView* v)
{
    InterlockedIncrement(&v->refs);
}

static void viewRelease(MbView* v)
{
    if (InterlockedDecrement(&v->refs) == 0) {
        if (v->hwnd)
            RemovePropW(v->hwnd, L"mbshell_view");
        free(v);
    }
}

static void viewMarkDestroyed(MbView* v)
{
    /* Downloads keep their own reference, so the view stays allocated until
     * the last job finishes; the flag stops callbacks from touching the UI. */
    v->destroying = 1;
}

/* ------------------------------------------------------------------------- */
/* JS string quoting                                                         */
/* ------------------------------------------------------------------------- */

static size_t jsQuote(const char* src, char* out, size_t cap)
{
    size_t o = 0;

    if (cap < 3)
        return 0;

    out[o++] = '"';
    for (; src && *src && o + 8 < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += (size_t)_snprintf(out + o, cap - o, "\\u%04x", (unsigned)c);
            } else {
                out[o++] = (char)c;
            }
            break;
        }
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------------- */
/* prelude: agavePostMessage / clipboard for legacy page scripts            */
/* ------------------------------------------------------------------------- */

static const char kPreludeJs[] =
    "(function(){"
    " if (window.__mbShellPrelude) return; window.__mbShellPrelude = 1;"
    " window.__mbClipboardSetText = function(t){"
    "   try { mbQuery(0x6d62636c, '__mbclip__' + String(t), function(){}); } catch (e) {}"
    " };"
    " if (!navigator.clipboard) navigator.clipboard = {};"
    " if (!navigator.clipboard.writeText) {"
    "   navigator.clipboard.writeText = function(t){"
    "     window.__mbClipboardSetText(t); return Promise.resolve();"
    "   };"
    " }"
    " window.agavePostMessage = function(t){"
    "   try { mbQuery(0x6d626d73, String(t), function(){}); } catch (e) {}"
    " };"
    /* Tab traversal leaves the page: Chromium's embedding API has a
     * "focus moved out" notification, miniblink exposes none, so the page
     * reports it itself. Only the ends of the tab order count: in the middle of
     * the document the browser keeps the keyboard and walks to the next
     * element. */
    " function __mbTabbables(){"
    "   var nodes = document.querySelectorAll('a[href],area[href],button,input,select,textarea,iframe,object,embed,[tabindex],[contenteditable=\"\"],[contenteditable=\"true\"]');"
    "   var out = [];"
    "   for (var i = 0; i < nodes.length; i++) {"
    "     var el = nodes[i];"
    "     if (el.tabIndex < 0 || el.disabled) continue;"
    "     if (!el.offsetWidth && !el.offsetHeight && el.getClientRects().length === 0) continue;"
    "     out.push(el);"
    "   }"
    "   return out;"
    " }"
    " document.addEventListener('keydown', function(e){"
    "   if (e.key !== 'Tab' || e.ctrlKey || e.altKey || e.metaKey) return;"
    "   try {"
    "     var list = __mbTabbables();"
    "     var i = list.indexOf(document.activeElement);"
    "     var last = i === list.length - 1;"
    "     var first = i <= 0;"
    "     if ((!e.shiftKey && (list.length === 0 || last)) || (e.shiftKey && first))"
    "       window.agavePostMessage('__agb_tab_end__' + (e.shiftKey ? 'shift' : ''));"
    "   } catch (err) {}"
    " }, true);"
    "})();";

static void injectPrelude(MbView* v)
{
    mbWebFrameHandle frame = mbWebFrameGetMainFrame(v->wv);
    if (!frame)
        return;
    mbRunJs(v->wv, frame, kPreludeJs, FALSE, NULL, NULL, NULL);
}

static void setClipboardTextA(const char* utf8)
{
    int len;
    wchar_t* wide;

    if (!utf8)
        return;

    len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len <= 0)
        return;

    wide = (wchar_t*)calloc((size_t)len, sizeof(wchar_t));
    if (!wide)
        return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, len);

    if (OpenClipboard(NULL)) {
        HGLOBAL mem;
        size_t bytes = (size_t)len * sizeof(wchar_t);

        EmptyClipboard();
        mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* dst = GlobalLock(mem);
            if (dst) {
                memcpy(dst, wide, bytes);
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }

    free(wide);
}

/* ------------------------------------------------------------------------- */
/* miniblink callbacks                                                       */
/* ------------------------------------------------------------------------- */

/* miniblink calls some of these on its loader thread (the probe checks the
 * thread).  The shell's Tab state and chrome are UI-thread only, so every
 * callback is posted to the control window and dispatched by its subclass. */
static void postEvent(MbView* v, int type, const char* a, const char* b,
                      int n1, int n2)
{
    MbEvent* e;

    if (!v || v->destroying || !v->hwnd)
        return;

    e = (MbEvent*)calloc(1, sizeof(*e));
    if (!e)
        return;

    e->type = type;
    e->n1 = n1;
    e->n2 = n2;
    if (a) {
        size_t n = strlen(a) + 1;
        e->a = (char*)malloc(n);
        if (e->a)
            memcpy(e->a, a, n);
    }
    if (b) {
        size_t n = strlen(b) + 1;
        e->b = (char*)malloc(n);
        if (e->b)
            memcpy(e->b, b, n);
    }

    if (!PostMessageW(v->hwnd, MBV_CB_MESSAGE, 0, (LPARAM)e)) {
        free(e->a);
        free(e->b);
        free(e);
    }
}

static void deliverEvent(MbView* v, MbEvent* e)
{    if (!v || v->destroying || !e)
        return;

    switch (e->type) {
    case MBV_EV_TITLE:
        copyStr(v->title, sizeof(v->title), e->a);
        if (v->cb.on_title)
            v->cb.on_title((AgaveWebView*)v, v->title, v->user);
        break;

    case MBV_EV_URL: {
        int changed = v->url[0] && e->a && strcmp(v->url, e->a) != 0;

        /* "about:blank" is the control's placeholder document, not a page the
         * host asked for: a reload must restart the real navigation. */
        if (e->a && e->a[0] && strcmp(e->a, "about:blank") != 0)
            v->committed = 1;

        copyStr(v->url, sizeof(v->url), e->a);
        v->canBack = e->n1;
        v->canForward = e->n2;

        /* A main-frame URL change is a navigation (link click, redirect).
         * mbOnNavigation fires for every subresource and cannot drive this:
         * on a heavy page it re-armed the loading state forever. */
        if (changed) {
            v->loading = 1;
            v->progress = 10;
            trace("ui: url committed, progress=10 url=%s", v->url);
            if (v->cb.on_progress)
                v->cb.on_progress((AgaveWebView*)v, v->progress, 0, v->user);
        }
        if (v->cb.on_url)
            v->cb.on_url((AgaveWebView*)v, v->url, v->user);
        break;
    }

    case MBV_EV_PROGRESS:
        v->loading = e->n2 ? 0 : 1;
        v->progress = e->n1;
        if (v->cb.on_progress)
            v->cb.on_progress((AgaveWebView*)v, v->progress, e->n2, v->user);
        break;

    case MBV_EV_STATUS:
        if (v->cb.on_status)
            v->cb.on_status((AgaveWebView*)v, e->a ? e->a : "", v->user);
        break;

    case MBV_EV_ERROR:
        if (v->cb.on_error)
            v->cb.on_error((AgaveWebView*)v, e->n1, e->a ? e->a : v->url, v->user);
        break;

    case MBV_EV_DOCREADY:
        injectPrelude(v);
        if (v->loading) {
            v->progress = 60;
            if (v->cb.on_progress)
                v->cb.on_progress((AgaveWebView*)v, v->progress, 0, v->user);
        }
        break;

    case MBV_EV_MESSAGE:
        /* Page -> host messages arrive on the engine's blink thread; they are
         * posted here so the host always touches its UI on the UI thread. */
        if (v->cb.on_message)
            v->cb.on_message((AgaveWebView*)v, e->a ? e->a : "", v->user);
        break;

    case MBV_EV_DESTROY:
        /* The popup was refused. By now the engine has returned from its
         * onCreateView callback, so the webview can go away safely; doing it
         * inside that callback left the engine pointing at freed memory and the
         * next window.open faulted in mb132_x64.dll. */
        agave_webview_destroy((AgaveWebView*)v);
        break;
    }
}

static void MB_CALL_TYPE mbvOnTitle(mbWebView wv, void* param, const utf8* title)
{
    (void)wv;
    trace("cb title: %s", title ? title : "");
    postEvent((MbView*)param, MBV_EV_TITLE, title, NULL, 0, 0);
}

static void MB_CALL_TYPE mbvOnUrl(mbWebView wv, void* param, const utf8* url,
                                  BOOL canGoBack, BOOL canGoForward)
{
    (void)wv;
    trace("cb url: %s back=%d fwd=%d", url ? url : "",
          canGoBack ? 1 : 0, canGoForward ? 1 : 0);
    postEvent((MbView*)param, MBV_EV_URL, url, NULL,
              canGoBack ? 1 : 0, canGoForward ? 1 : 0);
}

static void MB_CALL_TYPE mbvOnLoadingFinish(mbWebView wv, void* param,
                                            mbWebFrameHandle frameId,
                                            const utf8* url, mbLoadingResult result,
                                            const utf8* failedReason)
{
    MbView* v = (MbView*)param;
    mbWebFrameHandle mainFrame = mbWebFrameGetMainFrame(wv);

    if (!v || v->destroying)
        return;
    if (frameId && frameId != mainFrame)
        return;

    trace("cb loadingFinish result=%d url=%s failed=%s", (int)result,
          url ? url : "", failedReason ? failedReason : "");
    postEvent(v, MBV_EV_PROGRESS, NULL, NULL, 100, 1);

    /* A user stop is not an error: only a real failure gets the red status. */
    if (result == MB_LOADING_FAILED) {
        if (failedReason && failedReason[0])
            agbLog(AGB_LOG_NAV, "load failed (%s): %s", failedReason, url ? url : "");
        postEvent(v, MBV_EV_ERROR, url ? url : v->url, NULL, -2, 0);
    }
}

static void MB_CALL_TYPE mbvOnDocumentReady(mbWebView wv, void* param,
                                            mbWebFrameHandle frameId)
{
    MbView* v = (MbView*)param;

    if (!v || v->destroying)
        return;
    if (frameId && frameId != mbWebFrameGetMainFrame(wv))
        return;

    trace("cb documentReady");
    postEvent(v, MBV_EV_DOCREADY, NULL, NULL, 0, 0);
}

static void MB_CALL_TYPE mbvOnStatus(mbWebView wv, void* param, const utf8* url)
{
    (void)wv;
    postEvent((MbView*)param, MBV_EV_STATUS, url, NULL, 0, 0);
}

/* Load start (link click, form submit, back/forward, reload).  The URL is read
 * from mbOnURLChanged; here only the loading state is set, so a pushState URL
 * change cannot leave the toolbar stuck on the stop button. */
static BOOL MB_CALL_TYPE mbvOnNavigation(mbWebView wv, void* param,
                                         mbNavigationType navType, const utf8* url)
{
    /* Deliberately does not touch the loading state: this fires for every
     * subresource/iframe, and the toolbar must follow the main document. */
    (void)param;
    trace("cb navigation type=%d url=%s", (int)navType, url ? url : "");
    return TRUE;
}

static void MB_CALL_TYPE mbvOnConsole(mbWebView wv, void* param, mbConsoleLevel level,
                                      const utf8* message, const utf8* sourceName,
                                      unsigned sourceLine, const utf8* stackTrace)
{
    const char* tag = "con";
    (void)wv;
    (void)param;

    switch (level) {
    case mbLevelDebug:   tag = "con-debug"; break;
    case mbLevelInfo:    tag = "con-info";  break;
    case mbLevelWarning: tag = "con-warn";  break;
    case mbLevelError:   tag = "con-error"; break;
    default:             tag = "con-log";   break;
    }

    agbLog(AGB_LOG_UI, "[%s] %s (%s:%u)%s%s", tag,
           message ? message : "",
           sourceName ? sourceName : "?",
           sourceLine,
           stackTrace && stackTrace[0] ? "\n" : "",
           stackTrace && stackTrace[0] ? stackTrace : "");
}

static int mbvDialog(MbView* v, int type, const char* message, const char* def,
                     char* result, int resultSize)
{
    if (!v || !v->cb.on_dialog)
        return type == 1 ? 1 : 1;
    return v->cb.on_dialog((AgaveWebView*)v, type, message ? message : "",
                           def ? def : "", result, resultSize, v->user);
}

static void MB_CALL_TYPE mbvOnAlert(mbWebView wv, void* param, const utf8* msg)
{
    (void)wv;
    mbvDialog((MbView*)param, 0, msg, "", NULL, 0);
}

static BOOL MB_CALL_TYPE mbvOnConfirm(mbWebView wv, void* param, const utf8* msg)
{
    (void)wv;
    return mbvDialog((MbView*)param, 1, msg, "", NULL, 0) ? TRUE : FALSE;
}

static mbStringPtr MB_CALL_TYPE mbvOnPrompt(mbWebView wv, void* param, const utf8* msg,
                                            const utf8* defaultResult, BOOL* result)
{
    char answer[1024];
    int ok;

    (void)wv;
    answer[0] = 0;
    ok = mbvDialog((MbView*)param, 2, msg, defaultResult,
                   answer, sizeof(answer));
    if (result)
        *result = ok ? TRUE : FALSE;
    if (!ok)
        return NULL;
    return mbCreateStringWithCopy(answer, strlen(answer));
}

static void MB_CALL_TYPE mbvOnJsQuery(mbWebView wv, void* param, mbJsExecState es,
                                      int64_t queryId, int customMsg,
                                      const utf8* request)
{
    MbView* v = (MbView*)param;
    (void)wv;
    (void)es;
    (void)customMsg;

    (void)queryId;
    (void)customMsg;

    if (!v || v->destroying || !request)
        return;

    /* miniblink assigns queryId/customMsg itself; the payload is what carries
     * our meaning.  The prelude marks the clipboard channel explicitly. */
    if (!strncmp(request, "__mbclip__", 10)) {
        setClipboardTextA(request + 10);
        return;
    }

    /* Everything else is host UI state: hop to the control window's thread. */
    postEvent(v, MBV_EV_MESSAGE, request, NULL, 0, 0);
}

/* The page asked for a new window.  Create the control window first, then let
 * the shell adopt it as a tab (the shell calls set_user and reparents it). */
static mbWebView MB_CALL_TYPE mbvOnCreateView(mbWebView wv, void* param,
                                              mbNavigationType navType,
                                              const utf8* url,
                                              const mbWindowFeatures* features)
{
    MbView* from = (MbView*)param;
    MbView* popup;
    int x = 0, y = 0, w = 800, h = 600;

    (void)wv;
    (void)navType;

    if (!from || from->destroying)
        return NULL;

    agbLog(AGB_LOG_NAV, "mbv: new window url=%s", url ? url : "");

    if (features) {
        x = features->x;
        y = features->y;
        if (features->width > 0)  w = features->width;
        if (features->height > 0) h = features->height;
    }

    popup = (MbView*)agave_webview_create(from->parent, x, y, w, h,
                                          &from->cb, from->user);
    if (!popup)
        return NULL;

    copyStr(popup->url, sizeof(popup->url), url ? url : "");
    copyStr(popup->downloadDir, sizeof(popup->downloadDir), from->downloadDir);
    copyStr(popup->profileDir, sizeof(popup->profileDir), from->profileDir);
    popup->zoom = from->zoom;
    mbSetZoomFactor(popup->wv, (float)popup->zoom / 100.0f);

    if (from->cb.on_new_window) {
        if (!from->cb.on_new_window((AgaveWebView*)from, (AgaveWebView*)popup,
                                    url ? url : "", from->user)) {
            /* Off screen right away, destroyed on the control's own message
             * queue: mbDestroyWebView inside this callback corrupted the engine
             * (the following window.open crashed in mb132_x64.dll). */
            if (popup->wv)
                mbShowWindow(popup->wv, 0);
            if (popup->hwnd)
                ShowWindow(popup->hwnd, SW_HIDE);

            postEvent(popup, MBV_EV_DESTROY, NULL, NULL, 0, 0);
            return NULL;
        }
    }

    return popup->wv;
}

static BOOL MB_CALL_TYPE mbvOnDestroy(mbWebView wv, void* param, void* unuse)
{
    MbView* v = (MbView*)param;
    (void)wv; (void)unuse;

    if (!v || v->destroying)
        return TRUE;

    /* window.close() destroys the real window; tell the shell to close the
     * tab.  mbDestroyWebView from the host sets destroying first, so this only
     * fires for page-initiated closes. */
    if (v->cb.on_close_window) {
        v->destroying = 1;
        v->cb.on_close_window((AgaveWebView*)v, v->user);
    }
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* downloads                                                                 */
/* ------------------------------------------------------------------------- */

static void postDownload(MbView* v, MbDownload* d, int state)
{
    if (!v || v->destroying || !v->hwnd)
        return;

    if (!PostMessageW(v->hwnd, MBV_DL_MESSAGE, (WPARAM)state, (LPARAM)d)) {
        /* window is gone; the ctx is freed by the download finish path */
    }
}

static void MB_CALL_TYPE mbvDlRecv(void* ptr, mbNetJob job, const char* data, int length)
{
    MbDownload* d = (MbDownload*)ptr;
    (void)job;
    (void)data;

    if (!d || d->done || length <= 0)
        return;

    d->received += (double)length;
    postDownload(d->view, d, 1);            /* MC_DL_PROGRESS */
}

/* The engine may rename a file to avoid a collision
 * (name[yyyy-mm-dd-hh-mm-ss](n).ext), so the display path is resolved from the
 * directory after the transfer instead of being guessed up front. */
static void resolveDownloadPath(MbDownload* d)
{
    char pattern[MBV_PROFILE_MAX];
    char best[MAX_PATH];
    FILETIME bestTime;
    HANDLE find;
    WIN32_FIND_DATAA fd;
    size_t stemLen;

    if (!d->dir[0])
        return;

    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", d->dir);
    pattern[sizeof(pattern) - 1] = 0;

    memset(&bestTime, 0, sizeof(bestTime));
    best[0] = 0;
    stemLen = strlen(d->stem);

    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (stemLen && _strnicmp(fd.cFileName, d->stem, stemLen) != 0)
            continue;
        if (!best[0] || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            bestTime = fd.ftLastWriteTime;
            copyStr(best, sizeof(best), fd.cFileName);
        }
    } while (FindNextFileA(find, &fd));

    FindClose(find);

    if (best[0]) {
        _snprintf(d->path, sizeof(d->path) - 1, "%s\\%s", d->dir, best);
        d->path[sizeof(d->path) - 1] = 0;
    }
}

static void MB_CALL_TYPE mbvDlFinish(void* ptr, mbNetJob job, mbLoadingResult result)
{
    MbDownload* d = (MbDownload*)ptr;
    (void)job;

    if (!d || d->done)
        return;

    d->done = 1;
    d->error = (result == MB_LOADING_SUCCEEDED) ? 0 : (int)result;
    if (result == MB_LOADING_SUCCEEDED)
        resolveDownloadPath(d);

    {
        int state = (result == MB_LOADING_SUCCEEDED) ? MC_DL_FINISHED : MC_DL_FAILED;
        postDownload(d->view, d, state);
    }
}

static const char* fileNameFromUrl(const char* url)
{
    const char* p;
    const char* q;
    const char* name;

    if (!url)
        return "download.bin";

    p = strrchr(url, '/');
    name = p ? p + 1 : url;
    q = strchr(name, '?');
    if (!q)
        q = strchr(name, '#');
    if (q) {
        static char buf[512];
        size_t n = (size_t)(q - name);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, name, n);
        buf[n] = 0;
        return buf[0] ? buf : "download.bin";
    }
    return name[0] ? name : "download.bin";
}

static const char* fileNameFromDisposition(const char* disposition)
{
    const char* p;
    static char name[512];
    size_t o = 0;

    if (!disposition)
        return NULL;

    p = strstr(disposition, "filename=");
    if (!p)
        p = strstr(disposition, "filename*=");
    if (!p)
        return NULL;

    p = strchr(p, '=') + 1;
    while (*p == ' ' || *p == '"' || *p == '\'')
        ++p;

    while (*p && *p != '"' && *p != ';' && *p != '\r' && *p != '\n' && o + 1 < sizeof(name)) {
        char c = *p++;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
            c = '_';
        name[o++] = c;
    }
    name[o] = 0;

    return name[0] ? name : NULL;
}

/* Path allocation shared by both download entry points.  Returns NULL when the
 * job cannot be tracked. */
static MbDownload* downloadNew(MbView* v, const char* url, const char* disposition)
{
    MbDownload* d;
    const char* base;
    char name[MAX_PATH];
    char stem[MAX_PATH];
    char* dot;
    int n = 1;

    ensureDownloadLock();

    base = fileNameFromDisposition(disposition);
    if (!base || !base[0])
        base = fileNameFromUrl(url);

    copyStr(name, sizeof(name), base);
    copyStr(stem, sizeof(stem), name);
    dot = strrchr(stem, '.');
    if (dot && dot != stem)
        *dot = 0;
    else
        stem[0] = 0;

    for (;;) {
        char candidate[MBV_PROFILE_MAX];
        _snprintf(candidate, sizeof(candidate) - 1, "%s\\%s",
                  v->downloadDir[0] ? v->downloadDir : v->profileDir, name);
        candidate[sizeof(candidate) - 1] = 0;
        if (GetFileAttributesA(candidate) == INVALID_FILE_ATTRIBUTES)
            break;
        _snprintf(name, sizeof(name) - 1, "(%d) %s%s%s", n++, stem,
                  stem[0] ? "." : "", stem[0] ? (dot ? dot + 1 : "") : base);
        name[sizeof(name) - 1] = 0;
    }

    d = (MbDownload*)calloc(1, sizeof(*d));
    if (!d)
        return NULL;

    d->id = (int)InterlockedIncrement(&g_nextDownloadId);
    d->view = v;
    copyStr(d->url, sizeof(d->url), url);
    copyStr(d->dir, sizeof(d->dir),
            v->downloadDir[0] ? v->downloadDir : v->profileDir);
    copyStr(d->stem, sizeof(d->stem), stem[0] ? stem : base);
    _snprintf(d->path, sizeof(d->path) - 1, "%s\\%s", d->dir, name);
    d->path[sizeof(d->path) - 1] = 0;

    viewAddRef(v);

    EnterCriticalSection(&g_dlLock);
    d->next = g_downloads;
    g_downloads = d;

    /* Cap the history the downloads page can grow to. */
    {
        MbDownload* cur = g_downloads;
        int count = 0;

        while (cur) {
            ++count;
            cur = cur->next;
        }
        if (count > 200) {
            MbDownload** pp = &g_downloads;
            int keep = 200;

            while (*pp && keep-- > 0)
                pp = &(*pp)->next;
            while (*pp) {
                MbDownload* old = *pp;
                *pp = old->next;
                free(old);
            }
        }
    }
    LeaveCriticalSection(&g_dlLock);

    return d;
}

static void downloadForget(MbDownload* d)
{
    MbView* owner = d->view;

    EnterCriticalSection(&g_dlLock);
    {
        MbDownload** pp = &g_downloads;
        while (*pp) {
            if (*pp == d) {
                *pp = d->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    LeaveCriticalSection(&g_dlLock);

    if (owner)
        viewRelease(owner);
    free(d);
}

/* Both mbDownloadByUtf8Path paths end here.  `dataBind` comes from the
 * blink-thread callback; it is what makes the transfer owned by miniblink. */
static mbDownloadOpt downloadBegin(MbView* v, const char* url, size_t expected,
                                   const char* mime, const char* disposition,
                                   mbNetJob job, mbNetJobDataBind* dataBind)
{
    MbDownload* d;
    mbDownloadBind bind;
    mbDownloadOpt opt;

    if (!v || v->destroying)
        return kMbDownloadOptCancel;

    d = downloadNew(v, url, disposition);
    if (!d)
        return kMbDownloadOptCancel;

    d->total = (double)expected;

    memset(&bind, 0, sizeof(bind));
    bind.param = d;
    bind.recvCallback = mbvDlRecv;
    bind.finishCallback = mbvDlFinish;

    agbLog(AGB_LOG_NAV, "mbv: download %s -> %s", url ? url : "", d->path);
    /* Undocumented (the docx only covers mbPopupDialogAndDownload): the `path`
     * argument is a DIRECTORY, not a file name - the engine appends the
     * disposition/URL name inside it.  d->path is only the display path. */
    opt = mbDownloadByUtf8Path(v->wv, NULL,
                               v->downloadDir[0] ? v->downloadDir : v->profileDir,
                               expected, url ? url : "", mime, disposition,
                               job, dataBind, &bind);
    agbLog(AGB_LOG_NAV, "mbv: download opt=%d", (int)opt);

    if (opt == kMbDownloadOptCancel) {
        downloadForget(d);
        return kMbDownloadOptCancel;
    }

    postDownload(v, d, MC_DL_STARTED);
    return opt;
}

static mbDownloadOpt MB_CALL_TYPE mbvOnDownloadBlink(mbWebView wv, void* param,
                                                     size_t expectedContentLength,
                                                     const char* url, const char* mime,
                                                     const char* disposition,
                                                     mbNetJob job,
                                                     mbNetJobDataBind* dataBind)
{
    (void)wv;
    return downloadBegin((MbView*)param, url, expectedContentLength, mime,
                         disposition, job, dataBind);
}

static BOOL MB_CALL_TYPE mbvOnDownload(mbWebView wv, void* param, mbWebFrameHandle frameId,
                                       const char* url, void* downloadJob)
{
    mbDownloadOpt opt;
    (void)wv;
    (void)frameId;

    opt = downloadBegin((MbView*)param, url, 0, NULL, NULL,
                        (mbNetJob)downloadJob, NULL);
    return opt == kMbDownloadOptCancel ? FALSE : TRUE;
}

static void deliverDownload(MbView* v, int state, MbDownload* d)
{
    MC_DOWNLOAD_INFO info;

    if (!d)
        return;

    memset(&info, 0, sizeof(info));
    info.state = state;
    info.utf8_url = d->url;
    info.utf8_path = d->path;
    info.received = (unsigned long long)d->received;
    info.total = (unsigned long long)d->total;
    info.error = d->error;

    if (v && !v->destroying && v->cb.on_download)
        v->cb.on_download((AgaveWebView*)v, &info, v->user);

    if (state == MC_DL_FINISHED || state == MC_DL_FAILED || state == MC_DL_CANCELLED) {
        MbView* owner = d->view;

        /* Keep the finished row for minishell://downloads; only the view ref goes
         * away.  The list is capped in downloadNew(). */
        d->view = NULL;
        d->done = 1;
        if (owner)
            viewRelease(owner);
    }
}

/* ------------------------------------------------------------------------- */
/* context menu                                                              */
/* ------------------------------------------------------------------------- */

static void stripJsonString(const char* json, const char* key, char* out, size_t cap)
{
    const char* p;
    size_t o = 0;

    out[0] = 0;
    p = strstr(json, key);
    if (!p)
        return;
    p = strchr(p, ':');
    if (!p)
        return;
    ++p;
    while (*p == ' ' || *p == '\t')
        ++p;
    if (*p != '"')
        return;
    ++p;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 'r': out[o++] = '\r'; break;
            case 't': out[o++] = '\t'; break;
            case '"': out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case '/': out[o++] = '/';  break;
            case 'u': {
                /* Keep non-ASCII escapes out of the menu path; the usual
                 * controls/selection text is ASCII enough for this menu. */
                int i;
                for (i = 0; i < 4 && p[1]; ++i)
                    ++p;
                break;
            }
            default: out[o++] = *p; break;
            }
            ++p;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

static void showContextMenu(MbView* v, int x, int y)
{
    if (!v || v->destroying || !v->cb.on_context_menu)
        return;

    /* No page query. The engine exposes no "what is under the pointer" API, and
     * routing it through JS made the menu depend on the page answering at all
     * (one dropped answer left it dead until reload). The menu opens straight
     * from the control's right click; the editing commands stay listed and the
     * engine decides whether they apply. */
    v->ctxOpen = 1;
    v->cb.on_context_menu((AgaveWebView*)v, x, y, "", "", "", 0, v->user);
    v->ctxOpen = 0;
}

/* ------------------------------------------------------------------------- */
/* control window subclass                                                   */
/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK mbvViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MbView* v = viewFromHwnd(hwnd);
    WNDPROC original = v ? v->originalProc : NULL;

    /* Element inspector: the host owns the mouse. Swallowing here (the control's
     * own message path) stops the page from seeing hover/clicks without putting
     * a window above the page - an overlay window, even an invisible layered
     * child, makes the page stop repainting on resize. */
    if (v && v->inputCapture && !v->destroying) {
        switch (msg) {
        case WM_MOUSEMOVE:
            if (v->inputFn)
                v->inputFn((AgaveWebView*)v, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                           v->inputUser);
            return 0;

        case WM_LBUTTONUP:
            if (v->inputFn)
                v->inputFn((AgaveWebView*)v, 1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                           v->inputUser);
            return 0;

        case WM_RBUTTONUP:
            if (v->inputFn)
                v->inputFn((AgaveWebView*)v, 2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
                           v->inputUser);
            return 0;

        /* Swallow the presses too, otherwise the page would see half a click. */
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 0;

        case WM_SETCURSOR:
            SetCursor(LoadCursorW(NULL, IDC_CROSS));
            return TRUE;
        }
    }

    switch (msg) {
    /* The engine swallows right-clicks when its own menu is disabled: no
     * WM_CONTEXTMENU ever reaches the host (verified on real input). The
     * control window itself still receives every WM_RBUTTONUP, and this
     * subclass is already in that message path - so the shell opens its own
     * menu from here. */
    case WM_RBUTTONUP:
        if (!v || v->destroying || v->ctxOpen)
            break;
        if (GetTickCount() - v->ctxTick < 250)
            break;

        v->ctxTick = GetTickCount();
        showContextMenu(v, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_CONTEXTMENU: {
        POINT pt;

        if (!v || v->destroying)
            break;

        pt.x = GET_X_LPARAM(lp);
        pt.y = GET_Y_LPARAM(lp);
        if (pt.x == -1 && pt.y == -1)
            GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);

        /* Handle it outside miniblink's dispatch: the shell menu runs a modal
         * loop. */
        PostMessageW(hwnd, MBV_CTX_MESSAGE, (WPARAM)pt.x, (LPARAM)pt.y);
        return 0;
    }

    case MBV_CTX_MESSAGE:
        if (v && !v->destroying && !v->ctxOpen) {
            v->ctxTick = GetTickCount();
            showContextMenu(v, (int)(short)LOWORD(wp), (int)(short)LOWORD(lp));
        }
        return 0;

    case MBV_CB_MESSAGE: {
        MbEvent* e = (MbEvent*)lp;

        if (v && !v->destroying)
            deliverEvent(v, e);
        if (e) {
            free(e->a);
            free(e->b);
            free(e);
        }
        return 0;
    }

    case MBV_DL_MESSAGE: {
        int state = (int)wp;
        MbDownload* d = (MbDownload*)lp;

        if (v && !v->destroying)
            deliverDownload(v, state, d);
        return 0;
    }

    /* A click into the page must put the keyboard there.  The control is a
     * child window with its own HWND, so clicking it focuses the *window* -
     * miniblink only routes keys into the page after mbSetFocus, which used to
     * happen on tab switches and navigations only: click the page after using
     * the address bar and typing went nowhere. */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
        if (v && !v->destroying)
            mbSetFocus(v->wv);
        break;
    /* No mbSetFocus on WM_RBUTTONDOWN: miniblink drives its own context-menu
     * state from that message, and re-focusing in the middle of it suppressed
     * the WM_CONTEXTMENU the shell listens for. */

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (v && !v->destroying && v->cb.on_focus)
            v->cb.on_focus((AgaveWebView*)v, msg == WM_SETFOCUS ? 1 : 0, v->user);
        break;

    case WM_NCDESTROY:
        if (original)
            return CallWindowProcW(original, hwnd, msg, wp, lp);
        break;
    }

    return original ? CallWindowProcW(original, hwnd, msg, wp, lp)
                    : DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------- */
/* public API                                                               */
/* ------------------------------------------------------------------------- */

static void viewApplyProfile(MbView* v)
{
    wchar_t wide[MAX_PATH];

    if (!v->profileDir[0])
        return;

    wideFromUtf8(v->profileDir, wide, MAX_PATH);
    mbSetCookieJarPath(v->wv, wide);

    _snwprintf(wide, MAX_PATH, L"%hs\\LocalStorage", v->profileDir);
    wide[MAX_PATH - 1] = 0;
    mbSetLocalStorageFullPath(v->wv, wide);
}

static void viewInit(MbView* v)
{
    v->refs = 1;
    v->zoom = 100;
    v->visible = 1;
    v->uiThread = GetCurrentThreadId();

    copyStr(v->profileDir, sizeof(v->profileDir), g_profileDir);

    if (!v->downloadDir[0]) {
        _snprintf(v->downloadDir, sizeof(v->downloadDir) - 1, "%s\\downloads",
                  v->profileDir[0] ? v->profileDir : ".");
        v->downloadDir[sizeof(v->downloadDir) - 1] = 0;
        CreateDirectoryA(v->downloadDir, NULL);
    }
}

AgaveWebView* agave_webview_create(void* parent, int x, int y, int width, int height,
                                   const AgaveWebViewCallback* callbacks, void* user)
{
    MbView* v;

    ensureMbInit();

    if (width < 1)  width = 1;
    if (height < 1) height = 1;

    v = (MbView*)calloc(1, sizeof(*v));
    if (!v)
        return NULL;

    if (callbacks)
        v->cb = *callbacks;
    v->user = user;
    v->parent = (HWND)parent;
    viewInit(v);

    trace("create begin parent=%p rect=%d,%d %dx%d", (void*)v->parent, x, y, width, height);

    /* miniblink's control window shows the desktop through until its first
     * frame.  An opaque sibling underneath keeps a slow/never-loading page
     * from looking like a hole in the window. */
    registerBackdropClass();
    v->backdrop = CreateWindowExW(0, L"MiniShellViewBackdrop", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                  x, y, width, height, v->parent, NULL,
                                  GetModuleHandleW(NULL), NULL);

    /* The control paints nothing until its first frame, and the shell's own
     * surface is clipped around it (WS_CLIPCHILDREN) - so an untouched page
     * would be a hole straight through to the desktop.  Let the engine own the
     * placeholder: mbViewSettings.bgColor is what the view shows before any
     * document pixels exist, and it is also the page canvas afterwards. */
    {
        mbViewSettings settings;

        memset(&settings, 0, sizeof(settings));
        settings.size = sizeof(settings);
        settings.bgColor = 0xFFFFFFFF;   /* white page canvas, like Chrome */

        v->wv = mbCreateWebWindowEx(MB_WINDOW_TYPE_CONTROL, v->parent,
                                    x, y, width, height, &settings);
    }
    if (!v->wv) {
        agbLog(AGB_LOG_SHELL, "mbCreateWebWindow(CONTROL) failed: %lu",
               (unsigned long)GetLastError());
        if (v->backdrop)
            DestroyWindow(v->backdrop);
        free(v);
        return NULL;
    }

    v->hwnd = mbGetHostHWND(v->wv);
    trace("create: control wv=%p hwnd=%p (bg=white)", (void*)v->wv, (void*)v->hwnd);

    /* Explicit: the hole in the shell's surface must be filled by the engine,
     * not composited away. */
    mbSetTransparent(v->wv, FALSE);
    if (v->backdrop)
        SetWindowPos(v->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    v->rect.left = x;
    v->rect.top = y;
    v->rect.right = x + width;
    v->rect.bottom = y + height;

    SetPropW(v->hwnd, L"mbshell_view", (HANDLE)v);
    v->originalProc = (WNDPROC)SetWindowLongPtrW(v->hwnd, GWLP_WNDPROC,
                                                 (LONG_PTR)mbvViewProc);

    mbOnTitleChanged(v->wv, mbvOnTitle, v);
    mbOnURLChanged(v->wv, mbvOnUrl, v);
    mbOnLoadingFinish(v->wv, mbvOnLoadingFinish, v);
    mbOnDocumentReady(v->wv, mbvOnDocumentReady, v);
    mbOnNavigation(v->wv, mbvOnNavigation, v);
    mbOnMouseOverUrlChanged(v->wv, mbvOnStatus, v);
    mbOnConsole(v->wv, mbvOnConsole, v);
    mbOnAlertBox(v->wv, mbvOnAlert, v);
    mbOnConfirmBox(v->wv, mbvOnConfirm, v);
    mbOnPromptBox(v->wv, mbvOnPrompt, v);
    mbOnCreateView(v->wv, mbvOnCreateView, v);
    mbOnDestroy(v->wv, mbvOnDestroy, v);
    mbOnDownload(v->wv, mbvOnDownload, v);
    mbOnDownloadInBlinkThread(v->wv, mbvOnDownloadBlink, v);
    mbOnJsQuery(v->wv, mbvOnJsQuery, v);

    mbSetContextMenuEnabled(v->wv, FALSE);
    mbSetNavigationToNewWindowEnable(v->wv, TRUE);
    mbSetCspCheckEnable(v->wv, TRUE);
    mbSetCookieEnabled(v->wv, TRUE);

    /* Before the first navigation: without it, a machine whose only route out
     * is the Windows proxy never gets a response (and never an error). */
    ensureSystemProxy();
    if (g_systemProxySet) {
        mbSetProxy(v->wv, &g_systemProxy);
        trace("create: proxy %s:%u type=%d", g_systemProxy.hostname,
              (unsigned)g_systemProxy.port, (int)g_systemProxy.type);
    }

    viewApplyProfile(v);

    mbShowWindow(v->wv, 1);
    v->visible = 1;

    agbLog(AGB_LOG_SHELL, "mbv: create view=%p hwnd=%p", (void*)v, (void*)v->hwnd);
    trace("create done visible=%d", v->visible);
    return (AgaveWebView*)v;
}

AgaveWebView* agave_webview_create_popup(AgaveWebView* from, const char* url)
{
    MbView* src = (MbView*)from;
    MbView* v;

    if (!src)
        return NULL;

    v = (MbView*)agave_webview_create(src->parent, 0, 0,
                                      src->rect.right - src->rect.left,
                                      src->rect.bottom - src->rect.top,
                                      &src->cb, src->user);
    if (!v)
        return NULL;

    copyStr(v->downloadDir, sizeof(v->downloadDir), src->downloadDir);
    copyStr(v->profileDir, sizeof(v->profileDir), src->profileDir);
    if (url)
        agave_webview_navigate((AgaveWebView*)v, url);
    return (AgaveWebView*)v;
}

void agave_webview_destroy(AgaveWebView* view)
{
    MbView* v = (MbView*)view;

    if (!v) {
        return;
    }

    viewMarkDestroyed(v);

    if (v->hwnd && v->originalProc) {
        SetWindowLongPtrW(v->hwnd, GWLP_WNDPROC, (LONG_PTR)v->originalProc);
        RemovePropW(v->hwnd, L"mbshell_view");
        v->originalProc = NULL;
    }

    if (v->wv) {
        mbDestroyWebView(v->wv);
        v->wv = NULL;
        v->hwnd = NULL;
    }

    if (v->backdrop) {
        DestroyWindow(v->backdrop);
        v->backdrop = NULL;
    }

    viewRelease(v);
}

void agave_webview_set_user(AgaveWebView* view, void* user)
{
    MbView* v = (MbView*)view;
    if (v)
        v->user = user;
}

void agave_webview_set_rect(AgaveWebView* view, int x, int y, int width, int height)
{
    MbView* v = (MbView*)view;

    if (!v || !v->hwnd)
        return;
    if (width < 1)  width = 1;
    if (height < 1) height = 1;

    v->rect.left = x;
    v->rect.top = y;
    v->rect.right = x + width;
    v->rect.bottom = y + height;

    if (v->backdrop)
        SetWindowPos(v->backdrop, NULL, x, y, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    /* mbResize handles miniblink's internal size; MoveWindow places the real
     * child window (mbResize on a window-mode view only changes w/h). */
    SetWindowPos(v->hwnd, NULL, x, y, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    mbResize(v->wv, width, height);
}

void* agave_webview_hwnd(AgaveWebView* view)
{
    return view ? ((MbView*)view)->hwnd : NULL;
}

static void htmlEscapeAppend(char* out, size_t cap, size_t* used, const char* text)
{
    size_t n = *used;

    for (; text && *text && n + 8 < cap; ++text) {
        switch (*text) {
        case '<':  out[n++] = '&'; out[n++] = 'l'; out[n++] = 't'; out[n++] = ';'; break;
        case '>':  out[n++] = '&'; out[n++] = 'g'; out[n++] = 't'; out[n++] = ';'; break;
        case '&':  out[n++] = '&'; out[n++] = 'a'; out[n++] = 'm'; out[n++] = 'p'; out[n++] = ';'; break;
        case '"':  out[n++] = '&'; out[n++] = 'q'; out[n++] = 'u'; out[n++] = 'o'; out[n++] = 't'; out[n++] = ';'; break;
        default:   out[n++] = *text; break;
        }
    }
    out[n] = 0;
    *used = n;
}

/* minishell://downloads: the old engine had a built-in page.  This backend renders
 * the same idea from its own job table so Ctrl+J still goes somewhere real. */
static void loadDownloadsPage(MbView* v)
{
    size_t cap = 65536, used = 0;
    char* html = (char*)malloc(cap);

    if (!html)
        return;

    used += (size_t)_snprintf(html + used, cap - used,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>Downloads</title><style>"
        "body{font:14px/1.5 'Segoe UI',system-ui,sans-serif;margin:0;padding:32px 40px;"
        "background:#fff;color:#202124}"
        "@media (prefers-color-scheme:dark){body{background:#202124;color:#e8eaed}"
        "td{color:#e8eaed!important}thead th{color:#9aa0a6!important}"
        "table,th,td{border-color:#3c4043!important}}"
        "h1{font-size:20px;margin:0 0 4px}p{color:#5f6368;margin:0 0 24px}"
        "table{border-collapse:collapse;width:100%%}"
        "th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #e8eaed;"
        "vertical-align:top;word-break:break-all}"
        "th{font-weight:600;color:#5f6368;font-size:12px;text-transform:uppercase}"
        "code{font:12px Consolas,monospace}"
        "</style></head><body><h1>Downloads</h1>"
        "<p>Files are saved to the shell's download directory.</p>"
        "<table><thead><tr><th>File</th><th>State</th><th>Size</th><th>Source</th>"
        "</tr></thead><tbody>");

    ensureDownloadLock();
    EnterCriticalSection(&g_dlLock);
    {
        MbDownload* d = g_downloads;
        if (!d) {
            used += (size_t)_snprintf(html + used, cap - used,
                "<tr><td colspan=\"4\">No downloads yet.</td></tr>");
        }
        for (; d && used + 512 < cap; d = d->next) {
            char size[64];
            const char* state = !d->done ? "Downloading"
                              : (d->error ? "Failed" : "Done");

            if (d->total > 0)
                _snprintf(size, sizeof(size), "%.1f / %.1f MB",
                          d->received / 1048576.0, d->total / 1048576.0);
            else
                _snprintf(size, sizeof(size), "%.1f MB", d->received / 1048576.0);
            size[sizeof(size) - 1] = 0;

            used += (size_t)_snprintf(html + used, cap - used,
                "<tr><td><code>");
            htmlEscapeAppend(html, cap, &used, d->path);
            used += (size_t)_snprintf(html + used, cap - used,
                "</code></td><td>%s</td><td>%s</td><td><code>", state, size);
            htmlEscapeAppend(html, cap, &used, d->url);
            used += (size_t)_snprintf(html + used, cap - used, "</code></td></tr>");
        }
    }
    LeaveCriticalSection(&g_dlLock);

    used += (size_t)_snprintf(html + used, cap - used,
        "</tbody></table></body></html>");
    html[used < cap ? used : cap - 1] = 0;

    mbLoadHtmlWithBaseUrl(v->wv, html, "minishell://downloads");
    postEvent(v, MBV_EV_PROGRESS, NULL, NULL, 100, 1);
    free(html);
}

/* minishell://settings: a static page whose controls talk to the shell over
 * agavePostMessage ("__agb_settings_*__"); the shell answers with
 * "__agb_settings__key=value" through window.onAgaveMessage, one value per
 * message, so the page needs no JSON parser. The palette follows
 * prefers-color-scheme until the shell sends its own theme choice. */
static const char kSettingsHtml[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
"<meta name=\"color-scheme\" content=\"light dark\">"
"<title>Settings</title><style>"
":root{--bg:#fff;--fg:#202124;--muted:#5f6368;--line:#e8eaed;--card:#f8f9fa;--accent:#1a73e8}"
"@media (prefers-color-scheme:dark){:root{--bg:#202124;--fg:#e8eaed;--muted:#9aa0a6;"
"--line:#3c4043;--card:#292a2d;--accent:#8ab4f8}}"
"html[data-theme=light]{--bg:#fff;--fg:#202124;--muted:#5f6368;--line:#e8eaed;"
"--card:#f8f9fa;--accent:#1a73e8}"
"html[data-theme=dark]{--bg:#202124;--fg:#e8eaed;--muted:#9aa0a6;--line:#3c4043;"
"--card:#292a2d;--accent:#8ab4f8}"
"*{box-sizing:border-box}"
"body{font:16px/1.65 'Segoe UI',system-ui,sans-serif;margin:0;padding:30px 36px;"
"background:var(--bg);color:var(--fg)}"
"h1{font-size:24px;margin:0 0 4px}"
"p.sub{color:var(--muted);margin:0 0 22px}"
"section{border:1px solid var(--line);border-radius:10px;padding:14px 18px;"
"margin:0 0 16px;max-width:720px;background:var(--card)}"
"h2{font-size:14px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);"
"margin:0 0 10px;font-weight:600}"
".row{display:flex;align-items:center;justify-content:space-between;gap:16px;padding:8px 0}"
".row+.row{border-top:1px solid var(--line)}"
"label.opt{display:inline-flex;align-items:center;gap:6px;margin-right:18px;cursor:pointer}"
"select{font:inherit;padding:4px 8px;border-radius:6px;border:1px solid var(--line);"
"background:var(--bg);color:var(--fg)}"
/* Custom dropdown: the engine's native <select> popup needs two clicks (the
 * first only focuses) and opens at the wrong place, so the page draws its own
 * list right under the control. */
".dd{position:relative;min-width:158px}"
".dd-btn{width:100%;text-align:left;padding:6px 30px 6px 10px;border-radius:6px;"
"border:1px solid var(--line);background:var(--bg);color:var(--fg);cursor:pointer;"
"position:relative;font:inherit}"
".dd-btn::after{content:'';position:absolute;right:11px;top:50%;width:6px;height:6px;"
"border-right:2px solid currentColor;border-bottom:2px solid currentColor;"
"transform:translateY(-70%) rotate(45deg)}"
".dd-menu{position:absolute;left:0;top:calc(100% + 4px);min-width:100%;z-index:20;"
"display:none;padding:4px;border-radius:8px;border:1px solid var(--line);"
"background:var(--bg);box-shadow:0 8px 24px rgba(0,0,0,.18)}"
".dd.open .dd-menu{display:block}"
".dd-item{padding:5px 10px;border-radius:5px;cursor:pointer;white-space:nowrap}"
".dd-item:hover{background:var(--card)}"
".dd-item[aria-selected=true]{color:var(--accent);font-weight:600}"
"button{font:inherit;padding:6px 14px;border-radius:6px;border:1px solid var(--line);"
"background:var(--bg);color:var(--fg);cursor:pointer}"
"button:hover{border-color:var(--accent);color:var(--accent)}"
"code{font:14px Consolas,monospace;color:var(--muted);word-break:break-all}"
"</style></head><body>"
"<h1>Settings</h1><p class=\"sub\">Changes apply immediately and are saved to the profile.</p>"

"<section><h2>Appearance</h2><div class=\"row\"><div>"
"<label class=\"opt\"><input type=\"radio\" name=\"theme\" value=\"system\">System</label>"
"<label class=\"opt\"><input type=\"radio\" name=\"theme\" value=\"light\">Light</label>"
"<label class=\"opt\"><input type=\"radio\" name=\"theme\" value=\"dark\">Dark</label>"
"</div></div></section>"

"<section><h2>Search</h2><div class=\"row\"><div>Engine used by the address bar</div>"
"<div class=\"dd\" id=\"searchDD\" data-value=\"baidu\">"
"<button type=\"button\" class=\"dd-btn\">Baidu</button>"
"<div class=\"dd-menu\">"
"<div class=\"dd-item\" data-value=\"baidu\">Baidu</div>"
"<div class=\"dd-item\" data-value=\"bing\">Bing</div>"
"<div class=\"dd-item\" data-value=\"google\">Google</div>"
"<div class=\"dd-item\" data-value=\"duckduckgo\">DuckDuckGo</div>"
"</div></div></div></section>"

"<section><h2>Pages</h2>"
"<div class=\"row\"><div>Default zoom for new tabs</div>"
"<div class=\"dd\" id=\"zoomDD\" data-value=\"100\">"
"<button type=\"button\" class=\"dd-btn\">100</button>"
"<div class=\"dd-menu\">"
"<div class=\"dd-item\" data-value=\"50\">50</div>"
"<div class=\"dd-item\" data-value=\"67\">67</div>"
"<div class=\"dd-item\" data-value=\"75\">75</div>"
"<div class=\"dd-item\" data-value=\"80\">80</div>"
"<div class=\"dd-item\" data-value=\"90\">90</div>"
"<div class=\"dd-item\" data-value=\"100\">100</div>"
"<div class=\"dd-item\" data-value=\"110\">110</div>"
"<div class=\"dd-item\" data-value=\"125\">125</div>"
"<div class=\"dd-item\" data-value=\"150\">150</div>"
"<div class=\"dd-item\" data-value=\"175\">175</div>"
"<div class=\"dd-item\" data-value=\"200\">200</div>"
"</div></div></div>"
"<div class=\"row\"><div>Reopen the tabs from the last session</div>"
"<input type=\"checkbox\" id=\"restore\"></div></section>"

"<section><h2>Data</h2>"
"<div class=\"row\"><div>History: <span id=\"history\">0</span> entries</div>"
"<button id=\"clearHistory\">Clear history</button></div>"
"<div class=\"row\"><div>Downloads folder<br><code id=\"downloads\"></code></div>"
"<button id=\"openDownloads\">Open</button></div></section>"

"<section><h2>About</h2><div class=\"row\"><div>MiniShell, <span id=\"version\"></span>"
"<br><code id=\"profile\"></code></div></div></section>"

"<script>"
"function post(t){try{agavePostMessage(t)}catch(e){}}"
"function set(k,v){post('__agb_settings_set__'+k+'='+v)}"
"function ddClose(){var all=document.querySelectorAll('.dd.open');"
"for(var i=0;i<all.length;i++)all[i].classList.remove('open');}"
"function ddSelect(id,v){"
"var dd=document.getElementById(id);if(!dd)return;"
"dd.setAttribute('data-value',v);"
"var items=dd.querySelectorAll('.dd-item'),label='';"
"for(var i=0;i<items.length;i++){"
"var on=items[i].getAttribute('data-value')===String(v);"
"items[i].setAttribute('aria-selected',on?'true':'false');"
"if(on)label=items[i].textContent;}"
"if(label)dd.querySelector('.dd-btn').textContent=label;}"
"function ddWire(id,key){"
"var dd=document.getElementById(id);if(!dd)return;"
"var btn=dd.querySelector('.dd-btn'),menu=dd.querySelector('.dd-menu');"
"btn.addEventListener('click',function(e){"
"e.stopPropagation();var was=dd.classList.contains('open');ddClose();"
"if(!was)dd.classList.add('open');});"
"menu.addEventListener('click',function(e){"
"var it=e.target;"
"while(it&&it!==menu&&!it.classList.contains('dd-item'))it=it.parentNode;"
"if(!it||it===menu)return;"
"var v=it.getAttribute('data-value');"
"ddSelect(id,v);ddClose();set(key,v);});"
"}"
"function show(k,v){"
"if(k==='theme'){var r=document.querySelector('input[name=theme][value='+v+']');"
"if(r)r.checked=true;document.documentElement.setAttribute('data-theme',v==='system'?'':v);}"
"else if(k==='search'){ddSelect('searchDD',v);}"
"else if(k==='zoom'){ddSelect('zoomDD',v);}"
"else if(k==='restore'){document.getElementById('restore').checked=(''+v==='1');}"
"else if(k==='history'){document.getElementById('history').textContent=v;}"
"else if(k==='downloads'){document.getElementById('downloads').textContent=v;}"
"else if(k==='profile'){document.getElementById('profile').textContent=v;}"
"else if(k==='version'){document.getElementById('version').textContent=v;}"
"}"
"window.onAgaveMessage=function(m){"
"if(m.indexOf('__agb_settings__')!==0)return;"
"var p=m.slice(16),i=p.indexOf('=');"
"if(i>0)show(p.slice(0,i),p.slice(i+1));"
"};"
/* The shell injects agavePostMessage on document ready, i.e. after this script
 * has run, so the first request has to wait for it instead of firing once and
 * giving up (an inline post at parse time silently does nothing). */
"function boot(){"
"if(typeof agavePostMessage!=='function'){setTimeout(boot,60);return;}"
"post('__agb_settings_get__');}"
"function wire(){"
"ddWire('searchDD','search');"
"ddWire('zoomDD','zoom');"
"document.addEventListener('click',ddClose);"
"document.addEventListener('keydown',function(e){if(e.key==='Escape')ddClose();});"
"var t=document.querySelectorAll('input[name=theme]');"
"for(var i=0;i<t.length;i++)t[i].addEventListener('change',function(){set('theme',this.value)});"
"document.getElementById('restore').addEventListener('change',function(){"
"set('restore',this.checked?1:0)});"
"document.getElementById('clearHistory').addEventListener('click',function(){"
"if(confirm('Clear the browsing history?'))post('__agb_settings_do__clear-history')});"
"document.getElementById('openDownloads').addEventListener('click',function(){"
"post('__agb_settings_do__open-downloads')});"
"}"
"if(document.readyState==='loading')"
"document.addEventListener('DOMContentLoaded',wire);"
"else wire();"
"boot();"
"</script></body></html>";

static void loadSettingsPage(MbView* v)
{
    mbLoadHtmlWithBaseUrl(v->wv, kSettingsHtml, "minishell://settings");
    postEvent(v, MBV_EV_PROGRESS, NULL, NULL, 100, 1);
}

static void markLoading(MbView* v, int percent)
{
    if (!v || v->destroying)
        return;
    v->loading = 1;
    v->progress = percent;
    if (v->cb.on_progress)
        v->cb.on_progress((AgaveWebView*)v, percent, 0, v->user);
}

void agave_webview_navigate(AgaveWebView* view, const char* url)
{
    MbView* v = (MbView*)view;

    if (!v || v->destroying || !url)
        return;

    copyStr(v->url, sizeof(v->url), url);
    markLoading(v, 5);
    trace("navigate url=%s", url);

    if (!_strnicmp(url, "minishell://downloads", 21)) {
        loadDownloadsPage(v);
        return;
    }

    if (!_strnicmp(url, "minishell://settings", 20)) {
        loadSettingsPage(v);
        return;
    }

    if (!_strnicmp(url, "minishell://", 12)) {
        mbLoadHtmlWithBaseUrl(v->wv,
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>Not found</title></head><body style=\"font:14px 'Segoe UI',sans-serif;"
            "padding:32px\"><h1>Page not found</h1><p>This internal page does not exist "
            "in the miniblink backend.</p></body></html>", "about:blank");
        postEvent(v, MBV_EV_PROGRESS, NULL, NULL, 100, 1);
        return;
    }

    mbLoadURL(v->wv, url);
    trace("navigate: mbLoadURL queued url=%s", url);
}

void agave_webview_load_html(AgaveWebView* view, const char* html, const char* base_url)
{
    MbView* v = (MbView*)view;
    if (!v || v->destroying || !html)
        return;
    /* load_html does not always deliver loadingFinish (it may commit before
     * the URL callback posts), so close the progress state explicitly. */
    mbLoadHtmlWithBaseUrl(v->wv, html, base_url ? base_url : "about:blank");
    v->committed = 1;   /* it is a document even when no URL callback arrives */
    postEvent(v, MBV_EV_PROGRESS, NULL, NULL, 100, 1);
}

void agave_webview_back(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (v && !v->destroying) {
        markLoading(v, 5);
        mbGoBack(v->wv);
    }
}

void agave_webview_forward(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (v && !v->destroying) {
        markLoading(v, 5);
        mbGoForward(v->wv);
    }
}

void agave_webview_reload(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (v && !v->destroying) {
        trace("reload (engine url=%s loading=%d progress=%d committed=%d)",
              v->url, v->loading, v->progress, v->committed);

        /* mbReload() reloads the *committed* document.  Called while the first
         * navigation is still in flight it drops that load and starts nothing:
         * the view stays blank at 5% forever (no URL callback, no
         * loadingFinish).  Restart the navigation instead. */
        if (!v->committed) {
            char target[sizeof(v->url)];

            copyStr(target, sizeof(target), v->url);
            trace("reload: no document yet, restarting navigation url=%s", target);
            if (target[0])
                agave_webview_navigate((AgaveWebView*)v, target);
            return;
        }

        markLoading(v, 5);
        mbReload(v->wv);
        trace("reload: mbReload returned");
    }
}

void agave_webview_stop(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (v && !v->destroying) {
        trace("stop");
        mbStopLoading(v->wv);
    }
}

void agave_webview_interrupt(AgaveWebView* view)
{
    (void)view;
}

int agave_webview_busy(AgaveWebView* view, int minMs)
{
    (void)view;
    (void)minMs;
    return 0;
}

int agave_webview_eval(AgaveWebView* view, const char* script)
{
    MbView* v = (MbView*)view;
    mbWebFrameHandle frame;

    if (!v || !script)
        return AGW_CMD_ERR_BAD_ARGS;
    if (v->destroying)
        return AGW_CMD_ERR_NO_VIEW;

    frame = mbWebFrameGetMainFrame(v->wv);
    mbRunJs(v->wv, frame, script, FALSE, NULL, NULL, NULL);
    return AGW_CMD_OK;
}

int agave_webview_editor_command(AgaveWebView* view, const char* command)
{
    MbView* v = (MbView*)view;

    if (!v || !command)
        return 0;

    if (!_stricmp(command, "Copy"))          mbEditorCopy(v->wv);
    else if (!_stricmp(command, "Cut"))      mbEditorCut(v->wv);
    else if (!_stricmp(command, "Paste"))    mbEditorPaste(v->wv);
    else if (!_stricmp(command, "SelectAll")) mbEditorSelectAll(v->wv);
    else if (!_stricmp(command, "Unselect")) mbEditorUnSelect(v->wv);
    else if (!_stricmp(command, "Delete"))   mbEditorDelete(v->wv);
    else if (!_stricmp(command, "Undo"))     mbEditorUndo(v->wv);
    else if (!_stricmp(command, "Redo"))     mbEditorRedo(v->wv);
    else return 0;
    return 1;
}

void agave_webview_set_zoom(AgaveWebView* view, int percent)
{
    MbView* v = (MbView*)view;

    if (!v || percent <= 0)
        return;
    v->zoom = percent;
    mbSetZoomFactor(v->wv, (float)percent / 100.0f);
}

int agave_webview_zoom(AgaveWebView* view)
{
    return view ? ((MbView*)view)->zoom : 100;
}

void agave_webview_set_device_scale(AgaveWebView* view, int percent)
{
    MbView* v = (MbView*)view;
    if (!v)
        return;
    /* miniblink follows the process DPI (mbEnableHighDPISupport); the shell's
     * device scale is remembered so a future backend can apply it. */
    v->deviceScale = percent;
}

int agave_webview_device_scale(AgaveWebView* view)
{
    return view ? ((MbView*)view)->deviceScale : 100;
}

void agave_webview_set_visible(AgaveWebView* view, int visible)
{
    MbView* v = (MbView*)view;

    if (!v || v->destroying)
        return;
    v->visible = visible ? 1 : 0;
    mbShowWindow(v->wv, v->visible);
    if (v->backdrop)
        ShowWindow(v->backdrop, v->visible ? SW_SHOWNA : SW_HIDE);
}

void agave_webview_request_frame(AgaveWebView* view)
{
    (void)view;
}

int agave_webview_has_frame(AgaveWebView* view)
{
    return view ? 1 : 0;
}

const char* agave_webview_url(AgaveWebView* view)
{
    return view ? ((MbView*)view)->url : "";
}

const char* agave_webview_title(AgaveWebView* view)
{
    return view ? ((MbView*)view)->title : "";
}

int agave_webview_paint_to(AgaveWebView* view, void* hdc, int width, int height)
{
    MbView* v = (MbView*)view;
    mbRect rc;
    HDC src;

    if (!v || !hdc || !v->wv || v->destroying)
        return 0;

    /* Window-mode views keep their backing store in a DC, which is valid even
     * when the window is hidden - exactly what the headless probe needs.
     * PrintWindow on a hidden miniblink control just returns black. */
    src = mbGetLockedViewDC(v->wv);
    if (!src)
        return 0;

    memset(&rc, 0, sizeof(rc));
    mbGetSize(v->wv, &rc);
    if (rc.w <= 0 || rc.h <= 0) {
        rc.w = (int)(v->rect.right - v->rect.left);
        rc.h = (int)(v->rect.bottom - v->rect.top);
    }

    if (rc.w > 0 && rc.h > 0) {
        SetStretchBltMode((HDC)hdc, HALFTONE);
        SetBrushOrgEx((HDC)hdc, 0, 0, NULL);
        StretchBlt((HDC)hdc, 0, 0, width, height, src, 0, 0, rc.w, rc.h, SRCCOPY);
    }

    mbUnlockViewDC(v->wv);
    return 1;
}

void agave_webview_set_focus(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (!v || v->destroying)
        return;

    /* mbSetFocus only tells the engine who owns keyboard handling; the Win32
     * focus has to land on the control's HWND as well, otherwise the keys keep
     * going to the shell window and the page looks focused but receives
     * nothing. */
    if (v->hwnd && GetFocus() != v->hwnd)
        SetFocus(v->hwnd);
    mbSetFocus(v->wv);
}

void agave_webview_kill_focus(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (!v || v->destroying)
        return;

    /* Focus in this control is the child HWND's: leave it, and tell the engine
     * so its caret/IME state stops believing it is focused. */
    if (v->hwnd && GetFocus() == v->hwnd)
        SetFocus(NULL);
    mbKillFocus(v->wv);
}

int agave_webview_context_command(AgaveWebView* view, int item_id)
{
    MbView* v = (MbView*)view;

    if (!v || v->destroying || item_id == 0)
        return 0;

    /* The engine owns these: it targets the frame the menu was opened on and
     * keeps its own state (undo stack, clipboard, caret) consistent. */
    mbExecuteContextMenu(v->wv, item_id);
    agbLog(AGB_LOG_NAV, "context command item=0x%x -> engine", (unsigned)item_id);
    return 1;
}

void agave_webview_set_probe_modifiers(unsigned modifiers)
{
    (void)modifiers;
}

void agave_webview_set_input_capture(AgaveWebView* view, int enabled,
                                     AgaveWebViewInputFn fn, void* user)
{
    MbView* v = (MbView*)view;

    if (!v || v->destroying)
        return;

    v->inputFn = enabled ? fn : NULL;
    v->inputUser = enabled ? user : NULL;
    v->inputCapture = enabled ? 1 : 0;

    agbLog(AGB_LOG_NAV, "input capture %s", enabled ? "on" : "off");
}

void agave_webview_post_message(AgaveWebView* view, const char* message)
{
    MbView* v = (MbView*)view;
    char quoted[4096];
    char script[4300];

    if (!v || !message)
        return;

    jsQuote(message, quoted, sizeof(quoted));
    _snprintf(script, sizeof(script) - 1,
              "window.onAgaveMessage && window.onAgaveMessage(%s);", quoted);
    script[sizeof(script) - 1] = 0;
    agave_webview_eval(view, script);
}

/* JS find: CSS Highlight API is not guaranteed across miniblink builds, so
 * matches are wrapped in <mark data-mb-find> and removed again on the next
 * query.  State lives on window.__mbFindState. */
static const char kFindJs[] =
    "(function(){"
    " var q=%s, fwd=%s, cs=%s;"
    " var st=window.__mbFindState;"
    " function clear(){"
    "  if(!st) return;"
    "  for(var i=0;i<st.marks.length;i++){var m=st.marks[i];"
    "   if(m&&m.parentNode){var p=m.parentNode;"
    "    while(m.firstChild)p.insertBefore(m.firstChild,m);"
    "    p.removeChild(m);p.normalize();}}"
    "  st.marks=[];st.index=-1;st.text='';"
    " }"
    " if(!st)st=window.__mbFindState={text:'',cs:-1,marks:[],index:-1};"
    " if(!q){clear();try{agavePostMessage('__agb_findcount__0/0');}catch(e){}return;}"
    " if(st.text!==q||st.cs!==cs){clear();st.text=q;st.cs=cs;}"
    /* Marks can disappear behind our back: a SPA replaces the subtree, the page
     * re-renders, an infinite list recycles nodes. A detached mark has no
     * position, so counting it (or scrolling to it) is wrong - drop those and
     * let the scan below re-create the real ones. */
    " if(st.marks.length){"
    "  var docEl=document.documentElement;"
    "  for(var d=st.marks.length-1;d>=0;d--)"
    "   if(!docEl.contains(st.marks[d]))st.marks.splice(d,1);"
    "  if(!st.marks.length)st.index=-1;"
    " }"
    " if(!st.marks.length){"
    "  var needle=cs?q:q.toLowerCase();"
    "  var walker=document.createTreeWalker(document.body||document.documentElement,NodeFilter.SHOW_TEXT,"
    "   {acceptNode:function(n){"
    "    if(!n.nodeValue||!n.nodeValue.length)return NodeFilter.FILTER_REJECT;"
    "    var p=n.parentNode;if(!p)return NodeFilter.FILTER_REJECT;"
    "    var t=p.nodeName?p.nodeName.toLowerCase():'';"
    "    if(t==='script'||t==='style'||t==='noscript'||t==='textarea'||t==='mark')return NodeFilter.FILTER_REJECT;"
    "    return NodeFilter.FILTER_ACCEPT;}});"
    "  var nodes=[],n;while((n=walker.nextNode()))nodes.push(n);"
    "  for(var k=0;k<nodes.length;k++){"
    "   var node=nodes[k],text=node.nodeValue,hay=cs?text:text.toLowerCase();"
    "   var from=0,at,pieces=[];"
    "   while((at=hay.indexOf(needle,from))>=0){"
    "    if(at>from)pieces.push(document.createTextNode(text.slice(from,at)));"
    "    var mark=document.createElement('mark');"
    "    mark.setAttribute('data-mb-find','1');"
    "    mark.style.backgroundColor='#ffd54a';mark.style.color='#000';"
    "    mark.textContent=text.slice(at,at+needle.length);"
    "    pieces.push(mark);st.marks.push(mark);from=at+needle.length;}"
    "   if(pieces.length){var frag=document.createDocumentFragment();"
    "    for(var j=0;j<pieces.length;j++)frag.appendChild(pieces[j]);"
    "    if(from<text.length)frag.appendChild(document.createTextNode(text.slice(from)));"
    "    node.parentNode.replaceChild(frag,node);}}"
    " }"
    " if(!st.marks.length){try{agavePostMessage('__agb_findcount__0/0');}catch(e){}return;}"
    " st.index=st.index<0?(fwd?0:st.marks.length-1):(st.index+(fwd?1:-1)+st.marks.length)%%st.marks.length;"
    " for(var i=0;i<st.marks.length;i++)st.marks[i].style.outline=(i===st.index)?'2px solid #e8a000':'none';"
    /* Bring the hit into view. scrollIntoView() is the answer for anything that
     * is laid out in the flow, but it silently does nothing when the match sits
     * in a fixed/sticky subtree (the page has nothing to scroll), so the window
     * is centred on the element's own offset as a fallback. */
    " var cur=st.marks[st.index];"
    " if(cur){"
    "  var before=window.scrollY||window.pageYOffset||0;"
    "  try{cur.scrollIntoView({block:'center',inline:'nearest'});}"
    "  catch(e){try{cur.scrollIntoView();}catch(e2){}}"
    "  var after=window.scrollY||window.pageYOffset||0;"
    "  if(after===before){"
    "   var rc=cur.getBoundingClientRect();"
    "   var vh=window.innerHeight||document.documentElement.clientHeight||0;"
    "   if(rc.top<0||rc.bottom>vh){"
    "    var y=before+rc.top-Math.max(0,(vh-rc.height)/2);"
    "    if(y<0)y=0;"
    "    window.scrollTo(0,y);"
    "   }"
    "  }"
    " }"
    " try{agavePostMessage('__agb_findcount__'+(st.index+1)+'/'+st.marks.length);}catch(e){}"
    "})();";

int agave_webview_search(AgaveWebView* view, const char* text, int forward, int case_sensitive)
{
    MbView* v = (MbView*)view;
    char quoted[2048];
    char script[sizeof(kFindJs) + 2300];

    if (!v || v->destroying)
        return 0;

    jsQuote(text ? text : "", quoted, sizeof(quoted));
    _snprintf(script, sizeof(script) - 1, kFindJs, quoted,
              forward ? "1" : "0", case_sensitive ? "1" : "0");
    script[sizeof(script) - 1] = 0;

    agave_webview_eval(view, script);
    return 1;
}

/* mbCanGoBackOrForward faults when called before the view has a page (it
 * dereferences an internal navigation controller), and the shell calls this
 * while laying out chrome.  mbOnURLChanged already hands us both flags, so
 * mirror those and never re-enter the engine from the UI path. */
int agave_webview_can_back(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (!v || v->destroying)
        return 0;
    return v->canBack;
}

int agave_webview_can_forward(AgaveWebView* view)
{
    MbView* v = (MbView*)view;
    if (!v || v->destroying)
        return 0;
    return v->canForward;
}

void agave_webview_set_user_agent(AgaveWebView* view, const char* user_agent)
{
    MbView* v = (MbView*)view;
    if (v && user_agent && user_agent[0])
        mbSetUserAgent(v->wv, user_agent);
}

void agave_webview_set_download_dir(AgaveWebView* view, const char* directory)
{
    MbView* v = (MbView*)view;
    if (!v || !directory || !directory[0])
        return;
    copyStr(v->downloadDir, sizeof(v->downloadDir), directory);
    CreateDirectoryA(v->downloadDir, NULL);
}

void agave_webview_cancel_download(AgaveWebView* view, int id)
{
    (void)view;
    (void)id;
}

void agave_webview_set_log(const char* path_or_null)
{
    agbLogSetFile(path_or_null);
}

void agave_webview_set_profile_dir(const char* utf8_dir)
{
    copyStr(g_profileDir, sizeof(g_profileDir), utf8_dir);
}

void agave_webview_shutdown(void)
{
    mbUninit();
    agbLogFlush();
}


