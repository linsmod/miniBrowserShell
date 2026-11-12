/*
 * ui_chrome.c - toolbar, find bar, status bar and the UiChrome composition.
 */

#include "ui_chrome.h"

#include <stdio.h>
#include <string.h>

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

static int scale(const UiTheme* theme, int value)
{
    return uiThemeScale(theme, value);
}

static int startsWithNoCase(const char* text, const char* prefix)
{
    size_t n;

    if (!text || !prefix)
        return 0;

    n = strlen(prefix);

    return _strnicmp(text, prefix, n) == 0;
}

static UiRect partRect(UiChrome* chrome, UiPart part, int index);
static void paintKeyboardFocus(UiChrome* chrome, UiPainter* painter, int dx, int dy);

static void emitCommand(UiChrome* chrome, int command)
{    if (chrome && chrome->cb.command)
        chrome->cb.command(chrome->cb.user, command);
}

static void emitFocus(UiChrome* chrome, int field)
{
    if (chrome && chrome->cb.focus_field)
        chrome->cb.focus_field(chrome->cb.user, field);
}

/* ------------------------------------------------------------------------- */
/* init / teardown                                                           */
/* ------------------------------------------------------------------------- */

void uiChromeInit(UiChrome* chrome, UiThemeKind kind, unsigned dpi,
                  int offscreen, const UiChromeCallbacks* callbacks)
{
    if (!chrome)
        return;

    memset(chrome, 0, sizeof(*chrome));
    uiThemeInit(&chrome->theme, kind, dpi);
    chrome->offscreen = offscreen;
    chrome->focus_field = 0;
    chrome->hover_part = UI_PART_NONE;
    chrome->model.active = -1;

    if (callbacks)
        chrome->cb = *callbacks;

    uiButtonInit(&chrome->toolbar.back, UI_CMD_BACK, UI_ICON_BACK);
    uiButtonInit(&chrome->toolbar.forward, UI_CMD_FORWARD, UI_ICON_FORWARD);
    uiButtonInit(&chrome->toolbar.reload, UI_CMD_RELOAD, UI_ICON_RELOAD);
    uiButtonInit(&chrome->toolbar.go, UI_CMD_GO, UI_ICON_ARROW_RIGHT);
    uiButtonInit(&chrome->toolbar.menu, UI_CMD_MENU, UI_ICON_MENU);

    uiButtonInit(&chrome->findbar.prev, UI_CMD_FIND_PREV, UI_ICON_CHEVRON_UP);
    uiButtonInit(&chrome->findbar.next, UI_CMD_FIND_NEXT, UI_ICON_CHEVRON_DOWN);
    uiButtonInit(&chrome->findbar.close, UI_CMD_FIND_CLOSE, UI_ICON_CLOSE);

    uiButtonInit(&chrome->win_min, UI_CMD_MINIMIZE, UI_ICON_MINUS);
    uiButtonInit(&chrome->win_max, UI_CMD_MAXIMIZE, UI_ICON_MAXIMIZE);
    uiButtonInit(&chrome->win_close, UI_CMD_CLOSE_WINDOW, UI_ICON_CLOSE);
    chrome->win_min.window_control = 1;
    chrome->win_max.window_control = 1;
    chrome->win_close.window_control = 1;
    chrome->win_close.danger = 1;
}

void uiChromeFree(UiChrome* chrome)
{
    if (!chrome)
        return;

    uiThemeFree(&chrome->theme);
}

void uiChromeSetDpi(UiChrome* chrome, unsigned dpi)
{
    if (!chrome)
        return;

    uiThemeSetDpi(&chrome->theme, dpi);
}

void uiChromeSetNative(UiChrome* chrome, void* parent, void* address, void* find)
{
    if (!chrome)
        return;

    chrome->parent_hwnd = parent;
    chrome->address_hwnd = address;
    chrome->find_hwnd = find;
}

void uiChromeSetFocusField(UiChrome* chrome, int field)
{
    if (!chrome)
        return;

    chrome->focus_field = field;
}

/* ------------------------------------------------------------------------- */
/* layout                                                                    */
/* ------------------------------------------------------------------------- */

void uiChromeLayout(UiChrome* chrome, int width, int height)
{
    UiTheme* theme;
    UiChromeModel* model;
    int pad, gap, x, y;
    int toolbar_top, toolbar_h, strip_h, status_h;
    int content_top;
    int button_w, button_h;

    if (!chrome)
        return;

    theme = &chrome->theme;
    model = &chrome->model;

    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;

    chrome->width = width;
    chrome->height = height;

    strip_h = theme->tabstrip_h;
    toolbar_h = theme->toolbar_h;
    status_h = theme->status_h;
    pad = scale(theme, 6);
    gap = scale(theme, 2);
    button_w = theme->button_w;
    button_h = theme->button_h;

    /* Chrome's caption controls are self-drawn at the right edge of the
     * title bar; the tab strip gets everything to their left. */
    {
        int control_w = theme->window_btn_w;

        chrome->win_min.rect = uiRectMake(width - control_w * 3, 0, control_w, strip_h);
        chrome->win_max.rect = uiRectMake(width - control_w * 2, 0, control_w, strip_h);
        chrome->win_close.rect = uiRectMake(width - control_w, 0, control_w, strip_h);

        chrome->tabstrip.right_reserved = control_w * 3;
    }

    uiTabStripLayout(&chrome->tabstrip, theme, model,
                     uiRectMake(0, 0, width, strip_h));

    toolbar_top = strip_h;
    chrome->toolbar.rect = uiRectMake(0, toolbar_top, width, toolbar_h);

    y = toolbar_top + (toolbar_h - button_h) / 2;
    x = pad;

    chrome->toolbar.back.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;
    chrome->toolbar.forward.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;
    chrome->toolbar.reload.rect = uiRectMake(x, y, button_w, button_h);
    x += button_w + gap;

    /* Chrome has no divider between the nav buttons and the omnibox; the
     * gap alone groups them. */
    chrome->toolbar.separator = uiRectMake(0, 0, 0, 0);
    x += scale(theme, 8);

    {
        int menu_x = width - pad - button_w;
        int address_x = x + scale(theme, 4);
        int address_right = menu_x - scale(theme, 8);
        int address_h = theme->address_h;
        int address_y = toolbar_top + (toolbar_h - address_h) / 2;
        int go_size = scale(theme, 28);
        int edit_x;
        int edit_w;

        chrome->toolbar.menu.rect = uiRectMake(menu_x, y, button_w, button_h);

        if (address_right < address_x + scale(theme, 120))
            address_right = address_x + scale(theme, 120);
        if (address_right > width - pad)
            address_right = width - pad;

        chrome->toolbar.address = uiRectMake(address_x, address_y,
                                             address_right - address_x, address_h);
        chrome->toolbar.address_icon = uiRectMake(address_x + scale(theme, 11),
                                                  address_y + (address_h - scale(theme, 16)) / 2,
                                                  scale(theme, 16), scale(theme, 16));

        chrome->toolbar.go.rect = uiRectMake(
            chrome->toolbar.address.x + chrome->toolbar.address.w - scale(theme, 4) - go_size,
            address_y + (address_h - go_size) / 2,
            go_size, go_size);
        chrome->toolbar.go.icon = UI_ICON_ARROW_RIGHT;

        edit_x = chrome->toolbar.address_icon.x + chrome->toolbar.address_icon.w + scale(theme, 8);
        edit_w = chrome->toolbar.go.rect.x - edit_x - scale(theme, 6);

        if (edit_w < scale(theme, 40))
            edit_w = scale(theme, 40);

        chrome->address_edit = uiRectMake(edit_x,
                                          address_y + (address_h - scale(theme, 20)) / 2,
                                          edit_w, scale(theme, 20));
    }

    content_top = strip_h + toolbar_h;

    /* The page keeps its full height: the status bar is a bubble that floats
     * over the bottom-left corner and only exists while it has something to say
     * (see the statusbar block below). */
    chrome->content = uiRectMake(0, content_top, width, height - content_top);

    if (chrome->content.h < 0)
        chrome->content.h = 0;

    /* The find bar floats over the top-right of the page: it does not take a row
     * of its own (the page used to shift down by find_h whenever it opened).
     * The rect is in shell-client coordinates; the shell puts an overlay child
     * window exactly there and the native EDIT inside it.
     *
     * Chrome's shape (the reference): a white rounded card, a grey query pill
     * inside it, the "3/12" counter, a hairline, then prev / next / close. The
     * card used to *be* the pill, so the query, the counter and the buttons all
     * shared one grey capsule. */
    if (model->find_visible && chrome->content.w > scale(theme, 160)) {
        int cardPad = scale(theme, 8);              /* padding inside the card */
        int gap = scale(theme, 8);                  /* between the blocks */
        int bs = scale(theme, 28);                  /* icon button */
        int card_h = theme->find_field_h + cardPad * 2;
        int field_h = card_h - cardPad * 2;
        int count_w = scale(theme, 44);             /* "12/34" */
        int card_w;
        int right;

        card_w = cardPad + theme->find_field_w + gap + count_w + gap +
                 scale(theme, 1) + gap + bs * 3 + cardPad;

        if (card_w > chrome->content.w - pad * 2)
            card_w = chrome->content.w - pad * 2;
        if (card_w < scale(theme, 200))
            card_w = scale(theme, 200);
        if (card_h < scale(theme, 32))
            card_h = scale(theme, 32);

        chrome->findbar.rect = uiRectMake(
            chrome->content.x + chrome->content.w - pad - card_w,
            chrome->content.y + pad, card_w, card_h);

        if (chrome->findbar.rect.x < chrome->content.x + pad)
            chrome->findbar.rect.x = chrome->content.x + pad;

        /* Right to left: close, next, prev, hairline, counter, then the pill
         * takes whatever is left. */
        right = chrome->findbar.rect.x + chrome->findbar.rect.w - cardPad;

        chrome->findbar.close.rect = uiRectMake(right - bs,
                                                chrome->findbar.rect.y +
                                                    (card_h - bs) / 2, bs, bs);
        right -= bs;
        chrome->findbar.next.rect = uiRectMake(right - bs,
                                               chrome->findbar.rect.y +
                                                   (card_h - bs) / 2, bs, bs);
        right -= bs;
        chrome->findbar.prev.rect = uiRectMake(right - bs,
                                               chrome->findbar.rect.y +
                                                   (card_h - bs) / 2, bs, bs);
        right -= bs + gap;

        chrome->findbar.sep = uiRectMake(right - scale(theme, 1),
                                         chrome->findbar.rect.y + cardPad + scale(theme, 5),
                                         scale(theme, 1),
                                         card_h - (cardPad + scale(theme, 5)) * 2);
        right -= scale(theme, 1) + gap;

        chrome->findbar.count = uiRectMake(right - count_w,
                                           chrome->findbar.rect.y, count_w, card_h);
        right -= count_w + gap;

        chrome->findbar.field = uiRectMake(chrome->findbar.rect.x + cardPad,
                                            chrome->findbar.rect.y + cardPad,
                                            right - (chrome->findbar.rect.x + cardPad),
                                            field_h);
        if (chrome->findbar.field.w < scale(theme, 60))
            chrome->findbar.field.w = scale(theme, 60);

        /* The native EDIT sits inside the pill, inset like Chrome's text. */
        chrome->findbar.edit = uiRectMake(chrome->findbar.field.x + scale(theme, 8),
                                          chrome->findbar.field.y,
                                          chrome->findbar.field.w - scale(theme, 16),
                                          field_h);

        if (chrome->findbar.edit.w < scale(theme, 40))
            chrome->findbar.edit.w = scale(theme, 40);

        chrome->find_edit = chrome->findbar.edit;
    } else {
        memset(&chrome->findbar, 0, sizeof(chrome->findbar));
        chrome->find_edit = uiRectMake(0, 0, 0, 0);
    }

    /* Status bubble: bottom-left of the content, sized to its text (plus the
     * zoom pill when zoomed). Hidden when there is no message - "Ready" is not
     * a message, and a permanent row used to cost the page 24 px. A zero rect
     * means "not shown, nothing to hit". */
    memset(&chrome->statusbar, 0, sizeof(chrome->statusbar));

    {
        int has_message = model->status[0] != 0 &&
                          strcmp(model->status, "Ready") != 0;
        int zoomed = model->zoom != 100;
        int bubble_h = status_h;
        int pad;
        int zoom_w = zoomed ? scale(theme, 46) + scale(theme, 8) : 0;
        int max_w = chrome->content.w - scale(theme, 24);
        int text_w = 0;
        int text_h = 0;
        int w;

        if ((has_message || zoomed) && max_w > scale(theme, 90)) {
            uiMeasureText(theme, theme->font_small,
                          has_message ? model->status : "", &text_w, &text_h);

            /* Left/right padding mirrors the vertical one: the text is centred in
             * the bubble, so the air above it is the air that belongs beside it.
             * A fixed 8-12 DIP padding was visibly wider than the top/bottom. */
            if (text_h <= 0 || text_h > bubble_h - scale(theme, 4))
                text_h = bubble_h - scale(theme, 4);

            pad = (bubble_h - text_h) / 2;
            if (pad < scale(theme, 2))
                pad = scale(theme, 2);

            w = pad * 2 + text_w + zoom_w;

            if (w > max_w)
                w = max_w;
            if (w < scale(theme, 90))
                w = scale(theme, 90);

            chrome->statusbar.rect = uiRectMake(
                chrome->content.x + scale(theme, 8),
                height - bubble_h - scale(theme, 8), w, bubble_h);
            chrome->statusbar.text = uiRectMake(
                chrome->statusbar.rect.x + pad,
                chrome->statusbar.rect.y + pad,
                w - pad * 2 - zoom_w, bubble_h - pad * 2);

            if (zoomed) {
                int zh = bubble_h - scale(theme, 8);

                chrome->statusbar.zoom = uiRectMake(
                    chrome->statusbar.rect.x + w - zoom_w + scale(theme, 4),
                    chrome->statusbar.rect.y + scale(theme, 4),
                    scale(theme, 46), zh);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paintAddress(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiToolbar* bar = &chrome->toolbar;
    UiRect pill = bar->address;
    int state = uiChromeFieldState(chrome, 1);
    int focused = state == 2;
    int hovered = state == 1;
    unsigned fill = focused ? theme->field_focus
                            : (hovered ? theme->field_hover : theme->field);
    int icon_size = scale(theme, 16);
    UiRect icon = bar->address_icon;
    int secure = startsWithNoCase(chrome->model.address, "https://");

    if (pill.w <= 0)
        return;

    uiFillRoundRect(painter, pill, theme->address_radius, fill);

    if (focused)
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->focus_ring,
                          (float)scale(theme, 2));
    else if (hovered)
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->border_strong, 1.0f);
    else
        uiStrokeRoundRect(painter, pill, theme->address_radius, theme->border, 1.0f);

    uiIcon(painter, secure ? UI_ICON_LOCK : UI_ICON_GLOBE, icon,
           theme->icon, (1.5f * theme->scale));

    if (chrome->offscreen || !chrome->address_hwnd) {
        const char* text = chrome->model.address[0] ? chrome->model.address
                                                    : "Search or enter address";
        UiRect text_rect = uiRectMake(icon.x + icon.w + scale(theme, 8), pill.y,
                                      bar->go.rect.x - (icon.x + icon.w + scale(theme, 8)) - scale(theme, 6),
                                      pill.h);

        uiText(painter, text, text_rect, theme->font_address,
               chrome->model.address[0] ? theme->text : theme->text_faint, UI_ALIGN_LEFT);
    }

    (void)icon_size;
    uiButtonPaint(&bar->go, theme, painter);
}

/* The accent progress line under the toolbar is kept but not shown: the engine's
 * progress callback is too coarse to look like real progress (it jumps 10% at a
 * time and sat at 8% on every start), so the bar read as animation, not data.
 * The loading state is still visible through the reload button turning into a
 * stop button. */
#define CHROME_SHOW_PROGRESS 0

static void paintToolbar(UiChrome* chrome, UiPainter* painter)
{
    UiTheme* theme = &chrome->theme;
    UiToolbar* bar = &chrome->toolbar;
    int percent;

    uiFillRect(painter, bar->rect, theme->toolbar);

    uiButtonPaint(&bar->back, theme, painter);
    uiButtonPaint(&bar->forward, theme, painter);
    bar->reload.icon = chrome->model.loading ? UI_ICON_STOP : UI_ICON_RELOAD;
    uiButtonPaint(&bar->reload, theme, painter);

    if (bar->separator.w > 0)
        uiFillRect(painter, bar->separator, theme->border);

    paintAddress(chrome, painter);
    uiButtonPaint(&bar->menu, theme, painter);

    /* A hairline separates the toolbar from the page; progress rides on it. */
    uiFillRect(painter, uiRectMake(0, bar->rect.y + bar->rect.h - scale(theme, 1),
                                   bar->rect.w, scale(theme, 1)), theme->border);

    if (CHROME_SHOW_PROGRESS && chrome->model.loading) {
        percent = CLAMP(chrome->model.progress, 0, 100);

        if (percent <= 0)
            percent = 8;

        uiFillRect(painter, uiRectMake(0, bar->rect.y + bar->rect.h - scale(theme, 2),
                                       bar->rect.w * percent / 100, scale(theme, 2)),
                   theme->accent);
    }
}

/* Chrome's find bar: a white rounded card holding a grey query pill, the match
 * counter, a hairline, and the three icon buttons. It is painted into the
 * shell's overlay child window (see uiChromePaintFindOverlay), not into the
 * chrome, so the (dx, dy) offset maps its shell-client rects onto that window's
 * origin. */
static void paintFindBar(UiChrome* chrome, UiPainter* painter, int dx, int dy)
{
    UiTheme* theme = &chrome->theme;
    UiFindBar* bar = &chrome->findbar;
    UiButton prev = bar->prev;
    UiButton next = bar->next;
    UiButton close = bar->close;
    int state = uiChromeFieldState(chrome, 2);
    int focused = state == 2;
    int hovered = state == 1;
    int radius;
    int field_radius;
    UiRect card;
    UiRect field;
    UiRect count;
    char label[32];

    if (!chrome->model.find_visible || bar->rect.w <= 0 || bar->rect.h <= 0)
        return;

    card = uiRectMake(bar->rect.x + dx, bar->rect.y + dy, bar->rect.w, bar->rect.h);
    field = uiRectMake(bar->field.x + dx, bar->field.y + dy, bar->field.w, bar->field.h);
    count = uiRectMake(bar->count.x + dx, bar->count.y + dy, bar->count.w, bar->count.h);
    radius = scale(theme, 8);
    field_radius = scale(theme, 4);

    /* The card follows the active tab's surface, like the status bubble. */
    uiFillRoundRect(painter, card, radius, theme->surface);
    uiStrokeRoundRect(painter, card, radius, theme->border, 1.0f);

    uiFillRoundRect(painter, field, field_radius,
                    focused ? theme->field_focus
                            : (hovered ? theme->field_hover : theme->field));

    if (focused)
        uiStrokeRoundRect(painter, field, field_radius, theme->focus_ring,
                          (float)scale(theme, 2));

    if (chrome->offscreen || !chrome->find_hwnd) {
        const char* text = chrome->model.find_text[0] ? chrome->model.find_text
                                                     : "Find in page";
        UiRect text_rect = uiRectMake(field.x + scale(theme, 8), field.y,
                                      field.w - scale(theme, 16), field.h);

        uiText(painter, text, text_rect, theme->font_ui,
               chrome->model.find_text[0] ? theme->text : theme->text_faint,
               UI_ALIGN_LEFT);
    }

    /* "3/12", or "0/0" while nothing matches - the same shape Chrome shows. */
    if (chrome->model.find_total > 0) {
        int index = chrome->model.find_index;

        if (index < 1)
            index = 1;
        if (index > chrome->model.find_total)
            index = chrome->model.find_total;

        _snprintf(label, sizeof(label), "%d/%d", index, chrome->model.find_total);
    } else {
        _snprintf(label, sizeof(label), "0/0");
    }
    label[sizeof(label) - 1] = 0;

    uiText(painter, label, count, theme->font_ui, theme->text_muted, UI_ALIGN_CENTER);

    if (bar->sep.w > 0)
        uiFillRect(painter, uiRectMake(bar->sep.x + dx, bar->sep.y + dy,
                                       bar->sep.w, bar->sep.h), theme->border);

    prev.rect.x += dx;  prev.rect.y += dy;
    next.rect.x += dx;  next.rect.y += dy;
    close.rect.x += dx; close.rect.y += dy;

    uiButtonPaint(&prev, theme, painter);
    uiButtonPaint(&next, theme, painter);
    uiButtonPaint(&close, theme, painter);
}

void uiChromePaintFindOverlay(UiChrome* chrome, HDC hdc, int width, int height)
{
    UiPainter painter;

    if (!chrome || !hdc)
        return;

    if (!uiPainterBegin(&painter, hdc, &chrome->theme, width, height))
        return;

    paintFindBar(chrome, &painter, -chrome->findbar.rect.x, -chrome->findbar.rect.y);
    paintKeyboardFocus(chrome, &painter, -chrome->findbar.rect.x, -chrome->findbar.rect.y);

    uiPainterEnd(&painter);
}

/* The status bubble is painted into its own overlay window (like the find bar):
 * rounded card, the message, and the zoom pill when the page is zoomed. */
static void paintStatusBubble(UiChrome* chrome, UiPainter* painter, int dx, int dy)
{
    UiTheme* theme = &chrome->theme;
    UiStatusBar* bar = &chrome->statusbar;
    UiRect rect = uiRectMake(bar->rect.x + dx, bar->rect.y + dy,
                             bar->rect.w, bar->rect.h);
    UiRect text = uiRectMake(bar->text.x + dx, bar->text.y + dy,
                             bar->text.w, bar->text.h);

    if (bar->rect.w <= 0 || bar->rect.h <= 0)
        return;

    /* Same colours as the active tab (surface + text): the bubble is part of the
     * shell's own chrome, not a page overlay, and it should read as one family
     * with the tab it belongs to. */
    uiFillRoundRect(painter, rect, scale(theme, 8), theme->surface);
    uiStrokeRoundRect(painter, rect, scale(theme, 8), theme->border, 1.0f);

    uiText(painter, chrome->model.status, text, theme->font_small,
           theme->text, UI_ALIGN_LEFT);

    if (chrome->model.zoom != 100 && bar->zoom.w > 0) {
        char label[32];
        UiRect zoom = uiRectMake(bar->zoom.x + dx, bar->zoom.y + dy,
                                 bar->zoom.w, bar->zoom.h);

        _snprintf(label, sizeof(label), "%d%%", chrome->model.zoom);
        label[sizeof(label) - 1] = 0;

        uiFillRoundRect(painter, zoom, scale(theme, 8), theme->accent_soft);
        uiText(painter, label, zoom, theme->font_small, theme->accent, UI_ALIGN_CENTER);
    }
}

void uiChromePaintStatusOverlay(UiChrome* chrome, HDC hdc, int width, int height)
{
    UiPainter painter;

    if (!chrome || !hdc)
        return;

    if (!uiPainterBegin(&painter, hdc, &chrome->theme, width, height))
        return;

    paintStatusBubble(chrome, &painter,
                      -chrome->statusbar.rect.x, -chrome->statusbar.rect.y);

    uiPainterEnd(&painter);
}

/* The keyboard focus ring for the painted buttons. Chrome lights the whole
 * button, not a dotted rect: a 2px accent outline inset by 2px. */
static void paintKeyboardFocus(UiChrome* chrome, UiPainter* painter, int dx, int dy)
{
    UiTheme* theme = &chrome->theme;
    UiRect rect;
    int inset;

    if (chrome->focus_part == UI_PART_NONE)
        return;

    /* Find-bar buttons live in their own overlay window. */
    if (chrome->focus_part == UI_PART_FIND_PREV ||
        chrome->focus_part == UI_PART_FIND_NEXT ||
        chrome->focus_part == UI_PART_FIND_CLOSE) {
        if (dx == 0 && dy == 0)
            return;
    } else if (dx != 0 || dy != 0) {
        return;
    }

    rect = partRect(chrome, chrome->focus_part, -1);
    if (rect.w <= 0 || rect.h <= 0)
        return;

    inset = scale(theme, 2);
    rect = uiRectMake(rect.x + dx + inset, rect.y + dy + inset,
                      rect.w - inset * 2, rect.h - inset * 2);
    if (rect.w <= 0 || rect.h <= 0)
        return;

    uiStrokeRoundRect(painter, rect, scale(theme, 8), theme->focus_ring, 2.0f);
}

void uiChromePaint(UiChrome* chrome, HDC hdc)
{
    UiTheme* theme;
    UiPainter painter;

    if (!chrome || !hdc)
        return;

    theme = &chrome->theme;

    if (!uiPainterBegin(&painter, hdc, theme, chrome->width, chrome->height))
        return;

    uiFillRect(&painter, uiRectMake(0, 0, chrome->width, chrome->height), theme->window);

    uiTabStripPaint(&chrome->tabstrip, theme, &chrome->model, &painter);

    uiButtonPaint(&chrome->win_min, theme, &painter);
    uiButtonPaint(&chrome->win_max, theme, &painter);
    uiButtonPaint(&chrome->win_close, theme, &painter);

    paintToolbar(chrome, &painter);

    /* Keyboard focus ring (tab stops over the painted buttons). */
    paintKeyboardFocus(chrome, &painter, 0, 0);
    /* No find bar here: it floats over the page in its own overlay window
     * (uiChromePaintFindOverlay). */
    /* No status row: it is a bubble in its own overlay window now. */

    uiPainterEnd(&painter);
}

/* ------------------------------------------------------------------------- */
/* hit testing / input                                                       */
/* ------------------------------------------------------------------------- */

static int buttonAt(UiButton* button, int x, int y)
{
    return button && button->visible && uiRectContains(button->rect, x, y);
}

/* ------------------------------------------------------------------------- */
/* dirty rectangles                                                          */
/* ------------------------------------------------------------------------- */

static UiRect emptyRect(void)
{
    return uiRectMake(0, 0, 0, 0);
}

static int rectIsEmpty(UiRect r)
{
    return r.w <= 0 || r.h <= 0;
}

static UiRect rectUnion(UiRect a, UiRect b)
{
    int left;
    int top;
    int right;
    int bottom;

    if (rectIsEmpty(a))
        return b;
    if (rectIsEmpty(b))
        return a;

    left = a.x < b.x ? a.x : b.x;
    top = a.y < b.y ? a.y : b.y;
    right = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    bottom = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;

    return uiRectMake(left, top, right - left, bottom - top);
}

static void chromeMarkDirty(UiChrome* chrome, UiRect rect)
{
    if (!chrome || rectIsEmpty(rect))
        return;

    chrome->dirty = chrome->dirty_pending ? rectUnion(chrome->dirty, rect) : rect;
    chrome->dirty_pending = 1;
}

/* Screen-independent rect a part occupies. Deliberately generous: a tab maps
 * to the whole tab slot (the close chip changes the title's ellipsis width and
 * the chip itself lives inside it), a find-bar control to the whole pill (the
 * chevrons sit inside its border). */
static UiRect partRect(UiChrome* chrome, UiPart part, int index)
{
    UiTabStrip* strip = &chrome->tabstrip;
    UiToolbar* bar = &chrome->toolbar;
    UiFindBar* find = &chrome->findbar;

    switch (part) {
    case UI_PART_TAB:
    case UI_PART_TAB_CLOSE:
        if (index >= 0 && index < UI_MAX_TABS) {
            /* Grow sideways: the hairline separators that have to vanish next to
             * a hovered tab live in the strip's gaps, i.e. just outside the tab
             * rect. Without this they stayed painted until something else
             * repainted the strip. */
            int grow = chrome->theme.tab_gap + uiTabStripSeparatorWidth(&chrome->theme);
            UiRect tab = strip->tab[index];

            return uiRectMake(tab.x - grow, tab.y, tab.w + grow * 2, tab.h);
        }
        return emptyRect();

    case UI_PART_TAB_NEW:
        return strip->new_tab;

    case UI_PART_TAB_SEARCH:
        return strip->search;

    case UI_PART_BACK:     return bar->back.rect;
    case UI_PART_FORWARD:  return bar->forward.rect;
    case UI_PART_RELOAD:   return bar->reload.rect;
    /* The Go button lives inside the pill and hovering it also tints the pill
     * (paintAddress keys its hover off go.hover), so the pill is the unit. */
    case UI_PART_GO:       return bar->address;
    case UI_PART_MENU:     return bar->menu.rect;
    case UI_PART_ADDRESS:  return bar->address;

    case UI_PART_FIND_FIELD:
        return find->field;

    case UI_PART_FIND_PREV:
        return find->prev.rect;

    case UI_PART_FIND_NEXT:
        return find->next.rect;

    case UI_PART_FIND_CLOSE:
        return find->close.rect;

    case UI_PART_STATUS:       return chrome->statusbar.rect;
    case UI_PART_WINDOW_MIN:   return chrome->win_min.rect;
    case UI_PART_WINDOW_MAX:   return chrome->win_max.rect;
    case UI_PART_WINDOW_CLOSE: return chrome->win_close.rect;

    default:
        return emptyRect();
    }
}

/* ------------------------------------------------------------------------- */
/* keyboard tab stops                                                        */
/* ------------------------------------------------------------------------- */

/* The painted buttons that can take the keyboard focus ring, in visual order
 * (left to right): tab-search, new tab, back, forward, reload, [address field],
 * menu, then the find-bar buttons. The address field is a native EDIT with its
 * own focus state, so it is an *anchor* here: never a stop of its own, but the
 * place a Tab out of the field starts from (so forward goes right into the menu
 * and Shift+Tab goes left into reload, instead of both jumping to the far left
 * of the toolbar). */
static const int kFocusOrder[] = {
    UI_PART_TAB_SEARCH, UI_PART_TAB_NEW,
    UI_PART_BACK, UI_PART_FORWARD, UI_PART_RELOAD,
    UI_PART_ADDRESS,
    UI_PART_MENU,
    UI_PART_FIND_PREV, UI_PART_FIND_NEXT, UI_PART_FIND_CLOSE
};

static int focusStopVisible(UiChrome* chrome, int part)
{
    switch (part) {
    case UI_PART_TAB_SEARCH: return chrome->tabstrip.search.w > 0;
    case UI_PART_TAB_NEW:    return chrome->tabstrip.new_tab.w > 0;
    case UI_PART_BACK:       return chrome->toolbar.back.visible && chrome->toolbar.back.enabled;
    case UI_PART_FORWARD:    return chrome->toolbar.forward.visible && chrome->toolbar.forward.enabled;
    case UI_PART_RELOAD:     return chrome->toolbar.reload.visible;
    case UI_PART_MENU:       return chrome->toolbar.menu.visible;
    case UI_PART_ADDRESS:    return !chrome->offscreen;  /* native EDIT: a stop in the cycle */
    case UI_PART_FIND_PREV:
    case UI_PART_FIND_NEXT:
    case UI_PART_FIND_CLOSE: return chrome->model.find_visible;
    default:                 return 0;
    }
}

static void focusMarkDirty(UiChrome* chrome, int part)
{
    UiRect rect;

    if (!chrome || part == UI_PART_NONE)
        return;

    rect = partRect(chrome, part, -1);
    if (rect.w > 0 && rect.h > 0)
        chromeMarkDirty(chrome, rect);
}

int uiChromeFocusPart(const UiChrome* chrome)
{
    return chrome ? chrome->focus_part : UI_PART_NONE;
}

void uiChromeFocusClear(UiChrome* chrome)
{
    if (!chrome || chrome->focus_part == UI_PART_NONE)
        return;

    focusMarkDirty(chrome, chrome->focus_part);
    chrome->focus_part = UI_PART_NONE;
}

int uiChromeFocusStepFrom(UiChrome* chrome, int anchor, int forward)
{
    int count = (int)(sizeof(kFocusOrder) / sizeof(kFocusOrder[0]));
    int start = -1;
    int i, step;

    if (!chrome)
        return UI_PART_NONE;

    /* Where to start walking: the current ring position, else the caller's
     * anchor (UI_PART_ADDRESS when Tab leaves the native address field), else
     * the stop before/after the whole cycle. */
    if (chrome->focus_part != UI_PART_NONE)
        anchor = chrome->focus_part;

    for (i = 0; i < count; ++i) {
        if (kFocusOrder[i] == anchor) {
            start = i;
            break;
        }
    }

    i = (start >= 0) ? start : (forward ? -1 : count);

    /* No internal wrap: the ends are the page's neighbours in the shell's
     * focus cycle (page <-> chrome stops <-> address <-> page), so running off
     * an end means "leave the ring" and the shell decides which node is next.
     * Within the ring the walk is a plain sequence, so Tab and Shift+Tab are
     * the same list read in opposite directions. */
    for (step = 0; step < count; ++step) {
        i += forward ? 1 : -1;
        if (i < 0 || i >= count)
            break;

        if (!focusStopVisible(chrome, kFocusOrder[i]))
            continue;

        if (chrome->focus_part != kFocusOrder[i]) {
            focusMarkDirty(chrome, chrome->focus_part);
            chrome->focus_part = kFocusOrder[i];
            focusMarkDirty(chrome, chrome->focus_part);
        }

        return chrome->focus_part;
    }

    uiChromeFocusClear(chrome);
    return UI_PART_NONE;
}

int uiChromeFocusStep(UiChrome* chrome, int forward)
{
    return uiChromeFocusStepFrom(chrome, UI_PART_NONE, forward);
}

int uiChromeActivateFocus(UiChrome* chrome)
{
    if (!chrome)
        return 0;

    switch (chrome->focus_part) {
    case UI_PART_TAB_SEARCH:
        if (chrome->cb.tab_search)
            chrome->cb.tab_search(chrome->cb.user);
        return 1;
    case UI_PART_TAB_NEW:
        if (chrome->cb.new_tab)
            chrome->cb.new_tab(chrome->cb.user);
        return 1;
    case UI_PART_BACK:      emitCommand(chrome, UI_CMD_BACK); return 1;
    case UI_PART_FORWARD:   emitCommand(chrome, UI_CMD_FORWARD); return 1;
    case UI_PART_RELOAD:
        emitCommand(chrome, chrome->model.loading ? UI_CMD_STOP : UI_CMD_RELOAD);
        return 1;
    case UI_PART_MENU:      emitCommand(chrome, UI_CMD_MENU); return 1;
    case UI_PART_FIND_PREV: emitCommand(chrome, UI_CMD_FIND_PREV); return 1;
    case UI_PART_FIND_NEXT: emitCommand(chrome, UI_CMD_FIND_NEXT); return 1;
    case UI_PART_FIND_CLOSE: emitCommand(chrome, UI_CMD_FIND_CLOSE); return 1;
    default:                return 0;
    }
}

int uiChromeTakeDirty(UiChrome* chrome, UiRect* dirty)
{    int pending;

    if (!chrome)
        return 0;

    pending = chrome->dirty_pending;

    if (dirty)
        *dirty = pending ? chrome->dirty : emptyRect();

    chrome->dirty = emptyRect();
    chrome->dirty_pending = 0;

    return pending;
}

UiPart uiChromeHitTest(UiChrome* chrome, int x, int y)
{
    UiPart part;
    int index;

    if (!chrome)
        return UI_PART_NONE;

    if (buttonAt(&chrome->win_min, x, y)) return UI_PART_WINDOW_MIN;
    if (buttonAt(&chrome->win_max, x, y)) return UI_PART_WINDOW_MAX;
    if (buttonAt(&chrome->win_close, x, y)) return UI_PART_WINDOW_CLOSE;

    part = uiTabStripHit(&chrome->tabstrip, x, y, &index);

    if (part != UI_PART_NONE)
        return part;

    if (buttonAt(&chrome->toolbar.back, x, y)) return UI_PART_BACK;
    if (buttonAt(&chrome->toolbar.forward, x, y)) return UI_PART_FORWARD;
    if (buttonAt(&chrome->toolbar.reload, x, y)) return UI_PART_RELOAD;
    if (buttonAt(&chrome->toolbar.go, x, y)) return UI_PART_GO;
    if (buttonAt(&chrome->toolbar.menu, x, y)) return UI_PART_MENU;

    if (chrome->model.find_visible) {
        if (buttonAt(&chrome->findbar.prev, x, y)) return UI_PART_FIND_PREV;
        if (buttonAt(&chrome->findbar.next, x, y)) return UI_PART_FIND_NEXT;
        if (buttonAt(&chrome->findbar.close, x, y)) return UI_PART_FIND_CLOSE;
        if (uiRectContains(chrome->findbar.field, x, y)) return UI_PART_FIND_FIELD;
    }

    if (uiRectContains(chrome->toolbar.address, x, y))
        return UI_PART_ADDRESS;

    if (uiRectContains(chrome->statusbar.rect, x, y))
        return UI_PART_STATUS;

    return UI_PART_NONE;
}

int uiChromeTabAt(UiChrome* chrome, int x, int y)
{
    int index = -1;

    if (!chrome)
        return -1;

    uiTabStripHit(&chrome->tabstrip, x, y, &index);

    return index;
}

/* Which fill a text field's pill is currently painted with: 0 = normal,
 * 1 = hovered, 2 = focused. The native EDIT asks this too, because it paints
 * its own background and must land on exactly the pill's colour - hovering the
 * Go button (part of the pill) used to highlight the pill while the EDIT kept
 * `field`, so a differently-coloured rectangle appeared behind the text. */
int uiChromeFieldState(const UiChrome* chrome, int field)
{
    int hovered;

    if (!chrome)
        return 0;

    if (field == 1) {
        if (chrome->focus_field == 1)
            return 2;

        hovered = chrome->hover_part == UI_PART_ADDRESS ||
                  chrome->toolbar.go.hover;
    } else if (field == 2) {
        if (chrome->focus_field == 2)
            return 2;

        hovered = chrome->hover_part == UI_PART_FIND_FIELD;
    } else {
        return 0;
    }

    return hovered ? 1 : 0;
}

static int feedButtons(UiChrome* chrome, const UiMouseEvent* event)
{
    int fire;
    int handled = 0;
    UiToolbar* bar = &chrome->toolbar;

    if (uiButtonMouse(&bar->back, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_BACK);
        handled = 1;
    }
    if (uiButtonMouse(&bar->forward, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FORWARD);
        handled = 1;
    }
    if (uiButtonMouse(&bar->reload, event, &fire)) {
        if (fire) emitCommand(chrome, chrome->model.loading ? UI_CMD_STOP : UI_CMD_RELOAD);
        handled = 1;
    }
    if (uiButtonMouse(&bar->go, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_GO);
        handled = 1;
    }
    if (uiButtonMouse(&bar->menu, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MENU);
        handled = 1;
    }

    if (uiButtonMouse(&chrome->win_min, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MINIMIZE);
        handled = 1;
    }
    if (uiButtonMouse(&chrome->win_max, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_MAXIMIZE);
        handled = 1;
    }
    if (uiButtonMouse(&chrome->win_close, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_CLOSE_WINDOW);
        handled = 1;
    }

    return handled;
}

static int feedFindButtons(UiChrome* chrome, const UiMouseEvent* event)
{
    int fire;
    int handled = 0;
    UiFindBar* bar = &chrome->findbar;

    if (!chrome->model.find_visible)
        return 0;

    if (uiButtonMouse(&bar->prev, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_PREV);
        handled = 1;
    }
    if (uiButtonMouse(&bar->next, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_NEXT);
        handled = 1;
    }
    if (uiButtonMouse(&bar->close, event, &fire)) {
        if (fire) emitCommand(chrome, UI_CMD_FIND_CLOSE);
        handled = 1;
    }

    return handled;
}

static void updateToolbarModel(UiChrome* chrome)
{
    chrome->toolbar.back.enabled = chrome->model.can_back;
    chrome->toolbar.forward.enabled = chrome->model.can_forward;
    chrome->toolbar.menu.enabled = 1;
    chrome->toolbar.go.enabled = 1;
    chrome->toolbar.reload.enabled = 1;

    chrome->win_min.enabled = 1;
    chrome->win_max.enabled = 1;
    chrome->win_max.icon = chrome->model.maximized ? UI_ICON_RESTORE : UI_ICON_MAXIMIZE;
    chrome->win_close.enabled = 1;
}

int uiChromeMouse(UiChrome* chrome, const UiMouseEvent* event)
{
    UiPart part;
    UiPart before_part;
    int before_index;
    int index;

    if (!chrome || !event)
        return 0;

    before_part = chrome->hover_part;
    before_index = chrome->tabstrip.hover_tab;

    part = uiChromeHitTest(chrome, event->x, event->y);
    index = uiChromeTabAt(chrome, event->x, event->y);

    switch (event->type) {
    case UI_MOUSE_MOVE:
        chrome->hover_part = part;
        chrome->mouse_inside = part != UI_PART_NONE;
        uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, NULL);
        feedButtons(chrome, event);
        feedFindButtons(chrome, event);

        /* Only a change of part needs repainting: moving inside one part leaves
         * the dirty rect empty and costs no paint at all. */
        if (before_part != part || before_index != index) {
            chromeMarkDirty(chrome, partRect(chrome, before_part, before_index));
            chromeMarkDirty(chrome, partRect(chrome, part, index));
        }

        return chrome->mouse_inside;

    case UI_MOUSE_LEAVE:
        chrome->hover_part = UI_PART_NONE;
        chrome->mouse_inside = 0;
        uiTabStripLeave(&chrome->tabstrip);
        uiButtonReset(&chrome->toolbar.back);
        uiButtonReset(&chrome->toolbar.forward);
        uiButtonReset(&chrome->toolbar.reload);
        uiButtonReset(&chrome->toolbar.go);
        uiButtonReset(&chrome->toolbar.menu);
        uiButtonReset(&chrome->findbar.prev);
        uiButtonReset(&chrome->findbar.next);
        uiButtonReset(&chrome->findbar.close);
        /* The window buttons never fire a LEAVE of their own; without this a
         * red close button stayed red after the pointer went into the page. */
        uiButtonReset(&chrome->win_min);
        uiButtonReset(&chrome->win_max);
        uiButtonReset(&chrome->win_close);
        chromeMarkDirty(chrome, uiRectMake(0, 0, chrome->width, chrome->height));
        return 0;

    case UI_MOUSE_DOWN:
        chrome->hover_part = part;
        /* Press changes can touch a part that is no longer under the pointer
         * (press here, drag there), so clicks repaint the whole chrome. They
         * are rare; mouse moves are not. A press that hits nothing (toolbar
         * background) changes nothing and stays clean. */
        if (part != UI_PART_NONE)
            chromeMarkDirty(chrome, uiRectMake(0, 0, chrome->width, chrome->height));

        if (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE ||
            part == UI_PART_TAB_NEW || part == UI_PART_TAB_SEARCH) {
            uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, NULL);

            /* The press switches tabs, not the release: the click on a tab body
             * is the whole gesture. The close chip keeps waiting for the release
             * so a slight drag cannot kill a page. */
            if (part == UI_PART_TAB && index >= 0 && chrome->cb.activate_tab)
                chrome->cb.activate_tab(chrome->cb.user, index);

            return 1;
        }

        if (feedButtons(chrome, event))
            return 1;

        if (feedFindButtons(chrome, event))
            return 1;

        if (part == UI_PART_ADDRESS) {
            emitFocus(chrome, 1);
            return 1;
        }

        if (part == UI_PART_FIND_FIELD) {
            emitFocus(chrome, 2);
            return 1;
        }

        if (part == UI_PART_STATUS)
            return 1;

        return 0;

    case UI_MOUSE_UP: {
        UiTabAction action;
        int handled = 0;

        chromeMarkDirty(chrome, uiRectMake(0, 0, chrome->width, chrome->height));

        memset(&action, 0, sizeof(action));
        action.activate = -1;
        action.close = -1;

        if (chrome->tabstrip.pressed_tab >= 0 ||
            chrome->tabstrip.pressed_close >= 0 ||
            chrome->tabstrip.pressed_new_tab ||
            chrome->tabstrip.pressed_search) {
            uiTabStripMouse(&chrome->tabstrip, &chrome->model, event, &action);

            /* action.activate is deliberately ignored: the press switched the
             * tab already (see UI_MOUSE_DOWN), and firing it here too would
             * switch twice. */
            if (action.close >= 0 && chrome->cb.close_tab)
                chrome->cb.close_tab(chrome->cb.user, action.close);
            else if (action.new_tab && chrome->cb.new_tab)
                chrome->cb.new_tab(chrome->cb.user);
            else if (action.open_search && chrome->cb.tab_search)
                chrome->cb.tab_search(chrome->cb.user);

            handled = 1;
        }

        if (feedButtons(chrome, event))
            handled = 1;
        if (feedFindButtons(chrome, event))
            handled = 1;

        if (part == UI_PART_ADDRESS || part == UI_PART_FIND_FIELD || part == UI_PART_STATUS)
            handled = 1;

        return handled;
    }

    default:
        return 0;
    }
}

/* The model's enabled state is derived from can_back/forward; keep the button
 * structs in sync whenever the shell refreshes the snapshot. */
void uiChromeSync(UiChrome* chrome)
{
    updateToolbarModel(chrome);
}
