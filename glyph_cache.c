#include "cdwrite.h"
#include "LRU.h"
#include "glyph_cache.h"
#include "utils.h"

#include <string.h>
#include <wchar.h>

///

#define GLYPH_CAHCE_ARENA_CAPACITY MB(32)

//
// font
//

void font_register(Font* font, IDWriteFactory3* dwrite_factory, const wchar_t* font_name)
{
    IDWriteFont* dw_font = NULL;
    {
        IDWriteFontFamily* font_family = NULL;
        {
            IDWriteFontCollection* font_collection = NULL;
            IDWriteFactory3_GetSystemFontCollection(dwrite_factory, &font_collection, False);

            u32 family_index = 0;
            b32 family_exists = False;
            IDWriteFontCollection_FindFamilyName(font_collection, font_name, &family_index, &family_exists);
            Assert(family_exists);

            IDWriteFontCollection_GetFontFamily(font_collection, family_index, &font_family);
            IDWriteFontCollection_Release(font_collection);
        }
        IDWriteFontFamily_GetFirstMatchingFont(font_family, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, &dw_font);
        IDWriteFontFamily_Release(font_family);
    }
    IDWriteFont_CreateFontFace(dw_font, &font->face);
    IDWriteFont_Release(dw_font);

    IDWriteFontFace_QueryInterface(font->face, &IID_IDWriteFontFace3, (void**)&font->face3);
}

void font_unregister(Font* font)
{
    if (font->face3)
    {
        IDWriteFontFace3_Release(font->face3);
        font->face3 = NULL;
    }
    if (font->face)
    {
        IDWriteFontFace_Release(font->face);
        font->face = NULL;
    }
}

//
// atlas
//

void atlas_insert_glyph(GlyphAtlas* atlas, GlyphInfo* glyph_info, byte* glyph_bitmap)
{
    // Update the position information of glyph and atlas
    atlas->maxy = max(atlas->maxy, atlas->next_y + glyph_info->h);
    Assert(atlas->maxy <= atlas->h);
    if (atlas->next_x + glyph_info->w > atlas->w)
    {
        Assert(atlas->maxy + glyph_info->h <= atlas->h);
        atlas->next_y = atlas->maxy;
        atlas->next_x = 0;
    }
    glyph_info->atlas_x = atlas->next_x;
    glyph_info->atlas_y = atlas->next_y;

    // Insert glyph bitmap into atlas
    for (u32 y = 0; y < glyph_info->h; y++)
        for (u32 x = 0; x < glyph_info->w; x++)
        {
            u8 grayscale = glyph_bitmap[glyph_info->w * y + x];
            atlas->bitmap[(atlas->next_y + y) * atlas->w + (atlas->next_x + x)] = grayscale;
        }
    atlas->next_x += glyph_info->w;
}

//
// glyph cache
//

static b32 is_same_glyph_key(const void* a, const void* b, isize size)
{
    return memcmp(a, b, size) == 0;
}

void glyph_cache_init(GlyphCache* glyph_cache, const isize glyphs_length, IDWriteFactory3* dwrite_factory)
{
    glyph_cache->arena = arena_new(GLYPH_CAHCE_ARENA_CAPACITY);
    glyph_cache->atlas.w = GLYPH_ATLAS_WIDTH;
    glyph_cache->atlas.h = GLYPH_ATLAS_HEIGHT;
    GlyphAtlas* atlas = &glyph_cache->atlas;
    atlas->bitmap = arena_push(&glyph_cache->arena, sizeof(u8), _Alignof(u8), atlas->w * atlas->h);

    glyph_cache->lru_cache = lru_cache_create(&glyph_cache->arena, ((glyphs_length - 1) >> 1), glyphs_length,
                                              sizeof(GlyphKey), sizeof(GlyphInfo), fnv1a_hash, is_same_glyph_key);

    // Insert a 3x3 white region to bottom-right corner of glyph atlas
    GlyphInfo* white_glyph = (GlyphInfo*)glyph_cache->lru_cache.values_buf;
    white_glyph->atlas_x = GLYPH_ATLAS_WIDTH - 3;
    white_glyph->atlas_y = GLYPH_ATLAS_HEIGHT - 3;
    white_glyph->w = 3;
    white_glyph->h = 3;
    for (isize y = 0; y < 3; y++)
        for (isize x = 0; x < 3; x++)
            glyph_cache->atlas.bitmap[(white_glyph->atlas_y + y) * GLYPH_ATLAS_WIDTH + (white_glyph->atlas_x + x)] = 255;

    glyph_cache->dwrite_factory = dwrite_factory;
}

void glyph_cache_deinit(GlyphCache* glyph_cache)
{
    arena_release(&glyph_cache->arena);
    memset(glyph_cache, 0, sizeof(*glyph_cache));
}

u8* glyph_rasterize(Arena* arena, IDWriteFactory3* dwrite_factory, GlyphInfo* glyph_info, u32 codepoint,
                    const Font font, const f32 font_size, const u32 dpi)
{
    // Get pixel size & scale, see: https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels
    DWRITE_FONT_METRICS font_metrics = { 0 };
    IDWriteFontFace_GetMetrics(font.face, &font_metrics);
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    f32 physical_pixel_size = font_size * dpi_scale;

    // Get glyph indices
    u32 codepoints[1] = { codepoint };
    u16 glyph_indices[1] = { 0 };
    IDWriteFontFace_GetGlyphIndices(font.face, codepoints, 1, glyph_indices);

    // Get glyph advances
    f32 metrics_scale = physical_pixel_size / (f32)font_metrics.capHeight;
    DWRITE_GLYPH_METRICS design_metrics[1] = { 0 };
    IDWriteFontFace3_GetDesignGlyphMetrics(font.face3, glyph_indices, 1, design_metrics, False);
    f32 glyph_advances[1] = { design_metrics[0].advanceWidth * metrics_scale };
    glyph_info->xadvance = (u32)glyph_advances[0];

    // Get glyph offsets
    DWRITE_GLYPH_OFFSET glyph_offsets[1] = { 0 };

    // Glyph run analysis
    DWRITE_GLYPH_RUN run = {
        .fontFace = font.face,
        .fontEmSize = (f32)font_metrics.designUnitsPerEm * metrics_scale,
        .glyphCount = 1,
        .glyphIndices = glyph_indices,
        .glyphAdvances = glyph_advances,
        .glyphOffsets = glyph_offsets,
    };

    DWRITE_GRID_FIT_MODE grid_fit_mode = DWRITE_GRID_FIT_MODE_DEFAULT;
    DWRITE_RENDERING_MODE1 rendering_mode = DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC;

    IDWriteGlyphRunAnalysis* analysis = NULL;
    IDWriteFactory3_CreateGlyphRunAnalysis2(
        /* this             */ dwrite_factory,
        /* glyphRun         */ &run,
        /* transform        */ NULL,
        /* renderingMode    */ rendering_mode,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* gridFitMode      */ grid_fit_mode,
        /* antialiasMode    */ DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        /* baselineOriginX  */ 0.0f,
        /* baselineOriginY  */ 0.0f,
        /* glyphRunAnalysis */ &analysis);

    // Create glyph bitmap
    u8* glyph_bitmap;
    {
        // DWRITE_TEXTURE_ALIASED_1x1 is a misnomer, It actually outputs grayscale if we set up the analysis with AA enabled
        const DWRITE_TEXTURE_TYPE type = DWRITE_TEXTURE_ALIASED_1x1;

        RECT bounds = { 0 };
        IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, type, &bounds);

        glyph_info->xoff = bounds.left;
        glyph_info->yoff = bounds.top;
        glyph_info->w = bounds.right - bounds.left;
        glyph_info->h = bounds.bottom - bounds.top;

        u32 bitmap_size = glyph_info->w * glyph_info->h;
        glyph_bitmap = arena_push(arena, sizeof(u8), _Alignof(u8), bitmap_size);
        IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, type, &bounds, glyph_bitmap, bitmap_size);
        IDWriteGlyphRunAnalysis_Release(analysis);
    }
    return glyph_bitmap;
}

GlyphInfo* glyph_find_or_insert(GlyphCache* glyph_cache, u32 codepoint, const Font font, f32 font_size, LRUSignal* signal)
{
    GlyphKey key = { font, font_size, codepoint };
    u32 entry_index = lru_cache_find_or_evict(&glyph_cache->lru_cache, &key, signal);
    GlyphInfo* glyph_info = (GlyphInfo*)((byte*)glyph_cache->lru_cache.values_buf + entry_index * glyph_cache->lru_cache.value_size);
    return glyph_info;
}
