/*
 * gdiplus_c.h - C-compatible declarations for the GDI+ flat API.
 *
 * MSVC's <gdiplus.h> only works as C++ (the whole header sits inside
 * `namespace Gdiplus`).  The shell UI is plain C, so this header declares just
 * the flat entry points and enums ui_theme.c/ui_widgets.c/ui_menu_host.c use.
 * The DLL exports these names undecorated on x64, and gdiplus.lib imports
 * them, so nothing here needs C++.
 */

#ifndef GDIPLUS_C_H
#define GDIPLUS_C_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINGDIPAPI __stdcall
#define GDIPCONST const

typedef struct GpGraphics        GpGraphics;
typedef struct GpImage           GpImage;
typedef struct GpBitmap          GpBitmap;
typedef struct GpFont            GpFont;
typedef struct GpFontFamily      GpFontFamily;
typedef struct GpFontCollection  GpFontCollection;
typedef struct GpBrush           GpBrush;
typedef struct GpSolidFill       GpSolidFill;
typedef struct GpPen             GpPen;
typedef struct GpPath            GpPath;
typedef struct GpStringFormat    GpStringFormat;
typedef struct GpImageAttributes GpImageAttributes;

typedef int GpStatus;
#define Ok 0

/* GDI+ base scalar types (windows.h does not define them). */
typedef float REAL;
typedef DWORD ARGB;

typedef struct RectF {
    REAL X, Y, Width, Height;
} RectF;

typedef struct GpPointF {
    REAL X, Y;
} GpPointF;

typedef struct GpPoint {
    INT X, Y;
} GpPoint;

typedef struct GpRect {
    INT X, Y, Width, Height;
} GpRect;

/* gdiplusenums.h values (only the ones the shell uses). */
typedef enum GpUnit {
    UnitWorld = 0, UnitDisplay, UnitPixel, UnitPoint, UnitInch, UnitDocument,
    UnitMillimeter
} GpUnit;

typedef enum GpFillMode {
    FillModeWinding = 0, FillModeAlternate
} GpFillMode;

typedef enum GpPenAlignment {
    PenAlignmentCenter = 0, PenAlignmentInset
} GpPenAlignment;

typedef enum GpLineCap {
    LineCapFlat = 0, LineCapSquare, LineCapRound, LineCapTriangle,
    LineCapNoAnchor = 0x10, LineCapSquareAnchor, LineCapRoundAnchor,
    LineCapDiamondAnchor, LineCapArrowAnchor
} GpLineCap;

typedef enum GpLineJoin {
    LineJoinBevel = 0, LineJoinMiter, LineJoinRound, LineJoinMiterClipped
} GpLineJoin;

typedef enum GpCompositingQuality {
    CompositingQualityDefault = 0, CompositingQualityHighSpeed,
    CompositingQualityHighQuality, CompositingQualityGammaCorrected,
    CompositingQualityAssumeLinear
} GpCompositingQuality;

typedef enum GpSmoothingMode {
    SmoothingModeDefault = 0, SmoothingModeHighSpeed, SmoothingModeHighQuality,
    SmoothingModeNone, SmoothingModeAntiAlias, SmoothingModeAntiAlias8x4,
    SmoothingModeAntiAlias8x8
} GpSmoothingMode;

typedef enum GpPixelOffsetMode {
    PixelOffsetModeDefault = 0, PixelOffsetModeHighSpeed,
    PixelOffsetModeHighQuality, PixelOffsetModeNone, PixelOffsetModeHalf
} GpPixelOffsetMode;

typedef enum GpTextRenderingHint {
    TextRenderingHintSystemDefault = 0,
    TextRenderingHintSingleBitPerPixelGridFit,
    TextRenderingHintSingleBitPerPixel,
    TextRenderingHintAntiAliasGridFit,
    TextRenderingHintAntiAlias,
    TextRenderingHintClearTypeGridFit
} GpTextRenderingHint;

typedef enum GpStringTrimming {
    StringTrimmingNone = 0, StringTrimmingCharacter, StringTrimmingWord,
    StringTrimmingEllipsisCharacter, StringTrimmingEllipsisWord,
    StringTrimmingEllipsisPath
} GpStringTrimming;

typedef enum GpStringFormatFlags {
    StringFormatFlagsDirectionRightToLeft = 0x00000001,
    StringFormatFlagsDirectionVertical = 0x00000002,
    StringFormatFlagsNoFitBlackBox = 0x00000004,
    StringFormatFlagsDisplayFormatControl = 0x00000020,
    StringFormatFlagsNoFontFallback = 0x00000400,
    StringFormatFlagsMeasureTrailingSpaces = 0x00000800,
    StringFormatFlagsNoWrap = 0x00001000,
    StringFormatFlagsLineLimit = 0x00002000,
    StringFormatFlagsNoClip = 0x00004000
} GpStringFormatFlags;

typedef enum GpStringAlignment {
    StringAlignmentNear = 0, StringAlignmentCenter, StringAlignmentFar
} GpStringAlignment;

typedef enum GpHotkeyPrefix {
    HotkeyPrefixNone = 0, HotkeyPrefixShow, HotkeyPrefixHide
} GpHotkeyPrefix;

typedef enum GpFontStyle {
    FontStyleRegular = 0, FontStyleBold = 1, FontStyleItalic = 2,
    FontStyleBoldItalic = 3, FontStyleUnderline = 4, FontStyleStrikeout = 8
} GpFontStyle;

typedef enum GpCombineMode {
    CombineModeReplace = 0, CombineModeIntersect, CombineModeUnion,
    CombineModeXor, CombineModeExclude, CombineModeComplement
} GpCombineMode;

/* The Windows SDK spells these without the Gp prefix. */
typedef GpTextRenderingHint  TextRenderingHint;
typedef GpPixelOffsetMode    PixelOffsetMode;
typedef GpSmoothingMode      SmoothingMode;
typedef GpCompositingQuality CompositingQuality;
typedef GpStringAlignment    StringAlignment;
typedef GpStringTrimming     StringTrimming;
typedef GpUnit               Unit;
typedef GpFillMode           FillMode;
typedef GpLineCap            LineCap;
typedef GpLineJoin           LineJoin;
typedef GpFontStyle          FontStyle;

/* PixelFormat values from gdipluspixelformats.h. */
#define PixelFormat32bppARGB  (10 | (32 << 8) | 0x00040000 | 0x00020000 | 0x00200000)
#define PixelFormat32bppPARGB (11 | (32 << 8) | 0x00040000 | 0x00080000 | 0x00020000)
#define PixelFormat32bppBGR   (9  | (32 << 8) | 0x00020000)

/* Startup ---------------------------------------------------------------- */

typedef struct GdiplusStartupInput {
    UINT32 GdiplusVersion;
    void*  DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GdiplusStartupInput;

typedef ULONG_PTR GdiplusToken;

GpStatus WINGDIPAPI GdiplusStartup(ULONG_PTR* token,
                                   const GdiplusStartupInput* input,
                                   void* output);
void WINGDIPAPI GdiplusShutdown(ULONG_PTR token);

/* Graphics --------------------------------------------------------------- */

GpStatus WINGDIPAPI GdipCreateFromHDC(HDC hdc, GpGraphics** graphics);
GpStatus WINGDIPAPI GdipGetImageGraphicsContext(GpImage* image, GpGraphics** graphics);
GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics* graphics);
GpStatus WINGDIPAPI GdipGraphicsClear(GpGraphics* graphics, ARGB color);

GpStatus WINGDIPAPI GdipSetSmoothingMode(GpGraphics* g, GpSmoothingMode mode);
GpStatus WINGDIPAPI GdipSetPixelOffsetMode(GpGraphics* g, GpPixelOffsetMode mode);
GpStatus WINGDIPAPI GdipGetPixelOffsetMode(GpGraphics* g, GpPixelOffsetMode* mode);
GpStatus WINGDIPAPI GdipSetTextRenderingHint(GpGraphics* g, GpTextRenderingHint hint);
GpStatus WINGDIPAPI GdipGetTextRenderingHint(GpGraphics* g, GpTextRenderingHint* hint);
GpStatus WINGDIPAPI GdipSetCompositingQuality(GpGraphics* g, GpCompositingQuality q);

GpStatus WINGDIPAPI GdipSetClipRectI(GpGraphics* g, INT x, INT y, INT w, INT h,
                                     GpCombineMode mode);
GpStatus WINGDIPAPI GdipResetClip(GpGraphics* g);

/* Bitmaps ---------------------------------------------------------------- */

GpStatus WINGDIPAPI GdipCreateBitmapFromScan0(INT width, INT height, INT stride,
                                              INT format, BYTE* scan0,
                                              GpBitmap** bitmap);
GpStatus WINGDIPAPI GdipDisposeImage(GpImage* image);

/* Brushes / pens --------------------------------------------------------- */

GpStatus WINGDIPAPI GdipCreateSolidFill(ARGB color, GpSolidFill** brush);
GpStatus WINGDIPAPI GdipSetSolidFillColor(GpSolidFill* brush, ARGB color);
GpStatus WINGDIPAPI GdipDeleteBrush(GpBrush* brush);

GpStatus WINGDIPAPI GdipCreatePen1(ARGB color, REAL width, GpUnit unit, GpPen** pen);
GpStatus WINGDIPAPI GdipSetPenColor(GpPen* pen, ARGB color);
GpStatus WINGDIPAPI GdipSetPenWidth(GpPen* pen, REAL width);
GpStatus WINGDIPAPI GdipSetPenStartCap(GpPen* pen, GpLineCap cap);
GpStatus WINGDIPAPI GdipSetPenEndCap(GpPen* pen, GpLineCap cap);
GpStatus WINGDIPAPI GdipSetPenLineJoin(GpPen* pen, GpLineJoin join);
GpStatus WINGDIPAPI GdipDeletePen(GpPen* pen);

/* Paths ------------------------------------------------------------------ */

GpStatus WINGDIPAPI GdipCreatePath(GpFillMode mode, GpPath** path);
GpStatus WINGDIPAPI GdipDeletePath(GpPath* path);
GpStatus WINGDIPAPI GdipResetPath(GpPath* path);
GpStatus WINGDIPAPI GdipStartPathFigure(GpPath* path);
GpStatus WINGDIPAPI GdipClosePathFigure(GpPath* path);
GpStatus WINGDIPAPI GdipAddPathLine(GpPath* path, REAL x1, REAL y1, REAL x2, REAL y2);
GpStatus WINGDIPAPI GdipAddPathArc(GpPath* path, REAL x, REAL y, REAL w, REAL h,
                                   REAL start, REAL sweep);
GpStatus WINGDIPAPI GdipAddPathRectangle(GpPath* path, REAL x, REAL y, REAL w, REAL h);

/* Drawing ---------------------------------------------------------------- */

GpStatus WINGDIPAPI GdipDrawLine(GpGraphics* g, GpPen* pen, REAL x1, REAL y1,
                                 REAL x2, REAL y2);
GpStatus WINGDIPAPI GdipDrawLineI(GpGraphics* g, GpPen* pen, INT x1, INT y1,
                                  INT x2, INT y2);
GpStatus WINGDIPAPI GdipDrawRectangle(GpGraphics* g, GpPen* pen, REAL x, REAL y,
                                      REAL w, REAL h);
GpStatus WINGDIPAPI GdipDrawEllipse(GpGraphics* g, GpPen* pen, REAL x, REAL y,
                                    REAL w, REAL h);
GpStatus WINGDIPAPI GdipDrawEllipseI(GpGraphics* g, GpPen* pen, INT x, INT y,
                                     INT w, INT h);
GpStatus WINGDIPAPI GdipDrawArc(GpGraphics* g, GpPen* pen, REAL x, REAL y,
                                REAL w, REAL h, REAL start, REAL sweep);
GpStatus WINGDIPAPI GdipDrawPath(GpGraphics* g, GpPen* pen, GpPath* path);

GpStatus WINGDIPAPI GdipFillRectangleI(GpGraphics* g, GpBrush* brush, INT x, INT y,
                                       INT w, INT h);
GpStatus WINGDIPAPI GdipFillEllipseI(GpGraphics* g, GpBrush* brush, INT x, INT y,
                                     INT w, INT h);
GpStatus WINGDIPAPI GdipFillPie(GpGraphics* g, GpBrush* brush, REAL x, REAL y,
                                REAL w, REAL h, REAL start, REAL sweep);
GpStatus WINGDIPAPI GdipFillPolygon(GpGraphics* g, GpBrush* brush,
                                    GDIPCONST GpPointF* points, INT count,
                                    GpFillMode mode);
GpStatus WINGDIPAPI GdipFillPath(GpGraphics* g, GpBrush* brush, GpPath* path);

/* Text ------------------------------------------------------------------- */

GpStatus WINGDIPAPI GdipCreateFontFamilyFromName(GDIPCONST WCHAR* name,
                                                 GpFontCollection* collection,
                                                 GpFontFamily** family);
GpStatus WINGDIPAPI GdipDeleteFontFamily(GpFontFamily* family);
GpStatus WINGDIPAPI GdipCreateFont(GDIPCONST GpFontFamily* family, REAL size,
                                   INT style, GpUnit unit, GpFont** font);
GpStatus WINGDIPAPI GdipDeleteFont(GpFont* font);

GpStatus WINGDIPAPI GdipCreateStringFormat(INT flags, LANGID language,
                                           GpStringFormat** format);
GpStatus WINGDIPAPI GdipDeleteStringFormat(GpStringFormat* format);
GpStatus WINGDIPAPI GdipSetStringFormatFlags(GpStringFormat* format, INT flags);
GpStatus WINGDIPAPI GdipSetStringFormatAlign(GpStringFormat* format, GpStringAlignment align);
GpStatus WINGDIPAPI GdipSetStringFormatLineAlign(GpStringFormat* format, GpStringAlignment align);
GpStatus WINGDIPAPI GdipSetStringFormatTrimming(GpStringFormat* format, GpStringTrimming trim);
GpStatus WINGDIPAPI GdipSetStringFormatHotkeyPrefix(GpStringFormat* format, INT prefix);

GpStatus WINGDIPAPI GdipDrawString(GpGraphics* g, GDIPCONST WCHAR* text, INT length,
                                   GDIPCONST GpFont* font, GDIPCONST RectF* layout,
                                   GDIPCONST GpStringFormat* format,
                                   GDIPCONST GpBrush* brush);
GpStatus WINGDIPAPI GdipMeasureString(GpGraphics* g, GDIPCONST WCHAR* text,
                                      INT length, GDIPCONST GpFont* font,
                                      GDIPCONST RectF* layout,
                                      GDIPCONST GpStringFormat* format,
                                      RectF* bounds, INT* codepoints,
                                      INT* lines);

#ifdef __cplusplus
}
#endif

#endif /* GDIPLUS_C_H */
