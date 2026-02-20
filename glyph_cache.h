#pragma once

#include "cdwrite.h"
#include <stdint.h>

#define GLYPH_ATLAS_WIDTH  1024
#define GLYPH_ATLAS_HEIGHT 1024

#define FONT_SIZE 30

#define ASCII_START   32
#define GLYPHS_LENGTH (127 - ASCII_START + 1 + 1) // ascii glyphs ([32, 126]) plus a close icon glyph and a white region

///

typedef struct
{
    IDWriteFontFace* face;
    IDWriteFontFace3* face3;

    float size; // in pixel
    float dpi;

    uint16_t ascent;
    uint16_t descent;
    int16_t line_gap;
    uint16_t english_capital_height;
} Font;

typedef struct
{
    Font* font;

    // glyph metrics
    uint32_t codepoint;
    uint16_t w, h, xadvance;
    int16_t xoff, yoff;

    // to locate this glyph in the atlas
    uint16_t atlas_x, atlas_y;
} Glyph;

typedef struct
{
    uint8_t* bitmap;
    uint16_t w, h;
    // Manages the next position for placing glyphs within the atlas
    uint16_t next_x, next_y;
    // Tracks the y-coordinate of the next available position in the atlas,
    // which is determined by the maximum height of all glyphs placed in the current line plus the current line's
    // height.
    uint16_t maxy;
} GlyphAtlas;

typedef struct
{
    Glyph* glyphs;
    GlyphAtlas* atlas;
} GlyphCache;

///

Font* font_create(IDWriteFactory3* dwrite_factory, const wchar_t* font_name, const float font_size, const float dpi);
void font_destroy(Font* font);

void glyph_cache_init(GlyphCache* glyph_cache, const uint16_t glyphs_length);
void glyph_cache_pack_codepoints(IDWriteFactory3* dwrite_factory, GlyphCache* glyph_cache, Font* font,
                                 const uint32_t* codepoints, const uint16_t codepoints_length);
void glyph_cache_deinit(GlyphCache* glyph_cache);

void glyph_cache_init_and_fill(const HWND window, GlyphCache* glyph_cache, const wchar_t* font_family);

///

extern GlyphCache* g_glyph_cache;
