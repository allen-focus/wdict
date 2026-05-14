#pragma once

#include "cdwrite.h"
#include "LRU.h"
#include "utils.h"

#define GLYPH_ATLAS_WIDTH  2048
#define GLYPH_ATLAS_HEIGHT 2048

#define GLYPHS_CP_LENGTH 4096 // must be a power of two
#define GLYPHS_LENGTH    (GLYPHS_CP_LENGTH + 1) // plus an additional white rectangle glyph

#define ATLAS_GLYPH_MAP_CAPACITY 4096 // power of two

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

// Glyph identity for raster cache: (font, size, dpi, codepoint) since
// rasterization depends on physical pixel size which varies by DPI.
typedef struct
{
    const Font* font;
    f32 font_size; // logical pixels
    u32 dpi;
    u32 codepoint;
} GlyphKey;

// Metrics + cached bitmap produced by rasterization.
typedef struct
{
    u32 w, h, xadvance;
    i32 xoff, yoff;
    u8* bitmap; // pointer into raster cache arena
    u32 bitmap_size; // w * h
} GlyphRasterInfo;

// Physical-pixel atlas (CPU-side), per-window.
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

// Per-window hash table mapping GlyphKey → atlas position.
// Open addressing with FNV-1a hash.
typedef struct
{
    GlyphKey* keys;
    u16* atlas_x;
    u16* atlas_y;
    u32 capacity;
    u32 count;
} AtlasGlyphMap;

// Process-wide shared raster cache. Owns an LRU of rasterized glyph bitmaps
// keyed by (font, font_size, dpi, codepoint). Does NOT own a GPU atlas.
typedef struct
{
    Arena arena;
    LRUCache lru_cache; // key: GlyphKey, value: GlyphRasterInfo
    const DWriteContext* dwrite;
} GlyphRasterCache;

typedef struct
{
    GlyphRasterInfo* info;
    LRUSignal signal;
} GlyphRasterResult;

typedef struct
{
    b32 found;
    u16 atlas_x;
    u16 atlas_y;
} AtlasGlyphFindResult;

typedef struct
{
    u16 atlas_x;
    u16 atlas_y;
} AtlasGlyphPosition;

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

// Shared raster cache (process-wide).
void raster_cache_init(const DWriteContext* dwrite, GlyphRasterCache* cache, const isize glyphs_length);
void raster_cache_deinit(GlyphRasterCache* cache);

// Look up or insert a raster cache entry. If not rasterized yet (TOINSERT), caller must call
// raster_cache_rasterize() to fill in the metrics + bitmap.
GlyphRasterResult raster_cache_find_or_insert(GlyphRasterCache* cache, u32 codepoint, const Font* font, f32 font_size,
                                              u32 dpi);

// Rasterize a glyph into the cache entry. Allocates bitmap from the cache arena.
// Fills info->w, h, xadvance, xoff, yoff, bitmap, bitmap_size.
void raster_cache_rasterize(GlyphRasterCache* cache, GlyphRasterInfo* info, u32 codepoint, const Font* font,
                            f32 font_size, u32 dpi);

// Per-window atlas packing.
void atlas_glyph_map_init(AtlasGlyphMap* map, Arena* arena);
AtlasGlyphFindResult atlas_glyph_map_find(const AtlasGlyphMap* map, const GlyphKey* key);
void atlas_glyph_map_insert(AtlasGlyphMap* map, const GlyphKey* key, u16 x, u16 y);
AtlasGlyphPosition atlas_insert_glyph(GlyphAtlas* atlas, u32 w, u32 h, const u8* bitmap);
