/* crash_page.c - the "MiniShell crashed" window.
 *
 * A crashed process cannot be trusted to render anything: the fault may be in
 * the engine, on a worker thread, or with the heap half dead. So the exception
 * handlers only write the report and start `minishell.exe --crash-page <report>`
 * in a fresh process; this file is that process. It is a minimal host (one
 * window, one control, one local page) and touches nothing from the user's
 * profile.
 */
#include "crash_page.h"

#include "agave_webview.h"
#include "shell_ops.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AgaveWebView* s_view;
static HWND           s_wnd;
static char*          s_report;      /* UTF-8 text shown and copied          */
static size_t         s_reportLen;

/* ------------------------------------------------------------------------- */
/* page                                                                      */
/* ------------------------------------------------------------------------- */

static char* readFile(const char* path, size_t* outLen)
{
    FILE* file;
    long size;
    char* data;

    *outLen = 0;
    file = fopen(path, "rb");
    if (!file)
        return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size <= 0 || size > 512 * 1024) {
        fclose(file);
        return NULL;
    }

    rewind(file);
    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }

    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }

    data[size] = 0;
    fclose(file);
    *outLen = (size_t)size;
    return data;
}

static char* htmlEscape(const char* text, size_t len)
{
    char* out;
    size_t i;
    size_t o = 0;

    out = (char*)malloc(len * 6 + 1);
    if (!out)
        return NULL;

    for (i = 0; i < len; ++i) {
        switch (text[i]) {
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default: out[o++] = text[i]; break;
        }
    }

    out[o] = 0;
    return out;
}

static char* buildHtml(const char* report, size_t len)
{
    static const char head[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>MiniShell crash report</title>"
        "<style>"
        ":root{color-scheme:light dark}"
        "*{box-sizing:border-box}"
        "html,body{margin:0;height:100%}"
        "body{display:flex;flex-direction:column;height:100%;background:#f3f4f6;"
        "color:#16181d;font:14px/1.55 'Segoe UI',system-ui,-apple-system,sans-serif}"
        "header{padding:18px 22px 12px;background:#fff;border-bottom:1px solid #e3e5e8}"
        "h1{margin:0 0 6px;font-size:20px;font-weight:600}"
        "p{margin:0 0 12px;max-width:920px;color:#4b5563}"
        ".row{display:flex;align-items:center;gap:12px}"
        "button{font:inherit;padding:7px 16px;border:1px solid #2563eb;border-radius:6px;"
        "background:#2563eb;color:#fff;cursor:pointer}"
        "button:hover{background:#1d4ed8}"
        "#status{color:#047857;font-size:13px}"
        "pre{flex:1;margin:0;padding:18px 22px 40px;overflow:auto;white-space:pre;"
        "font:12.5px/1.5 Consolas,'Cascadia Mono',monospace}"
        "@media (prefers-color-scheme:dark){"
        "html,body{background:#111318;color:#e5e7eb}"
        "header{background:#1a1d24;border-color:#2a2f3a}"
        "p{color:#9ca3af}"
        "pre{color:#d1d5db}"
        "}"
        "</style></head><body>"
        "<header><h1>MiniShell crashed</h1>"
        "<p>Copy this report and send it to the maintainer. It contains no browsing "
        "data: no URLs, no page titles, no file paths, no account or machine names.</p>"
        "<div class=\"row\"><button id=\"copy\" onclick=\"copyReport()\">Copy report</button>"
        "<span id=\"status\"></span></div></header>"
        "<pre id=\"report\">";

    static const char tail[] =
        "</pre>"
        "<script>"
        "function copyReport(){"
        "  try{ agavePostMessage('__agb_crash_copy__'); }"
        "  catch(e){ document.getElementById('status').textContent="
        "    'Select the report and press Ctrl+C to copy it.'; }"
        "}"
        "</script></body></html>";

    char* escaped;
    char* html;
    size_t size;

    escaped = htmlEscape(report, len);
    if (!escaped)
        return NULL;

    size = sizeof(head) - 1 + strlen(escaped) + sizeof(tail) - 1 + 1;
    html = (char*)malloc(size);
    if (html) {
        memcpy(html, head, sizeof(head) - 1);
        strcpy(html + sizeof(head) - 1, escaped);
        strcat(html, tail);
    }

    free(escaped);
    return html;
}

/* ------------------------------------------------------------------------- */
/* window                                                                    */
/* ------------------------------------------------------------------------- */

static void copyReport(void)
{
    HGLOBAL buffer;
    void* dst;
    int wideLen;

    if (!s_report || s_reportLen == 0)
        return;

    wideLen = MultiByteToWideChar(CP_UTF8, 0, s_report, (int)s_reportLen, NULL, 0);
    if (wideLen <= 0)
        return;

    buffer = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wideLen + 1) * sizeof(wchar_t));
    if (!buffer)
        return;

    dst = GlobalLock(buffer);
    if (!dst) {
        GlobalFree(buffer);
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, s_report, (int)s_reportLen, (wchar_t*)dst, wideLen);
    ((wchar_t*)dst)[wideLen] = 0;
    GlobalUnlock(buffer);

    if (!OpenClipboard(s_wnd)) {
        GlobalFree(buffer);
        return;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, buffer))
        GlobalFree(buffer);
    CloseClipboard();
}

static void onCrashMessage(AgaveWebView* view, const char* message, void* user)
{
    (void)user;

    if (!message || strcmp(message, "__agb_crash_copy__") != 0)
        return;

    fprintf(stderr, "[crash-page] copy requested, %u bytes\n", (unsigned)s_reportLen);

    if (getenv("AGB_CRASH_PAGE_TEST")) {
        /* The headless test must not overwrite the developer's clipboard. */
        fprintf(stderr, "[crash-page] copy suppressed (AGB_CRASH_PAGE_TEST)\n");
    } else {
        copyReport();
    }
    agave_webview_eval(view,
        "try{document.getElementById('status').textContent="
        "'Copied to clipboard.';}catch(e){}");
}

static LRESULT CALLBACK crashPageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (s_view) {
            int w = LOWORD(lp);
            int h = HIWORD(lp);

            if (w > 0 && h > 0)
                agave_webview_set_rect(s_view, 0, 0, w, h);
        }
        return 0;

    case WM_SETFOCUS:
        if (s_view)
            agave_webview_set_focus(s_view);
        return 0;

    case WM_TIMER:
        KillTimer(hwnd, (UINT_PTR)wp);

        if (wp == 1) {
            DestroyWindow(hwnd);
        } else if (wp == 2 && s_view) {
            /* Test hook: drive the page's Copy button. */
            const char* script = getenv("AGB_CRASH_PAGE_TEST");

            if (script && script[0])
                agave_webview_eval(s_view, script);
            SetTimer(hwnd, 1, 800, NULL);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int isHidden(void)
{
    const char* hidden = getenv("AGB_SHELL_HIDDEN");

    return hidden && hidden[0] && !(hidden[0] == '0' && hidden[1] == 0);
}

int crashPageRun(const char* report_path)
{
    static const wchar_t kClassName[] = L"MiniShellCrashPage";
    AgaveWebViewCallback callbacks;
    WNDCLASSEXW wc;
    RECT work;
    MSG msg;
    char* html;
    char tempProfile[MAX_PATH * 2];
    char htmlPath[MAX_PATH];
    int width;
    int height;
    int hidden;
    int dpi;

    /* Never recurse: a crash inside this page must not spawn another page. */
    _putenv("AGB_CRASH_PAGE_CHILD=1");

    s_report = readFile(report_path, &s_reportLen);
    if (!s_report) {
        char message[512];

        _snprintf(message, sizeof(message),
                  "The crash report could not be read:\n\n%s", report_path);
        message[sizeof(message) - 1] = 0;
        MessageBoxA(NULL, message, "MiniShell crash report", MB_OK | MB_ICONERROR);
        return 1;
    }

    html = buildHtml(s_report, s_reportLen);
    if (!html) {
        MessageBoxA(NULL, "The crash report could not be rendered (out of memory).",
                    "MiniShell crash report", MB_OK | MB_ICONERROR);
        free(s_report);
        s_report = NULL;
        return 1;
    }

    if (getenv("AGB_CRASH_PAGE_SAVE")) {
        char temp[MAX_PATH];
        FILE* dump;

        GetTempPathA(MAX_PATH, temp);
        _snprintf(htmlPath, sizeof(htmlPath), "%sMiniShell-crash-page.html", temp);
        htmlPath[sizeof(htmlPath) - 1] = 0;
        dump = fopen(htmlPath, "wb");
        if (dump) {
            fwrite(html, 1, strlen(html), dump);
            fclose(dump);
        }
    }

    shellOps()->enable_dpi_awareness(NULL);

    {
        HDC screen = GetDC(NULL);

        dpi = screen ? (int)GetDeviceCaps(screen, LOGPIXELSX) : 96;
        if (screen)
            ReleaseDC(NULL, screen);
    }
    if (dpi < 96)
        dpi = 96;

    width = MulDiv(1060, dpi, 96);
    height = MulDiv(800, dpi, 96);

    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        int maxW = work.right - work.left - MulDiv(60, dpi, 96);
        int maxH = work.bottom - work.top - MulDiv(60, dpi, 96);

        if (width > maxW) width = maxW;
        if (height > maxH) height = maxH;
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = crashPageProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    hidden = isHidden();

    s_wnd = CreateWindowExW(0, kClassName, L"MiniShell crash report",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                            NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!s_wnd) {
        free(html);
        free(s_report);
        s_report = NULL;
        return 1;
    }

    /* The report contains no browsing data and the page is local, so a throwaway
     * profile keeps cookies/caches of the real one untouched. */
    {
        char temp[MAX_PATH];

        GetTempPathA(MAX_PATH, temp);
        _snprintf(tempProfile, sizeof(tempProfile), "%sMiniShellCrash", temp);
        tempProfile[sizeof(tempProfile) - 1] = 0;
        agave_webview_set_profile_dir(tempProfile);
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.on_message = onCrashMessage;

    {
        RECT client;

        GetClientRect(s_wnd, &client);
        s_view = agave_webview_create(s_wnd, 0, 0,
                                      client.right - client.left,
                                      client.bottom - client.top,
                                      &callbacks, NULL);
    }

    if (!s_view) {
        free(html);
        free(s_report);
        s_report = NULL;
        DestroyWindow(s_wnd);
        MessageBoxA(NULL, "The crash report page could not be created.\n\n"
                          "The report is in agb_crash.txt next to the browser.",
                    "MiniShell crash report", MB_OK | MB_ICONERROR);
        return 1;
    }

    agave_webview_load_html(s_view, html, "minishell://crash");
    agave_webview_set_focus(s_view);
    free(html);

    if (!hidden) {
        ShowWindow(s_wnd, SW_SHOW);
        UpdateWindow(s_wnd);
    } else {
        /* Headless smoke test: create, load, then leave without a desktop. */
        SetTimer(s_wnd, 1, 3000, NULL);

        if (getenv("AGB_CRASH_PAGE_TEST"))
            SetTimer(s_wnd, 2, 1200, NULL);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s_view) {
        agave_webview_destroy(s_view);
        s_view = NULL;
    }

    agave_webview_shutdown();
    free(s_report);
    s_report = NULL;

    return 0;
}

int crashPageLaunch(const char* report_path)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe[MAX_PATH];
    wchar_t report[MAX_PATH];
    wchar_t command[MAX_PATH * 2 + 64];

    if (getenv("AGB_CRASH_PAGE_CHILD"))
        return 0;

    if (!report_path || !report_path[0])
        return 0;

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return 0;

    if (!MultiByteToWideChar(CP_ACP, 0, report_path, -1, report, MAX_PATH))
        return 0;

    _snwprintf(command, sizeof(command) / sizeof(command[0]) - 1,
               L"\"%ls\" --crash-page \"%ls\"", exe, report);
    command[sizeof(command) / sizeof(command[0]) - 1] = 0;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}
