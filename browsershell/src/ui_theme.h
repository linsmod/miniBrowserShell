/*
 * ui_theme.h - design tokens, fonts, DPI scaling, GDI+ painter and icons.
 *
 * The chrome is drawn with the GDI+ flat API (antialiased paths), text with
 * GDI+ DrawString. Everything is measured from a 96-DPI baseline and scaled by
 * uiThemeScale(), so a 125%/150% monitor gets a proportional chrome instead of
 * a stretched one.
 */

#ifndef AGAVE_UI_THEME_H
#define AGAVE_UI_THEME_H

#include <windows.h>
#include "gdiplus_c.h"

typedef enum {
    UI_THEME_LIGHT = 0,
    UI_THEME_DARK = 1
} UiThemeKind;

typedef struct {
    int x, y, w, h;
} UiRect;

typedef enum {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER = 1,
    UI_ALIGN_RIGHT = 2
} UiAlign;

typedef enum {
    UI_ICON_NONE = 0,
    UI_ICON_BACK,
    UI_ICON_FORWARD,
    UI_ICON_RELOAD,
    UI_ICON_STOP,
    UI_ICON_MENU,
    UI_ICON_PLUS,
    UI_ICON_CLOSE,
    UI_ICON_SEARCH,
    UI_ICON_CHEVRON_UP,
    UI_ICON_CHEVRON_DOWN,
    UI_ICON_GLOBE,
    UI_ICON_LOCK,
    UI_ICON_ARROW_RIGHT,
    UI_ICON_DOWNLOAD,
    UI_ICON_MINUS,
    UI_ICON_MAXIMIZE,
    UI_ICON_RESTORE
} UiIcon;

/* 0xAARRGGBB */
#define UI_ARGB(a, r, g, b) \
    (((unsigned)(a) << 24) | ((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b))
#define UI_RGB(r, g, b) UI_ARGB(255, r, g, b)

typedef struct UiTheme {
    UiThemeKind kind;
    unsigned    dpi;
    float       scale;

    /* palette */
    unsigned window;        /* content/page canvas */
    unsigned chrome;        /* tab strip */
    unsigned toolbar;       /* toolbar + find row */
    unsigned surface;       /* active tab / popup */
    unsigned surface_alt;   /* unused: the status bubble follows the active tab */
    unsigned hover;
    unsigned pressed;
    unsigned field;         /* address pill / find pill */
    unsigned field_hover;
    unsigned field_focus;   /* pill while the EDIT owns focus */
    unsigned border;
    unsigned border_strong;
    unsigned icon;          /* toolbar glyphs */
    unsigned text;
    unsigned text_muted;
    unsigned text_faint;
    unsigned accent;
    unsigned accent_soft;
    unsigned danger;
    unsigned danger_soft;
    unsigned shadow;

    /* Interaction tints lifted from Chrome's dark UI (screenshots in the
     * design doc): tabs and toolbar glyphs get their own neutral grays, the
     * focused address pill a light-blue ring, window close a Windows red. */
    unsigned tab_hover;          /* inactive tab under the pointer */
    unsigned tab_pressed;        /* inactive tab while the button is held */
    unsigned tab_close_hover;    /* circular close chip inside a tab */
    unsigned tab_close_pressed;  /* close chip while the button is held */
    unsigned tab_sep;            /* hairline between two resting tabs */
    unsigned button_hover;       /* circular hover behind toolbar icons */
    unsigned button_pressed;
    unsigned close_hover;        /* find-bar close / neutral chips */
    unsigned focus_ring;         /* address + find field focus ring */
    unsigned window_close_hover; /* title-bar X, Chrome/Win11 red */

    /* Self-drawn menus (Chrome's context menu look). */
    unsigned menu_bg;
    unsigned menu_hover;
    unsigned menu_border;
    unsigned menu_sep;
    unsigned menu_text_disabled;
    unsigned menu_shortcut;
    unsigned menu_icon;
    unsigned menu_shadow;

    /* metrics (device px, already scaled) */
    int tabstrip_h;
    int toolbar_h;
    int find_h;
    int status_h;
    int tab_h;
    int tab_min_w;         /* background tab rect floor (16@96) = its favicon */
    int tab_active_min_w;  /* active tab rect floor (36@96): the one being read */
    int tab_max_w;
    int tab_gap;
    int tab_radius;
    int strip_pad;
    int window_btn_w;
    int button_w;
    int button_h;
    int button_radius;
    int address_h;
    int address_radius;
    int address_pad;
    int find_field_h;
    int find_field_w;
    int status_pad;
    int content_top;

    /* menu metrics (device px, already scaled) */
    int menu_item_h;
    int menu_sep_h;
    int menu_pad_x;
    int menu_pad_y;
    int menu_radius;
    int menu_item_radius;
    int menu_icon_col;
    int menu_min_w;
    int menu_max_w;
    int menu_gap;

    COLORREF field_color;
    COLORREF field_hover_color;
    COLORREF field_focus_color;
    COLORREF text_color;
    HBRUSH   field_brush;
    HBRUSH   field_hover_brush;
    HBRUSH   field_focus_brush;
    HBRUSH   surface_brush;

    HFONT  hfont_ui;
    HFONT  hfont_small;
    HFONT  hfont_address;
    GpFont* font_ui;
    GpFont* font_small;
    GpFont* font_address;
    GpFontFamily* family;

    GpStringFormat* fmt_left;
    GpStringFormat* fmt_center;
    GpStringFormat* fmt_right;
} UiTheme;

/* GDI+ lifetime. uiSpaceInit is idempotent and installs an atexit shutdown. */
int  uiSpaceInit(void);
void uiSpaceShutdown(void);

void uiThemeInit(UiTheme* theme, UiThemeKind kind, unsigned dpi);
void uiThemeSetDpi(UiTheme* theme, unsigned dpi);
void uiThemeFree(UiTheme* theme);

int uiThemeScale(const UiTheme* theme, int value);

int  uiRectContains(UiRect r, int x, int y);
UiRect uiRectInset(UiRect r, int dx, int dy);
UiRect uiRectMake(int x, int y, int w, int h);

/* Painter: one Begin/End per WM_PAINT (or one per offscreen frame). */
typedef struct {
    HDC         hdc;
    GpGraphics* g;
    const UiTheme* theme;
    int         width;
    int         height;
    GpSolidFill* fill;
    GpPen*       pen;
    GpPath*      path;
} UiPainter;

int  uiPainterBegin(UiPainter* p, HDC hdc, const UiTheme* theme, int width, int height);
/* Paint into a caller-owned GDI+ graphics (a 32bppPARGB bitmap for the
 * layered menu popup). uiPainterEnd deletes the graphics, like the HDC path. */
int  uiPainterBeginGraphics(UiPainter* p, GpGraphics* graphics, const UiTheme* theme,
                            int width, int height);
void uiPainterEnd(UiPainter* p);

void uiFillRect(UiPainter* p, UiRect r, unsigned argb);
void uiFillRoundRect(UiPainter* p, UiRect r, int radius, unsigned argb);
void uiStrokeRoundRect(UiPainter* p, UiRect r, int radius, unsigned argb, float stroke);
void uiFillCircle(UiPainter* p, int cx, int cy, int radius, unsigned argb);
/* Tab silhouette: rounded top corners and (when bottomRadius > 0) rounded
 * bottom corners; 0 gives the squared base that merges into the toolbar. */
void uiFillTabShape(UiPainter* p, UiRect r, int topRadius, int bottomRadius,
                    unsigned argb);
/* Pie slice at (cx, cy): degrees, 0 = +x, positive = clockwise (y grows down). */
void uiFillPie(UiPainter* p, int cx, int cy, int radius, float startAngle,
               float sweep, unsigned argb);
void uiStrokeCircle(UiPainter* p, int cx, int cy, int radius, unsigned argb, float stroke);
void uiLine(UiPainter* p, int x1, int y1, int x2, int y2, unsigned argb, float stroke);
void uiShadow(UiPainter* p, UiRect r, int radius, int spread, unsigned argb);

void uiText(UiPainter* p, const char* utf8, UiRect r, GpFont* font,
            unsigned argb, UiAlign align);
void uiTextW(UiPainter* p, const wchar_t* text, UiRect r, GpFont* font,
             unsigned argb, UiAlign align);
/* Text extents for layout (menu width, shortcut column). Returns 1 on
 * success; widths are ceil'ed device pixels. */
int  uiMeasureText(const UiTheme* theme, GpFont* font, const char* utf8,
                   int* width, int* height);
int  uiMeasureTextW(const UiTheme* theme, GpFont* font, const wchar_t* text,
                    int* width, int* height);
void uiIcon(UiPainter* p, UiIcon icon, UiRect r, unsigned argb, float stroke);

#endif /* AGAVE_UI_THEME_H */
