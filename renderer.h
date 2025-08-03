#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "glyph_cache.h"
#include "lib.h"
#include <stdint.h>

///

void swapchain_resize(const uint16_t client_width, const uint16_t client_height);

void renderer_init(const HWND window);
void renderer_flush_and_present(const uint16_t client_width, const uint16_t client_height);
void renderer_deinit();

void renderer_rect_push(const Rect target_rect, const Rect texture_rect, const Color color, const RectStyle style);

void renderer_draw_rect(const Rect rect, const Color color, const RectStyle style);
void renderer_draw_text(const char* text, const Pos pos, const Color color);

uint32_t renderer_get_text_width(const char* text);
uint32_t renderer_get_text_height(const char* text);
