#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "glyph_cache.h"
#include "lib.h"
#include <stdint.h>

///

void swapchain_resize(const uint16_t client_width, const uint16_t client_height);

void renderer_init(const HWND window, const GlyphCache* glyph_cache);
void renderer_flush_and_present(const uint16_t client_width, const uint16_t client_height);
void renderer_deinit();

void renderer_rect_push(const Rect target_rect, const Rect texture_rect, const Color color, const float corner_radius,
                        const float border_thickness, const Color border_color, const float shadow_sigma,
                        const Pos shadow_offset);

void renderer_draw_rect(const GlyphCache* glyph_cache, const Rect rect, const Color color, const float corner_radius,
                        const float border_thickness, const Color border_color, const float shadow_sigma,
                        const Pos shadow_offset);
void renderer_draw_text(const GlyphCache* glyph_cache, const char* text, const Pos pos, const Color color);

uint32_t renderer_get_text_width(const GlyphCache* glyph_cache, const char* text);
uint32_t renderer_get_text_height(const GlyphCache* glyph_cache, const char* text);
