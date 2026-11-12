/*
 * ui_tabsearch.c - Chrome's "Search tabs" popup.
 *
 * One layered top-level window per process (a singleton), the same rendering
 * pipeline as the menu popup: a 32bppPARGB DIB that GDI+ paints into, then
 * UpdateLayeredWindow. It is deliberately *not* modal: while it is open it holds
 * the mouse capture (an outside click dismisses it, like a menu), and the shell
 * forwards WM_KEYDOWN / WM_CHAR here so the query can be typed - a layered
 * window cannot host a native EDIT control.
 *
 * The caller owns the data: it hands over a snapshot array (open tabs first,
 * then recently closed) whenever the model changes, and gets two callbacks -
 * activate (open the tab, or reopen the closed one) and close (open tabs only).
 */

#include "ui_tabsearch.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui_widgets.h"

#define TS_WIDTH       360
#define TS_SEARCH_H    46
#define TS_HEADER_H    30
#define TS_ROW_H       46
#define TS_FAVICON     28
#define TS_MARGIN      2
#define TS_MAX_QUERY   128

enum {
    TS_ROW_ITEM = 0,       /* a tab (open or closed) */
    TS_ROW_HEADER          /* a section caption, not selectable */
};

typedef struct {
    int kind;             /* TS_ROW_ITEM / TS_ROW_HEADER */
    int item;             /* index into items[] */
    const char* label;    /* header caption */
} TsRow;

typedef struct {
    HWND hwnd;
    HWND owner;
    const UiTheme* theme;
    UiTabSearchCallbacks cb;

    UiTabSearchItem items[UI_TABSEARCH_MAX_ITEMS];
    int item_count;

    char query[TS_MAX_QUERY];
    TsRow rows[UI_TABSEARCH_MAX_ITEMS + 4];
    int row_count;
    int selected;         /* index into rows[], -1 */
    int scroll_y;         /* pixels hidden above the list */
    int content_h;        /* full list height below the search row */
    POINT origin;         /* screen position of the surface */

    int width;
    int height;
    HDC memdc;
    HBITMAP dib;
    HBITMAP old_bitmap;
    void* bits;
    GpBitmap* bitmap;
    int surface_w;
    int surface_h;
    int captured;
} UiTabSearch;

static UiTabSearch g_ts;

static int tsScale(int v)
{
    return uiThemeScale(g_ts.theme, v);
}

static int tsLowerHas(const char* haystack, const char* needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0)
        return 1;

    for (; *haystack; ++haystack) {
        for (i = 0; i < nlen; ++i) {
            char a = haystack[i];
            char b = needle[i];

            if (!a)
                return 0;
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }

        if (i == nlen)
            return 1;
    }

    return 0;
}

static void tsHostOf(const char* url, char* out, size_t out_size)
{
    const char* start = url;
    const char* slash;
    size_t len;

    if (strncmp(url, "http://", 7) == 0)
        start = url + 7;
    else if (strncmp(url, "https://", 8) == 0)
        start = url + 8;
    else if (strncmp(url, "file:///", 8) == 0)
        start = url + 8;

    slash = strchr(start, '/');
    len = slash ? (size_t)(slash - start) : strlen(start);

    if (len >= out_size)
        len = out_size - 1;

    memcpy(out, start, len);
    out[len] = 0;
}

static void tsAgeLabel(char* out, size_t out_size, int age_ms)
{
    if (age_ms < 0)
        age_ms = 0;

    if (age_ms < 60000)
        _snprintf(out, out_size, "just now");
    else if (age_ms < 3600000)
        _snprintf(out, out_size, "%d min ago", age_ms / 60000);
    else if (age_ms < 86400000)
        _snprintf(out, out_size, "%d h ago", age_ms / 3600000);
    else
        _snprintf(out, out_size, "%d d ago", age_ms / 86400000);

    out[out_size - 1] = 0;
}

static void tsRebuildRows(void)
{
    int open_header_done = 0;
    int closed_header_done = 0;
    int max_rows = (int)(sizeof(g_ts.rows) / sizeof(g_ts.rows[0])) - 1;
    int i;

    g_ts.row_count = 0;
    g_ts.selected = -1;

    for (i = 0; i < g_ts.item_count && g_ts.row_count < max_rows; ++i) {
        UiTabSearchItem* item = &g_ts.items[i];
        int matches = tsLowerHas(item->title, g_ts.query) ||
                      tsLowerHas(item->url, g_ts.query);

        if (!matches)
            continue;

        if (item->kind == UI_TABSEARCH_OPEN && !open_header_done) {
            g_ts.rows[g_ts.row_count].kind = TS_ROW_HEADER;
            g_ts.rows[g_ts.row_count].item = -1;
            g_ts.rows[g_ts.row_count].label = "Open tabs";
            ++g_ts.row_count;
            open_header_done = 1;
        }

        if (item->kind == UI_TABSEARCH_CLOSED && !closed_header_done) {
            g_ts.rows[g_ts.row_count].kind = TS_ROW_HEADER;
            g_ts.rows[g_ts.row_count].item = -1;
            g_ts.rows[g_ts.row_count].label = "Recently closed";
            ++g_ts.row_count;
            closed_header_done = 1;
        }

        g_ts.rows[g_ts.row_count].kind = TS_ROW_ITEM;
        g_ts.rows[g_ts.row_count].item = i;
        g_ts.rows[g_ts.row_count].label = NULL;
        ++g_ts.row_count;
    }

    if (g_ts.row_count == 0) {
        g_ts.rows[0].kind = TS_ROW_HEADER;
        g_ts.rows[0].item = -1;
        g_ts.rows[0].label = g_ts.query[0] ? "No matching tabs" : "No tabs";
        g_ts.row_count = 1;
    }

    for (i = 0; i < g_ts.row_count; ++i) {
        if (g_ts.rows[i].kind == TS_ROW_ITEM) {
            g_ts.selected = i;
            break;
        }
    }
}

static int tsListHeight(void)
{
    int headers = 0;
    int items = 0;
    int i;

    for (i = 0; i < g_ts.row_count; ++i) {
        if (g_ts.rows[i].kind == TS_ROW_HEADER)
            ++headers;
        else
            ++items;
    }

    return headers * tsScale(TS_HEADER_H) + items * tsScale(TS_ROW_H);
}

static void tsEnsureSurface(int width, int height)
{
    BITMAPINFO info;
    void* bits = NULL;
    HDC screen = GetDC(NULL);

    if (g_ts.memdc && g_ts.surface_w == width && g_ts.surface_h == height)
        return;

    if (g_ts.memdc) {
        SelectObject(g_ts.memdc, g_ts.old_bitmap);
        DeleteObject(g_ts.dib);
        DeleteDC(g_ts.memdc);
        g_ts.memdc = NULL;
    }

    if (g_ts.bitmap) {
        GdipDisposeImage((GpImage*)g_ts.bitmap);
        g_ts.bitmap = NULL;
    }

    g_ts.memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    if (!g_ts.memdc)
        return;

    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;   /* top-down */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    g_ts.dib = CreateDIBSection(g_ts.memdc, &info, DIB_RGB_COLORS, &bits, NULL, 0);

    if (!g_ts.dib || !bits) {
        DeleteDC(g_ts.memdc);
        g_ts.memdc = NULL;
        return;
    }

    g_ts.old_bitmap = SelectObject(g_ts.memdc, g_ts.dib);
    g_ts.bits = bits;

    if (GdipCreateBitmapFromScan0(width, height, width * 4,
                                  PixelFormat32bppPARGB, (BYTE*)bits,
                                  &g_ts.bitmap) != Ok) {
        g_ts.bitmap = NULL;
        return;
    }

    g_ts.surface_w = width;
    g_ts.surface_h = height;
}

static void tsRender(void)
{
    UiPainter painter;
    GpGraphics* graphics = NULL;
    POINT destination;
    POINT source;
    SIZE size;
    BLENDFUNCTION blend;
    HDC screen;
    const UiTheme* theme = g_ts.theme;
    UiRect panel;
    int width = g_ts.width + TS_MARGIN * 2;
    int height = g_ts.height + TS_MARGIN * 2;
    int y;
    int i;
    int list_bottom;

    if (!g_ts.hwnd || !theme)
        return;

    tsEnsureSurface(width, height);

    if (!g_ts.bitmap)
        return;

    memset(g_ts.bits, 0, (size_t)width * (size_t)height * 4);

    if (GdipGetImageGraphicsContext(g_ts.bitmap, &graphics) != Ok || !graphics)
        return;

    if (!uiPainterBeginGraphics(&painter, graphics, theme, width, height)) {
        GdipDeleteGraphics(graphics);
        return;
    }

    panel = uiRectMake(TS_MARGIN, TS_MARGIN, width - TS_MARGIN * 2,
                       height - TS_MARGIN * 2);

    uiFillRoundRect(&painter, panel, tsScale(8), theme->menu_bg);
    uiStrokeRoundRect(&painter, panel, tsScale(8), theme->menu_border, 1.0f);

    /* Search row: magnifier, query (or placeholder), the shortcut hint. */
    uiIcon(&painter, UI_ICON_SEARCH,
           uiRectMake(TS_MARGIN + tsScale(14),
                      TS_MARGIN + (tsScale(TS_SEARCH_H) - tsScale(16)) / 2,
                      tsScale(16), tsScale(16)),
           theme->icon, (1.5f * theme->scale));

    uiText(&painter, g_ts.query[0] ? g_ts.query : "Search tabs",
           uiRectMake(TS_MARGIN + tsScale(40), TS_MARGIN,
                      panel.w - tsScale(140), tsScale(TS_SEARCH_H)),
           g_ts.query[0] ? theme->font_ui : theme->font_small,
           g_ts.query[0] ? theme->text : theme->text_faint, UI_ALIGN_LEFT);

    uiText(&painter, "Ctrl+Shift+A",
           uiRectMake(panel.x + panel.w - tsScale(116), TS_MARGIN,
                      tsScale(104), tsScale(TS_SEARCH_H)),
           theme->font_small, theme->text_faint, UI_ALIGN_RIGHT);

    y = TS_MARGIN + tsScale(TS_SEARCH_H);
    uiFillRect(&painter, uiRectMake(TS_MARGIN + 1, y - 1, panel.w - 2, 1),
               theme->menu_sep);

    /* Rows, scrolled; anything outside the panel is skipped. */
    list_bottom = TS_MARGIN + height - TS_MARGIN;
    g_ts.content_h = tsListHeight();

    for (i = 0; i < g_ts.row_count; ++i) {
        TsRow* row = &g_ts.rows[i];
        int row_h = row->kind == TS_ROW_HEADER ? tsScale(TS_HEADER_H)
                                               : tsScale(TS_ROW_H);
        int top = y - g_ts.scroll_y;

        if (top + row_h <= y || top >= list_bottom) {
            y += row_h;
            continue;
        }

        if (row->kind == TS_ROW_HEADER) {
            uiText(&painter, row->label,
                   uiRectMake(TS_MARGIN + tsScale(12), top,
                              panel.w - tsScale(24), row_h),
                   theme->font_small, theme->text_faint, UI_ALIGN_LEFT);
        } else {
            UiTabSearchItem* item = &g_ts.items[row->item];
            int selected = (i == g_ts.selected);
            UiRect row_rect = uiRectMake(TS_MARGIN + tsScale(6), top,
                                         panel.w - tsScale(12), row_h - tsScale(2));
            UiRect favicon = uiRectMake(TS_MARGIN + tsScale(12), top + tsScale(9),
                                        tsScale(TS_FAVICON), tsScale(TS_FAVICON));
            UiRect title_rect = uiRectMake(TS_MARGIN + tsScale(52), top + tsScale(6),
                                           panel.w - tsScale(90), tsScale(18));
            UiRect sub_rect = uiRectMake(TS_MARGIN + tsScale(52), top + tsScale(25),
                                         panel.w - tsScale(90), tsScale(15));
            char host[128];
            char age[32];
            char sub[220];

            if (selected)
                uiFillRoundRect(&painter, row_rect, tsScale(6), theme->menu_hover);

            uiFaviconChip(&painter, theme, favicon, item->url, item->title);
            uiText(&painter, item->title, title_rect, theme->font_ui,
                   theme->text, UI_ALIGN_LEFT);

            tsHostOf(item->url, host, sizeof(host));
            tsAgeLabel(age, sizeof(age), item->age_ms);
            _snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", host, age);

            uiText(&painter, sub, sub_rect, theme->font_small,
                   theme->text_muted, UI_ALIGN_LEFT);

            if (item->kind == UI_TABSEARCH_OPEN && selected) {
                UiRect close = uiRectMake(row_rect.x + row_rect.w - tsScale(24),
                                          top + tsScale(13), tsScale(20), tsScale(20));

                uiFillCircle(&painter, close.x + close.w / 2,
                             close.y + close.h / 2, close.w / 2,
                             theme->tab_close_hover);
                uiIcon(&painter, UI_ICON_CLOSE,
                       uiRectMake(close.x + tsScale(4), close.y + tsScale(4),
                                  close.w - tsScale(8), close.h - tsScale(8)),
                       theme->text, (1.5f * theme->scale));
            }
        }

        y += row_h;
    }

    uiPainterEnd(&painter);

    destination.x = g_ts.origin.x;
    destination.y = g_ts.origin.y;
    source.x = 0;
    source.y = 0;
    size.cx = width;
    size.cy = height;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    screen = GetDC(NULL);
    UpdateLayeredWindow(g_ts.hwnd, screen, &destination, &size, g_ts.memdc,
                        &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

static int tsRowAt(int client_y)
{
    int y = tsScale(TS_SEARCH_H);
    int i;

    client_y += g_ts.scroll_y;

    for (i = 0; i < g_ts.row_count; ++i) {
        int row_h = g_ts.rows[i].kind == TS_ROW_HEADER ? tsScale(TS_HEADER_H)
                                                       : tsScale(TS_ROW_H);

        if (client_y >= y && client_y < y + row_h)
            return g_ts.rows[i].kind == TS_ROW_ITEM ? i : -1;

        y += row_h;
    }

    return -1;
}

static void tsClampScroll(void)
{
    int view = g_ts.height - tsScale(TS_SEARCH_H);
    int max = g_ts.content_h > view ? g_ts.content_h - view : 0;

    if (g_ts.scroll_y < 0)
        g_ts.scroll_y = 0;
    if (g_ts.scroll_y > max)
        g_ts.scroll_y = max;
}

static void tsScrollToSelection(void)
{
    int top = 0;
    int sel_top = 0;
    int sel_bottom = 0;
    int view = g_ts.height - tsScale(TS_SEARCH_H);
    int i;

    for (i = 0; i < g_ts.row_count; ++i) {
        int row_h = g_ts.rows[i].kind == TS_ROW_HEADER ? tsScale(TS_HEADER_H)
                                                       : tsScale(TS_ROW_H);

        if (i == g_ts.selected) {
            sel_top = top;
            sel_bottom = top + row_h;
        }

        top += row_h;
    }

    if (sel_top < g_ts.scroll_y)
        g_ts.scroll_y = sel_top;
    if (sel_bottom > g_ts.scroll_y + view)
        g_ts.scroll_y = sel_bottom - view;

    tsClampScroll();
}

static void tsMoveSelection(int direction)
{
    int i = g_ts.selected + direction;

    while (i >= 0 && i < g_ts.row_count && g_ts.rows[i].kind != TS_ROW_ITEM)
        i += direction;

    if (i < 0 || i >= g_ts.row_count)
        return;

    g_ts.selected = i;
    tsScrollToSelection();
    tsRender();
}

static void tsActivate(int row)
{
    int item;
    int kind;

    if (row < 0 || row >= g_ts.row_count || g_ts.rows[row].kind != TS_ROW_ITEM)
        return;

    item = g_ts.rows[row].item;
    kind = g_ts.items[item].kind;

    uiTabSearchClose();

    if (g_ts.cb.activate)
        g_ts.cb.activate(g_ts.cb.user, item);
    (void)kind;
}

static LRESULT CALLBACK tsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEMOVE: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int row = tsRowAt(point.y);

        if (row >= 0 && row != g_ts.selected) {
            g_ts.selected = row;
            tsRender();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int row = tsRowAt(point.y);

        if (row >= 0)
            g_ts.selected = row;
        else
            uiTabSearchClose();   /* padding / search row: dismiss like a menu */

        tsRender();
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int row = tsRowAt(point.y);

        if (row < 0 || g_ts.rows[row].kind != TS_ROW_ITEM)
            return 0;

        {
            UiTabSearchItem* item = &g_ts.items[g_ts.rows[row].item];

            /* The close cross occupies the right end of a hovered open row. */
            if (item->kind == UI_TABSEARCH_OPEN &&
                point.x > g_ts.width - tsScale(36) - TS_MARGIN) {
                if (g_ts.cb.close)
                    g_ts.cb.close(g_ts.cb.user, g_ts.rows[row].item);

                uiTabSearchClose();
                return 0;
            }
        }

        tsActivate(row);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        int delta = (int)(short)HIWORD(wParam);

        g_ts.scroll_y -= delta / 120 * tsScale(TS_ROW_H);
        tsClampScroll();
        tsRender();
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int uiTabSearchOpen(HWND owner, const UiTheme* theme,
                    const UiTabSearchItem* items, int count,
                    const UiTabSearchCallbacks* callbacks, int x, int y)
{
    RECT work = { 0, 0, 0, 0 };
    HMONITOR monitor;
    MONITORINFO info;
    int total;
    int max_h;
    static int class_registered;

    if (!theme || !items || count <= 0)
        return 0;

    g_ts.theme = theme;
    g_ts.owner = owner;

    if (callbacks)
        g_ts.cb = *callbacks;

    if (count > UI_TABSEARCH_MAX_ITEMS)
        count = UI_TABSEARCH_MAX_ITEMS;

    memcpy(g_ts.items, items, sizeof(UiTabSearchItem) * (size_t)count);
    g_ts.item_count = count;
    g_ts.query[0] = 0;
    g_ts.scroll_y = 0;
    tsRebuildRows();

    if (!g_ts.hwnd) {
        WNDCLASSA wc;

        if (!class_registered) {
            memset(&wc, 0, sizeof(wc));
            wc.lpfnWndProc = tsProc;
            wc.hInstance = GetModuleHandleA(NULL);
            wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
            wc.hbrBackground = NULL;
            wc.lpszClassName = "AgaveShellTabSearch";
            RegisterClassA(&wc);
            class_registered = 1;
        }

        g_ts.hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                    "AgaveShellTabSearch", "", WS_POPUP,
                                    0, 0, 0, 0, NULL, NULL,
                                    GetModuleHandleA(NULL), NULL);

        if (!g_ts.hwnd)
            return 0;
    }

    /* As tall as the list needs, capped by the monitor's work area. */
    total = tsScale(TS_SEARCH_H) + tsListHeight();
    monitor = MonitorFromWindow(owner ? owner : g_ts.hwnd, MONITOR_DEFAULTTONEAREST);
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);

    if (monitor && GetMonitorInfoA(monitor, &info))
        work = info.rcWork;

    max_h = (work.bottom - work.top) - 48;
    g_ts.height = total < max_h ? total : max_h;
    if (g_ts.height < tsScale(TS_SEARCH_H) + tsScale(TS_ROW_H))
        g_ts.height = tsScale(TS_SEARCH_H) + tsScale(TS_ROW_H);
    g_ts.width = tsScale(TS_WIDTH);
    g_ts.scroll_y = 0;

    g_ts.origin.x = x;
    g_ts.origin.y = y;

    if (g_ts.origin.x + g_ts.width + TS_MARGIN * 2 > work.right)
        g_ts.origin.x = work.right - g_ts.width - TS_MARGIN * 2;
    if (g_ts.origin.x < work.left)
        g_ts.origin.x = work.left;
    if (g_ts.origin.y + g_ts.height + TS_MARGIN * 2 > work.bottom)
        g_ts.origin.y = work.bottom - g_ts.height - TS_MARGIN * 2;
    if (g_ts.origin.y < work.top)
        g_ts.origin.y = work.top;

    SetWindowPos(g_ts.hwnd, HWND_TOPMOST, g_ts.origin.x, g_ts.origin.y,
                 g_ts.width + TS_MARGIN * 2, g_ts.height + TS_MARGIN * 2,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    tsRender();

    if (!g_ts.captured) {
        SetCapture(g_ts.hwnd);
        g_ts.captured = 1;
    }

    return 1;
}

void uiTabSearchRefresh(const UiTabSearchItem* items, int count)
{
    int keep;
    int i;

    if (!g_ts.hwnd || !g_ts.theme || count <= 0)
        return;

    if (count > UI_TABSEARCH_MAX_ITEMS)
        count = UI_TABSEARCH_MAX_ITEMS;

    memcpy(g_ts.items, items, sizeof(UiTabSearchItem) * (size_t)count);
    g_ts.item_count = count;

    keep = g_ts.selected >= 0 && g_ts.selected < g_ts.row_count &&
           g_ts.rows[g_ts.selected].kind == TS_ROW_ITEM
               ? g_ts.rows[g_ts.selected].item
               : -1;

    tsRebuildRows();

    if (keep >= 0) {
        for (i = 0; i < g_ts.row_count; ++i) {
            if (g_ts.rows[i].kind == TS_ROW_ITEM && g_ts.rows[i].item == keep) {
                g_ts.selected = i;
                break;
            }
        }
    }

    tsClampScroll();
    tsRender();
}

void uiTabSearchClose(void)
{
    if (!g_ts.hwnd)
        return;

    if (g_ts.captured) {
        ReleaseCapture();
        g_ts.captured = 0;
    }

    ShowWindow(g_ts.hwnd, SW_HIDE);
}

int uiTabSearchIsOpen(void)
{
    return g_ts.hwnd && IsWindowVisible(g_ts.hwnd);
}

void uiTabSearchKey(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!g_ts.hwnd || !uiTabSearchIsOpen())
        return;

    (void)lParam;

    if (message == WM_KEYDOWN) {
        switch (wParam) {
        case VK_UP:   tsMoveSelection(-1); break;
        case VK_DOWN: tsMoveSelection(1); break;
        case VK_RETURN:
            tsActivate(g_ts.selected);
            break;
        case VK_ESCAPE:
            uiTabSearchClose();
            break;
        default:
            break;
        }

        return;
    }

    if (message == WM_CHAR) {
        unsigned int ch = (unsigned int)wParam;
        size_t len = strlen(g_ts.query);

        if (ch == 8 || ch == 127) {
            if (len > 0)
                g_ts.query[len - 1] = 0;
        } else if (ch >= 0x20 && ch != 0x7F) {
            /* WM_CHAR is one UTF-16 unit; encode it as UTF-8 so CJK queries
             * match the UTF-8 titles/urls. Surrogates are dropped - they only
             * occur outside the BMP, and a query rarely needs them. */
            char encoded[4] = { 0 };
            int n = 0;

            if (ch < 0x80) {
                encoded[n++] = (char)ch;
            } else if (ch < 0x800) {
                encoded[n++] = (char)(0xC0 | (ch >> 6));
                encoded[n++] = (char)(0x80 | (ch & 0x3F));
            } else if (ch < 0xD800 || ch > 0xDFFF) {
                encoded[n++] = (char)(0xE0 | (ch >> 12));
                encoded[n++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                encoded[n++] = (char)(0x80 | (ch & 0x3F));
            }

            if (n > 0 && len + (size_t)n < sizeof(g_ts.query)) {
                memcpy(g_ts.query + len, encoded, (size_t)n);
                g_ts.query[len + n] = 0;
            }
        } else {
            return;
        }

        tsRebuildRows();
        tsClampScroll();
        tsRender();
    }
}
