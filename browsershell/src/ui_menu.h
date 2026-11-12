/*
 * ui_menu.h - model, layout, painting and input for the shell's self-drawn
 * menus.
 *
 * Chrome context menus are flat rounded panels with 30 px rows, a hover
 * highlight that ignores separators/disabled rows, right-aligned shortcut
 * text and side-opening submenus. This module owns all of that in plain C so
 * `--ui-shot` can lay out, hit-test and paint a menu into the offscreen DIB
 * without a window; `ui_menu_host.c` adds the layered Win32 popup and the
 * modal message loop.
 *
 * Strings are borrowed: a menu (and every submenu it points at) must stay
 * alive while a UiMenuSession is in use.
 */

#ifndef AGAVE_UI_MENU_H
#define AGAVE_UI_MENU_H

#include "ui_theme.h"

#define UI_MENU_MAX_ITEMS  24
/* Nesting depth of a session (root = 1, a submenu = 2, ...) - NOT the number of
 * panels: every submenu of a menu is a sibling panel, and counting them against
 * the depth silently dropped the third submenu (R196). */
#define UI_MENU_MAX_LEVELS 3
/* Panels a session can hold: the root plus one per submenu row. */
#define UI_MENU_MAX_PANELS 8

typedef enum {
    UI_MENU_ICON_NONE = 0,
    UI_MENU_ICON_LENS,
    UI_MENU_ICON_CHECK,
    UI_MENU_ICON_CHEVRON_RIGHT,
    UI_MENU_ICON_INCOGNITO,
    UI_MENU_ICON_GLOBE,
    UI_MENU_ICON_LINK,
    UI_MENU_ICON_DOWNLOAD
} UiMenuIcon;

#define UI_MENU_ITEM_DISABLED  0x1u
#define UI_MENU_ITEM_SEPARATOR 0x2u

typedef struct UiMenu UiMenu;

typedef struct {
    const char* label;      /* UTF-8; NULL for a separator */
    const char* shortcut;   /* UTF-8, right aligned; may be NULL */
    int         id;         /* command id returned when chosen; 0 = none */
    unsigned    flags;      /* UI_MENU_ITEM_* */
    UiMenuIcon  icon;
    const UiMenu* submenu;
} UiMenuItem;

struct UiMenu {
    UiMenuItem items[UI_MENU_MAX_ITEMS];
    int        count;
};

typedef enum {
    UI_MENU_ALIGN_LEFT = 0,   /* panel's left edge sits at the anchor */
    UI_MENU_ALIGN_RIGHT = 1   /* panel's right edge sits at the anchor */
} UiMenuAlign;

typedef struct {
    UiRect        rect;       /* screen-space panel */
    const UiMenu* menu;
    int           parent;     /* panel index, -1 for the root */
    int           parent_item;/* item in the parent that opened this panel */
    int           has_icon;   /* reserve the icon column for every row */
    int           item_top[UI_MENU_MAX_ITEMS];
    int           item_bottom[UI_MENU_MAX_ITEMS];
} UiMenuPanel;

typedef struct {
    UiMenuPanel panels[UI_MENU_MAX_PANELS];
    int         count;
    int         visible[UI_MENU_MAX_PANELS];
    int         hover_panel;  /* -1 when the pointer is outside every panel */
    int         hover_item;
    int         key_panel;    /* keyboard selection, -1 when the mouse owns */
    int         key_item;
} UiMenuSession;

void uiMenuInit(UiMenu* menu);
/* Append a row; returns its index or -1 when the menu is full. */
int  uiMenuAdd(UiMenu* menu, const char* label, const char* shortcut, int id,
               unsigned flags, UiMenuIcon icon, const UiMenu* submenu);
/* No-op when the menu is empty or already ends with a separator. */
int  uiMenuAddSeparator(UiMenu* menu);

/* Removes trailing separators (menus are assembled from optional blocks). */
void uiMenuTrim(UiMenu* menu);

/* Panel size at 96-DPI-scaled metrics. 1 on success. */
int  uiMenuMeasure(const UiMenu* menu, const UiTheme* theme,
                   int* width, int* height);

/* Lay out the root and every descendant submenu against a monitor work area
 * (screen coordinates). Only the root starts visible. */
void uiMenuSessionBegin(UiMenuSession* session, const UiMenu* root,
                        const UiTheme* theme, int anchor_x, int anchor_y,
                        UiMenuAlign align, const UiRect* work_area);

/* Mouse: hover + open/close submenus. */
void uiMenuSessionMove(UiMenuSession* session, const UiTheme* theme, int x, int y);

/* Keyboard. Returns 1 = dismiss (see *chosen), 0 = handled and still open,
 * -1 = not a menu key. */
int  uiMenuSessionKey(UiMenuSession* session, const UiTheme* theme, int vk,
                      int* chosen);

/* Mouse click. Returns 1 = dismiss with *chosen (0 = cancelled/outside),
 * 0 = still open (submenu opened or padding/disabled row clicked). */
int  uiMenuSessionClick(UiMenuSession* session, const UiTheme* theme,
                        int x, int y, int* chosen);

void uiMenuSessionPaint(UiMenuSession* session, const UiTheme* theme,
                        UiPainter* painter, int dx, int dy);

/* Introspection for the popup host and the offscreen regression. */
int  uiMenuSessionVisibleCount(const UiMenuSession* session);
int  uiMenuSessionHit(const UiMenuSession* session, int x, int y,
                      int* panel, int* item);
const UiMenuPanel* uiMenuSessionPanel(const UiMenuSession* session, int index);
UiRect uiMenuSessionBounds(const UiMenuSession* session);
/* Changes whenever something visible would move or highlight; the host only
 * re-renders when it differs. */
unsigned uiMenuSessionSignature(const UiMenuSession* session);

#endif /* AGAVE_UI_MENU_H */
