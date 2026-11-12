/*
 * ui_menu_host.c - a Chrome-style menu popup window.
 *
 * The menu is one layered window whose pixels come from a 32bppPARGB GDI+
 * bitmap: rounded panels and antialiased text keep their alpha, and the empty
 * area between the root panel and a submenu is visible through it. A 1/255
 * black cover over the rest of the window makes the whole popup receive the
 * mouse (so a click outside a panel dismisses it instead of reaching the page),
 * while remaining visually invisible. There is no drop shadow: the whole
 * surface is repainted on every hover change, and ten nested rounded-rect
 * blends per paint dominated that path.
 *
 * While open the host owns a modal message loop and mouse capture, the same
 * contract TrackPopupMenu had.
 */

#include "ui_menu_host.h"

#include <windows.h>
#include <windowsx.h>
#include "gdiplus_c.h"
#include <stdlib.h>
#include <string.h>

#define UI_MENU_CLASS L"AgaveShellMenuPopup"

typedef struct {
    UiMenuSession session;
    const UiTheme* theme;
    HWND hwnd;
    HWND previous_focus;
    int  shown;
    POINT reopen_point;   /* screen coords of the dismissing right click */
    int  done;
    int  chosen;
    int  has_capture;
    int  win_x;
    int  win_y;
    int  surface_w;
    int  surface_h;
    HDC  memdc;
    HBITMAP dib;
    HGDIOBJ old_bitmap;
    void* bits;
    GpBitmap* bitmap;
} UiMenuHost;

static UiMenuHost* g_menuHost;
static int g_menuClassReady;

static void hostDestroySurface(UiMenuHost* host)
{
    if (host->bitmap) {
        GdipDisposeImage(host->bitmap);
        host->bitmap = NULL;
    }

    if (host->memdc) {
        if (host->old_bitmap)
            SelectObject(host->memdc, host->old_bitmap);
        DeleteDC(host->memdc);
        host->memdc = NULL;
        host->old_bitmap = NULL;
    }

    if (host->dib) {
        DeleteObject(host->dib);
        host->dib = NULL;
    }

    host->bits = NULL;
    host->surface_w = 0;
    host->surface_h = 0;
}

static int hostEnsureSurface(UiMenuHost* host, int width, int height)
{
    BITMAPINFO info;
    HDC screen;
    void* bits = NULL;

    if (width <= 0 || height <= 0)
        return 0;

    if (host->memdc && host->surface_w == width && host->surface_h == height)
        return 1;

    hostDestroySurface(host);

    screen = GetDC(NULL);
    host->memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    if (!host->memdc)
        return 0;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   /* top-down: row 0 is the top */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    host->dib = CreateDIBSection(host->memdc, &info, DIB_RGB_COLORS, &bits,
                                 NULL, 0);

    if (!host->dib || !bits) {
        hostDestroySurface(host);
        return 0;
    }

    host->old_bitmap = SelectObject(host->memdc, host->dib);
    host->bits = bits;
    host->surface_w = width;
    host->surface_h = height;

    if (GdipCreateBitmapFromScan0(width, height, width * 4,
                                  PixelFormat32bppPARGB, (BYTE*)bits,
                                  &host->bitmap) != Ok) {
        hostDestroySurface(host);
        return 0;
    }

    return 1;
}

static void hostRender(UiMenuHost* host)
{
    UiRect bounds;
    UiPainter painter;
    GpGraphics* graphics = NULL;
    POINT destination;
    POINT source;
    SIZE size;
    BLENDFUNCTION blend;
    HDC screen;
    int margin;
    int width;
    int height;
    int wx;
    int wy;

    if (!host || !host->hwnd || !host->theme)
        return;

    bounds = uiMenuSessionBounds(&host->session);

    if (bounds.w <= 0 || bounds.h <= 0)
        return;

    /* No shadow any more (see uiMenuSessionPaint): the margin only has to keep
     * the panels' antialiased border inside the surface, and every pixel of it
     * is a blend the compositor has to run. */
    margin = uiThemeScale(host->theme, 2);
    wx = bounds.x - margin;
    wy = bounds.y - margin;
    width = bounds.w + margin * 2;
    height = bounds.h + margin * 2;

    if (!hostEnsureSurface(host, width, height))
        return;

    memset(host->bits, 0, (size_t)width * (size_t)height * 4);

    if (GdipGetImageGraphicsContext(host->bitmap, &graphics) != Ok || !graphics)
        return;

    if (!uiPainterBeginGraphics(&painter, graphics, host->theme, width, height)) {
        GdipDeleteGraphics(graphics);
        return;
    }

    uiMenuSessionPaint(&host->session, host->theme, &painter, -wx, -wy);
    uiPainterEnd(&painter);

    /* Opaque panels are already alpha=255; the shadow and AA edges have their
     * own alpha. Everything else gets alpha 1 so the popup can see the mouse
     * there without visibly tinting the desktop. */
    {
        unsigned char* pixels = (unsigned char*)host->bits;
        size_t count = (size_t)width * (size_t)height;
        size_t p;

        for (p = 0; p < count; ++p) {
            if (pixels[p * 4 + 3] == 0) {
                pixels[p * 4 + 0] = 0;
                pixels[p * 4 + 1] = 0;
                pixels[p * 4 + 2] = 0;
                pixels[p * 4 + 3] = 1;
            }
        }
    }

    destination.x = wx;
    destination.y = wy;
    source.x = 0;
    source.y = 0;
    size.cx = width;
    size.cy = height;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    screen = GetDC(NULL);
    UpdateLayeredWindow(host->hwnd, screen, &destination, &size, host->memdc,
                        &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(NULL, screen);

    host->win_x = wx;
    host->win_y = wy;
}

static void hostDismiss(UiMenuHost* host, int chosen)
{
    if (!host || host->done)
        return;

    host->chosen = chosen;
    host->done = 1;
}

static LRESULT CALLBACK menuHostProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam)
{
    UiMenuHost* host = g_menuHost;

    if (!host || host->hwnd != hwnd)
        return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_MOUSEMOVE: {
        POINT point;
        unsigned before;

        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
        ClientToScreen(hwnd, &point);

        before = uiMenuSessionSignature(&host->session);
        uiMenuSessionMove(&host->session, host->theme, point.x, point.y);

        if (before != uiMenuSessionSignature(&host->session))
            hostRender(host);

        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point;
        int chosen = 0;

        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
        ClientToScreen(hwnd, &point);

        if (uiMenuSessionClick(&host->session, host->theme, point.x, point.y,
                               &chosen))
            hostDismiss(host, chosen);
        else
            hostRender(host);

        return 0;
    }

    /* A right click closes the menu and asks the caller to re-open it where the
     * pointer went: the click was captured by the popup, so it never reached the
     * window below, and swallowing it is what made "right click twice" look like
     * show + hide instead of show + show. */
    case WM_RBUTTONUP: {
        POINT point;

        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
        ClientToScreen(hwnd, &point);
        host->reopen_point = point;

        hostDismiss(host, UI_MENU_REOPEN);
        return 0;
    }

    /* Every other extra button or the wheel just closes it, like Chrome. */
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        hostDismiss(host, 0);
        return 0;

    case WM_KEYDOWN: {
        int chosen = 0;
        int result = uiMenuSessionKey(&host->session, host->theme, (int)wParam,
                                      &chosen);

        if (result == 1)
            hostDismiss(host, chosen);
        else if (result == 0)
            hostRender(host);

        return 0;
    }

    case WM_CHAR:
        return 0;

    /* Focus loss alone is not a dismiss: the popup is not the active window
     * (SW_SHOWNOACTIVATE) and a hidden-owner test run never gives it focus.
     * Alt-Tab is caught by WM_ACTIVATEAPP, clicking elsewhere by capture. */
    case WM_ACTIVATEAPP:
        if (!wParam)
            hostDismiss(host, 0);
        return 0;

    case WM_CAPTURECHANGED:
        if (host->shown && (HWND)lParam != hwnd) {
            host->has_capture = 0;
            hostDismiss(host, 0);
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static int menuClassReady(HINSTANCE instance)
{
    WNDCLASSEXW cls;

    if (g_menuClassReady)
        return 1;

    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.style = 0;
    cls.lpfnWndProc = menuHostProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = NULL;
    cls.lpszClassName = UI_MENU_CLASS;

    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;

    g_menuClassReady = 1;
    return 1;
}

int uiMenuHostRun(const UiTheme* theme, const UiMenu* root, int x, int y,
                  UiMenuAlign align, HWND owner, POINT* reopen_at)
{
    UiMenuHost host;
    UiRect work;
    HMONITOR monitor;
    MONITORINFO info;
    POINT point;
    HINSTANCE instance;
    UiRect bounds;
    int margin;
    int width;
    int height;

    if (!theme || !root)
        return 0;

    memset(&host, 0, sizeof(host));
    host.theme = theme;

    point.x = x;
    point.y = y;

    monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);

    if (monitor && GetMonitorInfoW(monitor, &info)) {
        work = uiRectMake(info.rcWork.left, info.rcWork.top,
                          info.rcWork.right - info.rcWork.left,
                          info.rcWork.bottom - info.rcWork.top);
    } else {
        work = uiRectMake(0, 0, GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN));
    }

    uiMenuSessionBegin(&host.session, root, theme, x, y, align, &work);

    if (host.session.count <= 0)
        return 0;

    instance = GetModuleHandleW(NULL);

    if (!menuClassReady(instance))
        return 0;

    bounds = uiMenuSessionBounds(&host.session);
    margin = uiThemeScale(theme, 2);
    width = bounds.w + margin * 2;
    height = bounds.h + margin * 2;

    host.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                UI_MENU_CLASS, L"", WS_POPUP,
                                bounds.x - margin, bounds.y - margin,
                                width, height, owner, NULL, instance, NULL);

    if (!host.hwnd)
        return 0;

    g_menuHost = &host;
    SetWindowLongPtrW(host.hwnd, GWLP_USERDATA, (LONG_PTR)&host);

    hostRender(&host);

    host.previous_focus = GetFocus();
    ShowWindow(host.hwnd, SW_SHOWNOACTIVATE);
    host.shown = 1;

    if (SetCapture(host.hwnd))
        host.has_capture = 1;

    SetFocus(host.hwnd);

    while (!host.done) {
        MSG message;
        BOOL got = GetMessageW(&message, NULL, 0, 0);

        if (got == 0) {
            PostQuitMessage((int)message.wParam);
            break;
        }

        if (got == -1)
            break;

        if (message.message == WM_QUIT) {
            PostQuitMessage((int)message.wParam);
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (host.has_capture && GetCapture() == host.hwnd)
        ReleaseCapture();

    host.shown = 0;
    DestroyWindow(host.hwnd);
    host.hwnd = NULL;
    g_menuHost = NULL;

    if (host.previous_focus && IsWindow(host.previous_focus))
        SetFocus(host.previous_focus);

    if (reopen_at)
        *reopen_at = host.reopen_point;

    hostDestroySurface(&host);

    return host.chosen;
}
