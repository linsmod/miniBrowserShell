/*
 * ui_inspect.c - click-through marker window above the webview.
 *
 * A layered *popup* owned by the shell, drawn into a 32bpp DIB and pushed with
 * UpdateLayeredWindow. Two facts decided this shape:
 *
 *  - a layered child window is refused on this system (UpdateLayeredWindow
 *    returns 87), a layered popup works;
 *  - per-pixel alpha is what makes "clear the previous mark" a non-issue: every
 *    update starts from a fully transparent surface and draws exactly one box,
 *    so two indicators can never share the screen. The region-based version
 *    relied on SetWindowRgn, which layered windows ignore - that is how a stale
 *    mark stayed visible next to the new one.
 *
 * The popup is owned by the shell window (hidden with it, never in the
 * taskbar), positioned in screen coordinates, and click-through via
 * WM_NCHITTEST.
 */

#include "ui_inspect.h"
#include "ui_widgets.h"
#include "agb_log.h"

#include <string.h>

#define INSPECT_CLASS       L"MiniShellInspectOverlay"

/* Highlight colours, straight from the reference inspector.js (sky-400,
 * #38BDF8): a 16% wash plus a 95% ring. The theme accent turned muddy on pages
 * whose own design is blue, and the cyan reads as "this is the inspector" rather
 * than "this is a selected control". */
#define INSPECT_FILL_ARGB   0x2938BDF8u
#define INSPECT_BORDER_ARGB 0xF238BDF8u

typedef struct InspectSurface {
    HDC     dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    void*   bits;
    int     w, h;
} InspectSurface;

static InspectSurface g_surface;

static int inspScale(const UiInspect* insp, int dip)
{
    return insp->theme ? uiThemeScale(insp->theme, dip) : dip;
}

static void surfaceFree(void)
{
    if (g_surface.dc && g_surface.old)
        SelectObject(g_surface.dc, g_surface.old);
    if (g_surface.bitmap)
        DeleteObject(g_surface.bitmap);
    if (g_surface.dc)
        DeleteDC(g_surface.dc);

    memset(&g_surface, 0, sizeof(g_surface));
}

static int surfaceEnsure(int w, int h)
{
    BITMAPINFO info;

    if (g_surface.dc && g_surface.w >= w && g_surface.h >= h)
        return 1;

    surfaceFree();

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;      /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    g_surface.dc = CreateCompatibleDC(NULL);
    if (!g_surface.dc) {
        surfaceFree();
        return 0;
    }

    g_surface.bitmap = CreateDIBSection(g_surface.dc, &info, DIB_RGB_COLORS,
                                        &g_surface.bits, NULL, 0);
    if (!g_surface.bitmap || !g_surface.bits) {
        surfaceFree();
        return 0;
    }

    g_surface.old = SelectObject(g_surface.dc, g_surface.bitmap);
    g_surface.w = w;
    g_surface.h = h;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* drawing                                                                   */
/* ------------------------------------------------------------------------- */

/* The surface is grown, never shrunk, so its stride is usually wider than the
 * mark. Clearing w*h*4 bytes straight through therefore misses the right end of
 * the top rows and leaves the previous frame there - the mark showed a second,
 * half-transparent band. Clear row by row with the real stride. */
static void surfaceClear(InspectSurface* surface, int w, int h)
{
    unsigned char* rows = (unsigned char*)surface->bits;
    size_t stride = (size_t)surface->w * 4;
    int y;

    for (y = 0; y < h; ++y)
        memset(rows + (size_t)y * stride, 0, (size_t)w * 4);
}

/* Pushes a surface to a layered popup at screen coordinates. The surface holds
 * exactly the pixels to show: nothing else is composited over the page. */
static int presentLayered(HWND hwnd, InspectSurface* surface, int x, int y, int w, int h)
{
    POINT origin;
    SIZE size;
    POINT src;
    BLENDFUNCTION blend;

    origin.x = x;
    origin.y = y;
    size.cx = w;
    size.cy = h;
    src.x = 0;
    src.y = 0;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (!UpdateLayeredWindow(hwnd, NULL, &origin, &size, surface->dc, &src, 0,
                             &blend, ULW_ALPHA)) {
        static int logged;

        if (!logged) {
            logged = 1;
            agbLog(AGB_LOG_SHELL, "inspect overlay: UpdateLayeredWindow failed %lu",
                   (unsigned long)GetLastError());
        }
        return 0;
    }

    ShowWindow(hwnd, SW_SHOWNA);
    return 1;
}

/* The page area in screen pixels, taken from the control itself. Never from the
 * cached chrome content rect: a rect computed before the DPI/layout settled put
 * the mark off by the toolbar height until the next resize fixed it. */
static int pageArea(const UiInspect* insp, RECT* area)
{
    POINT tl = { 0, 0 };

    if (insp->target) {
        RECT client;

        if (GetClientRect(insp->target, &client) &&
            ClientToScreen(insp->target, &tl)) {
            area->left = tl.x + client.left;
            area->top = tl.y + client.top;
            area->right = tl.x + client.right;
            area->bottom = tl.y + client.bottom;
            return 1;
        }
    }

    if (insp->parent && ClientToScreen(insp->parent, &tl)) {
        area->left = tl.x + insp->x;
        area->top = tl.y + insp->y;
        area->right = area->left + insp->w;
        area->bottom = area->top + insp->h;
        return 1;
    }

    return 0;
}

/* The element rect, clipped to the page. 0 = nothing visible. Both rects are in
 * screen pixels. */
static int markBox(const UiInspect* insp, const RECT* area, RECT* box)
{
    box->left = area->left + insp->mark_x;
    box->top = area->top + insp->mark_y;
    box->right = box->left + insp->mark_w;
    box->bottom = box->top + insp->mark_h;

    if (box->left < area->left) box->left = area->left;
    if (box->top < area->top) box->top = area->top;
    if (box->right > area->right) box->right = area->right;
    if (box->bottom > area->bottom) box->bottom = area->bottom;

    if (box->right - box->left < 1 || box->bottom - box->top < 1)
        return 0;

    /* No outward expansion: the ring sits *on* the element's box, the way
     * Chrome's own inspector draws it, so the mark and the element line up
     * exactly. */
    return 1;
}

/* Only the frame is drawn here. The element info goes to the shell's bottom-left
 * status bubble (updateStatus): as a chip next to the mark it had to compete
 * with the element's own size, and a 2x2 target clipped it to nothing. */
static void inspectRender(UiInspect* insp)
{
    RECT area;
    RECT box;
    int w, h;
    int border;
    UiPainter painter;

    if (!insp || !insp->hwnd || !insp->has_mark)
        return;

    if (!insp->theme || !pageArea(insp, &area) || !markBox(insp, &area, &box)) {
        ShowWindow(insp->hwnd, SW_HIDE);
        return;
    }

    w = box.right - box.left;
    h = box.bottom - box.top;

    if (!surfaceEnsure(w, h))
        return;

    /* Start from transparent: the previous mark cannot survive. */
    surfaceClear(&g_surface, w, h);

    if (!uiPainterBegin(&painter, g_surface.dc, insp->theme, w, h)) {
        agbLog(AGB_LOG_SHELL, "inspect paint: painter failed %dx%d", w, h);
        return;
    }

    /* Ring thickness: 1.5 DIP rounded, i.e. 2 device pixels at 100-125% and 3 at
     * 150%. It is drawn as four integer rects instead of an antialiased stroke:
     * a GDI+ stroke on a 2.5 px pen smears over ~4 px of edge, which is what
     * read as "too thick". */
    border = (inspScale(insp, 3) + 1) / 2;
    if (border < 1)
        border = 1;
    if (border * 2 >= w || border * 2 >= h) {
        uiFillRect(&painter, uiRectMake(0, 0, w, h), INSPECT_BORDER_ARGB);
        uiPainterEnd(&painter);
        presentLayered(insp->hwnd, &g_surface, box.left, box.top, w, h);
        return;
    }

    uiFillRect(&painter, uiRectMake(border, border, w - border * 2, h - border * 2),
               INSPECT_FILL_ARGB);

    uiFillRect(&painter, uiRectMake(0, 0, w, border), INSPECT_BORDER_ARGB);
    uiFillRect(&painter, uiRectMake(0, h - border, w, border), INSPECT_BORDER_ARGB);
    uiFillRect(&painter, uiRectMake(0, border, border, h - border * 2), INSPECT_BORDER_ARGB);
    uiFillRect(&painter, uiRectMake(w - border, border, border, h - border * 2),
               INSPECT_BORDER_ARGB);

    uiPainterEnd(&painter);

    presentLayered(insp->hwnd, &g_surface, box.left, box.top, w, h);
}

static LRESULT CALLBACK inspectProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        /* Click-through: the page underneath keeps every input event. */
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
        /* Presented by UpdateLayeredWindow, never by GDI on this surface. */
        ValidateRect(hwnd, NULL);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

void uiInspectInit(UiInspect* insp, HWND parent, UiTheme* theme)
{
    static int registered;
    WNDCLASSEXW wc;

    if (!insp)
        return;

    memset(insp, 0, sizeof(*insp));
    insp->parent = parent;
    insp->theme = theme;

    if (!registered) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = inspectProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = NULL;
        wc.lpszClassName = INSPECT_CLASS;
        RegisterClassExW(&wc);
        registered = 1;
    }

    insp->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 INSPECT_CLASS, L"", WS_POPUP,
                                 0, 0, 1, 1, NULL, NULL,
                                 GetModuleHandleW(NULL), NULL);
    if (!insp->hwnd) {
        agbLog(AGB_LOG_SHELL, "inspect overlay: CreateWindowEx failed: %lu",
               (unsigned long)GetLastError());
        return;
    }

    /* Owned by the shell: hidden with it, never in the taskbar. */
    SetWindowLongPtrW(insp->hwnd, GWLP_HWNDPARENT, (LONG_PTR)parent);
    SetWindowLongPtrW(insp->hwnd, GWLP_USERDATA, (LONG_PTR)insp);
}

void uiInspectDestroy(UiInspect* insp)
{
    if (!insp)
        return;

    if (insp->hwnd) {
        DestroyWindow(insp->hwnd);
        insp->hwnd = NULL;
    }

    surfaceFree();
}

void uiInspectSetContentRect(UiInspect* insp, int x, int y, int w, int h)
{
    if (!insp || !insp->hwnd)
        return;

    insp->x = x;
    insp->y = y;
    insp->w = w > 0 ? w : 1;
    insp->h = h > 0 ? h : 1;


    if (insp->has_mark)
        inspectRender(insp);
}

void uiInspectSetMark(UiInspect* insp, int x, int y, int w, int h, const char* label)
{
    if (!insp || !insp->hwnd)
        return;

    /* No clipping here: the renderer clips against the page area it reads from
     * the live control, so a stale content rect cannot cut the mark short. */

    insp->has_mark = 1;
    insp->mark_x = x;
    insp->mark_y = y;
    insp->mark_w = w;
    insp->mark_h = h;

    if (label)
        _snprintf(insp->label, sizeof(insp->label) - 1, "%s", label);
    else
        insp->label[0] = 0;
    insp->label[sizeof(insp->label) - 1] = 0;

    insp->shown = 1;
    inspectRender(insp);
}

void uiInspectClear(UiInspect* insp)
{
    if (!insp || !insp->hwnd)
        return;

    insp->has_mark = 0;
    insp->label[0] = 0;
    ShowWindow(insp->hwnd, SW_HIDE);
}

void uiInspectShow(UiInspect* insp, int show)
{
    if (!insp || !insp->hwnd)
        return;

    insp->shown = show ? 1 : 0;

    if (!show) {
        ShowWindow(insp->hwnd, SW_HIDE);
        return;
    }

    if (insp->has_mark)
        inspectRender(insp);
}

int uiInspectIsShown(const UiInspect* insp)
{
    return insp && insp->shown;
}

void uiInspectRefresh(UiInspect* insp)
{
    if (!insp || !insp->hwnd || !insp->shown || !insp->has_mark)
        return;

    inspectRender(insp);
}

void uiInspectSetTarget(UiInspect* insp, HWND target)
{
    if (insp)
        insp->target = target;
}