/*
 * ui_tabsearch.[ch] - Chrome's "Search tabs" popup (Ctrl+Shift+A).
 *
 * A layered top-level popup that lists the open tabs and the recently closed
 * ones in a filter field (typed query + caret), with a scrollbar when the list
 * is taller than the popup. It is deliberately *not* modal: the popup takes
 * the mouse (capture) so an outside click dismisses it, while the keyboard stays
 * with the shell, which forwards WM_CHAR / WM_KEYDOWN to uiTabSearchKey().
 *
 * The caller owns the data: it hands over a snapshot array (open tabs first,
 * then recently closed) whenever the model changes, and gets callbacks for
 * "activate tab" / "close tab" / "reopen closed tab".
 */

#ifndef AGAVE_UI_TABSEARCH_H
#define AGAVE_UI_TABSEARCH_H

#include <windows.h>

#include "ui_theme.h"
#include "ui_widgets.h"   /* UI_MAX_TABS */

/* open tabs + recently closed, both capped by UI_MAX_TABS */
#define UI_TABSEARCH_MAX_ITEMS (UI_MAX_TABS * 2)

enum {
    UI_TABSEARCH_OPEN = 0,     /* an open tab: activate / close it */
    UI_TABSEARCH_CLOSED = 1    /* a recently closed tab: reopen it */
};

typedef struct {
    char title[256];
    char url[512];
    int  kind;                /* UI_TABSEARCH_OPEN / UI_TABSEARCH_CLOSED */
    int  age_ms;              /* for the "2h ago" column */
} UiTabSearchItem;

typedef struct {
    void* user;
    /* `index` is the index into the snapshot array that was last handed over. */
    void (*activate)(void* user, int index);
    void (*close)(void* user, int index);
} UiTabSearchCallbacks;

/* Shows the popup anchored at screen (x, y), replacing any previous snapshot. */
int  uiTabSearchOpen(HWND owner, const UiTheme* theme,
                     const UiTabSearchItem* items, int count,
                     const UiTabSearchCallbacks* callbacks, int x, int y);

/* The model changed while the popup is open: swap the snapshot and re-filter.
 * Selection survives when the same item is still in the list. */
void uiTabSearchRefresh(const UiTabSearchItem* items, int count);

void uiTabSearchClose(void);
int  uiTabSearchIsOpen(void);

/* Keyboard from the shell's loop: WM_KEYDOWN and WM_CHAR. */
void uiTabSearchKey(UINT message, WPARAM wParam, LPARAM lParam);

#endif /* AGAVE_UI_TABSEARCH_H */
