#pragma once

#include "glyph_cache.h"
#include "utils.h"
#include <windows.h>

///

#define VERTEX_CAPACITY    8192
#define VERTEX_IS_TEXT     1.0f
#define VERTEX_IS_NOT_TEXT 0.0f
#define CLIP_RECT_CAPACITY 256 // 1. must matches shader's define; 2. must be a power of two
#define CLIP_INDEX_SKIP    -1

///

typedef struct
{
    f32 r, g, b, a;
} ColorF32;

typedef struct
{
    f32 corner_radius;
    f32 border_thickness;
    Position shadow_offset;
    f32 shadow_sigma;
    f32 shear;
} VertexStyle;

typedef struct
{
    Rect target_rect;
    Rect texture_rect;
    ColorF32 corner_colors[4];
    ColorF32 border_color;
    ColorF32 shadow_color;
    VertexStyle style_params;
    f32 is_text;
    i32 clip_rect_index;
} Vertex;

typedef struct
{
    Vertex* data;
    u32 count;
} VertexCache;

typedef struct
{
    Rect* rects;
    u32 current_index;
    u32 count;
} ClipCache;

// clang-format off
typedef struct RendererShared
{
    ID3D11Device*        device;
    ID3D11DeviceContext* context;
    ID3D11SamplerState*  sampler_state;
    ID3D11BlendState*    blend_state;
    ID3D11InputLayout*   layout;
    ID3D11VertexShader*  vertex_shader;
    ID3D11PixelShader*   pixel_shader;
} RendererShared;
// clang-format on

// clang-format off
typedef struct Renderer
{
    IDXGISwapChain1*          swapchain;
    IDXGISwapChain2*          swapchain2;
    HANDLE                    frame_latency_waitable_object;
    ID3D11RenderTargetView*   render_target_view;
    ID3D11Texture2D*          glyph_atlas_texture;
    ID3D11ShaderResourceView* glyph_atlas_shader_resource_view;
    ID3D11Buffer*             vertex_buffer;
    ID3D11Buffer*             mvp_buffer;
    ID3D11Buffer*             clip_rect_buffer;
    VertexCache               vertex_cache;
    ClipCache                 clip_cache;
    RendererShared*           shared;
    Arena                     arena;
} Renderer;
// clang-format on

///

void renderer_shared_init(RendererShared* shared);
void renderer_shared_deinit(RendererShared* shared);

void renderer_init(Renderer* renderer, RendererShared* shared, const HWND window, const GlyphAtlas* glyph_atlas);
void renderer_deinit(Renderer* renderer);
void renderer_resize(Renderer* renderer, const u32 client_width, const u32 client_height);
void renderer_recreate_glyph_atlas_texture(Renderer* renderer, const GlyphAtlas* glyph_atlas);
void renderer_wait_for_last_submitted_frame(Renderer* renderer);
void renderer_flush_and_present(Renderer* renderer, const u32 client_width, const u32 client_height);

f32 renderer_get_text_width_for_dpi(Renderer* renderer, GlyphCache* glyph_cache, const String text, const Font* font,
                                    const f32 font_size, const u32 dpi);
f32 renderer_get_text_height_for_dpi(Renderer* renderer, GlyphCache* glyph_cache, const String text, const Font* font,
                                     const f32 font_size, const u32 dpi);

void renderer_draw_rect(Renderer* renderer, const GlyphCache* glyph_cache, const Rect rect, const Color color,
                        const RectStyle style, const Rect* clip);
void renderer_draw_text(Renderer* renderer, GlyphCache* glyph_cache, String text, const Position position,
                        const Color color, const Font* font, const f32 font_size, const u32 dpi, const Rect* clip);
