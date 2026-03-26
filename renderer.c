#include "pch.h"  // IWYU pragma: keep
#include "glyph_cache.h"
#include "math.h"
#include "shaders/d3d11_pshader.h"
#include "shaders/d3d11_vshader.h"
#include "utils.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define VERTEX_SIZE 1024

///

typedef struct
{
    f32 r, g, b, a;
} ColorF32;

typedef struct
{
    f32 corner_radius;
    f32 border_thickness;
    f32 enable_shadow;
    f32 is_text;
} VertexStyle;

typedef struct
{
    Rect target_rect;
    Rect texture_rect;
    ColorF32 color;
    ColorF32 border_color;
    VertexStyle style_params;
} Vertex;

typedef struct
{
    Vertex data[VERTEX_SIZE];
    u16 count;
} VertexStack;

typedef struct
{
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGISwapChain1* swapchain;
    ID3D11SamplerState* sampler_state;
    ID3D11BlendState* blend_state;
    ID3D11Texture2D* glyph_atlas_texture;
    ID3D11ShaderResourceView* glyph_atlas_shader_resource_view;
    ID3D11Buffer* vertex_buffer;
    ID3D11Buffer* constant_buffer;
    ID3D11InputLayout* layout;
    ID3D11VertexShader* vertex_shader;
    ID3D11PixelShader* pixel_shader;
    ID3D11RenderTargetView* render_target_view;
    // For mitigating the input latency issue
    IDXGISwapChain2* swapchain2;
    HANDLE frame_latency_waitable_object;
} RendererState;

///

static RendererState s_renderer_state;
static VertexStack s_vertex_stack = { 0 };

void renderer_wait_for_last_submitted_frame()
{
    WaitForSingleObjectEx(s_renderer_state.frame_latency_waitable_object, 1000, True);
}

//
// swapchain resize
//

void swapchain_resize(const u32 client_width, const u32 client_height)
{
    TracyCZone(ctx, 1);

    // Release old swapchain buffers
    ID3D11DeviceContext_OMSetRenderTargets(s_renderer_state.context, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(s_renderer_state.render_target_view);

    // Resize swapchain
    u32 flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
#endif
    IDXGISwapChain1_ResizeBuffers(s_renderer_state.swapchain, 0, client_width, client_height, DXGI_FORMAT_UNKNOWN,
                                  flags);

    // Create render target view for new backbuffer texture
    ID3D11Texture2D* texture;
    IDXGISwapChain1_GetBuffer(s_renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
    D3D11_RENDER_TARGET_VIEW_DESC desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D
    };
    ID3D11Device_CreateRenderTargetView(s_renderer_state.device, (ID3D11Resource*)texture, &desc,
                                        &s_renderer_state.render_target_view);
    ID3D11Texture2D_Release(texture);

    TracyCZoneEnd(ctx);
}

//
// renderer core
//

void renderer_init(const HWND window, const GlyphAtlas* glyph_atlas)
{
    // Create device and context
    {
        u32 flags = 0;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                          &s_renderer_state.device, NULL, &s_renderer_state.context);
    }

#ifndef NDEBUG
    // Enable debug information for D3D11
    {
        ID3D11InfoQueue* info;
        ID3D11Device_QueryInterface(s_renderer_state.device, &IID_ID3D11InfoQueue, (void**)&info);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_CORRUPTION, True);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_ERROR, True);
        ID3D11InfoQueue_Release(info);
    }

    // Enable debug information for DXGI
    {
        IDXGIInfoQueue* dxgiInfo;
        DXGIGetDebugInterface1(0, &IID_IDXGIInfoQueue, (void**)&dxgiInfo);
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, True);
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, True);
        IDXGIInfoQueue_Release(dxgiInfo);
    }
#endif

    // Create swapchain
    {
        IDXGIFactory2* factory;
        {
            IDXGIDevice* dxgi_device;
            ID3D11Device_QueryInterface(s_renderer_state.device, &IID_IDXGIDevice, (void**)&dxgi_device);
            IDXGIAdapter* dxgi_adapter;
            IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter);
            IDXGIAdapter_GetParent(dxgi_adapter, &IID_IDXGIFactory2, (void**)&factory);
            IDXGIAdapter_Release(dxgi_adapter);
            IDXGIDevice_Release(dxgi_device);
        }
        DXGI_SWAP_CHAIN_DESC1 desc = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = 2,
            .Scaling = DXGI_SCALING_NONE,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
        };
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
#endif
            IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown*)s_renderer_state.device, window, &desc, NULL, NULL,
                                                 &s_renderer_state.swapchain);
        IDXGIFactory_MakeWindowAssociation(factory, window, DXGI_MWA_NO_ALT_ENTER);
        IDXGIFactory2_Release(factory);
        IDXGISwapChain1_QueryInterface(s_renderer_state.swapchain, &IID_IDXGISwapChain2,
                                       (void**)&s_renderer_state.swapchain2);
        s_renderer_state.frame_latency_waitable_object =
            IDXGISwapChain2_GetFrameLatencyWaitableObject(s_renderer_state.swapchain2);
    }

    // Create render target view for backbuffer texture
    {
        ID3D11Texture2D* texture;
        IDXGISwapChain1_GetBuffer(s_renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
        D3D11_RENDER_TARGET_VIEW_DESC desc = {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D
        };
        ID3D11Device_CreateRenderTargetView(s_renderer_state.device, (ID3D11Resource*)texture, &desc,
                                            &s_renderer_state.render_target_view);
        ID3D11Texture2D_Release(texture);
    }

    // Create sampler state
    {
        D3D11_SAMPLER_DESC desc = {
            .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
            .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
            .ComparisonFunc = D3D11_COMPARISON_NEVER,
        };
        ID3D11Device_CreateSamplerState(s_renderer_state.device, &desc, &s_renderer_state.sampler_state);
    }

    // Create blend state
    {
        D3D11_BLEND_DESC desc = { .RenderTarget[0] = { .BlendEnable = True,
                                                       .SrcBlend = D3D11_BLEND_SRC_ALPHA,
                                                       .DestBlend = D3D11_BLEND_INV_SRC_ALPHA,
                                                       .BlendOp = D3D11_BLEND_OP_ADD,
                                                       .SrcBlendAlpha = D3D11_BLEND_ONE,
                                                       .DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA,
                                                       .BlendOpAlpha = D3D11_BLEND_OP_ADD,
                                                       .RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL } };
        ID3D11Device_CreateBlendState(s_renderer_state.device, &desc, &s_renderer_state.blend_state);
    }

    // Create texture (glyph atlas)
    {
        D3D11_TEXTURE2D_DESC desc = {
            .Width = glyph_atlas->w,
            .Height = glyph_atlas->h,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_R8_UNORM,
            .SampleDesc.Count = 1,
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };
        D3D11_SUBRESOURCE_DATA data = {
            .pSysMem = glyph_atlas->bitmap,
            .SysMemPitch = glyph_atlas->w,
        };
        ID3D11Device_CreateTexture2D(s_renderer_state.device, &desc, &data, &s_renderer_state.glyph_atlas_texture);
    }

    // Create texture view (glyph atlas)
    ID3D11Device_CreateShaderResourceView(s_renderer_state.device,
                                          (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0,
                                          &s_renderer_state.glyph_atlas_shader_resource_view);

    // Create vertex buffer
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth = sizeof(s_vertex_stack.data),
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_VERTEX_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        D3D11_SUBRESOURCE_DATA initial = { .pSysMem = s_vertex_stack.data };
        ID3D11Device_CreateBuffer(s_renderer_state.device, &desc, &initial, &s_renderer_state.vertex_buffer);
    }

    // Create constant buffer for delivering MVP (Model View Projection)
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth = sizeof(f32) * 4 * 4, // f32 mvp[4][4]
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        ID3D11Device_CreateBuffer(s_renderer_state.device, &desc, NULL, &s_renderer_state.constant_buffer);
    }

    // Create input layout, vertex shader, pixel shader
    {
        // clang-format off
        D3D11_INPUT_ELEMENT_DESC desc[] = {
            // SemanticName,      SemanticIndex, Format,                         InputSlot, AlignedByteOffset,                  InputSlotClass,                InstanceDataStepRate
            { "TARGET_RECT",      0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, target_rect),      D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "TEXTURE_RECT",     0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, texture_rect),     D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "COLOR",            0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, color),            D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "BORDER_COLOR",     0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, border_color),     D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "STYLE_PARAMS",     0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, style_params),     D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        };
        // clang-format on
        ID3D11Device_CreateVertexShader(s_renderer_state.device, d3d11_vshader, sizeof(d3d11_vshader), NULL,
                                        &s_renderer_state.vertex_shader);
        ID3D11Device_CreatePixelShader(s_renderer_state.device, d3d11_pshader, sizeof(d3d11_pshader), NULL,
                                       &s_renderer_state.pixel_shader);
        ID3D11Device_CreateInputLayout(s_renderer_state.device, desc, ARRAYSIZE(desc), d3d11_vshader,
                                       sizeof(d3d11_vshader), &s_renderer_state.layout);
    }
}

static void renderer_upload_glyph(const GlyphAtlas* atlas, const Glyph* glyph)
{
    D3D11_BOX box = {
        .left   = glyph->atlas_x,
        .top    = glyph->atlas_y,
        .front  = 0,
        .right  = glyph->atlas_x + glyph->w,
        .bottom = glyph->atlas_y + glyph->h,
        .back   = 1,
    };
    const u8* src = atlas->bitmap + glyph->atlas_y * atlas->w  + glyph->atlas_x;
    ID3D11DeviceContext_UpdateSubresource(
        s_renderer_state.context, (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0, &box, src, atlas->w, 0);
}

void renderer_recreate_glyph_atlas_texture(const GlyphAtlas* glyph_atlas)
{
    Assert(s_renderer_state.glyph_atlas_texture); // Should we check others?
    ID3D11Texture2D_Release(s_renderer_state.glyph_atlas_texture);
    ID3D11ShaderResourceView_Release(s_renderer_state.glyph_atlas_shader_resource_view);

    D3D11_TEXTURE2D_DESC desc = {
        .Width = glyph_atlas->w,
        .Height = glyph_atlas->h,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8_UNORM,
        .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };
    D3D11_SUBRESOURCE_DATA data = {
        .pSysMem = glyph_atlas->bitmap,
        .SysMemPitch = glyph_atlas->w,
    };
    ID3D11Device_CreateTexture2D(s_renderer_state.device, &desc, &data, &s_renderer_state.glyph_atlas_texture);

    // Create texture view (glyph atlas)
    ID3D11Device_CreateShaderResourceView(s_renderer_state.device,
                                          (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0,
                                          &s_renderer_state.glyph_atlas_shader_resource_view);
}

void renderer_flush_and_present(const u32 client_width, const u32 client_height)
{
    TracyCZone(ctx, 1);
    // Map vertex buffer
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.vertex_buffer, 0,
                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, s_vertex_stack.data, sizeof(Vertex) * s_vertex_stack.count);
        ID3D11DeviceContext_Unmap(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.vertex_buffer, 0);
    }

    // Map orthographic projection matrix to cbuffer
    {
        f32 L = 0.f;
        f32 R = (f32)client_width;
        f32 T = 0.f;
        f32 B = (f32)client_height;
        // clang-format off
        f32 mvp[4][4] = {
            { 2.0f / (R - L),    0.0f,              0.0f, 0.0f },
            { 0.0f,              2.0f / (T - B),    0.0f, 0.0f },
            { 0.0f,              0.0f,              0.5f, 0.0f },
            { (R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f },
        };
        // clang-format on
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.constant_buffer, 0,
                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, mvp, sizeof(mvp));
        ID3D11DeviceContext_Unmap(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.constant_buffer, 0);
    }

    // Set viewport
    D3D11_VIEWPORT viewport = {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = (FLOAT)client_width,
        .Height = (FLOAT)client_height,
        .MinDepth = 0,
        .MaxDepth = 1,
    };

    // Clear screen
    FLOAT color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(s_renderer_state.context, s_renderer_state.render_target_view, color);

    // IA-VS-RS-PS-OM, Draw
    // clang-format off
    u32 stride = sizeof(Vertex);
    u32 offset = 0;
    ID3D11DeviceContext_IASetInputLayout(s_renderer_state.context, s_renderer_state.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_renderer_state.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetVertexBuffers(s_renderer_state.context, 0, 1, &s_renderer_state.vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_VSSetConstantBuffers(s_renderer_state.context, 0, 1, &s_renderer_state.constant_buffer);
    ID3D11DeviceContext_VSSetShader(s_renderer_state.context, s_renderer_state.vertex_shader, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(s_renderer_state.context, 1, &viewport);
    ID3D11DeviceContext_PSSetShader(s_renderer_state.context, s_renderer_state.pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_renderer_state.context, 0, 1, &s_renderer_state.glyph_atlas_shader_resource_view);
    ID3D11DeviceContext_PSSetSamplers(s_renderer_state.context, 0, 1, &s_renderer_state.sampler_state);
    ID3D11DeviceContext_OMSetRenderTargets(s_renderer_state.context, 1, &s_renderer_state.render_target_view, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_renderer_state.context, s_renderer_state.blend_state, NULL, 0xffffffff);
    ID3D11DeviceContext_DrawInstanced(s_renderer_state.context, 4, s_vertex_stack.count, 0, 0);
    // clang-format on

    // Present
    b32 vsync = True;
    u32 flags = 0;
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
    vsync = False;
    flags |= DXGI_PRESENT_ALLOW_TEARING;
#endif
    IDXGISwapChain1_Present(s_renderer_state.swapchain, vsync, flags);

    // Reset vertex stack
    s_vertex_stack.count = 0;

    TracyCZoneEnd(ctx);
}

void renderer_deinit()
{
    ID3D11RenderTargetView_Release(s_renderer_state.render_target_view);
    ID3D11Buffer_Release(s_renderer_state.vertex_buffer);
    ID3D11Buffer_Release(s_renderer_state.constant_buffer);
    ID3D11InputLayout_Release(s_renderer_state.layout);
    ID3D11VertexShader_Release(s_renderer_state.vertex_shader);
    ID3D11PixelShader_Release(s_renderer_state.pixel_shader);
    ID3D11Texture2D_Release(s_renderer_state.glyph_atlas_texture);
    ID3D11ShaderResourceView_Release(s_renderer_state.glyph_atlas_shader_resource_view);
    ID3D11SamplerState_Release(s_renderer_state.sampler_state);
    ID3D11BlendState_Release(s_renderer_state.blend_state);
    IDXGISwapChain2_Release(s_renderer_state.swapchain2);
    IDXGISwapChain1_Release(s_renderer_state.swapchain);
    ID3D11DeviceContext_Release(s_renderer_state.context);
    ID3D11Device_Release(s_renderer_state.device);
}

//
// sRGB and linear
//

static f32 srgb_to_linear(f32 c)
{
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static ColorF32 color_srgb_to_linear(Color color_srgb)
{
    ColorF32 color_linear = {
        .r = srgb_to_linear(color_srgb.r / 255.0f),
        .g = srgb_to_linear(color_srgb.g / 255.0f),
        .b = srgb_to_linear(color_srgb.b / 255.0f),
        .a = color_srgb.a / 255.0f,
    };
    return color_linear;
}

//
// rect push
//

static void renderer_rect_push(const Rect target_rect, const Rect texture_rect, const Color color, const RectStyle style)
{
    Assert(s_vertex_stack.count != VERTEX_SIZE);

    Vertex* vertex = &s_vertex_stack.data[s_vertex_stack.count];

    // Update target rect
    {
        vertex->target_rect.xmin = target_rect.xmin;
        vertex->target_rect.ymin = target_rect.ymin;
        vertex->target_rect.xmax = target_rect.xmax;
        vertex->target_rect.ymax = target_rect.ymax;
    }

    // Update texture rect
    {
        vertex->texture_rect.xmin = texture_rect.xmin / (f32)GLYPH_ATLAS_WIDTH;
        vertex->texture_rect.ymin = texture_rect.ymin / (f32)GLYPH_ATLAS_HEIGHT;
        vertex->texture_rect.xmax = texture_rect.xmax / (f32)GLYPH_ATLAS_WIDTH;
        vertex->texture_rect.ymax = texture_rect.ymax / (f32)GLYPH_ATLAS_HEIGHT;
    }

    // Update color & border color
    vertex->color = color_srgb_to_linear(color);
    vertex->border_color = color_srgb_to_linear(style.border_color);

    // Update style parameters
    vertex->style_params.corner_radius = style.corner_radius;
    vertex->style_params.border_thickness = style.border_thickness;
    vertex->style_params.enable_shadow = (f32)style.enable_shadow;
    vertex->style_params.is_text = 0.0f; // is_text flag (0 = rect, 1 = text)

    s_vertex_stack.count++;
}

//
// text width & height
//

f32 renderer_get_text_width_for_dpi(const GlyphCache* glyph_cache, const String text, const u32 dpi, const Font* font,
                                    const f32 font_size)
{
    f32 text_width = 0;
    u32 codepoint = 0;
    byte* p = text.data;
    while (p - text.data < text.len)
    {
        p = utf8_decode(p, &codepoint);
        Glyph* glyph = glyph_lookup(glyph_cache->glyphs, codepoint, font, font_size);
        Assert(glyph);
        text_width += glyph->xadvance;
    }
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    return text_width / dpi_scale;
}

// TODO: Future support for multiple fonts per line is planned. This will require calculating text height
// based on varying font line spaces rather than relying on a single font's line space.
f32 renderer_get_text_height_for_dpi(const GlyphCache* glyph_cache, const String text, const u32 dpi, const Font* font,
                                     const f32 font_size)
{
    u32 codepoint = 0;
    utf8_decode(text.data, &codepoint); // Get first glyph codepoint of the text
    Assert(codepoint);
    Glyph* glyph = glyph_lookup(glyph_cache->glyphs, codepoint, font, font_size);
    return glyph->font_size;
}

//
// draw
//

void renderer_draw_rect(const GlyphCache* glyph_cache, const Rect rect, const Color color, const RectStyle style)
{
    // Calculate expanded rect
    Rect expanded_rect = rect;
    if (style.enable_shadow)
    {
        // NOTE: As we hard-coded shadow sigma and offset, we could just use the
        // pre-calculated original rect. The detail of that calculation is below:
        //   ```
        //   f32 shadow_sigma = 4;
        //   f32 shadow_radius = 3.0f * shadow_sigma; // covers 99.73% size
        //   expanded_rect.xmin -= shadow_radius + max(0, -shadow_offset.x);
        //   expanded_rect.xmax += shadow_radius + max(0, shadow_offset.x);
        //   expanded_rect.ymin -= shadow_radius + max(0, -shadow_offset.y);
        //   expanded_rect.ymax += shadow_radius + max(0, shadow_offset.y);
        //   ```
        expanded_rect.xmin -= 12;
        expanded_rect.xmax += 12;
        expanded_rect.ymin -= 12;
        expanded_rect.ymax += 14;
    }

    Glyph* glyph_white = &glyph_cache->glyphs[GLYPHS_LENGTH - 1];
    Rect glyph_white_rect = {
        .xmin = (f32)glyph_white->atlas_x,
        .ymin = (f32)glyph_white->atlas_y,
        .xmax = (f32)(glyph_white->atlas_x + glyph_white->w),
        .ymax = (f32)(glyph_white->atlas_y + glyph_white->h),
    };
    renderer_rect_push(expanded_rect, glyph_white_rect, color, style);
}

void renderer_draw_text(IDWriteFactory3* dwrite_factory, GlyphCache* glyph_cache, String text, const Position position,
                        const Color color, const u32 dpi, Font* font, const f32 font_size)
{
    f32 next_position_x = position.x;

    // Get physical pixel position of y
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    f32 position_y = position.y + renderer_get_text_height_for_dpi(glyph_cache, text, dpi, font, font_size) * dpi_scale;

    u32 codepoint = 0;
    byte* p = text.data;
    while (p - text.data < text.len)
    {
        p = utf8_decode(p, &codepoint);
        Glyph* glyph = glyph_lookup(glyph_cache->glyphs, codepoint, font, font_size);
        Assert(glyph);

        if (!glyph->valid)
        {
            GlyphAtlas* atlas = &glyph_cache->atlas;
            u8* glyph_bitmap = glyph_rasterize(&glyph_cache->arena, dwrite_factory, codepoint, glyph, font, font_size, dpi);
            if (glyph->codepoint != ' ')
            {
                atlas_insert_glyph(atlas, glyph, glyph_bitmap);
                renderer_upload_glyph(&glyph_cache->atlas, glyph);
            }
        }

        Rect target_rect = {
            .xmin = next_position_x + (f32)glyph->xoff,
            .ymin = position_y + (f32)glyph->yoff,
            .xmax = next_position_x + (f32)glyph->xoff + (f32)glyph->w,
            .ymax = position_y + (f32)glyph->yoff + (f32)glyph->h,
        };
        Rect texture_rect = {
            .xmin = (f32)glyph->atlas_x,
            .ymin = (f32)glyph->atlas_y,
            .xmax = (f32)(glyph->atlas_x + glyph->w),
            .ymax = (f32)(glyph->atlas_y + glyph->h),
        };
        RectStyle rect_style = { 0 };
        renderer_rect_push(target_rect, texture_rect, color, rect_style);

        // Mark this as text rendering by setting style_params[3]
        Vertex* vertex = &s_vertex_stack.data[s_vertex_stack.count - 1];
        vertex->style_params.is_text = 1.0f; // is_text flag

        // Update x position for next char
        next_position_x += (f32)glyph->xadvance;
    }
}
