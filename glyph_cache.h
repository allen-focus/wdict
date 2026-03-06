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
    f32 dpi;

    u16 ascent;
    u16 descent;
    i16 line_gap;
    u16 capital_letter_height;
} Font;

typedef struct
{
    Font* font;

    // glyph metrics
    u32 codepoint;
    u16 w, h, xadvance;
    i16 xoff, yoff;

    // to locate this glyph in the atlas
    u16 atlas_x, atlas_y;
} Glyph;

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

Font* font_create(IDWriteFactory3* dwrite_factory, const wchar_t* font_name, const f32 font_size, const f32 dpi);
void font_destroy(Font* font);

void glyph_cache_init(GlyphCache* glyph_cache, const u16 glyphs_length);
void glyph_cache_pack_codepoints(IDWriteFactory3* dwrite_factory, GlyphCache* glyph_cache, Font* font,
                                 const u32* codepoints, const u16 codepoints_length);
void glyph_cache_deinit(GlyphCache* glyph_cache);

void glyph_cache_init_and_fill(const HWND window, GlyphCache* glyph_cache, const wchar_t* font_family);

///

extern GlyphCache* g_glyph_cache;
