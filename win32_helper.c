#include "win32_helper.h"
#include "utils.h"

#include <imm.h>
#include <windows.h>

#pragma comment(lib, "user32")
#pragma comment(lib, "kernel32")
#pragma comment(lib, "imm32")

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

//
// IME
//

static String ime_read_str(const HWND window, Arena* arena, DWORD flag)
{
    HIMC himc = ImmGetContext(window);
    if (!himc)
        return (String){ 0 };

    LONG byte_len = ImmGetCompositionStringW(himc, flag, NULL, 0);
    if (byte_len <= 0)
    {
        ImmReleaseContext(window, himc);
        return (String){ 0 };
    }

    isize wlen = (isize)byte_len / (isize)sizeof(WCHAR);
    WCHAR* wstr = arena_push(arena, (isize)byte_len + sizeof(WCHAR), (isize)sizeof(WCHAR), 1);
    ImmGetCompositionStringW(himc, flag, wstr, byte_len);
    ImmReleaseContext(window, himc);

    /* Count UTF-8 bytes */
    isize utf8_len = 0;
    const u16* p = (const u16*)wstr;
    isize remaining = wlen;
    while (remaining > 0 && *p)
    {
        UnicodeDecode dec = utf16_decode(p);
        u8 buf[4];
        utf8_len += utf8_encode(buf, dec.codepoint);
        isize consumed = (const u16*)dec.next_p - p;
        p = (const u16*)dec.next_p;
        remaining -= consumed;
    }

    /* Convert to UTF-8 */
    u8* dst = arena_push(arena, utf8_len, 1, 1);
    u8* dst_cursor = dst;
    p = (const u16*)wstr;
    remaining = wlen;
    while (remaining > 0 && *p)
    {
        UnicodeDecode dec = utf16_decode(p);
        u8 buf[4];
        isize written = utf8_encode(buf, dec.codepoint);
        for (isize j = 0; j < written; j++)
            *dst_cursor++ = buf[j];
        isize consumed = (const u16*)dec.next_p - p;
        p = (const u16*)dec.next_p;
        remaining -= consumed;
    }

    return (String){ dst, utf8_len };
}

b32 win32_ime_is_composing(const HWND window)
{
    HIMC himc = ImmGetContext(window);
    if (!himc)
        return False;
    LONG len = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
    ImmReleaseContext(window, himc);
    return len > 0;
}

String win32_ime_get_composition(const HWND window, Arena* arena)
{
    return ime_read_str(window, arena, GCS_COMPSTR);
}

String win32_ime_get_result(const HWND window, Arena* arena)
{
    return ime_read_str(window, arena, GCS_RESULTSTR);
}

void win32_ime_update_candidate(const HWND window, const LONG client_x, const LONG client_y, Position* out_screen_pos)
{
    HIMC himc = ImmGetContext(window);
    if (!himc)
        return;

    POINT pt = { client_x, client_y };

    CANDIDATEFORM cf = { 0 };
    cf.dwIndex = 0;
    cf.dwStyle = CFS_CANDIDATEPOS;
    cf.ptCurrentPos = pt;
    ImmSetCandidateWindow(himc, &cf);

    COMPOSITIONFORM compf = { 0 };
    compf.dwStyle = CFS_POINT;
    compf.ptCurrentPos = pt;
    ImmSetCompositionWindow(himc, &compf);

    ImmReleaseContext(window, himc);

    if (out_screen_pos)
    {
        ClientToScreen(window, &pt);
        out_screen_pos->x = (f32)pt.x;
        out_screen_pos->y = (f32)pt.y;
    }
}

SystemTheme win32_get_system_theme(void)
{
    DWORD value = 1;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) == ERROR_SUCCESS)
    {
        DWORD size = sizeof(value);
        RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
        RegCloseKey(key);
    }
    return value != 0 ? SYSTEM_THEME_LIGHT : SYSTEM_THEME_DARK;
}
