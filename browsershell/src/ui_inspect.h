/*
 * ui_inspect.h - host-side marker for the inspect probe.
 *
 * The element highlight must not touch the page at all: an injected DOM node can
 * stretch the document, create scrollbars, or get captured by an ancestor's
 * transform. So the page only *reports* the target's getBoundingClientRect();
 * the frame is painted here, in a click-through layered popup above the webview.
 *
 * The frame is the whole surface: no label is drawn next to it. The element info
 * goes to the shell's bottom-left status bubble, which is sized for text and is
 * never clipped by a tiny target (a 2x2 element used to cut its own label away).
 */

#ifndef UI_INSPECT_H
#define UI_INSPECT_H

#include <windows.h>

#include "ui_theme.h"


typedef struct UiInspect {
    HWND   hwnd;          /* the overlay child (NULL until created) */
    HWND   parent;
    UiTheme* theme;       /* borrowed from the shell window */
    int    x, y, w, h;    /* content rect in parent client coords */
    int    mark_x, mark_y, mark_w, mark_h;
    int    has_mark;
    int    shown;
    char   label[256];
    HWND   target;         /* webview control: repainted where the mark was */
    int    layered;        /* 1 = UpdateLayeredWindow path, 0 = region fallback */

} UiInspect;

void uiInspectInit(UiInspect* insp, HWND parent, UiTheme* theme);
void uiInspectDestroy(UiInspect* insp);

/* The page area (where marks may be drawn). Marks outside are clipped by the
 * window region, exactly like overflow:hidden. */
void uiInspectSetContentRect(UiInspect* insp, int x, int y, int w, int h);

/* Target rect is in view (page) coordinates, relative to the content rect. */
void uiInspectSetMark(UiInspect* insp, int x, int y, int w, int h, const char* label);
void uiInspectClear(UiInspect* insp);

void uiInspectShow(UiInspect* insp, int show);
int  uiInspectIsShown(const UiInspect* insp);
/* Re-place the mark after the shell moved (screen coordinates of an owned
 * popup change with the owner). */
void uiInspectRefresh(UiInspect* insp);
/* The control under the marker: its previously covered area is invalidated so a
 * moved box cannot leave a ghost frame on the page. */
void uiInspectSetTarget(UiInspect* insp, HWND target);


#endif /* UI_INSPECT_H */