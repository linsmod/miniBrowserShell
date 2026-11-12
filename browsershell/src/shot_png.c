/* PNG writer built on Windows Imaging Component (no libpng in this project). */

#include "shot_png.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdio.h>

static int writeWithFactory(IWICImagingFactory* factory, const char* path,
                            const unsigned char* bits, int width, int height,
                            int stride, int bottomUp)
{
    wchar_t widePath[MAX_PATH];
    IWICBitmapEncoder* encoder = NULL;
    IWICStream* stream = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IWICBitmap* bitmap = NULL;
    int ok = 0;

    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, widePath, MAX_PATH))
        return 0;

    if (FAILED(factory->lpVtbl->CreateStream(factory, &stream)))
        goto done;
    if (FAILED(stream->lpVtbl->InitializeFromFilename(stream, widePath, GENERIC_WRITE)))
        goto done;
    if (FAILED(factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder)))
        goto done;
    if (FAILED(encoder->lpVtbl->Initialize(encoder, (IStream*)stream, WICBitmapEncoderNoCache)))
        goto done;
    if (FAILED(encoder->lpVtbl->CreateNewFrame(encoder, &frame, NULL)))
        goto done;
    if (FAILED(frame->lpVtbl->Initialize(frame, NULL)))
        goto done;

    if (FAILED(factory->lpVtbl->CreateBitmapFromMemory(
            factory, (UINT)width, (UINT)height, &GUID_WICPixelFormat32bppBGR,
            (UINT)stride, (UINT)(stride * height), (BYTE*)bits, &bitmap)))
        goto done;

    {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
        if (FAILED(frame->lpVtbl->SetPixelFormat(frame, &fmt)))
            goto done;
    }

    if (FAILED(frame->lpVtbl->WriteSource(frame, (IWICBitmapSource*)bitmap, NULL)))
        goto done;
    if (FAILED(frame->lpVtbl->Commit(frame)))
        goto done;
    if (FAILED(encoder->lpVtbl->Commit(encoder)))
        goto done;

    ok = 1;

done:
    if (bitmap) bitmap->lpVtbl->Release(bitmap);
    if (frame) frame->lpVtbl->Release(frame);
    if (encoder) encoder->lpVtbl->Release(encoder);
    if (stream) stream->lpVtbl->Release(stream);
    (void)bottomUp;
    return ok;
}

int shotWritePng(const char* path, const unsigned char* bits, int width,
                 int height, int stride)
{
    IWICImagingFactory* factory = NULL;
    int ok;

    if (!path || !bits || width <= 0 || height <= 0 || stride <= 0)
        return 0;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void**)&factory)))
        return 0;

    ok = writeWithFactory(factory, path, bits, width, height, stride, 0);
    factory->lpVtbl->Release(factory);
    return ok;
}
