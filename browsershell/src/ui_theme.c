/*
 * ui_theme.c - palette, DPI metrics, fonts, GDI+ painter and vector icons.
 */

#include "ui_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_ARGB_ALPHA(c) (((unsigned)(c) >> 24) & 0xFFu)

/* ------------------------------------------------------------------------- */
/* GDI+ lifetime                                                             */
/* ------------------------------------------------------------------------- */

static ULONG_PTR g_gdiplusToken;
static int g_gdiplusStarted;

static void registerBundledFonts(void);

void uiSpaceShutdown(void)
{
    if (!g_gdiplusStarted)
        return;

    g_gdiplusStarted = 0;
    GdiplusShutdown(g_gdiplusToken);
}

int uiSpaceInit(void)
{
    GdiplusStartupInput input;

    if (g_gdiplusStarted)
        return 1;

    registerBundledFonts();

    memset(&input, 0, sizeof(input));
    input.GdiplusVersion = 1;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = TRUE;

    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) != Ok)
        return 0;

    g_gdiplusStarted = 1;
    atexit(uiSpaceShutdown);

    return 1;
}

/* ------------------------------------------------------------------------- */
/* theme                                                                     */
/* ------------------------------------------------------------------------- */

static int scalePx(unsigned dpi, int value)
{
    int scaled = MulDiv(value, (int)dpi, 96);

    return scaled > 0 ? scaled : 1;
}

static COLORREF toColorRef(unsigned argb)
{
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

static HFONT makeGdiFont(int pixelHeight, int weight, const wchar_t* face)
{
    LOGFONTW lf;

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -pixelHeight;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    /* ClearType, the same subpixel AA the chrome's GDI+ text now uses: R167
     * chose grayscale here so the EDITs would not be cluster-coloured next to a
     * grayscale chrome, but a grayscale chrome next to ClearType Windows text
     * reads as faded/fuzzy (2026-11-12). Both sides are ClearType now, so the
     * address pill and the tab titles share one look. The EDITs always sit on an
     * opaque pill (WM_CTLCOLOREDIT returns the fill brush), which ClearType
     * requires. */
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);

    return CreateFontIndirectW(&lf);
}

/* The build copies `fonts/` next to the executable (fonts/NotoSansSC-*.ttf, the
 * same files the engine registers in FontCachePS.cpp). Register them privately
 * here too so the chrome gets the intended CJK face without depending on the
 * engine having started (--ui-shot never starts one) or on the face being
 * installed system-wide. Idempotent: AddFontResourceEx ref-counts. */
static void registerBundledFonts(void)
{
    wchar_t exe[MAX_PATH];
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    DWORD len;

    len = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (!len || len >= MAX_PATH)
        return;

    {
        wchar_t* slash = wcsrchr(exe, L'\\');

        if (!slash)
            return;

        slash[1] = 0;
    }

    _snwprintf(pattern, MAX_PATH, L"%sfonts\\*.ttf", exe);
    pattern[MAX_PATH - 1] = 0;

    find = FindFirstFileW(pattern, &data);

    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            wchar_t path[MAX_PATH];

            _snwprintf(path, MAX_PATH, L"%sfonts\\%s", exe, data.cFileName);
            path[MAX_PATH - 1] = 0;
            AddFontResourceExW(path, FR_PRIVATE, 0);
        }
    } while (FindNextFileW(find, &data));

    FindClose(find);
}

static void freeFonts(UiTheme* theme)
{
    if (!theme)
        return;

    if (theme->hfont_ui) { DeleteObject(theme->hfont_ui); theme->hfont_ui = NULL; }
    if (theme->hfont_small) { DeleteObject(theme->hfont_small); theme->hfont_small = NULL; }
    if (theme->hfont_address) { DeleteObject(theme->hfont_address); theme->hfont_address = NULL; }

    if (theme->font_ui) { GdipDeleteFont(theme->font_ui); theme->font_ui = NULL; }
    if (theme->font_small) { GdipDeleteFont(theme->font_small); theme->font_small = NULL; }
    if (theme->font_address) { GdipDeleteFont(theme->font_address); theme->font_address = NULL; }

    if (theme->fmt_left) { GdipDeleteStringFormat(theme->fmt_left); theme->fmt_left = NULL; }
    if (theme->fmt_center) { GdipDeleteStringFormat(theme->fmt_center); theme->fmt_center = NULL; }
    if (theme->fmt_right) { GdipDeleteStringFormat(theme->fmt_right); theme->fmt_right = NULL; }

    if (theme->family) { GdipDeleteFontFamily(theme->family); theme->family = NULL; }
}

static GpStringFormat* makeFormat(UiAlign align)
{
    GpStringFormat* format = NULL;

    if (GdipCreateStringFormat(0, LANG_NEUTRAL, &format) != Ok)
        return NULL;

    GdipSetStringFormatFlags(format, StringFormatFlagsNoWrap);
    GdipSetStringFormatTrimming(format, StringTrimmingEllipsisCharacter);
    GdipSetStringFormatHotkeyPrefix(format, HotkeyPrefixNone);
    GdipSetStringFormatLineAlign(format, StringAlignmentCenter);

    if (align == UI_ALIGN_CENTER)
        GdipSetStringFormatAlign(format, StringAlignmentCenter);
    else if (align == UI_ALIGN_RIGHT)
        GdipSetStringFormatAlign(format, StringAlignmentFar);
    else
        GdipSetStringFormatAlign(format, StringAlignmentNear);

    return format;
}

static void createFonts(UiTheme* theme)
{
    /* One family has to cover Latin AND CJK: GDI+ DrawString does no
     * cross-script family linking, so asking for Segoe UI and drawing Chinese
     * gets GDI's substitute face (SimSun-ish) while the Latin runs stay Segoe -
     * two different typefaces on one row, which is what "中英文字体不统一"
     * looked like. Both YaHei and the bundled Noto Sans SC carry Latin + CJK,
     * so whichever is installed keeps the chrome consistent. */
    static const wchar_t* const kFaces[] = {
        L"Microsoft YaHei UI",
        L"Microsoft YaHei",
        L"Noto Sans SC",
        L"Segoe UI",
        L"Arial"
    };
    const wchar_t* face = kFaces[0];
    int dpi = (int)theme->dpi;
    int i;

    theme->family = NULL;

    for (i = 0; i < (int)(sizeof(kFaces) / sizeof(kFaces[0])); ++i) {
        GpFontFamily* family = NULL;

        if (GdipCreateFontFamilyFromName(kFaces[i], NULL, &family) == Ok && family) {
            theme->family = family;
            face = kFaces[i];
            break;
        }
    }

    theme->hfont_ui = makeGdiFont(scalePx(dpi, 13), FW_NORMAL, face);
    theme->hfont_small = makeGdiFont(scalePx(dpi, 11), FW_NORMAL, face);
    theme->hfont_address = makeGdiFont(scalePx(dpi, 14), FW_NORMAL, face);

    if (theme->family) {
        GdipCreateFont(theme->family, (REAL)scalePx(dpi, 13), FontStyleRegular, UnitPixel, &theme->font_ui);
        GdipCreateFont(theme->family, (REAL)scalePx(dpi, 11), FontStyleRegular, UnitPixel, &theme->font_small);
        GdipCreateFont(theme->family, (REAL)scalePx(dpi, 14), FontStyleRegular, UnitPixel, &theme->font_address);
    }

    theme->fmt_left = makeFormat(UI_ALIGN_LEFT);
    theme->fmt_center = makeFormat(UI_ALIGN_CENTER);
    theme->fmt_right = makeFormat(UI_ALIGN_RIGHT);
}

static void applyPalette(UiTheme* theme)
{
    if (theme->kind == UI_THEME_DARK) {
        /* Chrome dark (incognito) sampled from the reference screenshots:
         * frame #1F2020, tab/toolbar #3C3C3C, pill #282828, hover #474747,
         * icon-button hover #505050, focus ring #A8C7FA. The toolbar is
         * LIGHTER than the tab strip and the pill is DARKER than both, which
         * is what gives Chrome's chrome its depth. */
        theme->window = UI_RGB(0x3C, 0x3C, 0x3C);
        theme->chrome = UI_RGB(0x1F, 0x20, 0x20);
        theme->toolbar = UI_RGB(0x3C, 0x3C, 0x3C);
        theme->surface = UI_RGB(0x3C, 0x3C, 0x3C);
        theme->surface_alt = UI_RGB(0x1F, 0x1F, 0x1F);
        theme->hover = UI_RGB(0x47, 0x47, 0x47);
        theme->pressed = UI_RGB(0x56, 0x56, 0x56);
        theme->field = UI_RGB(0x28, 0x28, 0x28);
        theme->field_hover = UI_RGB(0x45, 0x45, 0x45);
        /* Hover fill == the pill's hover border colour ("背景色和边框一致"), so
         * the pill reads as one flat shape while the pointer is on it. #5A5A5A
         * was blinding next to a #3C3C3C toolbar; #454545 keeps the step visible
         * without a flash. */
        theme->border_strong = UI_RGB(0x45, 0x45, 0x45);
        theme->field_focus = UI_RGB(0x3C, 0x3C, 0x3C);
        theme->border = UI_RGB(0x37, 0x37, 0x37);
        theme->icon = UI_RGB(0xC4, 0xC7, 0xC5);
        theme->text = UI_RGB(0xE8, 0xEA, 0xED);
        theme->text_muted = UI_RGB(0x9A, 0xA0, 0xA6);
        theme->text_faint = UI_RGB(0x7A, 0x7F, 0x85);
        theme->accent = UI_RGB(0x8A, 0xB4, 0xF8);
        theme->accent_soft = UI_RGB(0x1F, 0x3A, 0x5F);
        theme->danger = UI_RGB(0xF2, 0x8B, 0x82);
        theme->danger_soft = UI_RGB(0x3A, 0x24, 0x22);
        theme->shadow = UI_ARGB(0x80, 0x00, 0x00, 0x00);

        theme->tab_hover = UI_RGB(0x47, 0x47, 0x47);
        theme->tab_pressed = UI_RGB(0x50, 0x50, 0x50);
        theme->tab_close_hover = UI_RGB(0x5A, 0x5A, 0x5A);
        theme->tab_close_pressed = UI_RGB(0x6A, 0x6A, 0x6A);
        /* Dark gray: reads against the #1F2020 strip without becoming a border. */
        theme->tab_sep = UI_RGB(0x3C, 0x40, 0x43);
        theme->button_hover = UI_RGB(0x50, 0x50, 0x50);
        theme->button_pressed = UI_RGB(0x5A, 0x5A, 0x5A);
        theme->close_hover = UI_RGB(0x4A, 0x4A, 0x4A);
        theme->focus_ring = UI_RGB(0xA8, 0xC7, 0xFA);
        theme->window_close_hover = UI_RGB(0xC4, 0x2B, 0x1C);

        theme->menu_bg = UI_RGB(0x1F, 0x1F, 0x1F);
        theme->menu_hover = UI_RGB(0x3C, 0x40, 0x43);
        theme->menu_border = UI_RGB(0x37, 0x37, 0x37);
        theme->menu_sep = UI_RGB(0x37, 0x37, 0x37);
        theme->menu_text_disabled = UI_RGB(0x7A, 0x7A, 0x7A);
        theme->menu_shortcut = UI_RGB(0x9A, 0xA0, 0xA6);
        theme->menu_icon = UI_RGB(0xC8, 0xC8, 0xC8);
        theme->menu_shadow = UI_ARGB(0x66, 0x00, 0x00, 0x00);
    } else {
        theme->window = UI_RGB(0xFF, 0xFF, 0xFF);
        theme->chrome = UI_RGB(0xDE, 0xE1, 0xE6);
        theme->toolbar = UI_RGB(0xFF, 0xFF, 0xFF);
        theme->surface = UI_RGB(0xFF, 0xFF, 0xFF);
        theme->surface_alt = UI_RGB(0xF1, 0xF3, 0xF4);
        theme->hover = UI_RGB(0xE8, 0xEA, 0xED);
        theme->pressed = UI_RGB(0xDA, 0xDC, 0xE0);
        theme->field = UI_RGB(0xF1, 0xF3, 0xF4);
        theme->field_hover = UI_RGB(0xBD, 0xC1, 0xC6);
        theme->field_focus = UI_RGB(0xFF, 0xFF, 0xFF);
        theme->border = UI_RGB(0xDA, 0xDC, 0xE0);
        theme->border_strong = UI_RGB(0xBD, 0xC1, 0xC6);
        theme->icon = UI_RGB(0x5F, 0x63, 0x68);
        theme->text = UI_RGB(0x20, 0x21, 0x24);
        theme->text_muted = UI_RGB(0x5F, 0x63, 0x68);
        theme->text_faint = UI_RGB(0x80, 0x86, 0x8B);
        theme->accent = UI_RGB(0x1A, 0x73, 0xE8);
        theme->accent_soft = UI_RGB(0xE8, 0xF0, 0xFE);
        theme->danger = UI_RGB(0xD9, 0x30, 0x25);
        theme->danger_soft = UI_RGB(0xFC, 0xE8, 0xE6);
        theme->shadow = UI_ARGB(0x33, 0x00, 0x00, 0x00);

        theme->tab_hover = UI_RGB(0xD3, 0xD6, 0xDB);
        theme->tab_pressed = UI_RGB(0xC2, 0xC6, 0xCC);
        theme->tab_close_hover = UI_RGB(0xD0, 0xD3, 0xD8);
        theme->tab_close_pressed = UI_RGB(0xB4, 0xB9, 0xC0);
        /* Pale white: the light strip is #DEE1E6, so the hairline is lighter. */
        theme->tab_sep = UI_RGB(0xF4, 0xF6, 0xF8);
        theme->button_hover = UI_RGB(0xF1, 0xF3, 0xF4);
        theme->button_pressed = UI_RGB(0xE3, 0xE5, 0xE8);
        theme->close_hover = UI_RGB(0xE8, 0xEA, 0xED);
        theme->focus_ring = UI_RGB(0x1A, 0x73, 0xE8);
        theme->window_close_hover = UI_RGB(0xE8, 0x11, 0x23);

        theme->menu_bg = UI_RGB(0xFF, 0xFF, 0xFF);
        theme->menu_hover = UI_RGB(0xF1, 0xF3, 0xF4);
        theme->menu_border = UI_RGB(0xDA, 0xDC, 0xE0);
        theme->menu_sep = UI_RGB(0xE8, 0xEA, 0xED);
        theme->menu_text_disabled = UI_RGB(0xB0, 0xB4, 0xB8);
        theme->menu_shortcut = UI_RGB(0x5F, 0x63, 0x68);
        theme->menu_icon = UI_RGB(0x5F, 0x63, 0x68);
        theme->menu_shadow = UI_ARGB(0x33, 0x00, 0x00, 0x00);
    }

    theme->field_color = toColorRef(theme->field);
    theme->field_hover_color = toColorRef(theme->field_hover);
    theme->field_focus_color = toColorRef(theme->field_focus);
    theme->text_color = toColorRef(theme->text);

    if (theme->field_brush) { DeleteObject(theme->field_brush); theme->field_brush = NULL; }
    if (theme->field_hover_brush) { DeleteObject(theme->field_hover_brush); theme->field_hover_brush = NULL; }
    if (theme->field_focus_brush) { DeleteObject(theme->field_focus_brush); theme->field_focus_brush = NULL; }
    if (theme->surface_brush) { DeleteObject(theme->surface_brush); theme->surface_brush = NULL; }

    theme->field_brush = CreateSolidBrush(theme->field_color);
    theme->field_hover_brush = CreateSolidBrush(theme->field_hover_color);
    theme->field_focus_brush = CreateSolidBrush(theme->field_focus_color);
    theme->surface_brush = CreateSolidBrush(toColorRef(theme->surface));
}

static void applyMetrics(UiTheme* theme)
{
    unsigned dpi = theme->dpi;

    theme->tabstrip_h = scalePx(dpi, 40);
    theme->toolbar_h = scalePx(dpi, 44);
    theme->find_h = scalePx(dpi, 40);
    theme->status_h = scalePx(dpi, 24);
    theme->tab_h = scalePx(dpi, 32);
    /* Chrome's shrinking rules (user-measured), applied to the tab *rect*: a
     * background tab bottoms out at 16 px (exactly its favicon), the active one
     * at 36 px - it is the tab being read, so it keeps more room before the
     * icon has to go and only the close chip is left. */
    theme->tab_min_w = scalePx(dpi, 16);
    theme->tab_active_min_w = scalePx(dpi, 36);
    theme->tab_max_w = scalePx(dpi, 220);
    theme->tab_gap = scalePx(dpi, 2);
    theme->tab_radius = scalePx(dpi, 8);
    theme->strip_pad = scalePx(dpi, 8);
    theme->window_btn_w = scalePx(dpi, 46);
    theme->button_w = scalePx(dpi, 32);
    theme->button_h = scalePx(dpi, 32);
    theme->button_radius = scalePx(dpi, 8);
    theme->address_h = scalePx(dpi, 32);
    theme->address_radius = scalePx(dpi, 16);
    theme->address_pad = scalePx(dpi, 12);
    theme->find_field_h = scalePx(dpi, 30);
    theme->find_field_w = scalePx(dpi, 360);
    theme->status_pad = scalePx(dpi, 10);
    theme->content_top = 0;

    /* Chrome menus: 30 px rows at 96 DPI, 12 px side padding, 8 px panel
     * radius, 4 px item highlight radius. */
    theme->menu_item_h = scalePx(dpi, 30);
    theme->menu_sep_h = scalePx(dpi, 9);
    theme->menu_pad_x = scalePx(dpi, 12);
    theme->menu_pad_y = scalePx(dpi, 8);
    theme->menu_radius = scalePx(dpi, 8);
    theme->menu_item_radius = scalePx(dpi, 4);
    theme->menu_icon_col = scalePx(dpi, 26);
    theme->menu_min_w = scalePx(dpi, 180);
    theme->menu_max_w = scalePx(dpi, 460);
    theme->menu_gap = scalePx(dpi, 24);
}

void uiThemeInit(UiTheme* theme, UiThemeKind kind, unsigned dpi)
{
    if (!theme)
        return;

    memset(theme, 0, sizeof(*theme));

    if (!uiSpaceInit())
        return;

    theme->kind = kind;
    theme->dpi = dpi ? dpi : 96u;
    theme->scale = (float)theme->dpi / 96.0f;

    applyPalette(theme);
    applyMetrics(theme);
    createFonts(theme);
}

void uiThemeSetDpi(UiTheme* theme, unsigned dpi)
{
    if (!theme || dpi == 0 || dpi == theme->dpi)
        return;

    freeFonts(theme);
    theme->dpi = dpi;
    theme->scale = (float)dpi / 96.0f;
    applyMetrics(theme);
    createFonts(theme);
}

void uiThemeFree(UiTheme* theme)
{
    if (!theme)
        return;

    freeFonts(theme);

    if (theme->field_brush) { DeleteObject(theme->field_brush); theme->field_brush = NULL; }
    if (theme->field_hover_brush) { DeleteObject(theme->field_hover_brush); theme->field_hover_brush = NULL; }
    if (theme->field_focus_brush) { DeleteObject(theme->field_focus_brush); theme->field_focus_brush = NULL; }
    if (theme->surface_brush) { DeleteObject(theme->surface_brush); theme->surface_brush = NULL; }
}

int uiThemeScale(const UiTheme* theme, int value)
{
    if (!theme)
        return value;

    return scalePx(theme->dpi, value);
}

/* ------------------------------------------------------------------------- */
/* rect helpers                                                              */
/* ------------------------------------------------------------------------- */

UiRect uiRectMake(int x, int y, int w, int h)
{
    UiRect r;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;

    return r;
}

int uiRectContains(UiRect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

UiRect uiRectInset(UiRect r, int dx, int dy)
{
    r.x += dx;
    r.y += dy;
    r.w -= dx * 2;
    r.h -= dy * 2;

    if (r.w < 0)
        r.w = 0;
    if (r.h < 0)
        r.h = 0;

    return r;
}

/* ------------------------------------------------------------------------- */
/* painter                                                                   */
/* ------------------------------------------------------------------------- */

int uiPainterBegin(UiPainter* p, HDC hdc, const UiTheme* theme, int width, int height)
{
    if (!p || !hdc || !theme)
        return 0;

    memset(p, 0, sizeof(*p));

    if (!uiSpaceInit())
        return 0;

    if (GdipCreateFromHDC(hdc, &p->g) != Ok || !p->g)
        return 0;

    p->hdc = hdc;
    p->theme = theme;
    p->width = width;
    p->height = height;

    GdipSetSmoothingMode(p->g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(p->g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(p->g, TextRenderingHintAntiAliasGridFit);
    GdipSetCompositingQuality(p->g, CompositingQualityHighQuality);

    return 1;
}

int uiPainterBeginGraphics(UiPainter* p, GpGraphics* graphics, const UiTheme* theme,
                           int width, int height)
{
    if (!p || !graphics || !theme)
        return 0;

    memset(p, 0, sizeof(*p));

    p->g = graphics;
    p->theme = theme;
    p->width = width;
    p->height = height;

    GdipSetSmoothingMode(p->g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(p->g, PixelOffsetModeHalf);
    GdipSetTextRenderingHint(p->g, TextRenderingHintAntiAliasGridFit);
    GdipSetCompositingQuality(p->g, CompositingQualityHighQuality);

    return 1;
}

void uiPainterEnd(UiPainter* p)
{
    if (!p)
        return;

    if (p->fill) { GdipDeleteBrush(p->fill); p->fill = NULL; }
    if (p->pen) { GdipDeletePen(p->pen); p->pen = NULL; }
    if (p->path) { GdipDeletePath(p->path); p->path = NULL; }
    if (p->g) { GdipDeleteGraphics(p->g); p->g = NULL; }
}

static GpSolidFill* fillColor(UiPainter* p, unsigned argb)
{
    if (!p->fill) {
        if (GdipCreateSolidFill(argb, &p->fill) != Ok)
            return NULL;
    } else {
        GdipSetSolidFillColor(p->fill, argb);
    }

    return p->fill;
}

static GpPen* penColor(UiPainter* p, unsigned argb, float width)
{
    if (!p->pen) {
        if (GdipCreatePen1(argb, width, UnitPixel, &p->pen) != Ok)
            return NULL;
    } else {
        GdipSetPenColor(p->pen, argb);
        GdipSetPenWidth(p->pen, width);
    }

    GdipSetPenStartCap(p->pen, LineCapRound);
    GdipSetPenEndCap(p->pen, LineCapRound);
    GdipSetPenLineJoin(p->pen, LineJoinRound);

    return p->pen;
}

static GpPath* roundPath(UiPainter* p, UiRect r, int radius)
{
    int diameter = radius * 2;
    REAL x = (REAL)r.x, y = (REAL)r.y, w = (REAL)r.w, h = (REAL)r.h;
    REAL d;

    if (!p->path) {
        if (GdipCreatePath(FillModeWinding, &p->path) != Ok)
            return NULL;
    } else {
        GdipResetPath(p->path);
    }

    if (diameter > r.w)
        diameter = r.w;
    if (diameter > r.h)
        diameter = r.h;

    if (diameter <= 1) {
        GdipAddPathRectangle(p->path, x, y, w, h);
        return p->path;
    }

    d = (REAL)diameter;

    GdipAddPathArc(p->path, x, y, d, d, 180.0f, 90.0f);
    GdipAddPathArc(p->path, x + w - d, y, d, d, 270.0f, 90.0f);
    GdipAddPathArc(p->path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    GdipAddPathArc(p->path, x, y + h - d, d, d, 90.0f, 90.0f);
    GdipClosePathFigure(p->path);

    return p->path;
}

void uiFillRect(UiPainter* p, UiRect r, unsigned argb)
{
    GpSolidFill* brush;

    if (!p || !p->g)
        return;

    brush = fillColor(p, argb);

    if (brush)
        GdipFillRectangleI(p->g, brush, r.x, r.y, r.w, r.h);
}

void uiFillRoundRect(UiPainter* p, UiRect r, int radius, unsigned argb)
{
    GpSolidFill* brush;
    GpPath* path;

    if (!p || !p->g)
        return;

    brush = fillColor(p, argb);
    path = roundPath(p, r, radius);

    if (brush && path)
        GdipFillPath(p->g, brush, path);
}

void uiStrokeRoundRect(UiPainter* p, UiRect r, int radius, unsigned argb, float stroke)
{
    GpPen* pen;
    GpPath* path;

    if (!p || !p->g)
        return;

    pen = penColor(p, argb, stroke);
    path = roundPath(p, r, radius);

    if (pen && path)
        GdipDrawPath(p->g, pen, path);
}

void uiFillCircle(UiPainter* p, int cx, int cy, int radius, unsigned argb)
{
    GpSolidFill* brush;

    if (!p || !p->g || radius <= 0)
        return;

    brush = fillColor(p, argb);

    if (brush)
        GdipFillEllipseI(p->g, brush, cx - radius, cy - radius, radius * 2, radius * 2);
}

/* Tab silhouette: rounded top corners (radius `topRadius`), rounded bottom
 * corners (`bottomRadius`, 0 = classic squared base that merges into the
 * toolbar) and straight sides. Built as one path so the bottom corners are real
 * arcs - erasing a square corner with a pie leaves a visible blob. */
void uiFillTabShape(UiPainter* p, UiRect r, int topRadius, int bottomRadius,
                    unsigned argb)
{
    GpSolidFill* brush;
    GpPath* path;
    REAL x, y, w, h, dt, db;

    if (!p || !p->g || r.w <= 0 || r.h <= 0)
        return;

    brush = fillColor(p, argb);

    if (!brush)
        return;

    if (topRadius < 0)
        topRadius = 0;
    if (bottomRadius < 0)
        bottomRadius = 0;
    if (topRadius * 2 > r.w)
        topRadius = r.w / 2;
    if (bottomRadius * 2 > r.w)
        bottomRadius = r.w / 2;
    if (topRadius + bottomRadius > r.h) {
        int total = topRadius + bottomRadius;

        topRadius = total ? topRadius * r.h / total : 0;
        bottomRadius = r.h - topRadius;
    }

    if (!p->path) {
        if (GdipCreatePath(FillModeWinding, &p->path) != Ok)
            return;
    } else {
        GdipResetPath(p->path);
    }

    path = p->path;
    x = (REAL)r.x;
    y = (REAL)r.y;
    w = (REAL)r.w;
    h = (REAL)r.h;
    dt = (REAL)topRadius * 2.0f;
    db = (REAL)bottomRadius * 2.0f;

    /* Walk the outline clockwise with the straight edges spelled out. Adding
     * only the arcs and closing the figure draws a line across the middle of the
     * shape (that is how "rounded top + square base" turned into a sliver). */
    if (topRadius > 0) {
        GdipAddPathArc(path, x, y, dt, dt, 180.0f, 90.0f);            /* → (x + r, y) */
        GdipAddPathArc(path, x + w - dt, y, dt, dt, 270.0f, 90.0f);   /* → (x + w, y + r) */
        GdipAddPathLine(path, x + w, y + (REAL)topRadius,
                        x + w, y + h - (REAL)bottomRadius);
    } else {
        GdipStartPathFigure(path);
        GdipAddPathLine(path, x, y, x + w, y);
        GdipAddPathLine(path, x + w, y, x + w, y + h - (REAL)bottomRadius);
    }

    if (bottomRadius > 0) {
        GdipAddPathArc(path, x + w - db, y + h - db, db, db, 0.0f, 90.0f);
        GdipAddPathLine(path, x + w - (REAL)bottomRadius, y + h,
                        x + (REAL)bottomRadius, y + h);
        GdipAddPathArc(path, x, y + h - db, db, db, 90.0f, 90.0f);
        GdipAddPathLine(path, x, y + h - (REAL)bottomRadius,
                        x, y + (REAL)topRadius);
    } else {
        GdipAddPathLine(path, x + w, y + h, x, y + h);
        GdipAddPathLine(path, x, y + h, x, y + (REAL)topRadius);
    }

    GdipClosePathFigure(path);

    GdipFillPath(p->g, brush, path);
}

void uiFillPie(UiPainter* p, int cx, int cy, int radius, float startAngle,
               float sweep, unsigned argb)
{
    GpSolidFill* brush;

    if (!p || !p->g || radius <= 0)
        return;

    brush = fillColor(p, argb);

    if (brush)
        GdipFillPie(p->g, brush, (REAL)(cx - radius), (REAL)(cy - radius),
                    (REAL)(radius * 2), (REAL)(radius * 2), startAngle, sweep);
}

void uiStrokeCircle(UiPainter* p, int cx, int cy, int radius, unsigned argb, float stroke)
{
    GpPen* pen;

    if (!p || !p->g || radius <= 0)
        return;

    pen = penColor(p, argb, stroke);

    if (pen)
        GdipDrawEllipseI(p->g, pen, cx - radius, cy - radius, radius * 2, radius * 2);
}

void uiLine(UiPainter* p, int x1, int y1, int x2, int y2, unsigned argb, float stroke)
{
    GpPen* pen;

    if (!p || !p->g)
        return;

    pen = penColor(p, argb, stroke);

    if (pen)
        GdipDrawLineI(p->g, pen, x1, y1, x2, y2);
}

void uiShadow(UiPainter* p, UiRect r, int radius, int spread, unsigned argb)
{
    int i;
    unsigned baseAlpha = UI_ARGB_ALPHA(argb);

    if (!p || !p->g || spread <= 0)
        return;

    for (i = spread; i >= 1; --i) {
        UiRect layer = uiRectMake(r.x - i, r.y - i + spread / 2, r.w + i * 2, r.h + i * 2);
        unsigned alpha = baseAlpha / (unsigned)(i + 1);
        unsigned color = (argb & 0x00FFFFFFu) | (alpha << 24);

        uiFillRoundRect(p, layer, radius + i, color);
    }
}

void uiTextW(UiPainter* p, const wchar_t* text, UiRect r, GpFont* font,
             unsigned argb, UiAlign align)
{
    GpSolidFill* brush;
    GpStringFormat* format;
    RectF box;

    if (!p || !p->g || !text || !text[0] || !font)
        return;

    brush = fillColor(p, argb);

    if (!brush)
        return;

    format = align == UI_ALIGN_CENTER ? p->theme->fmt_center
           : align == UI_ALIGN_RIGHT  ? p->theme->fmt_right
                                      : p->theme->fmt_left;
    if (!format)
        return;

    box.X = (REAL)r.x;
    box.Y = (REAL)r.y;
    box.Width = (REAL)r.w;
    box.Height = (REAL)r.h;

    /* Two things decide how crisp chrome text looks, and both were wrong:
     *
     *  - PixelOffsetModeHalf (which the shapes use so their edges land on pixel
     *    centres) shifts every glyph by half a pixel. That defeats grid fitting
     *    and produces the uniform grey halo that reads as "fuzzy" - painfully so
     *    in light mode, where dark-on-light fringes are far more visible than
     *    bright-on-dark (2026-11-12).
     *  - The hint was AntiAliasGridFit (grayscale). Windows itself draws this
     *    text with ClearType, and so does Chrome; grayscale leaves the stems
     *    thin/faded next to the native EDIT controls. ClearTypeGridFit gives the
     *    subpixel-sharp stems, which is what a user compares against Chrome.
     *
     * Shapes keep Half + AntiAlias (the painter's defaults); restore both after
     * the glyphs so a row's text cannot change how the next icon is drawn. */
    {
        GpPixelOffsetMode savedOffset = PixelOffsetModeHalf;
        TextRenderingHint savedHint = TextRenderingHintAntiAliasGridFit;

        GdipGetPixelOffsetMode(p->g, &savedOffset);
        GdipGetTextRenderingHint(p->g, &savedHint);
        GdipSetPixelOffsetMode(p->g, PixelOffsetModeNone);
        GdipSetTextRenderingHint(p->g, TextRenderingHintClearTypeGridFit);
        GdipDrawString(p->g, text, -1, font, &box, format, (GpBrush*)brush);
        GdipSetTextRenderingHint(p->g, savedHint);
        GdipSetPixelOffsetMode(p->g, savedOffset);
    }
}

void uiText(UiPainter* p, const char* utf8, UiRect r, GpFont* font,
            unsigned argb, UiAlign align)
{
    wchar_t wide[1024];

    if (!utf8)
        return;

    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 1024) <= 0) {
        wide[0] = 0;
    }

    uiTextW(p, wide, r, font, argb, align);
}

/* ------------------------------------------------------------------------- */
/* text measurement                                                          */
/* ------------------------------------------------------------------------- */

static GpBitmap* g_measureBitmap;
static GpGraphics* g_measureGraphics;

static GpGraphics* measureGraphics(void)
{
    if (g_measureGraphics)
        return g_measureGraphics;

    if (!uiSpaceInit())
        return NULL;

    if (GdipCreateBitmapFromScan0(1, 1, 4, PixelFormat32bppPARGB, NULL,
                                  &g_measureBitmap) != Ok || !g_measureBitmap)
        return NULL;

    if (GdipGetImageGraphicsContext(g_measureBitmap, &g_measureGraphics) != Ok ||
        !g_measureGraphics)
        return NULL;

    GdipSetTextRenderingHint(g_measureGraphics, TextRenderingHintAntiAliasGridFit);

    return g_measureGraphics;
}

int uiMeasureTextW(const UiTheme* theme, GpFont* font, const wchar_t* text,
                   int* width, int* height)
{
    GpGraphics* graphics;
    RectF box;
    RectF bounds;

    if (width)
        *width = 0;
    if (height)
        *height = 0;

    if (!theme || !font || !text || !text[0])
        return 1;

    graphics = measureGraphics();

    if (!graphics)
        return 0;

    box.X = 0.0f;
    box.Y = 0.0f;
    box.Width = 100000.0f;
    box.Height = 1000.0f;

    if (GdipMeasureString(graphics, text, -1, font, &box, theme->fmt_left,
                          &bounds, NULL, NULL) != Ok)
        return 0;

    if (width)
        *width = (int)(bounds.Width + 0.999f);
    if (height)
        *height = (int)(bounds.Height + 0.999f);

    return 1;
}

int uiMeasureText(const UiTheme* theme, GpFont* font, const char* utf8,
                  int* width, int* height)
{
    wchar_t wide[1024];

    if (width)
        *width = 0;
    if (height)
        *height = 0;

    if (!utf8 || !utf8[0])
        return 1;

    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 1024) <= 0)
        return 0;

    return uiMeasureTextW(theme, font, wide, width, height);
}

/* ------------------------------------------------------------------------- */
/* vector icons                                                              */
/* ------------------------------------------------------------------------- */

static void polyline(UiPainter* p, GpPen* pen, const float* points, int count)
{
    GpPath* path;

    if (count < 4)
        return;

    path = roundPath(p, uiRectMake(0, 0, 0, 0), 0);

    if (!path)
        return;

    GdipResetPath(path);
    GdipStartPathFigure(path);

    {
        int i;

        for (i = 1; i < count / 2; ++i)
            GdipAddPathLine(path, points[(i - 1) * 2], points[(i - 1) * 2 + 1],
                            points[i * 2], points[i * 2 + 1]);
    }

    GdipDrawPath(p->g, pen, path);
}

static void filledTriangle(UiPainter* p, unsigned argb,
                           float x1, float y1, float x2, float y2, float x3, float y3)
{
    GpSolidFill* brush = fillColor(p, argb);
    GpPointF points[3];

    if (!brush)
        return;

    points[0].X = x1; points[0].Y = y1;
    points[1].X = x2; points[1].Y = y2;
    points[2].X = x3; points[2].Y = y3;

    GdipFillPolygon(p->g, brush, points, 3, FillModeWinding);
}

void uiIcon(UiPainter* p, UiIcon icon, UiRect r, unsigned argb, float stroke)
{
    float cx, cy, u, s;
    GpPen* pen;

    if (!p || !p->g || icon == UI_ICON_NONE)
        return;

    cx = (float)r.x + (float)r.w / 2.0f;
    cy = (float)r.y + (float)r.h / 2.0f;
    u = (float)(r.w < r.h ? r.w : r.h) / 2.0f;
    s = stroke > 0.5f ? stroke : u * 0.14f;

    if (s < 1.0f)
        s = 1.0f;

    pen = penColor(p, argb, s);

    if (!pen)
        return;

    switch (icon) {
    case UI_ICON_BACK: {
        float points[] = { cx + u * 0.35f, cy - u * 0.52f,
                           cx - u * 0.35f, cy,
                           cx + u * 0.35f, cy + u * 0.52f };
        polyline(p, pen, points, 6);
        break;
    }
    case UI_ICON_FORWARD: {
        float points[] = { cx - u * 0.35f, cy - u * 0.52f,
                           cx + u * 0.35f, cy,
                           cx - u * 0.35f, cy + u * 0.52f };
        polyline(p, pen, points, 6);
        break;
    }
    case UI_ICON_RELOAD: {
        float radius = u * 0.64f;

        GdipDrawArc(p->g, pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f,
                    40.0f, 285.0f);
        filledTriangle(p, argb,
                       cx + radius * 0.95f, cy - radius * 0.75f,
                       cx + radius * 0.15f, cy - radius * 1.05f,
                       cx + radius * 0.05f, cy - radius * 0.30f);
        break;
    }
    case UI_ICON_STOP: {
        float d = u * 0.55f;
        float points1[] = { cx - d, cy - d, cx + d, cy + d };
        float points2[] = { cx + d, cy - d, cx - d, cy + d };

        polyline(p, pen, points1, 4);
        polyline(p, pen, points2, 4);
        break;
    }
    case UI_ICON_MENU: {
        float half = u * 0.66f;
        float gap = u * 0.44f;
        int i;

        for (i = -1; i <= 1; ++i)
            GdipDrawLine(p->g, pen, cx - half, cy + gap * (float)i,
                         cx + half, cy + gap * (float)i);
        break;
    }
    case UI_ICON_PLUS: {
        float half = u * 0.62f;

        GdipDrawLine(p->g, pen, cx - half, cy, cx + half, cy);
        GdipDrawLine(p->g, pen, cx, cy - half, cx, cy + half);
        break;
    }
    case UI_ICON_CLOSE: {
        /* 0.55u: a 16 px icon box then spans ~8.8 px, like Chrome's tab close.
         * 0.50u read as a speck once the tab chip was already small. */
        float d = u * 0.55f;
        float points1[] = { cx - d, cy - d, cx + d, cy + d };
        float points2[] = { cx + d, cy - d, cx - d, cy + d };

        polyline(p, pen, points1, 4);
        polyline(p, pen, points2, 4);
        break;
    }
    case UI_ICON_SEARCH: {
        float radius = u * 0.52f;
        float ox = cx - u * 0.18f;
        float oy = cy - u * 0.18f;

        GdipDrawEllipse(p->g, pen, ox - radius, oy - radius, radius * 2.0f, radius * 2.0f);
        GdipDrawLine(p->g, pen, ox + radius * 0.62f, oy + radius * 0.62f,
                     cx + u * 0.68f, cy + u * 0.68f);
        break;
    }
    case UI_ICON_CHEVRON_UP: {
        float points[] = { cx - u * 0.42f, cy + u * 0.20f,
                           cx, cy - u * 0.24f,
                           cx + u * 0.42f, cy + u * 0.20f };
        polyline(p, pen, points, 6);
        break;
    }
    case UI_ICON_CHEVRON_DOWN: {
        float points[] = { cx - u * 0.42f, cy - u * 0.20f,
                           cx, cy + u * 0.24f,
                           cx + u * 0.42f, cy - u * 0.20f };
        polyline(p, pen, points, 6);
        break;
    }
    case UI_ICON_GLOBE: {
        float radius = u * 0.66f;
        float meridianW = radius * 0.72f;

        GdipDrawEllipse(p->g, pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        GdipDrawEllipse(p->g, pen, cx - meridianW, cy - radius,
                        meridianW * 2.0f, radius * 2.0f);
        GdipDrawLine(p->g, pen, cx - radius, cy, cx + radius, cy);
        break;
    }
    case UI_ICON_LOCK: {
        float bodyW = u * 0.94f;
        float bodyH = u * 0.72f;
        float bodyY = cy - u * 0.02f;

        uiFillRoundRect(p, uiRectMake((int)(cx - bodyW / 2), (int)bodyY,
                                      (int)bodyW, (int)bodyH),
                        (int)(u * 0.14f), argb);
        GdipDrawArc(p->g, pen, cx - bodyW * 0.32f, cy - u * 0.62f,
                    bodyW * 0.64f, bodyW * 0.64f, 180.0f, 180.0f);
        break;
    }
    case UI_ICON_ARROW_RIGHT: {
        float half = u * 0.62f;
        float points[] = { cx - u * 0.10f, cy - u * 0.42f,
                           cx + u * 0.34f, cy,
                           cx - u * 0.10f, cy + u * 0.42f };

        GdipDrawLine(p->g, pen, cx - half, cy, cx + half, cy);
        polyline(p, pen, points, 6);
        break;
    }
    case UI_ICON_DOWNLOAD: {
        float half = u * 0.55f;
        float points[] = { cx - u * 0.34f, cy + u * 0.02f,
                           cx, cy + u * 0.36f,
                           cx + u * 0.34f, cy + u * 0.02f };

        GdipDrawLine(p->g, pen, cx, cy - u * 0.62f, cx, cy + u * 0.36f);
        polyline(p, pen, points, 6);
        GdipDrawLine(p->g, pen, cx - half, cy + u * 0.66f, cx + half, cy + u * 0.66f);
        break;
    }
    case UI_ICON_MINUS: {
        float half = u * 0.62f;

        GdipDrawLine(p->g, pen, cx - half, cy, cx + half, cy);
        break;
    }
    case UI_ICON_MAXIMIZE: {
        float half = u * 0.44f;

        GdipDrawRectangle(p->g, pen, cx - half, cy - half, half * 2.0f, half * 2.0f);
        break;
    }
    case UI_ICON_RESTORE: {
        float half = u * 0.44f;
        float dx = u * 0.16f;
        float dy = u * 0.16f;

        GdipDrawRectangle(p->g, pen, cx - half + dx, cy - half - dy,
                          half * 2.0f, half * 2.0f);
        GdipDrawRectangle(p->g, pen, cx - half - dx, cy - half + dy,
                          half * 2.0f, half * 2.0f);
        break;
    }
    default:
        break;
    }
}
