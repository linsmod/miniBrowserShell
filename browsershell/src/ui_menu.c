/*
 * ui_menu.c - Chrome-style menu model, layout, painting and input.
 */

#include "ui_menu.h"

#include <string.h>

static int scale(const UiTheme* theme, int value)
{
    return uiThemeScale(theme, value);
}

static int isSeparator(const UiMenuItem* item)
{
    return item && (item->flags & UI_MENU_ITEM_SEPARATOR);
}

static int isEnabled(const UiMenuItem* item)
{
    return item && !(item->flags & UI_MENU_ITEM_DISABLED);
}

static int isSelectable(const UiMenuItem* item)
{
    return item && !isSeparator(item) && isEnabled(item);
}

/* ------------------------------------------------------------------------- */
/* model                                                                     */
/* ------------------------------------------------------------------------- */

void uiMenuInit(UiMenu* menu)
{
    if (!menu)
        return;

    memset(menu, 0, sizeof(*menu));
}

int uiMenuAdd(UiMenu* menu, const char* label, const char* shortcut, int id,
              unsigned flags, UiMenuIcon icon, const UiMenu* submenu)
{
    UiMenuItem* item;

    if (!menu || menu->count >= UI_MENU_MAX_ITEMS)
        return -1;

    item = &menu->items[menu->count];

    memset(item, 0, sizeof(*item));
    item->label = label;
    item->shortcut = shortcut;
    item->id = id;
    item->flags = flags;
    item->icon = icon;
    item->submenu = submenu;

    return menu->count++;
}

int uiMenuAddSeparator(UiMenu* menu)
{
    /* Collapse: a separator only separates, so one at the very top or right
     * after another one is a layout bug the caller should not have to think
     * about (the context menu is assembled from optional blocks). */
    if (!menu || menu->count == 0)
        return -1;

    if (menu->items[menu->count - 1].flags & UI_MENU_ITEM_SEPARATOR)
        return -1;

    return uiMenuAdd(menu, NULL, NULL, 0, UI_MENU_ITEM_SEPARATOR,
                     UI_MENU_ICON_NONE, NULL);
}

void uiMenuTrim(UiMenu* menu)
{
    if (!menu)
        return;

    /* Drop a trailing separator left over by a block that ended the menu. */
    while (menu->count > 0 &&
           (menu->items[menu->count - 1].flags & UI_MENU_ITEM_SEPARATOR))
        --menu->count;
}

/* ------------------------------------------------------------------------- */
/* measurement                                                               */
/* ------------------------------------------------------------------------- */

static int panelHasIcon(const UiMenu* menu)
{
    int i;

    for (i = 0; i < menu->count; ++i) {
        if (menu->items[i].icon != UI_MENU_ICON_NONE && !isSeparator(&menu->items[i]))
            return 1;
    }

    return 0;
}

static int panelHasSubmenu(const UiMenu* menu)
{
    int i;

    for (i = 0; i < menu->count; ++i) {
        if (menu->items[i].submenu && !isSeparator(&menu->items[i]))
            return 1;
    }

    return 0;
}

int uiMenuMeasure(const UiMenu* menu, const UiTheme* theme,
                  int* width, int* height)
{
    int i;
    int any_icon;
    int any_submenu;
    int label_col = 0;
    int right_col = 0;
    int rows = 0;
    int panel_w;

    if (width)
        *width = 0;
    if (height)
        *height = 0;

    if (!menu || !theme)
        return 0;

    any_icon = panelHasIcon(menu);
    any_submenu = panelHasSubmenu(menu);

    for (i = 0; i < menu->count; ++i) {
        const UiMenuItem* item = &menu->items[i];
        int label_w = 0;
        int shortcut_w = 0;

        if (isSeparator(item)) {
            rows += theme->menu_sep_h;
            continue;
        }

        if (item->label)
            uiMeasureText(theme, theme->font_ui, item->label, &label_w, NULL);

        if (label_w > label_col)
            label_col = label_w;

        if (item->shortcut)
            uiMeasureText(theme, theme->font_ui, item->shortcut, &shortcut_w, NULL);

        /* Theme-style rows: a value on the right AND a submenu chevron. Both
         * columns have to be reserved, or the text runs under the arrow. */
        if (item->submenu && shortcut_w > 0)
            shortcut_w += scale(theme, 16) + scale(theme, 8);

        if (shortcut_w > right_col)
            right_col = shortcut_w;

        rows += theme->menu_item_h;
    }

    if (any_submenu) {
        int arrow = scale(theme, 16);

        if (arrow > right_col)
            right_col = arrow;
    }

    panel_w = theme->menu_pad_x * 2 + (any_icon ? theme->menu_icon_col : 0) + label_col;

    if (right_col > 0)
        panel_w += theme->menu_gap + right_col;

    if (panel_w < theme->menu_min_w)
        panel_w = theme->menu_min_w;
    if (panel_w > theme->menu_max_w)
        panel_w = theme->menu_max_w;

    if (width)
        *width = panel_w;
    if (height)
        *height = theme->menu_pad_y * 2 + rows;

    return 1;
}

/* ------------------------------------------------------------------------- */
/* layout                                                                    */
/* ------------------------------------------------------------------------- */

static int clampInt(int value, int lo, int hi)
{
    if (hi < lo)
        return lo;
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

static int addPanel(UiMenuSession* session, const UiTheme* theme,
                    const UiMenu* menu, int parent, int parent_item,
                    int x, int y, const UiRect* work);

static int panelDepth(const UiMenuSession* session, int panel)
{
    int depth = 0;

    while (panel >= 0 && depth <= UI_MENU_MAX_LEVELS) {
        panel = session->panels[panel].parent;
        ++depth;
    }

    return depth;
}

static void layoutChildren(UiMenuSession* session, const UiTheme* theme,
                           int index, const UiRect* work)
{
    UiMenuPanel* panel = &session->panels[index];
    int i;

    for (i = 0; i < panel->menu->count; ++i) {
        const UiMenuItem* item = &panel->menu->items[i];
        int child_w = 0;
        int child_h = 0;
        int cx;
        int cy;

        if (!item->submenu || isSeparator(item))
            continue;

        /* Capacity is panels, depth is levels: a menu with three submenus is
         * three siblings (depth 2), not three levels deep. Running the old
         * single counter out of levels stopped after the second sibling, so the
         * third submenu never got a panel and hovering it opened nothing. */
        if (session->count >= UI_MENU_MAX_PANELS)
            return;

        if (panelDepth(session, index) + 1 >= UI_MENU_MAX_LEVELS)
            continue;

        if (!uiMenuMeasure(item->submenu, theme, &child_w, &child_h))
            continue;

        /* Overlap the parent by a couple of pixels, like Chrome: the child
         * shadow then reads as one popup instead of two islands. */
        cx = panel->rect.x + panel->rect.w - scale(theme, 2);

        if (cx + child_w > work->x + work->w)
            cx = panel->rect.x - child_w + scale(theme, 2);
        if (cx < work->x)
            cx = work->x;
        if (cx + child_w > work->x + work->w)
            cx = work->x + work->w - child_w;

        cy = panel->item_top[i] - theme->menu_pad_y;
        cy = clampInt(cy, work->y, work->y + work->h - child_h);

        addPanel(session, theme, item->submenu, index, i, cx, cy, work);
    }
}

static int addPanel(UiMenuSession* session, const UiTheme* theme,
                    const UiMenu* menu, int parent, int parent_item,
                    int x, int y, const UiRect* work)
{
    UiMenuPanel* panel;
    int index = session->count;
    int width = 0;
    int height = 0;
    int cy;
    int i;

    (void)work;

    if (index >= UI_MENU_MAX_PANELS)
        return -1;

    if (!uiMenuMeasure(menu, theme, &width, &height))
        return -1;

    panel = &session->panels[index];
    memset(panel, 0, sizeof(*panel));

    panel->rect = uiRectMake(x, y, width, height);
    panel->menu = menu;
    panel->parent = parent;
    panel->parent_item = parent_item;
    panel->has_icon = panelHasIcon(menu);

    cy = y + theme->menu_pad_y;

    for (i = 0; i < menu->count; ++i) {
        int row_h = isSeparator(&menu->items[i]) ? theme->menu_sep_h
                                                 : theme->menu_item_h;

        panel->item_top[i] = cy;
        cy += row_h;
        panel->item_bottom[i] = cy;
    }

    ++session->count;
    layoutChildren(session, theme, index, work);

    return index;
}

void uiMenuSessionBegin(UiMenuSession* session, const UiMenu* root,
                        const UiTheme* theme, int anchor_x, int anchor_y,
                        UiMenuAlign align, const UiRect* work_area)
{
    UiRect work;
    int width = 0;
    int height = 0;
    int x;
    int y;

    if (!session)
        return;

    memset(session, 0, sizeof(*session));
    session->hover_panel = -1;
    session->hover_item = -1;
    session->key_panel = -1;
    session->key_item = -1;

    if (!root || !theme || !uiMenuMeasure(root, theme, &width, &height))
        return;

    if (work_area) {
        work = *work_area;
    } else {
        work = uiRectMake(0, 0, GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN));
    }

    x = align == UI_MENU_ALIGN_RIGHT ? anchor_x - width : anchor_x;
    y = anchor_y;

    x = clampInt(x, work.x, work.x + work.w - width);
    if (y + height > work.y + work.h)
        y = work.y + work.h - height;
    y = clampInt(y, work.y, work.y + work.h - height);

    addPanel(session, theme, root, -1, -1, x, y, &work);
    session->visible[0] = session->count > 0 ? 1 : 0;
}

int uiMenuSessionHit(const UiMenuSession* session, int x, int y,
                     int* panel, int* item)
{
    int p;

    if (panel)
        *panel = -1;
    if (item)
        *item = -1;

    if (!session)
        return 0;

    /* Deepest panel first: submenus paint above their parents. */
    for (p = session->count - 1; p >= 0; --p) {
        const UiMenuPanel* mp = &session->panels[p];
        int i;

        if (!session->visible[p])
            continue;

        if (!uiRectContains(mp->rect, x, y))
            continue;

        if (panel)
            *panel = p;

        for (i = 0; i < mp->menu->count; ++i) {
            if (y >= mp->item_top[i] && y < mp->item_bottom[i]) {
                if (item)
                    *item = i;
                break;
            }
        }

        return 1;
    }

    return 0;
}

static int panelIsAncestor(const UiMenuSession* session, int ancestor, int panel)
{
    int p = panel;

    while (p >= 0) {
        if (p == ancestor)
            return 1;
        p = session->panels[p].parent;
    }

    return 0;
}

static int childPanelFor(const UiMenuSession* session, int panel, int item)
{
    int p;

    for (p = 0; p < session->count; ++p) {
        if (session->panels[p].parent == panel &&
            session->panels[p].parent_item == item)
            return p;
    }

    return -1;
}

/* Keep the ancestors of `panel` plus the submenu of the hovered item open;
 * close everything else. */
static void syncOpenPanels(UiMenuSession* session, int panel, int item)
{
    int p;

    for (p = 1; p < session->count; ++p) {
        int keep = 0;

        if (!session->visible[p])
            continue;

        if (panel >= 0) {
            keep = panelIsAncestor(session, p, panel);

            if (!keep && session->panels[p].parent == panel &&
                session->panels[p].parent_item == item)
                keep = 1;
        }

        if (!keep)
            session->visible[p] = 0;
    }

    if (panel >= 0 && item >= 0) {
        const UiMenuItem* mi = &session->panels[panel].menu->items[item];
        int child;

        if (mi->submenu) {
            child = childPanelFor(session, panel, item);

            if (child >= 0)
                session->visible[child] = 1;
        }
    }
}

void uiMenuSessionMove(UiMenuSession* session, const UiTheme* theme, int x, int y)
{
    int panel = -1;
    int item = -1;

    (void)theme;

    if (!session)
        return;

    session->key_panel = -1;
    session->key_item = -1;

    if (!uiMenuSessionHit(session, x, y, &panel, &item)) {
        session->hover_panel = -1;
        session->hover_item = -1;
        return;
    }

    session->hover_panel = panel;
    session->hover_item = item;

    if (item >= 0 && !isSelectable(&session->panels[panel].menu->items[item]))
        session->hover_item = -1;

    syncOpenPanels(session, panel, session->hover_item);
}

/* ------------------------------------------------------------------------- */
/* keyboard                                                                  */
/* ------------------------------------------------------------------------- */

static int firstSelectable(const UiMenu* menu, int dir)
{
    int i;

    if (dir >= 0) {
        for (i = 0; i < menu->count; ++i) {
            if (isSelectable(&menu->items[i]))
                return i;
        }
    } else {
        for (i = menu->count - 1; i >= 0; --i) {
            if (isSelectable(&menu->items[i]))
                return i;
        }
    }

    return -1;
}

static int nextSelectable(const UiMenu* menu, int from, int dir)
{
    int i;

    if (menu->count == 0)
        return -1;

    if (from < 0)
        return firstSelectable(menu, dir);

    i = from;

    for (;;) {
        i += dir;

        if (i < 0)
            i = menu->count - 1;
        else if (i >= menu->count)
            i = 0;

        if (i == from)
            return from;

        if (isSelectable(&menu->items[i]))
            return i;
    }
}

int uiMenuSessionKey(UiMenuSession* session, const UiTheme* theme, int vk,
                     int* chosen)
{
    UiMenuPanel* panel;
    const UiMenuItem* item;
    int child;

    (void)theme;

    if (chosen)
        *chosen = 0;

    if (!session || session->count <= 0)
        return -1;

    if (vk == VK_ESCAPE)
        return 1;

    if (session->key_panel < 0 || session->key_panel >= session->count) {
        session->key_panel = 0;
        session->key_item = -1;
    }

    panel = &session->panels[session->key_panel];

    switch (vk) {
    case VK_DOWN:
    case VK_UP:
        session->key_item = nextSelectable(panel->menu, session->key_item,
                                           vk == VK_DOWN ? 1 : -1);
        if (session->key_item < 0)
            return 0;
        session->hover_panel = session->key_panel;
        session->hover_item = session->key_item;
        syncOpenPanels(session, session->key_panel, session->key_item);
        return 0;

    case VK_HOME:
        session->key_item = firstSelectable(panel->menu, 1);
        session->hover_panel = session->key_panel;
        session->hover_item = session->key_item;
        syncOpenPanels(session, session->key_panel, session->key_item);
        return 0;

    case VK_END:
        session->key_item = firstSelectable(panel->menu, -1);
        session->hover_panel = session->key_panel;
        session->hover_item = session->key_item;
        syncOpenPanels(session, session->key_panel, session->key_item);
        return 0;

    case VK_RIGHT:
        if (session->key_item < 0)
            return 0;
        item = &panel->menu->items[session->key_item];
        child = childPanelFor(session, session->key_panel, session->key_item);

        if (item->submenu && child >= 0) {
            session->visible[child] = 1;
            session->key_panel = child;
            session->key_item = firstSelectable(session->panels[child].menu, 1);
            session->hover_panel = child;
            session->hover_item = session->key_item;
        }
        return 0;

    case VK_LEFT: {
        int parent;

        if (session->key_panel <= 0)
            return 0;

        parent = session->panels[session->key_panel].parent;
        session->visible[session->key_panel] = 0;
        session->key_item = session->panels[session->key_panel].parent_item;
        session->key_panel = parent;

        if (session->key_panel >= 0) {
            session->hover_panel = session->key_panel;
            session->hover_item = session->key_item;
        }
        return 0;
    }

    case VK_RETURN:
    case VK_SPACE:
        if (session->key_item < 0 || session->key_item >= panel->menu->count)
            return 0;
        item = &panel->menu->items[session->key_item];

        if (item->submenu) {
            child = childPanelFor(session, session->key_panel, session->key_item);

            if (child >= 0) {
                session->visible[child] = 1;
                session->key_panel = child;
                session->key_item = firstSelectable(session->panels[child].menu, 1);
                session->hover_panel = child;
                session->hover_item = session->key_item;
            }
            return 0;
        }

        if (isSelectable(item) && item->id != 0) {
            if (chosen)
                *chosen = item->id;
            return 1;
        }
        return 0;

    default:
        return -1;
    }
}

int uiMenuSessionClick(UiMenuSession* session, const UiTheme* theme,
                       int x, int y, int* chosen)
{
    UiMenuPanel* panel;
    const UiMenuItem* item;
    int p = -1;
    int i = -1;
    int child;

    (void)theme;

    if (chosen)
        *chosen = 0;

    if (!session)
        return 1;

    if (!uiMenuSessionHit(session, x, y, &p, &i)) {
        /* Click outside every panel dismisses the menu. */
        return 1;
    }

    if (i < 0)
        return 0;

    panel = &session->panels[p];
    item = &panel->menu->items[i];

    if (isSeparator(item) || !isEnabled(item))
        return 0;

    if (item->submenu) {
        child = childPanelFor(session, p, i);

        if (child >= 0) {
            session->visible[child] = 1;
            session->hover_panel = child;
            session->hover_item = firstSelectable(session->panels[child].menu, 1);
            session->key_panel = -1;
            session->key_item = -1;
        }
        return 0;
    }

    if (chosen)
        *chosen = item->id;

    return 1;
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paintMenuIcon(UiPainter* painter, const UiTheme* theme,
                          UiMenuIcon icon, UiRect box, unsigned color)
{
    float cx = (float)box.x + (float)box.w / 2.0f;
    float cy = (float)box.y + (float)box.h / 2.0f;
    float u = (float)(box.w < box.h ? box.w : box.h) / 2.0f;
    float stroke = 1.5f * theme->scale;

    if (stroke < 1.0f)
        stroke = 1.0f;

    switch (icon) {
    case UI_MENU_ICON_LENS: {
        float radius = u * 0.52f;

        uiStrokeCircle(painter, (int)cx, (int)(cy - u * 0.06f), (int)radius,
                       color, stroke);
        uiFillCircle(painter, (int)cx, (int)(cy - u * 0.06f), (int)(u * 0.16f),
                     color);
        uiLine(painter, (int)(cx + radius * 0.72f), (int)(cy + radius * 0.66f),
               (int)(cx + u * 0.86f), (int)(cy + u * 0.80f), color, stroke);
        break;
    }
    case UI_MENU_ICON_CHECK: {
        uiLine(painter, (int)(cx - u * 0.52f), (int)cy,
               (int)(cx - u * 0.12f), (int)(cy + u * 0.38f), color, stroke);
        uiLine(painter, (int)(cx - u * 0.12f), (int)(cy + u * 0.38f),
               (int)(cx + u * 0.54f), (int)(cy - u * 0.36f), color, stroke);
        break;
    }
    case UI_MENU_ICON_CHEVRON_RIGHT: {
        uiLine(painter, (int)(cx - u * 0.24f), (int)(cy - u * 0.42f),
               (int)(cx + u * 0.20f), (int)cy, color, stroke);
        uiLine(painter, (int)(cx + u * 0.20f), (int)cy,
               (int)(cx - u * 0.24f), (int)(cy + u * 0.42f), color, stroke);
        break;
    }
    case UI_MENU_ICON_INCOGNITO: {
        float lens = u * 0.30f;

        uiLine(painter, (int)(cx - u * 0.62f), (int)(cy + u * 0.16f),
               (int)(cx + u * 0.62f), (int)(cy + u * 0.16f), color, stroke);
        uiStrokeCircle(painter, (int)(cx - u * 0.36f), (int)(cy + u * 0.42f),
                       (int)lens, color, stroke);
        uiStrokeCircle(painter, (int)(cx + u * 0.36f), (int)(cy + u * 0.42f),
                       (int)lens, color, stroke);
        uiLine(painter, (int)(cx - u * 0.24f), (int)(cy - u * 0.24f),
               (int)(cx + u * 0.24f), (int)(cy - u * 0.24f), color, stroke);
        break;
    }
    case UI_MENU_ICON_LINK: {
        uiStrokeRoundRect(painter,
                          uiRectMake((int)(cx - u * 0.72f), (int)(cy - u * 0.26f),
                                     (int)(u * 0.92f), (int)(u * 0.52f)),
                          (int)(u * 0.26f), color, stroke);
        uiStrokeRoundRect(painter,
                          uiRectMake((int)(cx - u * 0.20f), (int)(cy - u * 0.26f),
                                     (int)(u * 0.92f), (int)(u * 0.52f)),
                          (int)(u * 0.26f), color, stroke);
        break;
    }
    case UI_MENU_ICON_GLOBE:
        uiIcon(painter, UI_ICON_GLOBE, box, color, stroke);
        break;
    case UI_MENU_ICON_DOWNLOAD:
        uiIcon(painter, UI_ICON_DOWNLOAD, box, color, stroke);
        break;
    default:
        break;
    }
}

static void paintPanel(UiPainter* painter, const UiTheme* theme,
                       const UiMenuSession* session, int index, int dx, int dy)
{
    const UiMenuPanel* panel = &session->panels[index];
    const UiMenu* menu = panel->menu;
    int any_submenu = panelHasSubmenu(menu);
    UiRect rect = uiRectMake(panel->rect.x + dx, panel->rect.y + dy,
                             panel->rect.w, panel->rect.h);
    int icon_size = scale(theme, 16);
    int i;

    for (i = 0; i < menu->count; ++i) {
        const UiMenuItem* item = &menu->items[i];
        int top = panel->item_top[i] + dy;
        int bottom = panel->item_bottom[i] + dy;
        int height = bottom - top;
        int highlighted;

        if (height <= 0)
            continue;

        if (isSeparator(item)) {
            uiLine(painter, rect.x + theme->menu_pad_x, top + height / 2,
                   rect.x + rect.w - theme->menu_pad_x, top + height / 2,
                   theme->menu_sep, 1.0f);
            continue;
        }

        highlighted = (session->hover_panel == index && session->hover_item == i) ||
                      (session->key_panel == index && session->key_item == i);

        if (highlighted)
            uiFillRoundRect(painter,
                            uiRectMake(rect.x + scale(theme, 4), top + scale(theme, 1),
                                       rect.w - scale(theme, 8), height - scale(theme, 2)),
                            theme->menu_item_radius, theme->menu_hover);

        if (item->icon != UI_MENU_ICON_NONE && panel->has_icon) {
            UiRect icon_box = uiRectMake(
                rect.x + theme->menu_pad_x + (theme->menu_icon_col - icon_size) / 2,
                top + (height - icon_size) / 2, icon_size, icon_size);
            unsigned icon_color = isEnabled(item) ? theme->menu_icon
                                                  : theme->menu_text_disabled;

            paintMenuIcon(painter, theme, item->icon, icon_box, icon_color);
        }

        {
            int label_x = rect.x + theme->menu_pad_x +
                          (panel->has_icon ? theme->menu_icon_col : 0);
            int tail = any_submenu ? scale(theme, 18) : 0;
            int shortcut_w = 0;
            /* A submenu row keeps its chevron in the right-most column, so its
             * shortcut (the current value, e.g. Theme -> "System") sits to the
             * LEFT of the arrow instead of on top of it. */
            int arrow_col = (any_submenu && item->submenu) ? scale(theme, 18) : 0;
            int gap;

            if (item->shortcut) {
                uiMeasureText(theme, theme->font_ui, item->shortcut,
                              &shortcut_w, NULL);

                if (item->submenu)
                    tail += shortcut_w + scale(theme, 8);
                else if (shortcut_w > tail)
                    tail = shortcut_w;
            }

            gap = tail > 0 ? scale(theme, 8) : 0;

            {
                int label_w = rect.x + rect.w - theme->menu_pad_x - tail - gap -
                              label_x;

                if (label_w < 0)
                    label_w = 0;

                uiText(painter, item->label ? item->label : "",
                       uiRectMake(label_x, top, label_w, height), theme->font_ui,
                       isEnabled(item) ? theme->text : theme->menu_text_disabled,
                       UI_ALIGN_LEFT);
            }

            if (item->shortcut && shortcut_w > 0) {
                uiText(painter, item->shortcut,
                       uiRectMake(rect.x + rect.w - theme->menu_pad_x - arrow_col -
                                      shortcut_w,
                                  top, shortcut_w, height),
                       theme->font_ui,
                       isEnabled(item) ? theme->menu_shortcut
                                       : theme->menu_text_disabled,
                       UI_ALIGN_LEFT);
            }

            if (any_submenu) {
                UiRect arrow = uiRectMake(rect.x + rect.w - theme->menu_pad_x -
                                              scale(theme, 16),
                                          top + (height - scale(theme, 16)) / 2,
                                          scale(theme, 16), scale(theme, 16));

                if (item->submenu)
                    paintMenuIcon(painter, theme, UI_MENU_ICON_CHEVRON_RIGHT,
                                  arrow, theme->menu_shortcut);
            }
        }
    }
}

void uiMenuSessionPaint(UiMenuSession* session, const UiTheme* theme,
                        UiPainter* painter, int dx, int dy)
{
    int p;

    if (!session || !theme || !painter || !painter->g)
        return;

    /* No drop shadow: it was ten nested rounded-rect fills per repaint (the
     * whole popup is re-rendered on every hover change), and that dominated the
     * cost of a mouse move. The panels keep their 1 px border instead; the
     * popup is still translucent outside them so clicks dismiss it. */
    for (p = 0; p < session->count; ++p) {
        const UiMenuPanel* panel = &session->panels[p];
        UiRect rect;

        if (!session->visible[p])
            continue;

        rect = uiRectMake(panel->rect.x + dx, panel->rect.y + dy,
                          panel->rect.w, panel->rect.h);

        uiFillRoundRect(painter, rect, theme->menu_radius, theme->menu_bg);
        uiStrokeRoundRect(painter, rect, theme->menu_radius, theme->menu_border,
                          1.0f);

        paintPanel(painter, theme, session, p, dx, dy);
    }
}

/* ------------------------------------------------------------------------- */
/* introspection                                                             */
/* ------------------------------------------------------------------------- */

int uiMenuSessionVisibleCount(const UiMenuSession* session)
{
    int p;
    int n = 0;

    if (!session)
        return 0;

    for (p = 0; p < session->count; ++p) {
        if (session->visible[p])
            ++n;
    }

    return n;
}

const UiMenuPanel* uiMenuSessionPanel(const UiMenuSession* session, int index)
{
    if (!session || index < 0 || index >= session->count)
        return NULL;

    return &session->panels[index];
}

UiRect uiMenuSessionBounds(const UiMenuSession* session)
{
    UiRect bounds = uiRectMake(0, 0, 0, 0);
    int have = 0;
    int p;

    if (!session)
        return bounds;

    for (p = 0; p < session->count; ++p) {
        const UiMenuPanel* panel = &session->panels[p];

        if (!session->visible[p])
            continue;

        if (!have) {
            bounds = panel->rect;
            have = 1;
        } else {
            int right = bounds.x + bounds.w;
            int bottom = bounds.y + bounds.h;
            int pright = panel->rect.x + panel->rect.w;
            int pbottom = panel->rect.y + panel->rect.h;

            if (panel->rect.x < bounds.x)
                bounds.x = panel->rect.x;
            if (panel->rect.y < bounds.y)
                bounds.y = panel->rect.y;
            if (pright > right)
                right = pright;
            if (pbottom > bottom)
                bottom = pbottom;

            bounds.w = right - bounds.x;
            bounds.h = bottom - bounds.y;
        }
    }

    return bounds;
}

unsigned uiMenuSessionSignature(const UiMenuSession* session)
{
    unsigned hash = 2166136261u;
    int p;

    if (!session)
        return 0;

#define MIX(v) do { hash ^= (unsigned)(v); hash *= 16777619u; } while (0)

    MIX(session->count);
    MIX(session->hover_panel + 1);
    MIX(session->hover_item + 1);
    MIX(session->key_panel + 1);
    MIX(session->key_item + 1);

    for (p = 0; p < session->count; ++p)
        MIX(session->visible[p]);

#undef MIX

    return hash;
}
