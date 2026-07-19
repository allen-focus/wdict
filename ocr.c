#pragma comment(lib, "gdi32.lib")

#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0x130000
#define COBJMACROS

#include "ocr.h"
#include <roapi.h>
#include <windows.graphics.imaging.h>
#include <windows.media.ocr.h>
#include <windows.storage.streams.h>
#include <winstring.h>

#pragma comment(lib, "runtimeobject.lib")

const IID IID___x_ABI_CWindows_CStorage_CStreams_CIBufferFactory = {
    0x71af914d, 0xc10f, 0x484b, { 0xbc, 0x50, 0x14, 0xbc, 0x62, 0x3b, 0x3a, 0x27 }
};
const IID IID___x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics = {
    0xdf0385db, 0x672f, 0x4a9d, { 0x80, 0x6e, 0xc2, 0x44, 0x2f, 0x34, 0x3e, 0x86 }
};
const IID IID___x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics = {
    0x5bffa85a, 0x3384, 0x3540, { 0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8 }
};
const IID IID___FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult = {
    0x989c1371, 0x444a, 0x5e7e, { 0xb1, 0x97, 0x9e, 0xaa, 0xf9, 0xd2, 0x82, 0x9a }
};

static const GUID IID_IBufferByteAccess = {
    0x905a0fef, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda }
};

typedef struct IBufferByteAccessVtbl
{
    BEGIN_INTERFACE
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(void* This, REFIID riid, void** ppv);
    ULONG(STDMETHODCALLTYPE* AddRef)(void* This);
    ULONG(STDMETHODCALLTYPE* Release)(void* This);
    HRESULT(STDMETHODCALLTYPE* Buffer)(void* This, BYTE** value);
    END_INTERFACE
} IBufferByteAccessVtbl;

typedef struct IBufferByteAccess
{
    IBufferByteAccessVtbl* lpVtbl;
} IBufferByteAccess;

typedef struct OcrCompletedHandler
{
    __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResultVtbl* lpVtbl;
    LONG refcount;

    __x_ABI_CWindows_CMedia_COcr_CIOcrEngine* engine;
    __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmap* swBitmap;

    HWND notify_window;
    i32 capture_x, capture_y;
    i32 cursor_x, cursor_y;
} OcrCompletedHandler;

static ULONG STDMETHODCALLTYPE
OcrHandler_AddRef(__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult* This)
{
    OcrCompletedHandler* h = (OcrCompletedHandler*)This;
    return InterlockedIncrement(&h->refcount);
}

static HRESULT STDMETHODCALLTYPE OcrHandler_QueryInterface(
    __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult* This, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID___FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult))
    {
        *ppv = This;
        OcrHandler_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
OcrHandler_Release(__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult* This)
{
    OcrCompletedHandler* h = (OcrCompletedHandler*)This;
    LONG rc = InterlockedDecrement(&h->refcount);
    if (rc == 0)
    {
        if (h->engine)
            __x_ABI_CWindows_CMedia_COcr_CIOcrEngine_Release(h->engine);
        if (h->swBitmap)
            __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmap_Release(h->swBitmap);
        free(h);
    }
    return (ULONG)rc;
}

static HRESULT STDMETHODCALLTYPE
OcrHandler_Invoke(__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult* This,
                  __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult* asyncInfo, AsyncStatus asyncStatus)
{
    OcrCompletedHandler* h = (OcrCompletedHandler*)This;

    if (asyncStatus == Completed)
    {
        __x_ABI_CWindows_CMedia_COcr_CIOcrResult* result = NULL;
        HRESULT hrGet = __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult_GetResults(asyncInfo, &result);

        if (SUCCEEDED(hrGet) && result)
        {
            HSTRING hsText = NULL;
            OcrWordBbox* pBbox = NULL;

            __FIVectorView_1_Windows__CMedia__COcr__COcrLine* lines = NULL;
            __x_ABI_CWindows_CMedia_COcr_CIOcrResult_get_Lines(result, &lines);
            if (lines)
            {
                UINT32 lineCount = 0;
                __FIVectorView_1_Windows__CMedia__COcr__COcrLine_get_Size(lines, &lineCount);

                for (UINT32 li = 0; li < lineCount && !hsText; li++)
                {
                    __x_ABI_CWindows_CMedia_COcr_CIOcrLine* line = NULL;
                    __FIVectorView_1_Windows__CMedia__COcr__COcrLine_GetAt(lines, li, &line);
                    if (!line)
                        continue;

                    __FIVectorView_1_Windows__CMedia__COcr__COcrWord* words = NULL;
                    __x_ABI_CWindows_CMedia_COcr_CIOcrLine_get_Words(line, &words);
                    if (words)
                    {
                        UINT32 wordCount = 0;
                        __FIVectorView_1_Windows__CMedia__COcr__COcrWord_get_Size(words, &wordCount);

                        for (UINT32 wi = 0; wi < wordCount && !hsText; wi++)
                        {
                            __x_ABI_CWindows_CMedia_COcr_CIOcrWord* word = NULL;
                            __FIVectorView_1_Windows__CMedia__COcr__COcrWord_GetAt(words, wi, &word);
                            if (!word)
                                continue;

                            struct __x_ABI_CWindows_CFoundation_CRect bbox = { 0 };
                            __x_ABI_CWindows_CMedia_COcr_CIOcrWord_get_BoundingRect(word, &bbox);

                            i32 wx = h->capture_x + (i32)bbox.X;
                            i32 wy = h->capture_y + (i32)bbox.Y;
                            i32 ww = (i32)bbox.Width;
                            i32 wh = (i32)bbox.Height;

                            if (h->cursor_x >= wx && h->cursor_x < wx + ww && h->cursor_y >= wy &&
                                h->cursor_y < wy + wh)
                            {
                                __x_ABI_CWindows_CMedia_COcr_CIOcrWord_get_Text(word, &hsText);
                                pBbox = (OcrWordBbox*)malloc(sizeof(OcrWordBbox));
                                if (pBbox)
                                {
                                    pBbox->screen_x = wx;
                                    pBbox->screen_y = wy;
                                    pBbox->screen_w = (u32)ww;
                                    pBbox->screen_h = (u32)wh;
                                }
                            }
                            __x_ABI_CWindows_CMedia_COcr_CIOcrWord_Release(word);
                        }
                        __FIVectorView_1_Windows__CMedia__COcr__COcrWord_Release(words);
                    }
                    __x_ABI_CWindows_CMedia_COcr_CIOcrLine_Release(line);
                }
                __FIVectorView_1_Windows__CMedia__COcr__COcrLine_Release(lines);
            }

            if (!hsText)
                __x_ABI_CWindows_CMedia_COcr_CIOcrResult_get_Text(result, &hsText);

            if (hsText)
            {
                UINT32 wlen;
                LPCWSTR raw = WindowsGetStringRawBuffer(hsText, &wlen);

                if (raw && wlen > 0)
                {
                    isize utf8_len = WideCharToMultiByte(CP_UTF8, 0, raw, (i32)wlen, NULL, 0, NULL, NULL);

                    if (utf8_len > 0)
                    {
                        u8* utf8_buf = (u8*)malloc((usize)utf8_len + 1);
                        if (utf8_buf)
                        {
                            WideCharToMultiByte(CP_UTF8, 0, raw, (i32)wlen, (char*)utf8_buf, (i32)utf8_len, NULL, NULL);
                            utf8_buf[utf8_len] = 0;

                            if (IsWindow(h->notify_window))
                                PostMessageW(h->notify_window, WM_APP_OCR_RESULT, (WPARAM)utf8_buf, (LPARAM)pBbox);
                            else
                            {
                                free(utf8_buf);
                                free(pBbox);
                            }
                        }
                        else
                        {
                            free(pBbox);
                        }
                    }
                }
                WindowsDeleteString(hsText);
            }
            __x_ABI_CWindows_CMedia_COcr_CIOcrResult_Release(result);
        }
    }

    __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult_Release(This);

    return S_OK;
}

static __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResultVtbl g_ocrHandlerVtbl = {
    OcrHandler_QueryInterface,
    OcrHandler_AddRef,
    OcrHandler_Release,
    OcrHandler_Invoke,
};

typedef struct
{
    DWORD Flags, Length, Padding1, Padding2;
    LPCWSTR Ptr;
} StaticHSTRING;

#define STATIC_HSTRING(Str)                                                                                            \
    (HSTRING) & (StaticHSTRING)                                                                                        \
    {                                                                                                                  \
        1, (DWORD)(sizeof(Str) / sizeof(*(Str)) - 1), 0, 0, Str                                                        \
    }

static b32 ocr_capture_screen(i32 x, i32 y, u32 w, u32 h, BITMAPINFO* out_bmi, BYTE** out_pixels, HBITMAP* out_hDib,
                              i32* out_cap_x, i32* out_cap_y)
{
    i32 vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    i32 vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    i32 vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    i32 vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    i32 cap_x = x > vx ? x : vx;
    i32 cap_y = y > vy ? y : vy;
    i32 cap_xmax = (x + (i32)w < vx + vw) ? x + (i32)w : vx + vw;
    i32 cap_ymax = (y + (i32)h < vy + vh) ? y + (i32)h : vy + vh;
    if (cap_x >= cap_xmax || cap_y >= cap_ymax)
        return False;

    u32 cap_w = (u32)(cap_xmax - cap_x);
    u32 cap_h = (u32)(cap_ymax - cap_y);

    *out_cap_x = cap_x;
    *out_cap_y = cap_y;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen)
        return False;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)cap_w;
    bmi.bmiHeader.biHeight = -(LONG)cap_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    BYTE* pixels = NULL;
    HBITMAP hDib = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (!hDib)
    {
        ReleaseDC(NULL, hdcScreen);
        return False;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hDib);
    BitBlt(hdcMem, 0, 0, (i32)cap_w, (i32)cap_h, hdcScreen, cap_x, cap_y, SRCCOPY);
    GdiFlush();

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    *out_bmi = bmi;
    *out_pixels = pixels;
    *out_hDib = hDib;
    return True;
}

static b32 s_ocr_initialized = False;

b32 ocr_init(void)
{
    if (s_ocr_initialized)
        return True;
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr))
        return False;
    s_ocr_initialized = True;
    return True;
}

void ocr_deinit(void)
{
    if (s_ocr_initialized)
    {
        RoUninitialize();
        s_ocr_initialized = False;
    }
}

void ocr_free(void* text)
{
    free(text);
}

void ocr_recognize_region_async(i32 x, i32 y, u32 w, u32 h, i32 cursor_x, i32 cursor_y, HWND notify_window)
{
    BITMAPINFO bmi;
    BYTE* pixels = NULL;
    HBITMAP hDib = NULL;
    i32 capture_x = 0, capture_y = 0;

    if (!ocr_capture_screen(x, y, w, h, &bmi, &pixels, &hDib, &capture_x, &capture_y))
        return;

    u32 capW = (u32)bmi.bmiHeader.biWidth;
    u32 capH = (u32)(-bmi.bmiHeader.biHeight);
    UINT32 bufSize = capW * capH * 4;

    __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult* asyncOp = NULL;
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer* buffer = NULL;
    IBufferByteAccess* bufAccess = NULL;

    __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmap* swBitmap = NULL;
    __x_ABI_CWindows_CMedia_COcr_CIOcrEngine* engine = NULL;

    HSTRING hsBuffer = STATIC_HSTRING(RuntimeClass_Windows_Storage_Streams_Buffer);
    __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory* bufFactory = NULL;
    HRESULT hr =
        RoGetActivationFactory(hsBuffer, &IID___x_ABI_CWindows_CStorage_CStreams_CIBufferFactory, (void**)&bufFactory);
    if (FAILED(hr))
        goto cleanup_gdi;

    hr = __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory_Create(bufFactory, bufSize, &buffer);
    if (FAILED(hr) || !buffer)
        goto cleanup;

    hr = __x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void**)&bufAccess);
    if (FAILED(hr) || !bufAccess)
        goto cleanup;

    BYTE* bufData = NULL;
    hr = bufAccess->lpVtbl->Buffer(bufAccess, &bufData);
    if (FAILED(hr) || !bufData)
        goto cleanup;

    memcpy(bufData, pixels, bufSize);
    __x_ABI_CWindows_CStorage_CStreams_CIBuffer_put_Length(buffer, bufSize);

    DeleteObject(hDib);
    hDib = NULL;

    HSTRING hsSwBmp = STATIC_HSTRING(RuntimeClass_Windows_Graphics_Imaging_SoftwareBitmap);
    __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics* swBmpStatics = NULL;
    hr = RoGetActivationFactory(hsSwBmp, &IID___x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics,
                                (void**)&swBmpStatics);
    if (FAILED(hr))
        goto cleanup;

    hr = __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics_CreateCopyWithAlphaFromBuffer(
        swBmpStatics, buffer, BitmapPixelFormat_Bgra8, (i32)capW, (i32)capH, BitmapAlphaMode_Ignore, &swBitmap);
    if (FAILED(hr) || !swBitmap)
        goto cleanup;

    if (bufAccess)
    {
        bufAccess->lpVtbl->Release(bufAccess);
        bufAccess = NULL;
    }
    if (buffer)
    {
        __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
        buffer = NULL;
    }
    if (bufFactory)
    {
        __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory_Release(bufFactory);
        bufFactory = NULL;
    }

    HSTRING hsOcr = STATIC_HSTRING(RuntimeClass_Windows_Media_Ocr_OcrEngine);
    __x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics* ocrStatics = NULL;
    hr = RoGetActivationFactory(hsOcr, &IID___x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics, (void**)&ocrStatics);
    if (FAILED(hr))
        goto cleanup;

    hr = __x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics_TryCreateFromUserProfileLanguages(ocrStatics, &engine);
    if (FAILED(hr) || !engine)
    {
        Assert(0);
        goto cleanup;
    }

    hr = __x_ABI_CWindows_CMedia_COcr_CIOcrEngine_RecognizeAsync(engine, swBitmap, &asyncOp);
    if (FAILED(hr) || !asyncOp)
        goto cleanup;

    {
        OcrCompletedHandler* handler = (OcrCompletedHandler*)calloc(1, sizeof(OcrCompletedHandler));
        if (!handler)
            goto cleanup;

        handler->lpVtbl = &g_ocrHandlerVtbl;
        handler->refcount = 2;
        handler->engine = engine;
        handler->swBitmap = swBitmap;
        handler->notify_window = notify_window;
        handler->capture_x = capture_x;
        handler->capture_y = capture_y;
        handler->cursor_x = cursor_x;
        handler->cursor_y = cursor_y;

        engine = NULL;
        swBitmap = NULL;

        hr = __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult_put_Completed(
            asyncOp, (__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult*)handler);

        if (!SUCCEEDED(hr))
        {
            __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult_Release(
                (__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult*)handler);
            __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult_Release(
                (__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult*)handler);
        }
        else
        {
            __FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult_Release(
                (__FIAsyncOperationCompletedHandler_1_Windows__CMedia__COcr__COcrResult*)handler);
        }
    }

    if (asyncOp)
    {
        __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult_Release(asyncOp);
        asyncOp = NULL;
    }
    if (ocrStatics)
    {
        __x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics_Release(ocrStatics);
        ocrStatics = NULL;
    }
    if (swBmpStatics)
    {
        __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics_Release(swBmpStatics);
        swBmpStatics = NULL;
    }

cleanup:
    if (engine)
        __x_ABI_CWindows_CMedia_COcr_CIOcrEngine_Release(engine);
    if (swBitmap)
        __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmap_Release(swBitmap);
    if (asyncOp)
        __FIAsyncOperation_1_Windows__CMedia__COcr__COcrResult_Release(asyncOp);
    if (ocrStatics)
        __x_ABI_CWindows_CMedia_COcr_CIOcrEngineStatics_Release(ocrStatics);
    if (swBmpStatics)
        __x_ABI_CWindows_CGraphics_CImaging_CISoftwareBitmapStatics_Release(swBmpStatics);
    if (bufAccess)
        bufAccess->lpVtbl->Release(bufAccess);
    if (buffer)
        __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release(buffer);
    if (bufFactory)
        __x_ABI_CWindows_CStorage_CStreams_CIBufferFactory_Release(bufFactory);

cleanup_gdi:
    if (hDib)
        DeleteObject(hDib);
}
