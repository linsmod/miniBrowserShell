/*
 * ui_widgets.h - reusable chrome widgets: UiButton and UiTabStrip.
 *
 * A widget owns its rect and interaction state and is painted from the model
 * snapshot; it never reaches into ShellWindow. The chrome composition lives in
 * ui_chrome.h.
 */

#ifndef AGAVE_UI_WIDGETS_H
#define AGAVE_UI_WIDGETS_H

#include "ui_theme.h"

/* Chrome keeps all tabs in the strip (it scrolls); we keep all of them too and
 * reach the ones that no longer fit through the tab-search popup. */
#define UI_MAX_TABS 128

typedef struct {
    int  used;
    char title[256];
    char url[512];
    int  hung;
} UiTabModel;

/* Snapshot filled by the shell's syncChrome() once per frame/event. */
typedef struct {
    UiTabModel tabs[UI_MAX_TABS];
    int        active;          /* model index, -1 when empty */
    int        can_back;
    int        can_forward;
    int        loading;
    int        progress;        /* 0..100 */
    char       address[512];
    char       status[1024];
    int        zoom;            /* percent */
    int        find_visible;
    char       find_text[256];
    int        maximized;       /* paint restore instead of maximize */
} UiChromeModel;

typedef enum {
    UI_PART_NONE = 0,
    UI_PART_TAB,
    UI_PART_TAB_CLOSE,
    UI_PART_TAB_NEW,
    UI_PART_BACK,
    UI_PART_FORWARD,
    UI_PART_RELOAD,
    UI_PART_GO,
    UI_PART_MENU,
    UI_PART_ADDRESS,
    UI_PART_FIND_FIELD,
    UI_PART_FIND_PREV,
    UI_PART_FIND_NEXT,
    UI_PART_FIND_CLOSE,
    UI_PART_STATUS,
    UI_PART_WINDOW_MIN,
    UI_PART_WINDOW_MAX,
    UI_PART_WINDOW_CLOSE,
    UI_PART_TAB_SEARCH
} UiPart;

typedef enum {
    UI_MOUSE_MOVE = 0,
    UI_MOUSE_DOWN = 1,
    UI_MOUSE_UP = 2,
    UI_MOUSE_LEAVE = 3
} UiMouseType;

typedef struct {
    int type;
    int x;
    int y;
} UiMouseEvent;

/* ------------------------------------------------------------------------- */
/* UiButton                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    int    id;            /* command ID passed back on click */
    UiIcon icon;
    UiRect rect;
    int    enabled;
    int    visible;
    int    toggled;
    int    hover;
    int    pressed;
    int    window_control;  /* full-height title-bar button, no rounding */
    int    danger;          /* close: red hover */
} UiButton;

void uiButtonInit(UiButton* button, int id, UiIcon icon);
void uiButtonPaint(UiButton* button, const UiTheme* theme, UiPainter* painter);
int  uiButtonHit(const UiButton* button, int x, int y);
/* 1 = the event belonged to the button; *fire = 1 on a completed click. */
int  uiButtonMouse(UiButton* button, const UiMouseEvent* event, int* fire);
void uiButtonReset(UiButton* button);

/* ------------------------------------------------------------------------- */
/* UiTabStrip                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    UiRect rect;
    int    right_reserved;         /* window controls on the title bar */
    UiRect tab[UI_MAX_TABS];       /* slot rect per model index */
    UiRect close[UI_MAX_TABS];     /* close hit rect */
    int    slot_of[UI_MAX_TABS];   /* visual slot -> model index */
    int    count;                  /* tabs that fit on the strip (painted) */
    int    total;                  /* used tabs, including the hidden tail */
    UiRect new_tab;
    UiRect search;                 /* tab-search entry at the left edge */

    int    hover_tab;              /* model index, -1 */
    int    hover_close;
    int    hover_new_tab;
    int    hover_search;
    int    pressed_tab;
    int    pressed_close;
    int    pressed_new_tab;
    int    pressed_search;
} UiTabStrip;

typedef struct {
    int activate;   /* model index or -1 */
    int close;      /* model index or -1 */
    int new_tab;    /* 1 = the + button completed */
    int open_search;/* 1 = the tab-search entry completed */
} UiTabAction;

/* Width (device px) of the hairline painted between two resting tabs. */
int     uiTabStripSeparatorWidth(const UiTheme* theme);

/* Favicon size for a tab of width `tab_w` with a close chip of `close_w`.
 * The icon never shrinks (it is already 16 px, its minimum), so this only
 * answers "does it fit": 0 = the active tab dropped it for the close button.
 * A background tab always keeps a full-size icon - its 16 px minimum is
 * exactly the icon. */
int     uiTabFaviconSize(const UiTheme* theme, int tab_w, int close_w, int active);

/* The letter chip used for favicons (url determines the colour, title the
 * letter) - the tab-search popup lists tabs that are not in the strip. */
void    uiFaviconChip(UiPainter* painter, const UiTheme* theme, UiRect rect,
                      const char* url, const char* title);

/* Lays the strip out. While compressed (the tabs no longer fit at tab_max_w)
 * the visible tab rects plus their gaps fill the strip's usable width exactly,
 * so the "+" keeps the same right edge whatever the tab count is - clicking
 * that spot never lands on a different control. Widths that would fall below
 * the per-item minimums are not squeezed further: the tabs that do not fit are
 * left out (strip->count < strip->total) and are reached through the tab-search
 * popup. */
void    uiTabStripLayout(UiTabStrip* strip, const UiTheme* theme,
                         const UiChromeModel* model, UiRect area);
void    uiTabStripPaint(UiTabStrip* strip, const UiTheme* theme,
                        const UiChromeModel* model, UiPainter* painter);
UiPart  uiTabStripHit(UiTabStrip* strip, int x, int y, int* model_index);
void    uiTabStripMouse(UiTabStrip* strip, const UiChromeModel* model,
                        const UiMouseEvent* event, UiTabAction* action);
void    uiTabStripLeave(UiTabStrip* strip);

#endif /* AGAVE_UI_WIDGETS_H */
