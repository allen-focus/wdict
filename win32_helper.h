#pragma once

#include "utils.h"
#include <windows.h>

typedef enum
{
    SYSTEM_THEME_DARK,
    SYSTEM_THEME_LIGHT,
} SystemTheme;

void win32_clipboard_copy(const HWND window, const String text);
String win32_clipboard_paste(const HWND window, Arena* arena);

String win32_ime_get_composition(HWND window, Arena* arena);
String win32_ime_get_result(HWND window, Arena* arena);
void win32_ime_update_candidate(HWND window, LONG client_x, LONG client_y, Position* out_screen_pos);

SystemTheme win32_get_system_theme(void);

/* Returns True and fills *out_color if the user enabled accent colors for title bars
   and window borders. out_color is the exact border color computed from the registry
   (accent blended with 0xd9d9d9 per ColorizationColorBalance). */
b32 win32_get_accent_border_color(Color* out_color);
