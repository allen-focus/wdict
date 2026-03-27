#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "utils.h"

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

// TODO: Not used
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

void atlas_insert_glyph(GlyphAtlas* atlas, Glyph* glyph, byte* glyph_bitmap)
{
    // Update the position information of glyph and atlas
    atlas->maxy = max(atlas->maxy, atlas->next_y + glyph->info.h);
    Assert(atlas->maxy <= atlas->h);
    if (atlas->next_x + glyph->info.w > atlas->w)
    {
        Assert(atlas->maxy + glyph->info.h <= atlas->h);
        atlas->next_y = atlas->maxy;
        atlas->next_x = 0;
    }
    glyph->info.atlas_x = atlas->next_x;
    glyph->info.atlas_y = atlas->next_y;

    // Insert glyph bitmap into atlas
    for (u32 y = 0; y < glyph->info.h; y++)
        for (u32 x = 0; x < glyph->info.w; x++)
        {
            u8 grayscale = glyph_bitmap[glyph->info.w * y + x];
            atlas->bitmap[(atlas->next_y + y) * atlas->w + (atlas->next_x + x)] = grayscale;
        }
    atlas->next_x += glyph->info.w;
}

//
// glyph cache
//
// NOTE: Currently, it lacks advanced features such as LRU (Least Recently Used) management or
// other optimization techniques, serving merely as a straightforward storage mechanism for glyphs
// and their associated atlas data.
//

void glyph_cache_init(GlyphCache* glyph_cache, const isize glyphs_length)
{
    GlyphAtlas* atlas = &glyph_cache->atlas;
    atlas->bitmap = arena_push(&glyph_cache->arena, sizeof(u8), _Alignof(u8), atlas->w * atlas->h);
    glyph_cache->glyphs = arena_push(&glyph_cache->arena, sizeof(Glyph), _Alignof(Glyph), glyphs_length);

    // Insert a 3x3 white region to bottom-right corner of glyph atlas
    Glyph* white_glyph = &glyph_cache->glyphs[GLYPHS_LENGTH - 1];
    white_glyph->info.atlas_x = GLYPH_ATLAS_WIDTH - 3;
    white_glyph->info.atlas_y = GLYPH_ATLAS_HEIGHT - 3;
    white_glyph->info.w = 3;
    white_glyph->info.h = 3;
    for (isize y = 0; y < 3; y++)
        for (isize x = 0; x < 3; x++)
            glyph_cache->atlas.bitmap[(white_glyph->info.atlas_y + y) * GLYPH_ATLAS_WIDTH + (white_glyph->info.atlas_x + x)] = 255;
}

void glyph_cache_deinit(GlyphCache* glyph_cache)
{
    glyph_cache->glyphs = NULL;

    GlyphAtlas* atlas = &glyph_cache->atlas;
    atlas->bitmap = NULL;
    atlas->maxy = 0;
    atlas->next_x = 0;
    atlas->next_y = 0;

    arena_pop_to(&glyph_cache->arena, 0);
}

u8* glyph_rasterize(Arena* arena, IDWriteFactory3* dwrite_factory, u32 codepoint, Glyph* glyph, Font* font,
                    f32 font_size, const u32 dpi)
{
    glyph->info.valid = True;

    glyph->key.font = font;
    glyph->key.font_size = font_size;
    glyph->key.codepoint = codepoint;

    // Get pixel size & scale, see: https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels
    DWRITE_FONT_METRICS font_metrics = { 0 };
    IDWriteFontFace_GetMetrics(font->face, &font_metrics);
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    f32 physical_pixel_size = font_size * dpi_scale;

    // Get glyph indices
    glyph->key.codepoint = codepoint;
    u32 codepoints[1] = { glyph->key.codepoint };
    u16 glyph_indices[1] = { 0 };
    IDWriteFontFace_GetGlyphIndices(font->face, codepoints, 1, glyph_indices);

    // Get glyph advances
    f32 metrics_scale = physical_pixel_size / (f32)font_metrics.capHeight;
    DWRITE_GLYPH_METRICS design_metrics[1] = { 0 };
    IDWriteFontFace3_GetDesignGlyphMetrics(font->face3, glyph_indices, 1, design_metrics, False);
    f32 glyph_advances[1] = { design_metrics[0].advanceWidth * metrics_scale };
    glyph->info.xadvance = (u32)glyph_advances[0];

    // Get glyph offsets
    DWRITE_GLYPH_OFFSET glyph_offsets[1] = { 0 };

    // Glyph run analysis
    DWRITE_GLYPH_RUN run = {
        .fontFace = font->face,
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

        glyph->info.xoff = bounds.left;
        glyph->info.yoff = bounds.top;
        glyph->info.w = bounds.right - bounds.left;
        glyph->info.h = bounds.bottom - bounds.top;

        u32 bitmap_size = glyph->info.w * glyph->info.h;
        glyph_bitmap = arena_push(arena, sizeof(u8), _Alignof(u8), bitmap_size);
        IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, type, &bounds, glyph_bitmap, bitmap_size);
        IDWriteGlyphRunAnalysis_Release(analysis);
    }
    return glyph_bitmap;
}

Glyph* glyph_lookup(Glyph* glyphs, u32 codepoint, Font* font, f32 font_size)
{
    GlyphKey key = { font, font_size, codepoint };
    isize idx = fnv1a_hash((void*)&key, sizeof(key)) & (GLYPHS_CP_LENGTH - 1);
    for (isize i = 0; i < GLYPHS_CP_LENGTH; i++)
    {
        Glyph* glyph = &glyphs[idx];
        if (!glyph->info.valid || glyph->key.codepoint == codepoint)
            return glyph;
        idx = (idx + 1) & (GLYPHS_CP_LENGTH - 1);
    }
    return NULL;
}
