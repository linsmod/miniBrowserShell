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
#define INSPECT_BORDER_DIP  2
#define INSPECT_CHIP_H_DIP  18
#define INSPECT_PAD_DIP     4

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

static void inspectChipRect(const UiInspect* insp, RECT* chip, const RECT* mark, int* chipW)
{
    int pad = inspScale(insp, INSPECT_PAD_DIP);
    int border = inspScale(insp, INSPECT_BORDER_DIP);
    int textW = 0;
    int textH = 0;
    int chipH;
    int w;

    if (insp->label[0] && insp->theme)
        uiMeasureText(insp->theme, insp->theme->font_small, insp->label, &textW, &textH);

    if (textW < 0)
        textW = 0;

    chipH = inspScale(insp, INSPECT_CHIP_H_DIP);
    if (textH + 6 > chipH)
        chipH = textH + 6;

    w = textW + pad * 2;
    *chipW = w;

    chip->left = mark->left - border;
    chip->right = chip->left + w;
    chip->top = mark->top - border - chipH - 2;

    /* No room above (element at the top edge): sit inside the box instead of
     * being clipped away. */
    if (chip->top < 0)
        chip->top = mark->top + 2;

    chip->bottom = chip->top + chipH;
}

/* The whole thing (box + chip), clipped to the content rect. 0 = nothing to
 * draw. All coordinates are in content (page) space. */
static int inspectLayout(const UiInspect* insp, RECT* total, RECT* box, RECT* chip, int* chipW)
{
    RECT mark;
    int border = inspScale(insp, INSPECT_BORDER_DIP);

    *chipW = 0;

    mark.left = insp->mark_x;
    mark.top = insp->mark_y;
    mark.right = insp->mark_x + insp->mark_w;
    mark.bottom = insp->mark_y + insp->mark_h;

    if (mark.left < 0) mark.left = 0;
    if (mark.top < 0) mark.top = 0;
    if (mark.right > insp->w) mark.right = insp->w;
    if (mark.bottom > insp->h) mark.bottom = insp->h;

    if (mark.right - mark.left < 1 || mark.bottom - mark.top < 1)
        return 0;

    box->left = mark.left - border;
    box->top = mark.top - border;
    box->right = mark.right + border;
    box->bottom = mark.bottom + border;

    *total = *box;

    if (insp->label[0]) {
        inspectChipRect(insp, chip, &mark, chipW);
        if (*chipW > 0) {
            if (chip->left < total->left) total->left = chip->left;
            if (chip->top < total->top) total->top = chip->top;
            if (chip->right > total->right) total->right = chip->right;
            if (chip->bottom > total->bottom) total->bottom = chip->bottom;
        }
    }

    return 1;
}

static void inspectDraw(UiInspect* insp, HDC hdc, int width, int height,
                        const RECT* box, const RECT* chip, int chipW)
{
    UiTheme* theme = insp->theme;
    UiPainter painter;
    int border;

    if (!theme)
        return;

    if (!uiPainterBegin(&painter, hdc, theme, width, height)) {
        agbLog(AGB_LOG_SHELL, "inspect paint: painter failed %dx%d", width, height);
        return;
    }

    border = inspScale(insp, INSPECT_BORDER_DIP);

    uiFillRect(&painter,
               uiRectMake(box->left, box->top, box->right - box->left, box->bottom - box->top),
               (theme->accent & 0x00FFFFFFu) | 0x33000000u);
    uiStrokeRoundRect(&painter,
                      uiRectMake(box->left, box->top, box->right - box->left, box->bottom - box->top),
                      0, theme->accent, (float)border);

    if (chipW > 0 && insp->label[0]) {
        UiRect chipRect = uiRectMake(chip->left, chip->top,
                                     chip->right - chip->left, chip->bottom - chip->top);
        int pad = inspScale(insp, INSPECT_PAD_DIP);

        uiFillRoundRect(&painter, chipRect, 3, theme->accent);
        uiText(&painter, insp->label,
               uiRectMake(chipRect.x + pad, chipRect.y + 1,
                          chipRect.w - pad * 2, chipRect.h - 2),
               theme->font_small, 0xFFFFFFFFu, UI_ALIGN_LEFT);
    }

    uiPainterEnd(&painter);
}

static void inspectRender(UiInspect* insp)
{
    RECT total;
    RECT box;
    RECT chip;
    RECT chipT;
    int chipW = 0;
    int w, h;
    POINT origin;
    SIZE size;
    POINT src = { 0, 0 };
    BLENDFUNCTION blend;

    if (!insp || !insp->hwnd || !insp->has_mark)
        return;

    if (!inspectLayout(insp, &total, &box, &chip, &chipW))
        return;

    w = total.right - total.left;
    h = total.bottom - total.top;
    if (w < 1 || h < 1)
        return;

    if (!surfaceEnsure(w, h))
        return;

    /* Start from transparent: the previous mark cannot survive. */
    memset(g_surface.bits, 0, (size_t)w * h * 4);

    box.left -= total.left;
    box.top -= total.top;
    box.right -= total.left;
    box.bottom -= total.top;

    chipT = chip;
    chipT.left -= total.left;
    chipT.top -= total.top;
    chipT.right -= total.left;
    chipT.bottom -= total.top;

    inspectDraw(insp, g_surface.dc, w, h, &box, &chipT, chipW);

    origin.x = insp->x + total.left;
    origin.y = insp->y + total.top;
    ClientToScreen(insp->parent, &origin);

    size.cx = w;
    size.cy = h;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (!UpdateLayeredWindow(insp->hwnd, NULL, &origin, &size, g_surface.dc, &src, 0,
                             &blend, ULW_ALPHA)) {
        static int logged;

        if (!logged) {
            logged = 1;
            agbLog(AGB_LOG_SHELL, "inspect overlay: UpdateLayeredWindow failed %lu",
                   (unsigned long)GetLastError());
        }
        return;
    }

    ShowWindow(insp->hwnd, SW_SHOWNA);
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

    /* Clip to the content rect: a target half scrolled out keeps its visible
     * part, nothing is drawn outside the page. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > insp->w) w = insp->w - x;
    if (y + h > insp->h) h = insp->h - y;

    if (w <= 0 || h <= 0) {
        uiInspectClear(insp);
        return;
    }

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