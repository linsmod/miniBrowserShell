/*
 * ui_favicon.c - decode the icon bytes the engine delivered into a GDI+ image.
 *
 * Windows has the decoders already: WIC reads PNG/JPEG/BMP/GIF natively, and
 * classic .ico files are a container of either PNGs or bottom-up DIBs. WIC's
 * ICO support differs between builds, so an .ico is parsed here first: the
 * largest entry wins (favicons ship 16/32/48 px variants), a PNG entry goes back
 * through WIC, a DIB entry is converted to premultiplied BGRA by hand.
 *
 * Everything is scaled by the caller, not here: the decoded image keeps its own
 * size so a 48 px icon still looks right on a 200% monitor.
 */

#include "ui_favicon.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>

/* A favicon bigger than this is not an icon; refuse instead of allocating
 * megabytes for a page that points its <link rel=icon> at a photo. */
#define FAVICON_MAX_SIDE 256

struct UiFavicon {
    int width;
    int height;
    unsigned char* pixels;   /* premultiplied BGRA, width*height*4 */
    GpImage* image;          /* built from pixels once, owned by this struct */
};

/* ------------------------------------------------------------------------- */
/* WIC                                                                       */
/* ------------------------------------------------------------------------- */

static IWICImagingFactory* factoryGet(void)
{
    static IWICImagingFactory* factory;
    static int tried;

    if (tried)
        return factory;

    tried = 1;

    /* Callers are the UI thread (tab painting) and --ui-shot. RPC_E_CHANGED_MODE
     * means somebody already initialized COM with another model: WIC still
     * works, it just means we did not initialize it. */
    {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            return NULL;
    }

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void**)&factory)))
        factory = NULL;

    return factory;
}

/* Any format WIC knows, converted to premultiplied BGRA. */
static int wicDecode(const unsigned char* data, int size, int* outWidth,
                     int* outHeight, unsigned char** outPixels)
{
    IWICImagingFactory* factory = factoryGet();
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    unsigned char* pixels = NULL;
    UINT width = 0, height = 0;
    UINT stride, bytes;
    int ok = 0;

    if (!factory || !data || size <= 0)
        return 0;

    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream)))
        goto done;
    if (FAILED(stream->lpVtbl->InitializeFromMemory(stream, (BYTE*)data, (DWORD)size)))
        goto done;
    if (FAILED(factory->lpVtbl->CreateDecoderFromStream(
            factory, (IStream*)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder)))
        goto done;
    if (FAILED(decoder->lpVtbl->GetFrame(decoder, 0, &frame)))
        goto done;
    if (FAILED(factory->lpVtbl->CreateFormatConverter(factory, &converter)))
        goto done;
    if (FAILED(converter->lpVtbl->Initialize(
            converter, (IWICBitmapSource*)frame, &GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
        goto done;
    if (FAILED(converter->lpVtbl->GetSize(converter, &width, &height)))
        goto done;
    if (!width || !height || width > FAVICON_MAX_SIDE || height > FAVICON_MAX_SIDE)
        goto done;

    stride = width * 4;
    bytes = stride * height;
    pixels = (unsigned char*)malloc(bytes);
    if (!pixels)
        goto done;
    if (FAILED(converter->lpVtbl->CopyPixels(converter, NULL, stride, bytes, pixels))) {
        free(pixels);
        pixels = NULL;
        goto done;
    }

    *outWidth = (int)width;
    *outHeight = (int)height;
    *outPixels = pixels;
    ok = 1;

done:
    if (converter) converter->lpVtbl->Release(converter);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* ICO                                                                       */
/* ------------------------------------------------------------------------- */

static unsigned rd16(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned rd32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

/* One ICONDIRENTRY's payload as premultiplied BGRA. `p` starts at the
 * BITMAPINFOHEADER; the DIB height covers the XOR bitmap and the AND mask. */
static int icoDib(const unsigned char* p, int len, int* outWidth, int* outHeight,
                  unsigned char** outPixels)
{
    unsigned headerSize, width, height, bpp, compression, colors;
    unsigned entries, x, y;
    size_t paletteBytes, pixelOffset, maskOffset, stride, maskStride;
    unsigned char* pixels;
    int anyAlpha = 0;

    if (len < 40)
        return 0;

    headerSize = rd32(p);
    width = rd32(p + 4);
    height = rd32(p + 8) / 2;
    bpp = rd16(p + 14);
    compression = rd32(p + 16);
    colors = rd32(p + 32);

    if (headerSize < 40 || headerSize > (unsigned)len || compression != 0)
        return 0;
    if (!width || !height || width > FAVICON_MAX_SIDE || height > FAVICON_MAX_SIDE)
        return 0;
    if (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1)
        return 0;

    entries = 0;
    paletteBytes = 0;
    if (bpp <= 8) {
        entries = colors ? colors : (1u << bpp);
        paletteBytes = (size_t)entries * 4;
    }

    stride = ((width * bpp + 31u) / 32u) * 4u;
    maskStride = ((width + 31u) / 32u) * 4u;
    pixelOffset = headerSize + paletteBytes;
    if (pixelOffset + stride * height > (size_t)len)
        return 0;
    maskOffset = pixelOffset + stride * height;

    pixels = (unsigned char*)calloc((size_t)width * height, 4);
    if (!pixels)
        return 0;

    /* 32bpp icons sometimes carry a zero alpha channel for an icon that is
     * meant to be opaque; the AND mask then says which pixels are holes. */
    if (bpp == 32) {
        for (y = 0; y < height && !anyAlpha; ++y) {
            const unsigned char* row = p + pixelOffset + (size_t)(height - 1 - y) * stride;

            for (x = 0; x < width; ++x) {
                if (row[x * 4 + 3]) {
                    anyAlpha = 1;
                    break;
                }
            }
        }
    }

    for (y = 0; y < height; ++y) {
        const unsigned char* row = p + pixelOffset + (size_t)(height - 1 - y) * stride;
        unsigned char* dst = pixels + (size_t)y * width * 4;

        for (x = 0; x < width; ++x) {
            unsigned r = 0, g = 0, b = 0, a = 255;

            if (bpp == 32) {
                b = row[x * 4 + 0];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
            } else if (bpp == 24) {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else {
                unsigned index;
                const unsigned char* entry;

                if (bpp == 8)
                    index = row[x];
                else if (bpp == 4)
                    index = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 0x0Fu;
                else
                    index = (row[x / 8] >> (7 - (x & 7))) & 1u;

                if (index >= entries)
                    index = 0;
                entry = p + headerSize + (size_t)index * 4;
                b = entry[0];
                g = entry[1];
                r = entry[2];
            }

            /* Without a real alpha channel the AND mask marks the holes. */
            if (bpp != 32 || !anyAlpha) {
                size_t maskRow = maskOffset + (size_t)(height - 1 - y) * maskStride;

                if (maskOffset + maskStride * height <= (size_t)len &&
                    (p[maskRow + x / 8] & (0x80u >> (x & 7))))
                    a = 0;
            }

            dst[x * 4 + 0] = (unsigned char)(b * a / 255);
            dst[x * 4 + 1] = (unsigned char)(g * a / 255);
            dst[x * 4 + 2] = (unsigned char)(r * a / 255);
            dst[x * 4 + 3] = (unsigned char)a;
        }
    }

    *outWidth = (int)width;
    *outHeight = (int)height;
    *outPixels = pixels;
    return 1;
}

/* Classic Windows icon container: header + one entry per size. */
static int icoDecode(const unsigned char* data, int size, int* outWidth,
                     int* outHeight, unsigned char** outPixels)
{
    int count, i, best = -1, bestScore = -1;
    const unsigned char* payload;
    int payloadLen;

    if (size < 6 || rd16(data) != 0 || rd16(data + 2) != 1)
        return 0;

    count = (int)rd16(data + 4);
    if (count <= 0 || 6 + count * 16 > size)
        return 0;

    for (i = 0; i < count; ++i) {
        const unsigned char* e = data + 6 + i * 16;
        int w = e[0] ? e[0] : 256;
        int h = e[1] ? e[1] : 256;
        int bpp = (int)rd16(e + 6);
        int len = (int)rd32(e + 8);
        int off = (int)rd32(e + 12);
        int score;

        if (w <= 0 || h <= 0 || len <= 0 || off < 0 || off + len > size)
            continue;

        /* Largest area first, then the deepest entry of that size. */
        score = w * h * 100 + bpp * 100 + (int)rd32(e + 8) / 1024;

        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }

    if (best < 0)
        return 0;

    payload = data + (int)rd32(data + 6 + best * 16 + 12);
    payloadLen = (int)rd32(data + 6 + best * 16 + 8);

    if (payloadLen >= 8 && !memcmp(payload, "\x89PNG\r\n\x1a\n", 8))
        return wicDecode(payload, payloadLen, outWidth, outHeight, outPixels);

    return icoDib(payload, payloadLen, outWidth, outHeight, outPixels);
}

/* ------------------------------------------------------------------------- */
/* public                                                                    */
/* ------------------------------------------------------------------------- */

UiFavicon* uiFaviconDecode(const void* data, int size)
{
    UiFavicon* icon;
    int width = 0, height = 0;
    unsigned char* pixels = NULL;
    int decoded;

    if (!data || size <= 0)
        return NULL;

    decoded = icoDecode((const unsigned char*)data, size, &width, &height, &pixels);

    if (!decoded)
        decoded = wicDecode((const unsigned char*)data, size, &width, &height, &pixels);

    if (!decoded)
        return NULL;

    icon = (UiFavicon*)calloc(1, sizeof(*icon));
    if (!icon) {
        free(pixels);
        return NULL;
    }

    icon->width = width;
    icon->height = height;
    icon->pixels = pixels;

    /* GdipCreateBitmapFromScan0 wraps the buffer instead of copying it, so the
     * pixels have to outlive the image - they are freed together below. */
    if (GdipCreateBitmapFromScan0(width, height, width * 4, PixelFormat32bppPARGB,
                                  pixels, &icon->image) != Ok) {
        icon->image = NULL;
        free(pixels);
        free(icon);
        return NULL;
    }

    return icon;
}

void uiFaviconFree(UiFavicon* icon)
{
    if (!icon)
        return;

    if (icon->image)
        GdipDisposeImage(icon->image);
    free(icon->pixels);
    free(icon);
}

int uiFaviconWidth(const UiFavicon* icon)
{
    return icon ? icon->width : 0;
}

int uiFaviconHeight(const UiFavicon* icon)
{
    return icon ? icon->height : 0;
}

int uiFaviconPixel(const UiFavicon* icon, int x, int y, unsigned* rgb)
{
    const unsigned char* p;
    unsigned a, b, g, r;

    if (!icon || !rgb || x < 0 || y < 0 || x >= icon->width || y >= icon->height)
        return 0;

    p = icon->pixels + ((size_t)y * (size_t)icon->width + (size_t)x) * 4;
    b = p[0];
    g = p[1];
    r = p[2];
    a = p[3];

    /* Undo the premultiplication so the returned colour is what the icon file
     * said, not its blend onto black. */
    if (a && a != 255) {
        r = r * 255u / a;
        g = g * 255u / a;
        b = b * 255u / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
    }

    *rgb = (r << 16) | (g << 8) | b;
    return 1;
}

void uiFaviconPaint(UiPainter* painter, const UiFavicon* icon, UiRect rect)
{
    GpInterpolationMode saved;

    if (!painter || !painter->g || !icon || !icon->image)
        return;
    if (rect.w <= 0 || rect.h <= 0)
        return;

    if (GdipGetInterpolationMode(painter->g, &saved) != Ok)
        saved = InterpolationModeDefault;

    GdipSetInterpolationMode(painter->g, InterpolationModeHighQualityBicubic);
    GdipDrawImageRectI(painter->g, icon->image, rect.x, rect.y, rect.w, rect.h);
    GdipSetInterpolationMode(painter->g, saved);
}
