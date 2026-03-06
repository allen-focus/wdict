#pragma once

#include "pch.h"  // IWYU pragma: keep
#include "lib.h"
#include <stdint.h>

#define GLYPH_ATLAS_WIDTH  1024
#define GLYPH_ATLAS_HEIGHT 1024

#define FONT_SIZE 16

#define ASCII_START   32
#define GLYPHS_LENGTH (127 - ASCII_START + 1 + 1) // ascii glyphs ([32, 126]) plus a close icon glyph and a white region

///

typedef struct
{
    IDWriteFontFace* face;
    IDWriteFontFace3* face3;
    f32 size; // Controls capital letter height in pixels
} Font;

// physical pixels, not logic
typedef struct
{
    Font* font;
    u32 codepoint;
    u32 w, h, xadvance;
    i32 xoff, yoff;
    u16 atlas_x, atlas_y;
} Glyph;

// physical pixels, not logic
typedef struct
{
    u8* bitmap;
    u16 w, h;
    // Manages the next position for placing glyphs within the atlas
    u16 next_x, next_y;
    // Tracks the y-coordinate of the next available position in the atlas,
    // which is determined by the maximum height of all glyphs placed in the current line plus the current line's
    // height.
    u16 maxy;
} GlyphAtlas;

typedef struct
{
    Glyph* glyphs;
    GlyphAtlas* atlas;
} GlyphCache;

///

Font* font_create(IDWriteFactory3* dwrite_factory, const wchar_t* font_name, const f32 font_size);
void font_destroy(Font* font);

void glyph_cache_init(GlyphCache* glyph_cache, const u32 glyphs_length);
void glyph_cache_pack_codepoints(IDWriteFactory3* dwrite_factory, GlyphCache* glyph_cache, Font* font,
                                 const u32* codepoints, const i32 codepoints_length, const u32 dpi);
void glyph_cache_deinit(GlyphCache* glyph_cache);

void glyph_cache_init_and_fill(const HWND window, GlyphCache* glyph_cache, const wchar_t* font_family, const u32 dpi);

///

extern GlyphCache* g_glyph_cache;
