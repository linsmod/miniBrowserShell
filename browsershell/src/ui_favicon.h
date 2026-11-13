/*
 * ui_favicon.h - favicons for the tab strip.
 *
 * The engine hands over the *encoded* icon the page named (PNG/ICO/BMP/JPEG/
 * GIF), exactly as it was downloaded. Decoding belongs to the host: this module
 * turns those bytes into a premultiplied BGRA image that the GDI+ painter can
 * blit at any DPI.
 *
 * Icons are opaque values owned by whoever decoded them (the shell keeps one
 * per tab, frees it on navigation and on tab close). uiFaviconDecode copies the
 * input, so the engine's buffer can go away as soon as the callback returns.
 */

#ifndef AGAVE_UI_FAVICON_H
#define AGAVE_UI_FAVICON_H

#include "ui_theme.h"

typedef struct UiFavicon UiFavicon;

UiFavicon* uiFaviconDecode(const void* data, int size);
void       uiFaviconFree(UiFavicon* icon);

/* Decoded pixel size (1x1 for a failed decode, which never happens: the
 * function returns NULL instead). */
int        uiFaviconWidth(const UiFavicon* icon);
int        uiFaviconHeight(const UiFavicon* icon);

/* Test hook for --ui-shot: one decoded pixel as 0xRRGGBB (alpha is dropped).
 * Returns 0 when (x, y) is outside the image. */
int        uiFaviconPixel(const UiFavicon* icon, int x, int y, unsigned* rgb);

/* Draws the icon scaled into `rect`, smoothing the downscale (32 px icons on a
 * 16 px tab used to alias hard). The graphics' interpolation mode is restored
 * afterwards, so following text is unaffected. */
void       uiFaviconPaint(UiPainter* painter, const UiFavicon* icon, UiRect rect);

#endif /* AGAVE_UI_FAVICON_H */
