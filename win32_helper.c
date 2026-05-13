#include "utils.h"

void win32_clipboard_copy(const HWND window, const String text)
{
    if (!window || text.len == 0)
        return;

    isize utf16_len = 0;
    isize pos = 0;
    while (pos < text.len)
    {
        UnicodeDecode dec = utf8_decode(text.data + pos);
        u16 buf[2];
        utf16_len += utf16_encode(buf, dec.codepoint);
        pos = dec.next_p - text.data;
    }

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (utf16_len + 1) * sizeof(u16));
    if (!hGlobal)
        return;
    u16* dst = (u16*)GlobalLock(hGlobal);
    pos = 0;
    while (pos < text.len)
    {
        UnicodeDecode dec = utf8_decode(text.data + pos);
        u16 buf[2];
        isize written = utf16_encode(buf, dec.codepoint);
        for (isize j = 0; j < written; j++)
            *dst++ = buf[j];
        pos = dec.next_p - text.data;
    }
    *dst = 0;
    GlobalUnlock(hGlobal);

    if (OpenClipboard(window))
    {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hGlobal);
        CloseClipboard();
    }
    else
    {
        GlobalFree(hGlobal);
    }
}

String win32_clipboard_paste(const HWND window, Arena* arena)
{
    String result = { 0 };
    if (!window)
        return result;

    if (!OpenClipboard(window))
        return result;
    HGLOBAL hGlobal = GetClipboardData(CF_UNICODETEXT);
    if (!hGlobal)
    {
        CloseClipboard();
        return result;
    }

    u16* src = (u16*)GlobalLock(hGlobal);
    if (!src)
    {
        CloseClipboard();
        return result;
    }

    /* Count UTF-8 bytes needed */
    isize utf8_len = 0;
    u16* p = src;
    while (*p)
    {
        UnicodeDecode dec = utf16_decode(p);
        u8 buf[4];
        utf8_len += utf8_encode(buf, dec.codepoint);
        p = (u16*)dec.next_p;
    }

    /* Allocate and convert */
    result.data = arena_push(arena, utf8_len, 1, 1);
    result.len = utf8_len;
    u8* dst8 = result.data;
    p = src;
    while (*p)
    {
        UnicodeDecode dec = utf16_decode(p);
        u8 buf[4];
        isize written = utf8_encode(buf, dec.codepoint);
        for (isize j = 0; j < written; j++)
            *dst8++ = buf[j];
        p = (u16*)dec.next_p;
    }

    GlobalUnlock(hGlobal);
    CloseClipboard();
    return result;
}
