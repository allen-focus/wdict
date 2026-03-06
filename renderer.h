#pragma once

#include "pch.h"  // IWYU pragma: keep
#include "lib.h"
#include "string.h"

#include <stdint.h>

///

void swapchain_resize(const u32 client_width, const u32 client_height);

void renderer_init(const HWND window);
void renderer_recreate_glyph_atlas_texture();
void renderer_flush_and_present(const u32 client_width, const u32 client_height);
void renderer_deinit();

void renderer_rect_push(const Rect target_rect, const Rect texture_rect, const Color color, const RectStyle style);

void renderer_draw_rect(const Rect rect, const Color color, const RectStyle style);
void renderer_draw_text(String text, const Position position, const Color color, const u32 dpi);

f32 renderer_get_text_width_for_dpi(const String text, const u32 dpi);
f32 renderer_get_text_height_for_dpi(const String text, const u32 dpi);
