#include "glyph_cache.h"
#include "utils.h"

#include <string.h>
#include <wchar.h>

///

#define GLYPH_CAHCE_ARENA_CAPACITY MB(32)

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

// NOTE:
//   Somehow, filtering by weight/style using font properties does not work as expected.
//   The resulting font set is always empty (count == 0).
//   ```
//   void font_register_from_system(IDWriteFactory5* dwrite_factory, wchar_t* font_name, Font* font)
//   {
//       /* Get system font set */
//       IDWriteFontSet* font_set = NULL;
//       IDWriteFactory5_GetSystemFontSet(dwrite_factory, &font_set);
//       {
//           /* filter font set */
//           IDWriteFontSet* filtered_set = NULL;
//           DWRITE_FONT_PROPERTY properties[] = { { DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, font_name, NULL },
//                                                 { DWRITE_FONT_PROPERTY_ID_WEIGHT, L"400", NULL },
//                                                 { DWRITE_FONT_PROPERTY_ID_STYLE, L"0", NULL } };
//           IDWriteFontSet_GetMatchingFonts1(font_set, properties, countof(properties), &filtered_set);
//           UINT32 count = IDWriteFontSet_GetFontCount(filtered_set);
//           Assert(count > 0);
//           {
//               /* Get the first matched font */
//               IDWriteFontFaceReference* face_ref = NULL;
//               IDWriteFontSet_GetFontFaceReference(filtered_set, 0, &face_ref);
//               IDWriteFontFaceReference_CreateFontFace(face_ref, &font->face3);
//               IDWriteFontFaceReference_Release(face_ref);
//           }
//           IDWriteFontSet_Release(filtered_set);
//       }
//       IDWriteFontSet_Release(font_set);
//   }
//   ```
//   So we just use old IDWriteFontCollection interface.

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
// atlas
//

void atlas_insert_glyph(GlyphAtlas* atlas, GlyphInfo* glyph_info, byte* glyph_bitmap)
{
    /* Update the position information of glyph and atlas */
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

    /* Insert glyph bitmap into atlas */
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

void glyph_cache_init(const DWriteContext* dwrite, GlyphCache* glyph_cache, const isize glyphs_length)
{
    glyph_cache->arena = arena_new(GLYPH_CAHCE_ARENA_CAPACITY);
    glyph_cache->atlas.w = GLYPH_ATLAS_WIDTH;
    glyph_cache->atlas.h = GLYPH_ATLAS_HEIGHT;
    GlyphAtlas* atlas = &glyph_cache->atlas;
    atlas->bitmap = arena_push(&glyph_cache->arena, sizeof(u8), _Alignof(u8), atlas->w * atlas->h);

    glyph_cache->lru_cache = lru_cache_create(&glyph_cache->arena, ((glyphs_length - 1) >> 1), glyphs_length,
                                              sizeof(GlyphKey), sizeof(GlyphInfo), fnv1a_hash, is_same_glyph_key);

    /* Insert a 3x3 white region to bottom-right corner of glyph atlas */
    GlyphInfo* white_glyph = (GlyphInfo*)glyph_cache->lru_cache.values_buf;
    white_glyph->atlas_x = GLYPH_ATLAS_WIDTH - 3;
    white_glyph->atlas_y = GLYPH_ATLAS_HEIGHT - 3;
    white_glyph->w = 3;
    white_glyph->h = 3;
    for (isize y = 0; y < 3; y++)
        for (isize x = 0; x < 3; x++)
            atlas->bitmap[(white_glyph->atlas_y + y) * GLYPH_ATLAS_WIDTH + (white_glyph->atlas_x + x)] = 255;

    glyph_cache->dwrite = dwrite;
}

void glyph_cache_deinit(GlyphCache* glyph_cache)
{
    arena_release(&glyph_cache->arena);
    memset(glyph_cache, 0, sizeof(*glyph_cache));
}

u8* glyph_rasterize(const DWriteContext* dwrite, Arena* arena, GlyphInfo* glyph_info, u32 codepoint, const Font* font,
                    const f32 font_size, const u32 dpi)
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
    glyph_info->xadvance = (u32)glyph_advances[0];

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
    u8* glyph_bitmap;
    {
        // DWRITE_TEXTURE_ALIASED_1x1 is a misnomer, It actually outputs grayscale if we set up the analysis with AA
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

    IDWriteFontFace_Release(font_face);
    return glyph_bitmap;
}

GlyphFindOrInsertResult glyph_find_or_insert(GlyphCache* glyph_cache, u32 codepoint, const Font* font, f32 font_size)
{
    GlyphFindOrInsertResult result = { 0 };
    GlyphKey key = { font, font_size, codepoint };
    LRUCacheFindOrEvictResult lru_result = lru_cache_find_or_evict(&glyph_cache->lru_cache, &key);
    result.signal = lru_result.signal;
    result.info =
        (GlyphInfo*)((byte*)glyph_cache->lru_cache.values_buf + lru_result.index * glyph_cache->lru_cache.value_size);
    return result;
}
