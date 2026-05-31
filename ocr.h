#pragma once

#include "utils.h"
#include <windows.h>

typedef struct
{
    i32 screen_x, screen_y;
    u32 screen_w, screen_h;
} OcrWordBbox;

#define WM_APP_OCR_RESULT  (WM_APP + 0x100)
#define WM_APP_OCR_TRIGGER (WM_APP + 0x101)

b32  ocr_init(void);
void ocr_deinit(void);

void ocr_recognize_region_async(i32 x, i32 y, u32 w, u32 h,
                                i32 cursor_x, i32 cursor_y,
                                HWND notify_window);

void ocr_free(void* text);
