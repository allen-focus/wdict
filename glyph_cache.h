#pragma once

#include "cdwrite.h"
#include "LRU.h"
#include "utils.h"

#define GLYPH_ATLAS_WIDTH  2048
#define GLYPH_ATLAS_HEIGHT 2048

#define GLYPHS_CP_LENGTH 4096 // must be a power of two
#define GLYPHS_LENGTH    (GLYPHS_CP_LENGTH + 1) // plus an additional white rectangle glyph

///

/* Font weight */
#define FONT_WEIGHT_THIN        L"100"
#define FONT_WEIGHT_EXTRA_LIGHT L"200"
#define FONT_WEIGHT_ULTRA_LIGHT L"200"
#define FONT_WEIGHT_LIGHT       L"300"
#define FONT_WEIGHT_SEMI_LIGHT  L"350"
#define FONT_WEIGHT_NORMAL      L"400"
#define FONT_WEIGHT_REGULAR     L"400"
#define FONT_WEIGHT_MEDIUM      L"500"
#define FONT_WEIGHT_DEMI_BOLD   L"600"
#define FONT_WEIGHT_SEMI_BOLD   L"600"
#define FONT_WEIGHT_BOLD        L"700"
#define FONT_WEIGHT_EXTRA_BOLD  L"800"
#define FONT_WEIGHT_ULTRA_BOLD  L"800"
#define FONT_WEIGHT_BLACK       L"900"
#define FONT_WEIGHT_HEAVY       L"900"
#define FONT_WEIGHT_EXTRA_BLACK L"950"
#define FONT_WEIGHT_ULTRA_BLACK L"950"

/* Font style */
#define FONT_STYLE_NORMAL  L"0"
#define FONT_STYLE_OBLIQUE L"1"
#define FONT_STYLE_ITALIC  L"2"

///

typedef struct
{
    IDWriteFactory5* factory;
    IDWriteInMemoryFontFileLoader* in_memory_font_file_loader;
} DWriteContext;

typedef struct
{
    IDWriteFontFace3* face3;
} Font;

typedef struct
{
    const Font* font;
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
    const DWriteContext* dwrite;
} GlyphCache;

typedef struct
{
    GlyphInfo* info;
    LRUSignal signal;
} GlyphFindOrInsertResult;

///

void dwrite_init(DWriteContext* dwrite);
void dwrite_deinit(DWriteContext* dwrite);

void font_register_from_system(DWriteContext* dwrite, wchar_t* font_name, DWRITE_FONT_WEIGHT weight,
                               DWRITE_FONT_STYLE style, Font* font);
void font_register_from_local_file(DWriteContext* dwrite, wchar_t* font_file_path, DWRITE_FONT_WEIGHT weight,
                                   DWRITE_FONT_STYLE style, Font* font);
void font_register_from_malloc_heap_memory(DWriteContext* dwrite, void* data, u32 size, DWRITE_FONT_WEIGHT weight,
                                           DWRITE_FONT_STYLE style, Font* font);
void font_register_from_resource(DWriteContext* dwrite, wchar_t* resource_name, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_FONT_STYLE style, Font* font);
void font_unregister(Font* font);

// `glyph_length` muse be a power of two plus 1. (e.g. 4096 + 1)
void glyph_cache_init(const DWriteContext* dwrite, GlyphCache* glyph_cache, const isize glyphs_length);
void glyph_cache_deinit(GlyphCache* glyph_cache);
u8* glyph_rasterize(const DWriteContext* dwrite, Arena* arena, GlyphInfo* glyph_info, u32 codepoint, const Font* font,
                    const f32 font_size, const u32 dpi);

GlyphFindOrInsertResult glyph_find_or_insert(GlyphCache* glyph_cache, u32 codepoint, const Font* font, f32 font_size);

void atlas_insert_glyph(GlyphAtlas* atlas, GlyphInfo* glyph_info, byte* glyph_bitmap);
