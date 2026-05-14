#include "glyph_cache.h"
#include "utils.h"

#include <string.h>
#include <wchar.h>

///

#define RASTER_CACHE_ARENA_CAPACITY MB(32)

//
// IUnknown object that owns font data buffer
//

typedef struct
{
    IUnknown iface;
    ULONG ref_count;
    void* data;
    UINT32 size;
} FontDataOwner;

ULONG WINAPI FontDataOwner_AddRef(IUnknown* this)
{
    FontDataOwner* obj = (FontDataOwner*)this;
    return ++obj->ref_count;
}

ULONG WINAPI FontDataOwner_Release(IUnknown* this)
{
    FontDataOwner* obj = (FontDataOwner*)this;
    ULONG rc = --obj->ref_count;

    if (rc == 0)
    {
        free(obj->data);
        free(obj);
    }

    return rc;
}

// ppv: Pointer to Pointer to Void
HRESULT WINAPI FontDataOwner_QueryInterface(IUnknown* this, REFIID riid, void** ppv)
{
    if (!ppv)
        return E_INVALIDARG;

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *ppv = this;
        FontDataOwner_AddRef(this);
        return S_OK;
    }

    return E_NOINTERFACE;
}

static IUnknownVtbl s_font_data_owner_vtbl = { FontDataOwner_QueryInterface, FontDataOwner_AddRef,
                                               FontDataOwner_Release };

FontDataOwner* FontDataOwner_Create(void* data, UINT32 size)
{
    FontDataOwner* obj = (FontDataOwner*)malloc(sizeof(FontDataOwner));
    obj->iface.lpVtbl = &s_font_data_owner_vtbl;
    obj->ref_count = 1;
    obj->data = data;
    obj->size = size;
    return obj;
}

//
// font
//

// NOTE: The interface for handling in-memory font data requires Windows 10 Creators Update version (v1703).
void dwrite_init(DWriteContext* dwrite)
{
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (void**)&dwrite->factory);

    IDWriteFactory5_CreateInMemoryFontFileLoader(dwrite->factory, &dwrite->in_memory_font_file_loader);
    IDWriteFactory5_RegisterFontFileLoader(dwrite->factory, (IDWriteFontFileLoader*)dwrite->in_memory_font_file_loader);
}

void dwrite_deinit(DWriteContext* dwrite)
{
    IDWriteFactory5_UnregisterFontFileLoader(dwrite->factory,
                                             (IDWriteFontFileLoader*)dwrite->in_memory_font_file_loader);
    IDWriteInMemoryFontFileLoader_Release(dwrite->in_memory_font_file_loader);
    IDWriteFactory5_Release(dwrite->factory);
}

// NOTE:
//   Somehow, filtering by weight/style using font properties does not work as expected.
//   The resulting font set is always empty (count == 0).
//   ```
//   DWRITE_FONT_PROPERTY properties[] = { { DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, font_name, NULL },
//                                         { DWRITE_FONT_PROPERTY_ID_WEIGHT, L"400", NULL },
//                                         { DWRITE_FONT_PROPERTY_ID_STYLE, L"0", NULL } };
//   IDWriteFontSet_GetMatchingFonts1(font_set, properties, countof(properties), &filtered_set);
//   UINT32 count = IDWriteFontSet_GetFontCount(filtered_set);
//   ```
//   So we just use old IDWriteFontCollection interface.
static void font_register(IDWriteFontCollection1* collection, u32 family_index, DWRITE_FONT_WEIGHT weight,
                          DWRITE_FONT_STYLE style, Font* font)
{
    Assert(collection);
    IDWriteFontFace* font_face;
    {
        IDWriteFont* dw_font = NULL;
        {
            IDWriteFontFamily* font_family = NULL;
            IDWriteFontCollection1_GetFontFamily(collection, family_index, &font_family);
            IDWriteFontFamily_GetFirstMatchingFont(font_family, weight, DWRITE_FONT_STRETCH_NORMAL, style, &dw_font);
            IDWriteFontFamily_Release(font_family);
        }
        IDWriteFont_CreateFontFace(dw_font, &font_face);
        IDWriteFont_Release(dw_font);
    }
    IDWriteFontFace_QueryInterface(font_face, &IID_IDWriteFontFace3, (void**)&font->face3);
    IDWriteFontFace_Release(font_face);
}

void font_register_from_system(DWriteContext* dwrite, wchar_t* font_name, DWRITE_FONT_WEIGHT weight,
                               DWRITE_FONT_STYLE style, Font* font)
{
    /* Get font collection from system */
    IDWriteFontCollection1* font_collection = NULL;
    IDWriteFactory5_GetSystemFontCollection1(dwrite->factory, False, &font_collection, False);

    /* Find font family by family name */
    u32 family_index = 0;
    b32 family_exists = False;
    IDWriteFontCollection1_FindFamilyName(font_collection, font_name, &family_index, &family_exists);
    Assert(family_exists);

    /* Register font */
    font_register(font_collection, family_index, weight, style, font);

    /* Clean */
    IDWriteFontCollection1_Release(font_collection);
}

static void font_register_from_font_file(DWriteContext* dwrite, IDWriteFontFile* font_file, DWRITE_FONT_WEIGHT weight,
                                         DWRITE_FONT_STYLE style, Font* font)
{
    IDWriteFontCollection1* font_collection = NULL;
    {
        IDWriteFontSet* font_set;
        {
            IDWriteFontSetBuilder1* font_set_builder;
            {
                IDWriteFactory5_CreateFontSetBuilder1(dwrite->factory, &font_set_builder);
                IDWriteFontSetBuilder1_AddFontFile(font_set_builder, font_file);
            }
            IDWriteFontSetBuilder1_CreateFontSet(font_set_builder, &font_set);
            IDWriteFontSetBuilder1_Release(font_set_builder);
        }
        IDWriteFactory5_CreateFontCollectionFromFontSet(dwrite->factory, font_set, &font_collection);
        IDWriteFontSet_Release(font_set);
    }
    font_register(font_collection, 0, weight, style, font);
    IDWriteFontCollection1_Release(font_collection);
}

// See: https://learn.microsoft.com/en-us/windows/win32/directwrite/custom-font-sets-win10
void font_register_from_local_file(DWriteContext* dwrite, wchar_t* font_file_path, DWRITE_FONT_WEIGHT weight,
                                   DWRITE_FONT_STYLE style, Font* font)
{
    IDWriteFontFile* font_file;
    IDWriteFactory5_CreateFontFileReference(dwrite->factory, font_file_path, NULL, &font_file);
    font_register_from_font_file(dwrite, font_file, weight, style, font);
    IDWriteFontFile_Release(font_file);
}

void font_register_from_malloc_heap_memory(DWriteContext* dwrite, void* data, u32 size, DWRITE_FONT_WEIGHT weight,
                                           DWRITE_FONT_STYLE style, Font* font)
{
    IDWriteFontFile* font_file;
    {
        FontDataOwner* font_data_owner = FontDataOwner_Create(data, size);
        IDWriteInMemoryFontFileLoader_CreateInMemoryFontFileReference(
            dwrite->in_memory_font_file_loader, (IDWriteFactory*)dwrite->factory, font_data_owner->data,
            font_data_owner->size, (IUnknown*)font_data_owner, &font_file);
        // NOTE: Release our initial ref. Object now owned solely by DirectWrite.
        FontDataOwner_Release((IUnknown*)font_data_owner);
    }
    font_register_from_font_file(dwrite, font_file, weight, style, font);
    IDWriteFontFile_Release(font_file);
}

void font_register_from_resource(DWriteContext* dwrite, wchar_t* resource_name, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_FONT_STYLE style, Font* font)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    HRSRC resource_info = FindResourceW(instance, resource_name, (LPCWSTR)RT_RCDATA);
    HGLOBAL resource_data = LoadResource(instance, resource_info);
    void* data = LockResource(resource_data);
    UINT32 size = (UINT32)SizeofResource(instance, resource_info);

    void* heap_data = malloc(size);
    memcpy(heap_data, data, size);

    font_register_from_malloc_heap_memory(dwrite, heap_data, size, weight, style, font);
}

void font_unregister(Font* font)
{
    if (font->face3)
    {
        IDWriteFontFace3_Release(font->face3);
        font->face3 = NULL;
    }
}

//
// atlas (per-window, CPU-side packing)
//

void atlas_glyph_map_init(AtlasGlyphMap* map, Arena* arena)
{
    map->capacity = ATLAS_GLYPH_MAP_CAPACITY;
    map->keys = arena_push(arena, sizeof(GlyphKey), _Alignof(GlyphKey), map->capacity);
    map->atlas_x = arena_push(arena, sizeof(u16), _Alignof(u16), map->capacity);
    map->atlas_y = arena_push(arena, sizeof(u16), _Alignof(u16), map->capacity);
    memset(map->keys, 0, sizeof(GlyphKey) * map->capacity);
    map->count = 0;
}

AtlasGlyphFindResult atlas_glyph_map_find(const AtlasGlyphMap* map, const GlyphKey* key)
{
    AtlasGlyphFindResult result = { 0 };
    if (map->count == 0)
        return result;
    u32 idx = fnv1a_hash(key, sizeof(*key)) & (map->capacity - 1);
    GlyphKey void_key = { 0 };
    while (memcmp(&map->keys[idx], &void_key, sizeof(void_key)) != 0)
    {
        if (memcmp(&map->keys[idx], key, sizeof(*key)) == 0)
        {
            result.found = True;
            result.atlas_x = map->atlas_x[idx];
            result.atlas_y = map->atlas_y[idx];
            return result;
        }
        idx = (idx + 1) & (map->capacity - 1);
    }
    return result;
}

void atlas_glyph_map_insert(AtlasGlyphMap* map, const GlyphKey* key, u16 x, u16 y)
{
    Assert(map->count < map->capacity);
    u32 idx = fnv1a_hash(key, sizeof(*key)) & (map->capacity - 1);
    GlyphKey void_key = { 0 };
    while (memcmp(&map->keys[idx], &void_key, sizeof(void_key)) != 0)
        idx = (idx + 1) & (map->capacity - 1);
    memcpy(&map->keys[idx], key, sizeof(*key));
    map->atlas_x[idx] = x;
    map->atlas_y[idx] = y;
    map->count++;
}

AtlasGlyphPosition atlas_insert_glyph(GlyphAtlas* atlas, u32 w, u32 h, const u8* bitmap)
{
    atlas->maxy = max(atlas->maxy, atlas->next_y + h);
    Assert(atlas->maxy <= atlas->h);
    if (atlas->next_x + w > (u32)atlas->w)
    {
        Assert(atlas->maxy + h <= atlas->h);
        atlas->next_y = atlas->maxy;
        atlas->next_x = 0;
    }
    AtlasGlyphPosition pos = { atlas->next_x, atlas->next_y };

    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
            atlas->bitmap[(atlas->next_y + y) * atlas->w + (atlas->next_x + x)] = bitmap[w * y + x];

    atlas->next_x += (u16)w;
    return pos;
}

//
// raster cache (process-wide shared)
//

static b32 is_same_glyph_key(const void* a, const void* b, isize size)
{
    (void)size;
    return memcmp(a, b, sizeof(GlyphKey)) == 0;
}

void raster_cache_init(const DWriteContext* dwrite, GlyphRasterCache* cache, const isize glyphs_length)
{
    cache->arena = arena_new(RASTER_CACHE_ARENA_CAPACITY);
    cache->lru_cache = lru_cache_create(&cache->arena, ((glyphs_length - 1) >> 1), glyphs_length, sizeof(GlyphKey),
                                        sizeof(GlyphRasterInfo), fnv1a_hash, is_same_glyph_key);
    cache->dwrite = dwrite;
}

void raster_cache_deinit(GlyphRasterCache* cache)
{
    arena_release(&cache->arena);
    memset(cache, 0, sizeof(*cache));
}

static void raster_cache_rasterize_impl(const DWriteContext* dwrite, Arena* arena, GlyphRasterInfo* info, u32 codepoint,
                                        const Font* font, const f32 font_size, const u32 dpi)
{
    IDWriteFontFace* font_face;
    IDWriteFontFace3_QueryInterface(font->face3, &IID_IDWriteFontFace, (void**)&font_face);

    /* Get pixel size & scale, see:
    https://learn.microsoft.com/en-us/windows/win32/learnwin32/dpi-and-device-independent-pixels */
    DWRITE_FONT_METRICS font_metrics = { 0 };
    IDWriteFontFace3_GetMetrics(font->face3, &font_metrics);
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    f32 physical_pixel_size = font_size * dpi_scale;

    /* Get glyph indices */
    u32 codepoints[1] = { codepoint };
    u16 glyph_indices[1] = { 0 };
    IDWriteFontFace3_GetGlyphIndices(font->face3, codepoints, 1, glyph_indices);

    /* Get glyph advances */
    f32 metrics_scale = physical_pixel_size / (f32)font_metrics.capHeight;
    DWRITE_GLYPH_METRICS design_metrics[1] = { 0 };
    IDWriteFontFace3_GetDesignGlyphMetrics(font->face3, glyph_indices, 1, design_metrics, False);
    f32 glyph_advances[1] = { design_metrics[0].advanceWidth * metrics_scale };
    info->xadvance = (u32)glyph_advances[0];

    /* Get glyph offsets */
    DWRITE_GLYPH_OFFSET glyph_offsets[1] = { 0 };

    /* Glyph run analysis */
    DWRITE_GLYPH_RUN run = {
        .fontFace = font_face,
        .fontEmSize = (f32)font_metrics.designUnitsPerEm * metrics_scale,
        .glyphCount = 1,
        .glyphIndices = glyph_indices,
        .glyphAdvances = glyph_advances,
        .glyphOffsets = glyph_offsets,
    };

    DWRITE_GRID_FIT_MODE grid_fit_mode = DWRITE_GRID_FIT_MODE_DEFAULT;
    DWRITE_RENDERING_MODE1 rendering_mode = DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC;

    IDWriteGlyphRunAnalysis* analysis = NULL;
    IDWriteFactory5_CreateGlyphRunAnalysis2(
        /* this             */ dwrite->factory,
        /* glyphRun         */ &run,
        /* transform        */ NULL,
        /* renderingMode    */ rendering_mode,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* gridFitMode      */ grid_fit_mode,
        /* antialiasMode    */ DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        /* baselineOriginX  */ 0.0f,
        /* baselineOriginY  */ 0.0f,
        /* glyphRunAnalysis */ &analysis);

    /* Create glyph bitmap */
    {
        // DWRITE_TEXTURE_ALIASED_1x1 is a misnomer, It actually outputs grayscale if we set up the analysis with AA
        const DWRITE_TEXTURE_TYPE type = DWRITE_TEXTURE_ALIASED_1x1;

        RECT bounds = { 0 };
        IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, type, &bounds);

        info->xoff = bounds.left;
        info->yoff = bounds.top;
        info->w = bounds.right - bounds.left;
        info->h = bounds.bottom - bounds.top;

        u32 bitmap_size = info->w * info->h;
        info->bitmap_size = bitmap_size;
        info->bitmap = arena_push(arena, sizeof(u8), _Alignof(u8), bitmap_size);
        IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, type, &bounds, info->bitmap, bitmap_size);
        IDWriteGlyphRunAnalysis_Release(analysis);
    }

    IDWriteFontFace_Release(font_face);
}

void raster_cache_rasterize(GlyphRasterCache* cache, GlyphRasterInfo* info, u32 codepoint, const Font* font,
                            f32 font_size, u32 dpi)
{
    raster_cache_rasterize_impl(cache->dwrite, &cache->arena, info, codepoint, font, font_size, dpi);
}

GlyphRasterResult raster_cache_find_or_insert(GlyphRasterCache* cache, u32 codepoint, const Font* font, f32 font_size,
                                              u32 dpi)
{
    GlyphRasterResult result = { 0 };
    GlyphKey key = { font, font_size, dpi, codepoint };
    LRUCacheFindOrEvictResult lru_result = lru_cache_find_or_evict(&cache->lru_cache, &key);
    result.signal = lru_result.signal;
    result.info =
        (GlyphRasterInfo*)((byte*)cache->lru_cache.values_buf + lru_result.index * cache->lru_cache.value_size);
    return result;
}
