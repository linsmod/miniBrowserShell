/*
 * ui_menu_host.h - modal Win32 host for ui_menu.
 */

#ifndef AGAVE_UI_MENU_HOST_H
#define AGAVE_UI_MENU_HOST_H

#include "ui_menu.h"

/*
 * uiMenuHostRun returns 0 when the menu was dismissed. A right click closes it
 * too, but reports this sentinel instead: the click never reached the window
 * below (the popup owned the mouse capture), so the caller can replay it where
 * the pointer is and get the "second right click re-opens the menu" behaviour
 * Chrome has. Negative, so it can never collide with a command id.
 */
#define UI_MENU_REOPEN (-1)

/*
 * Shows `root` as a layered top-level popup anchored at screen (x, y) and
 * runs it modally (its own message loop, like TrackPopupMenu did). Returns
 * the chosen item id, 0 when the menu was dismissed, or UI_MENU_REOPEN when a
 * right click dismissed it.
 *
 * `align` picks which panel edge sits at the anchor: LEFT for context menus
 * at the pointer, RIGHT for the app menu under the toolbar button.
 *
 * `reopen_at` (may be NULL) receives the screen point of that dismissing right
 * click, so the caller replays it exactly where the user clicked instead of
 * asking where the cursor happens to be later.
 */
int uiMenuHostRun(const UiTheme* theme, const UiMenu* root, int x, int y,
                  UiMenuAlign align, HWND owner, POINT* reopen_at);

#endif /* AGAVE_UI_MENU_HOST_H */
