#pragma once

#include "cdwrite.h"
#include "LRU.h"
#include "utils.h"

#define GLYPH_ATLAS_WIDTH          2048
#define GLYPH_ATLAS_HEIGHT         2048

#define GLYPHS_CP_LENGTH 4096 // must be a power of two
#define GLYPHS_LENGTH (GLYPHS_CP_LENGTH + 1) // Plus an additional white rectangle glyph

///

typedef struct
{
    IDWriteFontFace* face;
    IDWriteFontFace3* face3;
} Font;

typedef struct
{
    Font font;
    f32 font_size; // Controls capital letter height in pixels
    u32 codepoint;
} GlyphKey;

typedef struct
{
    u32 w, h, xadvance;
    i32 xoff, yoff;
    u16 atlas_x, atlas_y;
} GlyphInfo;

// physical pixels, not logic
typedef struct
{
    byte* bitmap;
    u16 w, h;
    // Manages the next position for placing glyphs within the atlas
    u16 next_x, next_y;
    // Tracks the y-coordinate of the next available position in the atlas,
    // which is determined by the maximum height of all glyphs placed in the
    // current line plus the current line's height.
    u16 maxy;
} GlyphAtlas;

typedef struct
{
    Arena arena;
    GlyphAtlas atlas;
    LRUCache lru_cache;
    IDWriteFactory3* dwrite_factory;
} GlyphCache;

///

void font_register(Font* font, IDWriteFactory3* dwrite_factory, const wchar_t* font_name);
void font_unregister(Font* font);

// `glyph_length` muse be a power of two plus 1. (e.g. 4096 + 1)
void glyph_cache_init(GlyphCache* glyph_cache, const isize glyphs_length, IDWriteFactory3* dwrite_factory);
void glyph_cache_deinit(GlyphCache* glyph_cache);
u8* glyph_rasterize(Arena* arena, IDWriteFactory3* dwrite_factory, GlyphInfo* glyph_info, u32 codepoint,
                    const Font font, const f32 font_size, const u32 dpi);

GlyphInfo* glyph_find_or_insert(GlyphCache* glyph_cache, u32 codepoint, const Font font, f32 font_size, LRUSignal* signal);

void atlas_insert_glyph(GlyphAtlas* atlas, GlyphInfo* glyph_info, byte* glyph_bitmap);
