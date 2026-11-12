/*
 * ui_widgets.c - UiButton and UiTabStrip.
 */

#include "ui_widgets.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int tabVisible(const UiChromeModel* model, int index)
{
    return index >= 0 && index < UI_MAX_TABS && model->tabs[index].used;
}

static wchar_t firstCharacter(const char* utf8)
{
    wchar_t wide[8];
    int n;
    int i;

    if (!utf8 || !utf8[0])
        return L'?';

    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 8);

    if (n <= 0)
        return L'?';

    for (i = 0; i < n - 1; ++i) {
        if (wide[i] != L' ' && wide[i] != L'\t')
            return wide[i];
    }

    return L'?';
}

/* Stable, pleasant color per site: same URL -> same chip color. */
static unsigned faviconColor(const char* url)
{
    static const unsigned palette[] = {
        0xFF1A73E8, 0xFFD93025, 0xFF188038, 0xFFE37400,
        0xFF9334E6, 0xFF00897B, 0xFF3949AB, 0xFFC5221F,
        0xFF6D4C41, 0xFF00838F
    };
    unsigned hash = 2166136261u;
    const char* p;

    for (p = url ? url : ""; *p; ++p) {
        hash ^= (unsigned char)*p;
        hash *= 16777619u;
    }

    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}

/* Where the active tab meets the toolbar, the silhouette flares outwards: the
 * side turns into a quarter arc tangent to the toolbar's top edge, so the base
 * is wider than the body (Chrome). The arc is the corner square minus a quarter
 * circle centred on the square's outer corner.
 *
 * Painted BEFORE the tabs, never inside paintTab: the square reaches into the
 * neighbouring tab's rect, and stamping the circle back after that neighbour's
 * hover pill had been drawn bit a notch out of the pill. Under the tabs, the
 * pill simply covers the flare where it overlaps. */
static void paintTabBaseFlare(UiPainter* painter, const UiTheme* theme, UiRect rect,
                              unsigned tab_color)
{
    int flare = uiThemeScale(theme, 8);
    int base = rect.y + rect.h;

    if (rect.w <= flare * 2 || rect.h <= flare)
        return;

    /* Left: square [x-flare, x] x [base-flare, base] minus the quarter circle
     * centred on its outer (bottom-left) corner. */
    uiFillRect(painter, uiRectMake(rect.x - flare, base - flare, flare, flare),
               tab_color);
    uiFillPie(painter, rect.x - flare, base - flare, flare, 0.0f, 90.0f,
              theme->chrome);

    /* Right: same, mirrored (quarter circle pointing left-down). */
    uiFillRect(painter, uiRectMake(rect.x + rect.w, base - flare, flare, flare),
               tab_color);
    uiFillPie(painter, rect.x + rect.w + flare, base - flare, flare, 90.0f, 90.0f,
              theme->chrome);
}

static void fillTabShape(UiPainter* painter, const UiTheme* theme, UiRect rect,
                         unsigned color)
{
    uiFillTabShape(painter, rect, theme->tab_radius, 0, color);
}

/* Chrome's *inactive* tab hover/press is a rounded pill that floats inside the
 * tab slot and leaves a hairline of strip at the bottom; only the active tab
 * merges into the toolbar (fillTabShape squares that one off). Reusing
 * fillTabShape here made the hover read as a box welded to the toolbar. */
static void fillTabPill(UiPainter* painter, const UiTheme* theme, UiRect rect,
                        int radius, unsigned color)
{
    UiRect pill;
    int inset;

    if (rect.w <= 0 || rect.h <= 0)
        return;

    inset = uiThemeScale(theme, 1);

    /* A 128-tab strip can squeeze a tab to a few pixels: GDI+ needs the radius
     * smaller than half the pill, otherwise the arc degenerates. */
    if (radius > rect.w / 2)
        radius = rect.w / 2;

    if (rect.h - inset * 3 < 2) {
        uiFillRoundRect(painter, rect, radius, color);
        return;
    }

    pill = uiRectMake(rect.x, rect.y + inset, rect.w, rect.h - inset * 3);
    uiFillRoundRect(painter, pill, radius, color);
}

static void paintFavicon(UiPainter* painter, const UiTheme* theme,
                         UiRect rect, const char* url, const char* title)
{
    unsigned color = faviconColor(url);
    wchar_t letter[2];
    UiRect text;

    uiFillRoundRect(painter, rect, uiThemeScale(theme, 4), color);

    letter[0] = firstCharacter(title && title[0] ? title : url);
    letter[1] = 0;

    text = uiRectMake(rect.x, rect.y, rect.w, rect.h);
    uiTextW(painter, letter, text, theme->font_small, 0xFFFFFFFF, UI_ALIGN_CENTER);
}

/* Public for the tab-search popup, which lists tabs that are not in the strip. */
void uiFaviconChip(UiPainter* painter, const UiTheme* theme,
                   UiRect rect, const char* url, const char* title)
{
    paintFavicon(painter, theme, rect, url, title);
}

static void paintTabClose(UiPainter* painter, const UiTheme* theme,
                          UiRect hit, int hovered, int pressed)
{
    /* 20 px chip with the full 16 px glyph box inside it. The old code used a
     * 16 px chip *and* inset the icon by another 4 px on each side, so the
     * glyph box was 8 px and the X itself 4x4 device pixels - the "× 太小"
     * report. */
    int chip = uiThemeScale(theme, 10);
    int glyph = uiThemeScale(theme, 16);
    int cx = hit.x + hit.w / 2;
    int cy = hit.y + hit.h / 2;
    unsigned color = (hovered || pressed) ? theme->text : theme->text_muted;
    UiRect icon;

    if (glyph > hit.w)
        glyph = hit.w;

    icon = uiRectMake(cx - glyph / 2, cy - glyph / 2, glyph, glyph);

    if (hovered || pressed) {
        /* Chrome's tab close is a neutral chip; only the title-bar X turns
         * red. */
        uiFillCircle(painter, cx, cy, chip,
                     pressed ? theme->tab_close_pressed : theme->tab_close_hover);
    }

    uiIcon(painter, UI_ICON_CLOSE, icon, color,
           ((hovered || pressed) ? 1.5f : 1.4f) * theme->scale);
}

/* ------------------------------------------------------------------------- */
/* UiButton                                                                  */
/* ------------------------------------------------------------------------- */

void uiButtonInit(UiButton* button, int id, UiIcon icon)
{
    if (!button)
        return;

    memset(button, 0, sizeof(*button));
    button->id = id;
    button->icon = icon;
    button->enabled = 1;
    button->visible = 1;
}

void uiButtonReset(UiButton* button)
{
    if (!button)
        return;

    button->hover = 0;
    button->pressed = 0;
}

void uiButtonPaint(UiButton* button, const UiTheme* theme, UiPainter* painter)
{
    UiRect icon;
    unsigned background = 0;
    unsigned color;
    int hovered;

    if (!button || !button->visible || button->rect.w <= 0 || button->rect.h <= 0)
        return;

    hovered = button->hover && button->enabled;

    if (button->window_control) {
        /* Title-bar buttons run the full tab-strip height, like Chrome's. */
        if (button->danger && (button->pressed || hovered))
            background = theme->window_close_hover;
        else if (button->pressed && button->enabled)
            background = theme->button_pressed;
        else if (hovered)
            background = theme->button_hover;

        if (background)
            uiFillRect(painter, button->rect, background);

        if (button->danger && background == theme->window_close_hover)
            color = 0xFFFFFFFFu;
        else if (!button->enabled)
            color = theme->text_faint;
        else
            color = theme->icon;

        {
            int size = uiThemeScale(theme, 16);
            int cx = button->rect.x + button->rect.w / 2;
            int cy = button->rect.y + button->rect.h / 2;

            icon = uiRectMake(cx - size / 2, cy - size / 2, size, size);
        }

        uiIcon(painter, button->icon, icon, color, (1.4f * theme->scale));
        return;
    }

    /* Chrome's toolbar icon buttons get a circular tint, not a rounded
     * square; the radius is the full button so the shape reads as a circle
     * even at 125%/150%. */
    if (button->pressed && button->enabled)
        background = theme->button_pressed;
    else if (button->toggled)
        background = theme->button_pressed;
    else if (hovered)
        background = theme->button_hover;

    if (background) {
        int cx = button->rect.x + button->rect.w / 2;
        int cy = button->rect.y + button->rect.h / 2;
        int radius = (button->rect.w < button->rect.h ? button->rect.w
                                                      : button->rect.h) / 2;

        uiFillCircle(painter, cx, cy, radius, background);
    }

    if (!button->enabled)
        color = theme->text_faint;
    else if (button->toggled)
        color = theme->text;
    else
        color = theme->icon;

    /* A fixed 8 px inset was only right for the 32 px toolbar buttons; the
     * 24 px find-bar buttons were left with an 8 px glyph box (chevrons ~3 px
     * wide). Size the box off the button instead, capped at Chrome's 16 px. */
    {
        int side = button->rect.w < button->rect.h ? button->rect.w : button->rect.h;
        int box = side - uiThemeScale(theme, 8);
        int cx = button->rect.x + button->rect.w / 2;
        int cy = button->rect.y + button->rect.h / 2;

        if (box > uiThemeScale(theme, 16))
            box = uiThemeScale(theme, 16);
        if (box < uiThemeScale(theme, 10))
            box = uiThemeScale(theme, 10);

        icon = uiRectMake(cx - box / 2, cy - box / 2, box, box);
    }

    uiIcon(painter, button->icon, icon, color, (1.7f * theme->scale));
}

int uiButtonHit(const UiButton* button, int x, int y)
{
    return button && button->visible && button->enabled &&
           uiRectContains(button->rect, x, y);
}

int uiButtonMouse(UiButton* button, const UiMouseEvent* event, int* fire)
{
    int inside;

    if (fire)
        *fire = 0;

    if (!button || !button->visible || !event)
        return 0;

    inside = button->enabled && uiRectContains(button->rect, event->x, event->y);

    switch (event->type) {
    case UI_MOUSE_MOVE:
        button->hover = inside;
        return inside;

    case UI_MOUSE_DOWN:
        if (!inside)
            return 0;
        button->pressed = 1;
        button->hover = 1;
        return 1;

    case UI_MOUSE_UP: {
        int wasPressed = button->pressed;

        button->pressed = 0;
        button->hover = inside;

        if (wasPressed && inside) {
            if (fire)
                *fire = 1;
            return 1;
        }

        return inside;
    }

    case UI_MOUSE_LEAVE:
        button->hover = 0;
        button->pressed = 0;
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* UiTabStrip                                                               */
/* ------------------------------------------------------------------------- */

void uiTabStripLayout(UiTabStrip* strip, const UiTheme* theme,
                      const UiChromeModel* model, UiRect area)
{
    int i;
    int slot[UI_MAX_TABS];   /* slot width per visible tab (rect + tab_gap) */
    int visible;
    int total;
    int active_slot;
    int active_exists;
    int compressed;
    int x;
    int tab_y;
    int available;
    int gap;
    int plus;
    int pad;
    /* Hover/press are model indices, not slot positions, so a relayout must not
     * throw them away: the pointer does not move when the window is resized, but
     * the hover used to be wiped here and only came back on the next mouse move. */
    int hover_tab;
    int hover_close;
    int pressed_tab;
    int pressed_close;
    int hover_new;
    int pressed_new;
    int hover_search;
    int pressed_search;

    if (!strip || !theme || !model)
        return;

    hover_tab = strip->hover_tab;
    hover_close = strip->hover_close;
    pressed_tab = strip->pressed_tab;
    pressed_close = strip->pressed_close;
    hover_new = strip->hover_new_tab;
    pressed_new = strip->pressed_new_tab;
    hover_search = strip->hover_search;
    pressed_search = strip->pressed_search;

    strip->rect = area;
    strip->count = 0;
    strip->total = 0;
    strip->hover_tab = -1;
    strip->hover_close = -1;
    strip->pressed_tab = -1;
    strip->pressed_close = -1;
    strip->hover_new_tab = 0;
    strip->pressed_new_tab = 0;
    strip->hover_search = 0;
    strip->pressed_search = 0;

    memset(strip->tab, 0, sizeof(strip->tab));
    memset(strip->close, 0, sizeof(strip->close));
    memset(strip->slot_of, 0, sizeof(strip->slot_of));

    for (i = 0; i < UI_MAX_TABS; ++i) {
        if (tabVisible(model, i))
            strip->slot_of[strip->count++] = i;
    }

    total = strip->count;

    pad = theme->strip_pad;
    gap = theme->tab_gap;
    plus = theme->button_w;
    /* The tab-search entry (left) and the + (right) both live in the strip. */
    available = area.w - pad * 2 - plus * 2 - gap * 2 - strip->right_reserved;

    if (available < 0)
        available = 0;

    active_slot = -1;
    for (i = 0; i < total; ++i) {
        if (strip->slot_of[i] == model->active)
            active_slot = i;
    }

    active_exists = active_slot >= 0;
    /* Compressed = the tabs no longer fit at their natural width. From here on
     * the visible rects + gaps fill `available` exactly, which is what pins the
     * "+" to the right edge (see the header comment). */
    compressed = total > 0 && total * theme->tab_max_w > available;

    visible = total;

    if (compressed) {
        /* Every item costs its minimum width plus one gap; an active tab costs
         * 20 more than a background one (tab_active_min_w - tab_min_w). Taking
         * that off the width up front turns the capacity question into a single
         * division. Beyond this count the tabs cannot meet their minimums, so
         * the tail is hidden instead of squeezed (the tab-search popup is how
         * they are reached). */
        int item = theme->tab_min_w + gap;
        int usable = available -
                     (active_exists ? theme->tab_active_min_w - theme->tab_min_w : 0);
        int fit = item > 0 ? usable / item : 0;

        if (fit < 1)
            fit = 1;    /* degenerate window: one tab beats none */
        if (fit < visible)
            visible = fit;

        strip->count = visible;

        if (visible > 0) {
            int rects = available - visible * gap;  /* width left for the rects */
            int active_rect = 0;
            int bg_n = visible;
            int bg_base;
            int bg_rem;
            int k = 0;

            if (rects < visible)
                rects = visible;    /* degenerate: 1 px each */

            bg_base = rects / visible;

            /* The active tab keeps a wider minimum, but only when the equal
             * share would take it below it AND the other tabs still get at
             * least 1 px each. Without the second half a minimized window
             * (WM_SIZE 0x0 -> available < 0 -> rects == 1) would hand the
             * active tab everything and divide by a zero background count. */
            if (active_exists && active_slot < visible && visible > 1 &&
                bg_base < theme->tab_active_min_w &&
                rects - theme->tab_active_min_w >= visible - 1) {
                active_rect = theme->tab_active_min_w;
                --bg_n;
                bg_base = (rects - active_rect) / bg_n;
            }

            bg_rem = rects - active_rect - bg_base * bg_n;

            for (i = 0; i < visible; ++i) {
                int w;

                if (active_rect && i == active_slot) {
                    slot[i] = active_rect + gap;
                    continue;
                }

                /* Spread the leftover pixels instead of dumping them on one
                 * tab: the sum still equals `rects`, so the "+" cannot drift. */
                w = bg_base + ((k + 1) * bg_rem) / bg_n - (k * bg_rem) / bg_n;
                ++k;

                if (w < 1)
                    w = 1;

                slot[i] = w + gap;
            }
        }
    } else {
        for (i = 0; i < visible; ++i)
            slot[i] = theme->tab_max_w + gap;
    }

    x = area.x + pad + plus + gap;
    tab_y = area.y + area.h - theme->tab_h;

    strip->search = uiRectMake(area.x + pad, tab_y, plus, theme->button_h);

    for (i = 0; i < visible; ++i) {
        int index = strip->slot_of[i];
        int w = slot[i] - gap;
        int close_w = uiThemeScale(theme, 24);

        /* A slot is never narrower than its gap, so the rect stays positive. */
        if (w < 1)
            w = 1;

        strip->tab[index] = uiRectMake(x, tab_y, w, theme->tab_h);

        /* Every tab carries its close chip, hovered or not (Chrome's default).
         * A background tab loses it once the strip is squeezed below half the
         * natural width - there the title needs the room more. The active tab
         * keeps it whatever happens: closing the tab you are looking at must
         * never need a menu. The rect is the single source of truth - a zero
         * rect means "not painted, not clickable"; the only case that still
         * yields a zero rect for the active tab is a tab narrower than the chip
         * itself, where nothing fits. */
        if ((index == model->active || w * 2 >= theme->tab_max_w) &&
            w > close_w + uiThemeScale(theme, 2)) {
            strip->close[index] = uiRectMake(x + w - close_w - uiThemeScale(theme, 2),
                                             tab_y + (theme->tab_h - close_w) / 2,
                                             close_w, close_w);
        } else {
            strip->close[index] = uiRectMake(0, 0, 0, 0);
        }

        x += slot[i];
    }

    strip->total = total;
    strip->count = visible;

    /* Compressed: `x` is exactly the fixed right edge, so the clamp below is a
     * no-op. Natural width: the "+" follows the last tab (Chrome) and the clamp
     * only guards a window too narrow for the reserved controls. */
    strip->new_tab = uiRectMake(x + gap,
                                area.y + area.h - theme->button_h,
                                theme->button_w, theme->button_h);

    if (strip->new_tab.x + strip->new_tab.w >
        area.x + area.w - pad - strip->right_reserved)
        strip->new_tab.x = area.x + area.w - pad - strip->right_reserved - strip->new_tab.w;

    if (strip->new_tab.x < area.x + pad)
        strip->new_tab.x = area.x + pad;

    /* Re-apply what the pointer had before the relayout (see the saves above).
     * A tab or chip that no longer exists is simply dropped. */
    if (hover_tab >= 0 && tabVisible(model, hover_tab))
        strip->hover_tab = hover_tab;
    if (hover_close >= 0 && tabVisible(model, hover_close) &&
        strip->close[hover_close].w > 0)
        strip->hover_close = hover_close;
    if (pressed_tab >= 0 && tabVisible(model, pressed_tab))
        strip->pressed_tab = pressed_tab;
    if (pressed_close >= 0 && tabVisible(model, pressed_close) &&
        strip->close[pressed_close].w > 0)
        strip->pressed_close = pressed_close;

    strip->hover_new_tab = hover_new;
    strip->pressed_new_tab = pressed_new;
    strip->hover_search = hover_search;
    strip->pressed_search = pressed_search;
}

UiPart uiTabStripHit(UiTabStrip* strip, int x, int y, int* model_index)
{
    int i;

    if (model_index)
        *model_index = -1;

    if (!strip || !uiRectContains(strip->rect, x, y))
        return UI_PART_NONE;

    /* The search entry and "+" before the tabs: both stay clickable where they
     * overlap an overflowing tab. */
    if (uiRectContains(strip->search, x, y))
        return UI_PART_TAB_SEARCH;

    if (uiRectContains(strip->new_tab, x, y))
        return UI_PART_TAB_NEW;

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];

        if (uiRectContains(strip->close[index], x, y)) {
            if (model_index)
                *model_index = index;
            return UI_PART_TAB_CLOSE;
        }

        if (uiRectContains(strip->tab[index], x, y)) {
            if (model_index)
                *model_index = index;
            return UI_PART_TAB;
        }
    }

    if (uiRectContains(strip->new_tab, x, y))
        return UI_PART_TAB_NEW;

    return UI_PART_NONE;
}

static void paintTab(UiPainter* painter, const UiTheme* theme,
                     const UiChromeModel* model, UiTabStrip* strip,
                     int index, int active, int hovered, int close_hovered,
                     int pressed, int close_pressed)
{
    UiRect rect = strip->tab[index];
    UiRect favicon;
    UiRect title;
    int show_close = strip->close[index].w > 0;
    int close_w = show_close ? strip->close[index].w : 0;
    int icon_full = uiThemeScale(theme, 16);
    int icon = icon_full;
    int title_min = uiThemeScale(theme, 12);
    int pad_l = uiThemeScale(theme, 10);
    int pad_mid = uiThemeScale(theme, 8);
    int pad_r = uiThemeScale(theme, 8);
    int title_w;
    int spare;

    if (rect.w <= 0)
        return;

    /* Layout already decided whether this tab has a chip; the rect is the single
     * source of truth for "painted" and "clickable". */
    if (active)
        fillTabShape(painter, theme, rect, theme->surface);
    else if (pressed)
        fillTabPill(painter, theme, rect, theme->tab_radius, theme->tab_pressed);
    else if (hovered)
        fillTabPill(painter, theme, rect, theme->tab_radius, theme->tab_hover);

    /* Shrinking order: title, then padding. The icon never shrinks - it is
     * already its minimum size (16 px), and a background tab's 16 px minimum
     * width is exactly that icon. The active tab is the one exception: when the
     * close chip no longer fits beside the icon the chip wins and the icon is
     * dropped, so at 36 px the active tab shows the close button only. */
    title_w = rect.w - pad_l - icon - pad_mid - close_w - pad_r;

    if (title_w < title_min) {
        title_w = 0;
        spare = rect.w - icon - close_w;

        if (spare >= pad_l + pad_mid + pad_r) {
            /* Enough room for the full padding: centre the icon + close group. */
            pad_l = (rect.w - icon - close_w) / 2;
            pad_mid = 0;
            pad_r = 0;
        } else if (spare > 0) {
            /* Paddings shrink evenly until they are gone. */
            pad_l = spare / 3;
            pad_mid = spare / 3;
            pad_r = spare - pad_l - pad_mid;
        } else {
            /* Paddings are gone: check whether the icon still fits next to the
             * chip (the active tab drops it, a background tab keeps it). */
            pad_l = pad_mid = pad_r = 0;
            icon = uiTabFaviconSize(theme, rect.w, close_w, active);
        }
    }

    /* Never let the icon spill into the neighbour. */
    if (icon > rect.w - close_w) {
        icon = rect.w - close_w;
        if (icon < 0)
            icon = 0;
    }

    if (icon > 0) {
        favicon = uiRectMake(rect.x + pad_l,
                             rect.y + (rect.h - icon) / 2, icon, icon);

        if (icon < icon_full)
            favicon.x = rect.x + (rect.w - close_w - icon) / 2;

        paintFavicon(painter, theme, favicon,
                     model->tabs[index].url, model->tabs[index].title);

        if (model->tabs[index].hung && icon >= uiThemeScale(theme, 12))
            uiFillCircle(painter, favicon.x + favicon.w - 1, favicon.y + 1,
                         uiThemeScale(theme, 3), theme->danger);

        title.x = favicon.x + favicon.w + pad_mid;
    } else {
        title.x = rect.x + pad_l;
    }

    title.y = rect.y;
    title.h = rect.h;
    title.w = 0;

    if (title_w >= title_min) {
        title.w = show_close
                      ? strip->close[index].x - title.x - uiThemeScale(theme, 2)
                      : rect.x + rect.w - title.x - pad_r;
    }

    if (title.w >= title_min) {
        const char* label = model->tabs[index].title[0] ? model->tabs[index].title : "New tab";
        unsigned color = active ? theme->text
                                : (hovered ? theme->text : theme->text_muted);

        uiText(painter, label, title, theme->font_ui, color, UI_ALIGN_LEFT);
    }

    if (show_close)
        paintTabClose(painter, theme, strip->close[index], close_hovered,
                      close_pressed);
}

/* Shrinking order, expressed as "how big is the favicon for this tab":
 *   1. title has room with full padding + full icon      -> full icon
 *   2. title is gone, padding is being squeezed to zero  -> full icon
 *   3. padding is gone, the icon needs the room          -> full icon
 *      - the active tab is the one exception: when its close chip no longer
 *        fits next to the icon, the chip wins and the icon is dropped, so at
 *        its 36 px minimum the active tab shows only the close button
 *      - a background tab always keeps its full-size icon; 16 px is both the
 *        tab minimum and the icon size, so there is nothing to scale
 * Exposed so --ui-shot can assert the rule without painting. */
int uiTabFaviconSize(const UiTheme* theme, int tab_w, int close_w, int active)
{
    int icon = uiThemeScale(theme, 16);

    if (icon + close_w <= tab_w)
        return icon;            /* the icon fits at its natural size */

    if (active)
        return 0;               /* close button only */

    /* Degenerate only (a window narrower than one tab's minimum): clip. */
    icon = tab_w - close_w;

    return icon > 0 ? icon : 0;
}

int uiTabStripSeparatorWidth(const UiTheme* theme)
{
    /* 2 px at 96 DPI ("a bit thicker"); the layout gap is tab_gap, so this fills
     * the gap exactly instead of eating into a tab. */
    return uiThemeScale(theme, 2);
}

/* A tab that merges into the strip visually: the active one (its shape is
 * squared off into the toolbar) and the one under the pointer (its pill already
 * separates itself). Separators never touch those. */
static int tabIsFlush(const UiChromeModel* model, const UiTabStrip* strip, int index)
{
    return index == model->active ||
           index == strip->hover_tab ||
           index == strip->pressed_tab;
}

/* Chrome's strip keeps a hairline between neighbours. It lives inside the
 * layout gap, so it always has its own room and never eats a tab; it is skipped
 * where either neighbour is flush (see above) - which is exactly "no line on
 * both sides of the active/hovered tab". */
static void paintTabSeparators(UiPainter* painter, const UiTheme* theme,
                               const UiChromeModel* model, UiTabStrip* strip)
{
    int width = uiTabStripSeparatorWidth(theme);
    int height = uiThemeScale(theme, 16);
    int i;

    if (height > theme->tab_h - uiThemeScale(theme, 10))
        height = theme->tab_h - uiThemeScale(theme, 10);
    if (height < 2)
        height = 2;

    for (i = 1; i < strip->count; ++i) {
        int left = strip->slot_of[i - 1];
        int right = strip->slot_of[i];
        const UiRect* tab = &strip->tab[right];

        if (tab->w <= 0)
            continue;

        if (tabIsFlush(model, strip, left) || tabIsFlush(model, strip, right))
            continue;

        uiFillRect(painter,
                   uiRectMake(tab->x - (theme->tab_gap + width) / 2,
                              tab->y + (tab->h - height) / 2, width, height),
                   theme->tab_sep);
    }
}

void uiTabStripPaint(UiTabStrip* strip, const UiTheme* theme,
                     const UiChromeModel* model, UiPainter* painter)
{
    int i;
    int active = model->active;

    if (!strip || !theme || !model || !painter || !painter->g)
        return;

    uiFillRect(painter, strip->rect, theme->chrome);

    /* Tabs never paint under the window buttons: the layout makes the visible
     * ones fit exactly, so this only guards the degenerate case. */
    GdipSetClipRectI(painter->g, strip->rect.x, strip->rect.y,
                     strip->rect.w - strip->right_reserved, strip->rect.h,
                     CombineModeReplace);

    paintTabSeparators(painter, theme, model, strip);

    /* Under every tab: the active tab's base flare reaches into its neighbours'
     * rects, so it must be painted first or it eats a notch out of their pills.
     * A hidden active tab has a zero rect - nothing to flare. */
    if (tabVisible(model, active) && strip->tab[active].w > 0)
        paintTabBaseFlare(painter, theme, strip->tab[active], theme->surface);

    for (i = 0; i < strip->count; ++i) {
        int index = strip->slot_of[i];

        if (index == active)
            continue;

        paintTab(painter, theme, model, strip, index, 0,
                 strip->hover_tab == index, strip->hover_close == index,
                 strip->pressed_tab == index, strip->pressed_close == index);
    }

    if (tabVisible(model, active) && strip->tab[active].w > 0) {
        paintTab(painter, theme, model, strip, active, 1,
                 strip->hover_tab == active, strip->hover_close == active,
                 strip->pressed_tab == active, strip->pressed_close == active);
    }

    /* The two strip buttons: circular hover, glyphs on top. */
    {
        int radius = uiThemeScale(theme, 12);

        if (strip->hover_new_tab || strip->pressed_new_tab)
            uiFillCircle(painter, strip->new_tab.x + strip->new_tab.w / 2,
                         strip->new_tab.y + strip->new_tab.h / 2,
                         radius, strip->pressed_new_tab ? theme->button_pressed
                                                        : theme->button_hover);

        uiIcon(painter, UI_ICON_PLUS,
               uiRectInset(strip->new_tab, uiThemeScale(theme, 9),
                           uiThemeScale(theme, 9)),
               theme->icon, (1.7f * theme->scale));

        if (strip->hover_search || strip->pressed_search)
            uiFillCircle(painter, strip->search.x + strip->search.w / 2,
                         strip->search.y + strip->search.h / 2,
                         radius, strip->pressed_search ? theme->button_pressed
                                                       : theme->button_hover);

        uiIcon(painter, UI_ICON_CHEVRON_DOWN,
               uiRectInset(strip->search, uiThemeScale(theme, 9),
                           uiThemeScale(theme, 9)),
               theme->icon, (1.7f * theme->scale));
    }

    GdipResetClip(painter->g);
}

void uiTabStripMouse(UiTabStrip* strip, const UiChromeModel* model,
                     const UiMouseEvent* event, UiTabAction* action)
{
    UiPart part;
    int index = -1;

    if (action) {
        action->activate = -1;
        action->close = -1;
        action->new_tab = 0;
    }

    if (!strip || !model || !event)
        return;

    switch (event->type) {
    case UI_MOUSE_LEAVE:
        strip->hover_tab = -1;
        strip->hover_close = -1;
        strip->hover_new_tab = 0;
        strip->pressed_tab = -1;
        strip->pressed_close = -1;
        strip->pressed_new_tab = 0;
        return;

    case UI_MOUSE_MOVE:
        part = uiTabStripHit(strip, event->x, event->y, &index);
        strip->hover_tab = (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->hover_close = (part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->hover_new_tab = (part == UI_PART_TAB_NEW);
        strip->hover_search = (part == UI_PART_TAB_SEARCH);
        return;

    case UI_MOUSE_DOWN:
        part = uiTabStripHit(strip, event->x, event->y, &index);
        strip->pressed_tab = (part == UI_PART_TAB || part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->pressed_close = (part == UI_PART_TAB_CLOSE) ? index : -1;
        strip->pressed_new_tab = (part == UI_PART_TAB_NEW);
        strip->pressed_search = (part == UI_PART_TAB_SEARCH);
        return;

    case UI_MOUSE_UP: {
        int pressed_tab = strip->pressed_tab;
        int pressed_close = strip->pressed_close;
        int pressed_new = strip->pressed_new_tab;
        int pressed_search = strip->pressed_search;

        strip->pressed_tab = -1;
        strip->pressed_close = -1;
        strip->pressed_new_tab = 0;
        strip->pressed_search = 0;

        part = uiTabStripHit(strip, event->x, event->y, &index);

        if (pressed_close >= 0 && part == UI_PART_TAB_CLOSE && index == pressed_close) {
            if (action)
                action->close = index;
            return;
        }

        if (pressed_tab >= 0 && part == UI_PART_TAB && index == pressed_tab) {
            if (action)
                action->activate = index;
            return;
        }

        if (pressed_new && part == UI_PART_TAB_NEW) {
            if (action)
                action->new_tab = 1;
            return;
        }

        if (pressed_search && part == UI_PART_TAB_SEARCH) {
            if (action)
                action->open_search = 1;
            return;
        }

        return;
    }

    default:
        return;
    }
}

void uiTabStripLeave(UiTabStrip* strip)
{
    if (!strip)
        return;

    strip->hover_tab = -1;
    strip->hover_close = -1;
    strip->hover_new_tab = 0;
    strip->hover_search = 0;
    strip->pressed_tab = -1;
    strip->pressed_close = -1;
    strip->pressed_new_tab = 0;
    strip->pressed_search = 0;
}
