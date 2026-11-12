/*
 * ui_chrome.h - browser chrome composition: tab strip + toolbar + find bar +
 * status bar. The shell fills a UiChromeModel snapshot and forwards
 * layout/paint/mouse; commands come back as UI_CMD_* (same numeric IDs as the
 * old native buttons, so logs and WM_COMMAND handling stay compatible).
 */

#ifndef AGAVE_UI_CHROME_H
#define AGAVE_UI_CHROME_H

#include "ui_widgets.h"

/* Command IDs. main.c aliases ID_* to these; the numbers are the historical
 * WM_COMMAND IDs and must not change (menu handlers/probes grep them). */
#define UI_CMD_BACK       1001
#define UI_CMD_FORWARD    1002
#define UI_CMD_RELOAD     1003
#define UI_CMD_STOP       1004
#define UI_CMD_ADDRESS    1005
#define UI_CMD_GO         1006
#define UI_CMD_NEWTAB     1007
#define UI_CMD_MENU       1009
#define UI_CMD_FIND_EDIT  1010
#define UI_CMD_FIND_NEXT  1011
#define UI_CMD_FIND_PREV  1012
#define UI_CMD_FIND_CLOSE 1013
#define UI_CMD_MINIMIZE   1014
#define UI_CMD_MAXIMIZE   1015
#define UI_CMD_CLOSE_WINDOW 1016

typedef struct {
    void* user;
    void (*command)(void* user, int command_id);
    void (*activate_tab)(void* user, int index);
    void (*close_tab)(void* user, int index);
    void (*new_tab)(void* user);
    void (*tab_search)(void* user);
    /* A click landed on the address/find field chrome; 1 = address, 2 = find. */
    void (*focus_field)(void* user, int field);
} UiChromeCallbacks;

typedef struct {
    UiRect rect;
    UiButton back;
    UiButton forward;
    UiButton reload;
    UiButton go;
    UiButton menu;
    UiRect address;        /* address pill */
    UiRect address_icon;
    UiRect separator;
} UiToolbar;

typedef struct {
    UiRect rect;        /* the card */
    UiRect field;       /* the query pill inside it */
    UiRect edit;        /* where the native EDIT goes */
    UiRect count;       /* "3/12" */
    UiRect sep;         /* hairline between the count and the buttons */
    UiButton prev;
    UiButton next;
    UiButton close;
} UiFindBar;

typedef struct {
    UiRect rect;
    UiRect text;
    UiRect zoom;
} UiStatusBar;

typedef struct UiChrome {
    UiTheme theme;
    UiChromeModel model;
    UiChromeCallbacks cb;

    int width;
    int height;
    int offscreen;          /* no native EDITs: paint field text ourselves */

    void* parent_hwnd;
    void* address_hwnd;
    void* find_hwnd;

    int focus_field;        /* 0 none, 1 address, 2 find */
    int focus_part;         /* UiPart with the keyboard focus ring, or none */
    int hover_part;         /* UiPart */
    int mouse_inside;

    /* Union of the chrome areas whose pixels the last uiChromeMouse() changed.
     * The shell turns this into one InvalidateRect() instead of repainting the
     * whole window on every mouse move (the chrome is HTCLIENT, so WM_MOUSEMOVE
     * used to change hover state with nothing invalidated at all). */
    UiRect dirty;
    int    dirty_pending;

    UiTabStrip tabstrip;
    UiToolbar toolbar;
    UiFindBar findbar;
    UiStatusBar statusbar;

    /* Title-bar controls, drawn at the top-right of the tab strip. The tab
     * layout reserves their width so a shrinking tab never runs under them. */
    UiButton win_min;
    UiButton win_max;
    UiButton win_close;

    UiRect content;
    UiRect address_edit;    /* where the native EDIT goes */
    UiRect find_edit;
} UiChrome;

void uiChromeInit(UiChrome* chrome, UiThemeKind kind, unsigned dpi,
                  int offscreen, const UiChromeCallbacks* callbacks);
void uiChromeFree(UiChrome* chrome);
void uiChromeSetDpi(UiChrome* chrome, unsigned dpi);
void uiChromeSetNative(UiChrome* chrome, void* parent, void* address, void* find);
void uiChromeSetFocusField(UiChrome* chrome, int field);

/* width/height are client pixels. Also fills address_edit / find_edit so the
 * shell can move its two native controls. */
void uiChromeLayout(UiChrome* chrome, int width, int height);
void uiChromePaint(UiChrome* chrome, HDC hdc);

/* The find bar floats over the page (Chrome's overlay), so it lives in its own
 * child window instead of a chrome row: this paints the card into a DC whose
 * origin is that window's client. The shell positions the window at
 * chrome->findbar.rect and the native EDIT at chrome->find_edit. */
void uiChromePaintFindOverlay(UiChrome* chrome, HDC hdc, int width, int height);

/* Same pattern for the status bubble: a bottom-left overlay that only exists
 * while the model has a message (or the page is zoomed). A zero statusbar rect
 * means "hidden". */
void uiChromePaintStatusOverlay(UiChrome* chrome, HDC hdc, int width, int height);

/* Refresh derived widget state (enabled back/forward) after the model changed. */
void uiChromeSync(UiChrome* chrome);

/* Which fill a text field's pill is painted with: 0 normal, 1 hovered,
 * 2 focused. `field` is 1 (address) or 2 (find). The shell asks for this when
 * it answers WM_CTLCOLOREDIT, so the native EDIT's background always matches the
 * pill it sits in. */
int uiChromeFieldState(const UiChrome* chrome, int field);

/* 1 = the event was consumed by the chrome. */
int uiChromeMouse(UiChrome* chrome, const UiMouseEvent* event);

/* Keyboard tab stops over the painted chrome. Every toolbar / find-bar button
 * is paint, not a native control, so the chrome owns the focus ring and the
 * shell only forwards Tab / Shift+Tab / Enter / Space / Escape.
 *
 * uiChromeFocusStep() walks the ring: forward=1 next, 0 previous. It returns
 * the new part, or UI_PART_NONE when the walk left the chrome (the shell then
 * hands the keyboard to the page or to the address field). */
int  uiChromeFocusStep(UiChrome* chrome, int forward);
/* Same walk, but starting from `anchor` (a UiPart such as UI_PART_ADDRESS that
 * is not itself a stop) instead of from the current ring position. */
int  uiChromeFocusStepFrom(UiChrome* chrome, int anchor, int forward);
int  uiChromeFocusPart(const UiChrome* chrome);
void uiChromeFocusClear(UiChrome* chrome);
/* Enter/Space on the focused part: runs the same command a click would. */
int  uiChromeActivateFocus(UiChrome* chrome);

/* Areas the last event needs repainted: 1 = *dirty is valid (the call clears
 * it, so a caller that ignores it never accumulates stale rectangles). */
int uiChromeTakeDirty(UiChrome* chrome, UiRect* dirty);

/* For tests: which part is at (x,y), and which tab (model index, -1 if none). */
UiPart uiChromeHitTest(UiChrome* chrome, int x, int y);
int    uiChromeTabAt(UiChrome* chrome, int x, int y);

#endif /* AGAVE_UI_CHROME_H */
