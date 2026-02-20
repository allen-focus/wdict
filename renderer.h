#pragma once

#include "pch.h"  // IWYU pragma: keep
#include "lib.h"
#include <stdint.h>


///

void swapchain_resize(const u16 client_width, const u16 client_height);

void renderer_init(const HWND window);
void renderer_flush_and_present(const u16 client_width, const u16 client_height);
void renderer_deinit();

void renderer_rect_push(const Rect target_rect, const Rect texture_rect, const Color color, const RectStyle style);

void renderer_draw_rect(const Rect rect, const Color color, const RectStyle style);
void renderer_draw_text(const char* text, const Position position, const Color color);

u32 renderer_get_text_width(const char* text);
u32 renderer_get_text_height(const char* text);