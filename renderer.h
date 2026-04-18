#pragma once

#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "utils.h"

///

void renderer_resize(const u32 client_width, const u32 client_height);

void renderer_init(const HWND window, const GlyphAtlas* glyph_atlas);
void renderer_recreate_glyph_atlas_texture(const GlyphAtlas* glyph_atlas);
void renderer_wait_for_last_submitted_frame();
void renderer_flush_and_present(const u32 client_width, const u32 client_height);
void renderer_deinit();

f32 renderer_get_text_width_for_dpi(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                    const u32 dpi);
f32 renderer_get_text_height_for_dpi(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                     const u32 dpi);

void renderer_draw_rect(const GlyphCache* glyph_cache, const Rect rect, const Color color, const RectStyle style,
                        const Rect* clip);
void renderer_draw_text(GlyphCache* glyph_cache, String text, const Position position, const Color color,
                        const Font* font, const f32 font_size, const u32 dpi, const Rect* clip);
