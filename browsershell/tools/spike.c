/* Spike: validate miniblink's MB_WINDOW_TYPE_CONTROL as an embedded tab view.
 * Build: cl spike.c /I..\src /link ..\lib\mb132_x64.lib user32.lib gdi32.lib
 */
#include <windows.h>
#include <stdio.h>
#include "mb.h"

/* mb.h has the callback typedef but no prototype for this export. */
MB_EXTERN_C void MB_CALL_TYPE mbOnMouseOverUrlChanged(mbWebView webView,
                                                      mbMouseOverUrlChangedCallback callback,
                                                      void* param);

static mbWebView g_view;
static int g_created;

static LRESULT CALLBACK ShellProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        if (g_view) {
            mbResize(g_view, LOWORD(lp), HIWORD(lp));
            if (g_created)
                mbShowWindow(g_view, 1);
        }
        return 0;
    }
    case WM_SETFOCUS:
        if (g_view) mbSetFocus(g_view);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void MB_CALL_TYPE onTitle(mbWebView v, void* p, const utf8* title)
{
    (void)v; (void)p;
    printf("[title] %s\n", title ? title : "");
    fflush(stdout);
}

static void MB_CALL_TYPE onUrl(mbWebView v, void* p, const utf8* url, BOOL back, BOOL fwd)
{
    (void)v; (void)p;
    printf("[url] %s back=%d fwd=%d\n", url ? url : "", back, fwd);
    fflush(stdout);
}

static void MB_CALL_TYPE onFinish(mbWebView v, void* p, mbWebFrameHandle frame,
                                  const utf8* url, mbLoadingResult result, const utf8* reason)
{
    (void)p; (void)frame;
    printf("[finish] result=%d url=%s reason=%s\n", (int)result, url ? url : "",
           reason ? reason : "");
    fflush(stdout);

    if (result == MB_LOADING_SUCCEEDED) {
        mbMemBuf* shot = mbGetWindowScreenshotSync(v, kMbImageFormatPng);
        if (shot && shot->data && shot->length) {
            FILE* f = fopen("spike-shot.png", "wb");
            if (f) {
                fwrite(shot->data, 1, shot->length, f);
                fclose(f);
                printf("[shot] %lu bytes\n", (unsigned long)shot->length);
            }
            mbFreeMemBuf(shot);
        } else {
            printf("[shot] no data\n");
        }
        fflush(stdout);
    }
}

static void MB_CALL_TYPE onStatus(mbWebView v, void* p, const utf8* url)
{
    (void)v; (void)p;
    printf("[hover] %s\n", url ? url : "");
    fflush(stdout);
}

static void MB_CALL_TYPE onConsole(mbWebView v, void* p, mbConsoleLevel level,
                                   const utf8* message, const utf8* source,
                                   unsigned line, const utf8* stack)
{
    (void)v; (void)p; (void)source; (void)line; (void)stack;
    printf("[console:%d] %s\n", (int)level, message ? message : "");
    fflush(stdout);
}

int main(int argc, char** argv)
{
    WNDCLASSEXW wc = {0};
    HWND hwnd;
    MSG msg;
    const char* url = argc > 1 ? argv[1] : "https://example.com/";

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ShellProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MBSpikeShell";
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"miniblink control spike",
                           WS_OVERLAPPEDWINDOW, 80, 60, 1100, 760,
                           NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 1100, 780, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    printf("shell hwnd=%p\n", (void*)hwnd);
    fflush(stdout);

    mbInit(NULL);

    g_view = mbCreateWebWindow(MB_WINDOW_TYPE_CONTROL, hwnd, 0, 0, 1080, 700);
    if (!g_view) {
        printf("mbCreateWebWindow(CONTROL) failed\n");
        return 1;
    }
    g_created = 1;
    printf("view=%p hwnd=%p\n", (void*)g_view, (void*)mbGetHostHWND(g_view));

    mbOnTitleChanged(g_view, onTitle, NULL);
    mbOnURLChanged(g_view, onUrl, NULL);
    mbOnLoadingFinish(g_view, onFinish, NULL);
    mbOnMouseOverUrlChanged(g_view, onStatus, NULL);
    mbOnConsole(g_view, onConsole, NULL);
    mbSetNavigationToNewWindowEnable(g_view, TRUE);
    mbShowWindow(g_view, 1);

    mbLoadURL(g_view, url);
    mbSetFocus(g_view);

    printf("loading %s\n", url);
    fflush(stdout);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_view)
        mbDestroyWebView(g_view);
    mbUninit();
    return 0;
}
