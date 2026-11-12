/* Verifies that a host-initiated fetch gets chunked didReceiveData callbacks,
 * i.e. real download progress with Content-Length.
 *
 *   mbNetCreateWebUrlRequest -> mbNetStartUrlRequest
 *   callbacks: didReceiveResponse / didReceiveData / didFinishLoading / didFail
 */
#include <windows.h>
#include <stdio.h>
#include "mb.h"

static long long g_total;
static long long g_expected;
static int g_chunks;
static mbWebView g_view;
static const char* g_url = "https://www.baidu.com/";

static void MB_CALL_TYPE onResponse(mbWebView wv, void* param, mbWebUrlRequestPtr req,
                                    mbWebUrlResponsePtr resp)
{
    (void)wv; (void)param; (void)req;
    g_expected = mbNetGetExpectedContentLength(resp);
    printf("[response] status=%d content-length=%lld\n",
           mbNetGetHttpStatusCode(resp), g_expected);
    fflush(stdout);
}

static void MB_CALL_TYPE onData(mbWebView wv, void* param, mbWebUrlRequestPtr req,
                                const char* data, int len)
{
    (void)wv; (void)param; (void)req; (void)data;
    g_total += len;
    ++g_chunks;
    if (g_chunks <= 5 || g_total == g_expected)
        printf("[data] chunk#%d +%d total=%lld/%lld\n", g_chunks, len, g_total, g_expected);
    fflush(stdout);
}

static void MB_CALL_TYPE onFinish(mbWebView wv, void* param, mbWebUrlRequestPtr req,
                                  double finishTime)
{
    (void)wv; (void)param; (void)req;
    printf("[finish] chunks=%d total=%lld expected=%lld time=%.1fms\n",
           g_chunks, g_total, g_expected, finishTime);
    fflush(stdout);
    PostQuitMessage(0);
}

static void MB_CALL_TYPE onFail(mbWebView wv, void* param, mbWebUrlRequestPtr req,
                                const utf8* error)
{
    (void)wv; (void)param; (void)req;
    printf("[fail] %s\n", error ? error : "");
    fflush(stdout);
    PostQuitMessage(0);
}

static mbUrlRequestCallbacks g_cbs;

static void MB_CALL_TYPE doStartOnBlink(void* p1, void* p2)
{
    mbWebView view = (mbWebView)p1;
    const char* url = (const char*)p2;
    mbWebUrlRequestPtr request;
    int rc;

    printf("[step] create request (blink thread)\n"); fflush(stdout);
    request = mbNetCreateWebUrlRequest(url, "GET", "text/html");
    printf("[step] request=%p\n", (void*)request); fflush(stdout);
    if (!request)
        return;
    rc = mbNetStartUrlRequest(view, request, NULL, &g_cbs);
    printf("[start] rc=%d\n", rc);
    fflush(stdout);
}

static void startRequest(void)
{
    g_cbs.willRedirectCallback = NULL;
    g_cbs.didReceiveResponseCallback = onResponse;
    g_cbs.didReceiveDataCallback = onData;
    g_cbs.didFailCallback = onFail;
    g_cbs.didFinishLoadingCallback = onFinish;

    printf("[start] %s\n", g_url);
    fflush(stdout);
    mbCallBlinkThreadSync(doStartOnBlink, g_view, (void*)g_url);
    printf("[step] blink call returned\n");
    fflush(stdout);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_TIMER && w == 1) {
        printf("[step] timer\n"); fflush(stdout);
        KillTimer(h, 1);
        startRequest();
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv)
{
    WNDCLASSEXW wc = {0};
    HWND hwnd;
    MSG msg;

    if (argc > 1)
        g_url = argv[1];

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MBNetProgress";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"net progress",
                           WS_OVERLAPPEDWINDOW, 40, 40, 600, 400,
                           NULL, NULL, wc.hInstance, NULL);

    printf("[step] mbInit\n"); fflush(stdout);
    mbInit(NULL);
    printf("[step] create view\n"); fflush(stdout);
    g_view = mbCreateWebWindow(MB_WINDOW_TYPE_CONTROL, hwnd, 0, 0, 580, 360);
    printf("[step] view=%p\n", (void*)g_view); fflush(stdout);
    if (!g_view) {
        printf("view create failed\n");
        return 1;
    }
    SetTimer(hwnd, 1, 1200, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
