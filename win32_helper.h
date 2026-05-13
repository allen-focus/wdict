#pragma once

#include "utils.h"

void win32_clipboard_copy(const HWND window, const String text);
String win32_clipboard_paste(const HWND window, Arena* arena);
