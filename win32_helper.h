#pragma once

#include "utils.h"
#include <windows.h>

void win32_clipboard_copy(const HWND window, const String text);
String win32_clipboard_paste(const HWND window, Arena* arena);

b32   win32_ime_is_composing(HWND window);
String win32_ime_get_composition(HWND window, Arena* arena);
String win32_ime_get_result(HWND window, Arena* arena);
