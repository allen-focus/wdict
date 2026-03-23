#pragma once

#include "pch.h"  // IWYU pragma: keep
#include "arena.h"
#include "lib.h"

#include <stdint.h>

#define GLYPH_ATLAS_WIDTH  1024
#define GLYPH_ATLAS_HEIGHT 1024

#define ASCII_START   32
#define GLYPHS_LENGTH 1024

///

typedef struct
{
    IDWriteFontFace* face;
    IDWriteFontFace3* face3;
} Font;

// physical pixels, not logic
typedef struct
{
    u32 codepoint;
    f32 font_size; // Controls capital letter height in pixels

    u32 w, h, xadvance;
    i32 xoff, yoff;

    u16 atlas_x, atlas_y;
} Glyph;

// physical pixels, not logic
typedef struct
{
    byte* bitmap;
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
    Arena arena;
    GlyphAtlas atlas;
    Glyph* glyphs;
} GlyphCache;

///

void font_register(Font* font, IDWriteFactory3* dwrite_factory, const wchar_t* font_name);
void font_unregister(Font* font);

void glyph_cache_init(GlyphCache* glyph_cache, const isize glyphs_length);
void glyph_cache_deinit(GlyphCache* glyph_cache);
u8* glyph_rasterize(Arena* arena, IDWriteFactory3* dwrite_factory, Font* font, const u32 codepoint, Glyph* glyph,
                    const u32 dpi, f32 font_size);
Glyph* glyph_lookup(Glyph* glyphs, const u32 codepoint);

void atlas_insert_glyph(GlyphAtlas* atlas, Glyph* glyph, byte* glyph_bitmap);
