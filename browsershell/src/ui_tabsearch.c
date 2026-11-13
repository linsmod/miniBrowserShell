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
#define TS_SEARCH_H    50
#define TS_HEADER_H    30
#define TS_ROW_H       46
#define TS_FAVICON     28
#define TS_MARGIN      2
#define TS_MAX_QUERY   128
#define TS_SCROLL_W    4
#define TS_SCROLL_MIN  24
#define TS_FIELD_INSET 8

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
    int dragging_scroll;  /* the scrollbar thumb is being dragged */
    int scroll_drag_offset;
    int wheel_accum;      /* WM_MOUSEWHEEL leftovers (precision touchpads) */
    int caret_on;         /* blink phase of the query caret */
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

static int tsListHeight(void);

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

    g_ts.content_h = tsListHeight();
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

/* ------------------------------------------------------------------------- */
/* scrollbar geometry                                                         */
/* ------------------------------------------------------------------------- */

static void tsClampScroll(void);
static void tsRender(void);

static int tsScrollView(void)
{
    return g_ts.height - tsScale(TS_SEARCH_H);
}

static int tsScrollbarVisible(void)
{
    return g_ts.content_h > tsScrollView() && tsScrollView() > 0;
}

/* Space the rows leave free on the right while the scrollbar is shown. */
static int tsScrollReserve(void)
{
    return tsScrollbarVisible() ? tsScale(TS_SCROLL_W + 6) : 0;
}

static UiRect tsScrollTrack(void)
{
    int view = tsScrollView();
    int x = TS_MARGIN + g_ts.width - tsScale(TS_SCROLL_W + 6);
    int y = TS_MARGIN + tsScale(TS_SEARCH_H) + tsScale(4);
    int h = view - tsScale(8);

    if (h < tsScale(TS_SCROLL_MIN))
        h = tsScale(TS_SCROLL_MIN);

    return uiRectMake(x, y, tsScale(TS_SCROLL_W), h);
}

static UiRect tsScrollThumb(void)
{
    UiRect track = tsScrollTrack();
    int view = tsScrollView();
    int max = g_ts.content_h > view ? g_ts.content_h - view : 0;
    int thumb_h;
    int y;

    if (max <= 0)
        return track;

    thumb_h = (int)((long long)track.h * view / g_ts.content_h);
    if (thumb_h < tsScale(TS_SCROLL_MIN))
        thumb_h = tsScale(TS_SCROLL_MIN);
    if (thumb_h > track.h)
        thumb_h = track.h;

    y = track.y + (int)((long long)g_ts.scroll_y * (track.h - thumb_h) / max);
    return uiRectMake(track.x, y, track.w, thumb_h);
}

/* 1 when the point is on the scrollbar; *on_thumb says whether it is the thumb
 * (drag) or the track (jump). */
static int tsScrollbarHit(int x, int y, int* on_thumb)
{
    UiRect track;
    UiRect thumb;

    if (!tsScrollbarVisible())
        return 0;

    track = tsScrollTrack();
    if (x < track.x - tsScale(4) || x >= track.x + track.w + tsScale(4) ||
        y < track.y || y >= track.y + track.h)
        return 0;

    thumb = tsScrollThumb();
    if (on_thumb)
        *on_thumb = (y >= thumb.y && y < thumb.y + thumb.h);
    return 1;
}

static void tsScrollFromThumb(int thumb_y)
{
    UiRect track = tsScrollTrack();
    UiRect thumb = tsScrollThumb();
    int view = tsScrollView();
    int max = g_ts.content_h > view ? g_ts.content_h - view : 0;
    int span = track.h - thumb.h;

    if (max <= 0 || span <= 0)
        return;

    if (thumb_y < track.y)
        thumb_y = track.y;
    if (thumb_y > track.y + span)
        thumb_y = track.y + span;

    g_ts.scroll_y = (int)((long long)(thumb_y - track.y) * max / span);
    tsClampScroll();
    tsRender();
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
    int list_top;
    int reserve;

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

    /* The filter is a real input box, not a flat menu row: rounded fill,
     * border, magnifier and a caret after the query. The popup cannot host a
     * native EDIT (layered window), so the shell forwards the keys. */
    {
        UiRect field = uiRectMake(TS_MARGIN + tsScale(TS_FIELD_INSET),
                                  TS_MARGIN + tsScale(7),
                                  panel.w - tsScale(TS_FIELD_INSET * 2),
                                  tsScale(TS_SEARCH_H) - tsScale(14));
        UiRect icon = uiRectMake(field.x + tsScale(10),
                                 field.y + (field.h - tsScale(16)) / 2,
                                 tsScale(16), tsScale(16));
        UiRect text = uiRectMake(field.x + tsScale(34), field.y,
                                 field.w - tsScale(34) - tsScale(94), field.h);
        GpFont* font = g_ts.query[0] ? theme->font_ui : theme->font_small;
        const char* label = g_ts.query[0] ? g_ts.query : "Search tabs";
        int text_w = 0;
        int text_h = 0;
        int caret_x;
        int caret_w = tsScale(1);

        uiFillRoundRect(&painter, field, tsScale(6), theme->field);
        uiStrokeRoundRect(&painter, field, tsScale(6), theme->border_strong, 1.0f);
        uiIcon(&painter, UI_ICON_SEARCH, icon, theme->icon, (1.5f * theme->scale));

        if (!g_ts.query[0]) {
            /* Keep the placeholder clear of the caret sitting at the start. */
            text.x += tsScale(3);
            text.w -= tsScale(3);
        }

        uiText(&painter, label, text, font,
               g_ts.query[0] ? theme->text : theme->text_faint, UI_ALIGN_LEFT);

        if (caret_w < 1)
            caret_w = 1;

        if (!uiMeasureText(theme, font, g_ts.query[0] ? g_ts.query : "",
                           &text_w, &text_h))
            text_w = g_ts.query[0] ? (int)strlen(g_ts.query) * tsScale(8) : 0;

        caret_x = text.x + text_w + tsScale(1);
        if (caret_x > text.x + text.w - tsScale(2))
            caret_x = text.x + text.w - tsScale(2);

        if (g_ts.caret_on)
            uiFillRect(&painter,
                       uiRectMake(caret_x, field.y + tsScale(9), caret_w,
                                  field.h - tsScale(18)),
                       theme->text);

        uiText(&painter, "Ctrl+Shift+A",
               uiRectMake(field.x + field.w - tsScale(94), field.y,
                          tsScale(86), field.h),
               theme->font_small, theme->text_faint, UI_ALIGN_RIGHT);
    }

    y = TS_MARGIN + tsScale(TS_SEARCH_H);
    uiFillRect(&painter, uiRectMake(TS_MARGIN + 1, y - 1, panel.w - 2, 1),
               theme->menu_sep);

    /* Rows, scrolled; anything outside the panel is skipped. The scrollbar
     * takes a strip on the right, so rows leave that space free. */
    list_bottom = height - TS_MARGIN;
    list_top = y;
    g_ts.content_h = tsListHeight();

    reserve = tsScrollReserve();

    /* A scrollbar drag can leave scroll_y between rows; clip the rows to the
     * list viewport so a half-visible row cannot paint over the search field. */
    GdipSetClipRectI(painter.g, TS_MARGIN, list_top, g_ts.width,
                     list_bottom - list_top, CombineModeReplace);

    for (i = 0; i < g_ts.row_count; ++i) {
        TsRow* row = &g_ts.rows[i];
        int row_h = row->kind == TS_ROW_HEADER ? tsScale(TS_HEADER_H)
                                               : tsScale(TS_ROW_H);
        int top = y - g_ts.scroll_y;

        /* Visibility is relative to the list viewport, not to the running y:
         * comparing with y skipped every row once scroll_y reached one row
         * height, so the list went blank after the first wheel notch. */
        if (top + row_h <= list_top || top >= list_bottom) {
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
                                         panel.w - tsScale(12) - reserve,
                                         row_h - tsScale(2));
            UiRect favicon = uiRectMake(TS_MARGIN + tsScale(12), top + tsScale(9),
                                        tsScale(TS_FAVICON), tsScale(TS_FAVICON));
            UiRect title_rect = uiRectMake(TS_MARGIN + tsScale(52), top + tsScale(6),
                                           panel.w - tsScale(90) - reserve, tsScale(18));
            UiRect sub_rect = uiRectMake(TS_MARGIN + tsScale(52), top + tsScale(25),
                                         panel.w - tsScale(90) - reserve, tsScale(15));
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

    GdipResetClip(painter.g);

    if (tsScrollbarVisible()) {
        UiRect track = tsScrollTrack();
        UiRect thumb = tsScrollThumb();

        uiFillRoundRect(&painter, track, track.w / 2, theme->menu_sep);
        uiFillRoundRect(&painter, thumb, thumb.w / 2, theme->text_faint);
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

static void tsMoveSelectionPage(int direction)
{
    int view = tsScrollView();
    int step = tsScale(TS_ROW_H);
    int steps = step > 0 ? view / step : 1;
    int target = g_ts.selected;
    int i;

    if (steps < 1)
        steps = 1;

    for (i = 0; i < steps; ++i) {
        int j = target + direction;

        while (j >= 0 && j < g_ts.row_count && g_ts.rows[j].kind != TS_ROW_ITEM)
            j += direction;
        if (j < 0 || j >= g_ts.row_count)
            break;
        target = j;
    }

    if (target != g_ts.selected) {
        g_ts.selected = target;
        tsScrollToSelection();
        tsRender();
    }
}

static void tsSelectEdge(int first)
{
    int i = first ? 0 : g_ts.row_count - 1;

    for (; i >= 0 && i < g_ts.row_count; i += first ? 1 : -1) {
        if (g_ts.rows[i].kind == TS_ROW_ITEM) {
            g_ts.selected = i;
            tsScrollToSelection();
            tsRender();
            return;
        }
    }
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

static void tsWheel(int delta)
{
    int step = tsScale(TS_ROW_H);

    /* Accumulate: a precision touchpad sends deltas below WHEEL_DELTA and the
     * old delta/120 arithmetic turned those into zero (no scrolling). */
    g_ts.wheel_accum += delta;

    while (g_ts.wheel_accum >= WHEEL_DELTA) {
        g_ts.wheel_accum -= WHEEL_DELTA;
        g_ts.scroll_y -= step;
    }
    while (g_ts.wheel_accum <= -WHEEL_DELTA) {
        g_ts.wheel_accum += WHEEL_DELTA;
        g_ts.scroll_y += step;
    }

    tsClampScroll();
    tsRender();
}

void uiTabSearchWheel(int delta)
{
    if (!uiTabSearchIsOpen())
        return;

    tsWheel(delta);
}

static LRESULT CALLBACK tsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_MOUSEACTIVATE:
        /* Never take activation: the shell keeps the keyboard so its loop can
         * forward the query to uiTabSearchKey(). */
        return MA_NOACTIVATE;

    case WM_CAPTURECHANGED:
        if (g_ts.captured) {
            g_ts.captured = 0;
            uiTabSearchClose();
        }
        return 0;

    case WM_TIMER:
        if (wParam == 1) {
            g_ts.caret_on = !g_ts.caret_on;
            tsRender();
        }
        return 0;

    case WM_MOUSEMOVE: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int on_thumb = 0;
        int row;

        if (g_ts.dragging_scroll) {
            tsScrollFromThumb(point.y - g_ts.scroll_drag_offset);
            return 0;
        }

        if (tsScrollbarHit(point.x, point.y, &on_thumb))
            return 0;

        row = tsRowAt(point.y);
        if (row >= 0 && row != g_ts.selected) {
            g_ts.selected = row;
            tsRender();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int on_thumb = 0;
        int row;

        if (tsScrollbarHit(point.x, point.y, &on_thumb)) {
            UiRect thumb = tsScrollThumb();

            g_ts.dragging_scroll = 1;
            g_ts.scroll_drag_offset = on_thumb ? point.y - thumb.y : thumb.h / 2;
            tsScrollFromThumb(point.y - g_ts.scroll_drag_offset);
            return 0;
        }

        /* The search field is the filter input: a click there must not dismiss
         * the popup. Only a click outside the panel closes it, like a menu. */
        if (point.x >= TS_MARGIN && point.x < TS_MARGIN + g_ts.width &&
            point.y >= TS_MARGIN && point.y < TS_MARGIN + tsScale(TS_SEARCH_H)) {
            g_ts.caret_on = 1;
            tsRender();
            return 0;
        }

        if (point.x < TS_MARGIN || point.x >= TS_MARGIN + g_ts.width ||
            point.y < TS_MARGIN || point.y >= TS_MARGIN + g_ts.height) {
            uiTabSearchClose();
            return 0;
        }

        row = tsRowAt(point.y);
        if (row >= 0)
            g_ts.selected = row;

        tsRender();
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int on_thumb = 0;
        int row;

        if (g_ts.dragging_scroll) {
            g_ts.dragging_scroll = 0;
            return 0;
        }

        if (tsScrollbarHit(point.x, point.y, &on_thumb))
            return 0;

        row = tsRowAt(point.y);
        if (row < 0 || g_ts.rows[row].kind != TS_ROW_ITEM)
            return 0;

        {
            UiTabSearchItem* item = &g_ts.items[g_ts.rows[row].item];
            int row_w = (g_ts.width - TS_MARGIN * 2) - tsScale(12) - tsScrollReserve();
            int close_left = TS_MARGIN + tsScale(6) + row_w - tsScale(24);

            /* The close cross occupies the right end of a hovered open row. */
            if (item->kind == UI_TABSEARCH_OPEN &&
                point.x >= close_left && point.x < close_left + tsScale(20)) {
                if (g_ts.cb.close)
                    g_ts.cb.close(g_ts.cb.user, g_ts.rows[row].item);

                uiTabSearchClose();
                return 0;
            }
        }

        tsActivate(row);
        return 0;
    }

    case WM_MOUSEWHEEL:
        tsWheel((int)(short)HIWORD(wParam));
        return 0;

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
    g_ts.dragging_scroll = 0;
    g_ts.wheel_accum = 0;
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

        g_ts.hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                        WS_EX_NOACTIVATE,
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

    g_ts.caret_on = 1;
    tsRender();

    if (!g_ts.captured) {
        SetCapture(g_ts.hwnd);
        g_ts.captured = 1;
    }

    SetTimer(g_ts.hwnd, 1, GetCaretBlinkTime() > 0 ? GetCaretBlinkTime() : 530, NULL);

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

    KillTimer(g_ts.hwnd, 1);

    if (g_ts.captured) {
        g_ts.captured = 0;      /* before ReleaseCapture: WM_CAPTURECHANGED must not recurse */
        ReleaseCapture();
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

    g_ts.caret_on = 1;

    if (message == WM_KEYDOWN) {
        switch (wParam) {
        case VK_UP:    tsMoveSelection(-1); break;
        case VK_DOWN:  tsMoveSelection(1); break;
        case VK_PRIOR: tsMoveSelectionPage(-1); break;
        case VK_NEXT:  tsMoveSelectionPage(1); break;
        case VK_HOME:  tsSelectEdge(1); break;
        case VK_END:   tsSelectEdge(0); break;
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
        g_ts.scroll_y = 0;      /* a new filter starts at the top */
        tsScrollToSelection();
        tsClampScroll();
        tsRender();
    }
}
