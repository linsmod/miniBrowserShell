/* Proof that miniblink can rewrite a response body.
 *
 *   mbOnLoadUrlBegin -> mbNetHookRequest(job)   (buffer the real body)
 *                    -> return FALSE             (mb still performs the request)
 *   mbOnLoadUrlEnd   -> mbNetSetData(job, ...)  (replace the body)
 *                    -> mbNetSetMIMEType(...)
 *
 * This demo rewrites example.com into a one-line page, then reads it back.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "mb.h"

MB_EXTERN_C void MB_CALL_TYPE mbOnMouseOverUrlChanged(mbWebView, mbMouseOverUrlChangedCallback, void*);

static mbWebView g_view;
static int g_done;
static const char kRewrittenHtml[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>REWRITTEN</title>"
    "</head><body><h1 id=\"r\">REWRITTEN BY HOST</h1></body></html>";

static BOOL MB_CALL_TYPE onLoadUrlBegin(mbWebView wv, void* param,
                                        const char* url, void* job)
{
    (void)wv; (void)param;
    if (url && strstr(url, "example.com")) {
        if (getenv("DIRECT")) {
            printf("[hook] begin %s -> direct fake response\n", url);
            fflush(stdout);
            mbNetSetMIMEType(job, "text/html");
            mbNetSetData(job, (void*)kRewrittenHtml, (int)strlen(kRewrittenHtml));
            return TRUE;
        }
        printf("[hook] begin %s -> hook request\n", url);
        fflush(stdout);
        mbNetHookRequest(job);
    }
    return FALSE;   /* false: mb continues the request (and calls onLoadUrlEnd) */
}

static void MB_CALL_TYPE onLoadUrlEnd(mbWebView wv, void* param, const char* url,
                                      void* job, void* buf, int len)
{
    (void)wv; (void)param; (void)buf;
    printf("[hook] end %s (%d bytes) -> replace body\n", url ? url : "", len);
    fflush(stdout);
    mbNetSetMIMEType(job, "text/html");
    mbNetSetData(job, (void*)kRewrittenHtml, (int)strlen(kRewrittenHtml));
}

static void MB_CALL_TYPE onReadBack(mbWebView wv, void* p, mbJsExecState es, mbJsValue v)
{
    (void)wv; (void)p;
    printf("[result] %s\n", mbJsToString(es, v));
    fflush(stdout);
    PostQuitMessage(0);
}

static void readBack(void)
{
    mbWebFrameHandle frame = mbWebFrameGetMainFrame(g_view);

    mbRunJs(g_view, frame,
            "document.title + ' | ' + (document.getElementById('r')||{}).textContent"
            " + ' | ' + document.body.innerHTML.slice(0,80)",
            FALSE, onReadBack, NULL, NULL);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_TIMER && !g_done) {
        g_done = 1;
        KillTimer(h, 1);
        readBack();
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void MB_CALL_TYPE onFinish(mbWebView wv, void* p, mbWebFrameHandle f,
                                  const utf8* u, mbLoadingResult r, const utf8* why)
{
    (void)p; (void)f; (void)why;
    if (r != MB_LOADING_SUCCEEDED)
        return;
    printf("[finish] %s\n", u ? u : "");
    fflush(stdout);
    /* give the replaced document a moment to commit/parse */
    SetTimer(GetParent(mbGetHostHWND(wv)), 1, 800, NULL);
}

int main(void)
{
    WNDCLASSEXW wc = {0};
    HWND hwnd;
    MSG msg;

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"MBNetHookDemo";
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"net hook demo",
                           WS_OVERLAPPEDWINDOW, 40, 40, 900, 640,
                           NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);

    mbInit(NULL);
    g_view = mbCreateWebWindow(MB_WINDOW_TYPE_CONTROL, hwnd, 0, 0, 880, 600);
    mbOnLoadUrlBegin(g_view, onLoadUrlBegin, NULL);
    mbOnLoadUrlEnd(g_view, onLoadUrlEnd, NULL);
    mbOnLoadingFinish(g_view, onFinish, NULL);
    mbShowWindow(g_view, 1);
    mbLoadURL(g_view, "https://example.com/");

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
