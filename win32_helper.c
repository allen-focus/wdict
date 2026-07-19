#include "win32_helper.h"
#include "utils.h"

#include <imm.h>
#include <math.h>
#include <windows.h>

#pragma comment(lib, "user32")
#pragma comment(lib, "kernel32")
#pragma comment(lib, "imm32")
#pragma comment(lib, "advapi32")

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

b32 win32_get_accent_border_color(Color* out_color)
{
    HKEY key = NULL;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM", 0, KEY_READ, &key);
    if (status != ERROR_SUCCESS)
        return False;

    DWORD colorization_color = 0;
    DWORD size = sizeof(DWORD);
    status = RegGetValueW(key, NULL, L"ColorizationColor", RRF_RT_REG_DWORD, NULL, &colorization_color, &size);
    if (status != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return False;
    }

    DWORD balance = 0;
    size = sizeof(DWORD);
    status = RegGetValueW(key, NULL, L"ColorizationColorBalance", RRF_RT_REG_DWORD, NULL, &balance, &size);
    if (status != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return False;
    }

    DWORD prevalence = 0;
    size = sizeof(DWORD);
    status = RegGetValueW(key, NULL, L"ColorPrevalence", RRF_RT_REG_DWORD, NULL, &prevalence, &size);

    RegCloseKey(key);

    if (!prevalence)
        return False; /* accent colors not enabled for title bars and borders */

    /**
     * Blend the accent color with fixed color 0xd9d9d9 using the balance ratio.
     * This matches the exact border color DWM would use. See:
     * https://handmade.network/forums/articles/t/9073-custom_window_title_bar_and_almost_correctly_drawing_windows_10_borders
     */
    f32 factor_a = (f32)balance / 100.0f;
    f32 factor_b = (f32)(100 - balance) / 100.0f;

    f32 a_r = (f32)((colorization_color >> 16) & 0xff);
    f32 a_g = (f32)((colorization_color >> 8) & 0xff);
    f32 a_b = (f32)(colorization_color & 0xff);

    out_color->r = (u8)roundf(a_r * factor_a + 217.0f * factor_b);
    out_color->g = (u8)roundf(a_g * factor_a + 217.0f * factor_b);
    out_color->b = (u8)roundf(a_b * factor_a + 217.0f * factor_b);
    out_color->a = 255;

    return True;
}
