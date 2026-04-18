#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "math.h"
#include "shaders/d3d11_pshader.h"
#include "shaders/d3d11_vshader.h"
#include "utils.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"

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
} VertexStyle;

typedef struct
{
    Rect target_rect;
    Rect texture_rect;
    ColorF32 color;
    ColorF32 border_color;
    VertexStyle style_params;
    f32 is_text;
    i32 clip_rect_index;
} Vertex;

typedef struct
{
    Vertex data[VERTEX_CAPACITY];
    u32 count;
} VertexCache;

typedef struct
{
    Rect rects[CLIP_RECT_CAPACITY];
    u32 current_index;
    u32 count;
} ClipCache;

// clang-format off
typedef struct
{
    ID3D11Device*             device;
    ID3D11DeviceContext*      context;
    IDXGISwapChain1*          swapchain;
    ID3D11SamplerState*       sampler_state;
    ID3D11BlendState*         blend_state;
    ID3D11Texture2D*          glyph_atlas_texture;
    ID3D11ShaderResourceView* glyph_atlas_shader_resource_view;
    ID3D11Buffer*             vertex_buffer;
    ID3D11Buffer*             mvp_buffer;
    ID3D11Buffer*             clip_rect_buffer;
    ID3D11InputLayout*        layout;
    ID3D11VertexShader*       vertex_shader;
    ID3D11PixelShader*        pixel_shader;
    ID3D11RenderTargetView*   render_target_view;
    IDXGISwapChain2*          swapchain2;
    HANDLE                    frame_latency_waitable_object;
} RendererState;
// clang-format on

///

static RendererState s_renderer_state;
static VertexCache s_vertex_cache = { 0 };
static ClipCache s_clip_cache = { 0 };

void renderer_wait_for_last_submitted_frame()
{
    WaitForSingleObjectEx(s_renderer_state.frame_latency_waitable_object, 1000, True);
}

//
// renderer core
//

void renderer_init(const HWND window, const GlyphAtlas* glyph_atlas)
{
    /* Create device and context */
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

    /* Create swapchain */
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

    /* Create render target view for backbuffer texture */
    {
        ID3D11Texture2D* texture;
        IDXGISwapChain1_GetBuffer(s_renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
        D3D11_RENDER_TARGET_VIEW_DESC desc = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                               .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D };
        ID3D11Device_CreateRenderTargetView(s_renderer_state.device, (ID3D11Resource*)texture, &desc,
                                            &s_renderer_state.render_target_view);
        ID3D11Texture2D_Release(texture);
    }

    /* Create sampler state */
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

    /* Create blend state */
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

    /* Create texture (glyph atlas) */
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

    /* Create texture view (glyph atlas) */
    ID3D11Device_CreateShaderResourceView(s_renderer_state.device,
                                          (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0,
                                          &s_renderer_state.glyph_atlas_shader_resource_view);

    /* Create vertex buffer */
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth = sizeof(s_vertex_cache.data),
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_VERTEX_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        D3D11_SUBRESOURCE_DATA initial = { .pSysMem = s_vertex_cache.data };
        ID3D11Device_CreateBuffer(s_renderer_state.device, &desc, &initial, &s_renderer_state.vertex_buffer);
    }

    /* Create constant buffer for delivering MVP (Model View Projection) */
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth = sizeof(f32) * 4 * 4, // f32 mvp[4][4]
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        ID3D11Device_CreateBuffer(s_renderer_state.device, &desc, NULL, &s_renderer_state.mvp_buffer);
    }

    /* Create constant buffer for clipping */
    {
        D3D11_BUFFER_DESC desc = {
            .ByteWidth = sizeof(s_clip_cache.rects),
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        ID3D11Device_CreateBuffer(s_renderer_state.device, &desc, NULL, &s_renderer_state.clip_rect_buffer);
    }

    /* Create input layout, vertex shader, pixel shader */
    {
        // clang-format off
        D3D11_INPUT_ELEMENT_DESC desc[] = {
            // SemanticName,  SemanticIndex, Format,                         InputSlot, AlignedByteOffset,                 InputSlotClass,                InstanceDataStepRate
            { "TARGET_RECT",  0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, target_rect),     D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "TEXTURE_RECT", 0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, texture_rect),    D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "COLOR",        0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, color),           D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "BORDER_COLOR", 0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, border_color),    D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "STYLE_PARAMS", 0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, style_params),    D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "IS_TEXT",      0,             DXGI_FORMAT_R32_FLOAT,          0,         offsetof(Vertex, is_text),         D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "CLIP_INDEX",   0,             DXGI_FORMAT_R32_SINT,           0,         offsetof(Vertex, clip_rect_index), D3D11_INPUT_PER_INSTANCE_DATA, 1 }
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

static void renderer_upload_glyph(const GlyphAtlas* atlas, const GlyphInfo* glyph_info)
{
    D3D11_BOX box = {
        .left = glyph_info->atlas_x,
        .top = glyph_info->atlas_y,
        .front = 0,
        .right = glyph_info->atlas_x + glyph_info->w,
        .bottom = glyph_info->atlas_y + glyph_info->h,
        .back = 1,
    };
    const u8* src = atlas->bitmap + glyph_info->atlas_y * atlas->w + glyph_info->atlas_x;
    ID3D11DeviceContext_UpdateSubresource(
        s_renderer_state.context, (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0, &box, src, atlas->w, 0);
}

void renderer_recreate_glyph_atlas_texture(const GlyphAtlas* glyph_atlas)
{
    Assert(s_renderer_state.glyph_atlas_texture); // TODO: Should we check others?
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

    /* Create texture view (glyph atlas) */
    ID3D11Device_CreateShaderResourceView(s_renderer_state.device,
                                          (ID3D11Resource*)s_renderer_state.glyph_atlas_texture, 0,
                                          &s_renderer_state.glyph_atlas_shader_resource_view);
}

static void map_vertex_buffer()
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11DeviceContext_Map(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.vertex_buffer, 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, s_vertex_cache.data, sizeof(Vertex) * s_vertex_cache.count);
    ID3D11DeviceContext_Unmap(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.vertex_buffer, 0);
}

static void map_clip_rects_to_cbuffer()
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11DeviceContext_Map(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.clip_rect_buffer, 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, s_clip_cache.rects, sizeof(Rect) * CLIP_RECT_CAPACITY);
    ID3D11DeviceContext_Unmap(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.clip_rect_buffer, 0);
}

// Map orthographic projection matrix to cbuffer
static void map_mvp_to_cbuffer(const u32 client_width, const u32 client_height)
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
    ID3D11DeviceContext_Map(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.mvp_buffer, 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, mvp, sizeof(mvp));
    ID3D11DeviceContext_Unmap(s_renderer_state.context, (ID3D11Resource*)s_renderer_state.mvp_buffer, 0);
}

void renderer_flush_and_present(const u32 client_width, const u32 client_height)
{
    TracyCZone(ctx, 1);

    /* Map buffer */
    map_vertex_buffer();
    map_clip_rects_to_cbuffer();
    map_mvp_to_cbuffer(client_width, client_height);

    /* Set viewport */
    D3D11_VIEWPORT viewport = {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = (FLOAT)client_width,
        .Height = (FLOAT)client_height,
        .MinDepth = 0,
        .MaxDepth = 1,
    };

    /* Clear screen */
    FLOAT color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(s_renderer_state.context, s_renderer_state.render_target_view, color);

    // clang-format off
    /* IA-VS-RS-PS-OM, Draw */
    u32 stride = sizeof(Vertex);
    u32 offset = 0;
    ID3D11DeviceContext_IASetInputLayout(s_renderer_state.context, s_renderer_state.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_renderer_state.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetVertexBuffers(s_renderer_state.context, 0, 1, &s_renderer_state.vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_IASetVertexBuffers(s_renderer_state.context, 0, 1, &s_renderer_state.vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_VSSetConstantBuffers(s_renderer_state.context, 0, 1, &s_renderer_state.mvp_buffer);
    ID3D11DeviceContext_VSSetShader(s_renderer_state.context, s_renderer_state.vertex_shader, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_renderer_state.context, 1, 1, &s_renderer_state.clip_rect_buffer);
    ID3D11DeviceContext_RSSetViewports(s_renderer_state.context, 1, &viewport);
    ID3D11DeviceContext_PSSetShader(s_renderer_state.context, s_renderer_state.pixel_shader, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_renderer_state.context, 0, 1, &s_renderer_state.glyph_atlas_shader_resource_view);
    ID3D11DeviceContext_PSSetSamplers(s_renderer_state.context, 0, 1, &s_renderer_state.sampler_state);
    ID3D11DeviceContext_OMSetRenderTargets(s_renderer_state.context, 1, &s_renderer_state.render_target_view, NULL);
    ID3D11DeviceContext_OMSetBlendState(s_renderer_state.context, s_renderer_state.blend_state, NULL, 0xffffffff);
    ID3D11DeviceContext_DrawInstanced(s_renderer_state.context, 4, s_vertex_cache.count, 0, 0);
    // clang-format on

    /* Present */
    b32 vsync = True;
    u32 flags = 0;
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
    vsync = False;
    flags |= DXGI_PRESENT_ALLOW_TEARING;
#endif
    IDXGISwapChain1_Present(s_renderer_state.swapchain, vsync, flags);

    /* Reset vertex stack & clip rect cache */
    memset(&s_vertex_cache, 0, sizeof(s_vertex_cache));
    memset(&s_clip_cache, 0, sizeof(s_clip_cache));

    TracyCZoneEnd(ctx);
}

void renderer_deinit()
{
    ID3D11RenderTargetView_Release(s_renderer_state.render_target_view);
    ID3D11Buffer_Release(s_renderer_state.vertex_buffer);
    ID3D11Buffer_Release(s_renderer_state.mvp_buffer);
    ID3D11Buffer_Release(s_renderer_state.clip_rect_buffer);
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
// swapchain resize
//

void renderer_resize(const u32 client_width, const u32 client_height)
{
    TracyCZone(ctx, 1);

    /* Release old swapchain buffers */
    ID3D11DeviceContext_OMSetRenderTargets(s_renderer_state.context, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(s_renderer_state.render_target_view);

    /* Resize swapchain */
    u32 flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#if !defined(NDEBUG) || defined(TRACY_ENABLE)
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
#endif
    IDXGISwapChain1_ResizeBuffers(s_renderer_state.swapchain, 0, client_width, client_height, DXGI_FORMAT_UNKNOWN,
                                  flags);

    /* Create render target view for new backbuffer texture */
    ID3D11Texture2D* texture;
    IDXGISwapChain1_GetBuffer(s_renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
    D3D11_RENDER_TARGET_VIEW_DESC desc = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                           .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D };
    ID3D11Device_CreateRenderTargetView(s_renderer_state.device, (ID3D11Resource*)texture, &desc,
                                        &s_renderer_state.render_target_view);
    ID3D11Texture2D_Release(texture);

    TracyCZoneEnd(ctx);
}

//
// sRGB and linear
//

static f32 srgb_to_linear(f32 c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
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

static void renderer_set_clip_rect(const Rect* clip)
{
    u32 start_index = fnv1a_hash(clip, sizeof(*clip)) & (CLIP_RECT_CAPACITY - 1);

    u32 clip_rect_index = start_index;
    Rect void_rect = { 0, 0, 0, 0 };

    /* Open addressing */
    while (memcmp(&s_clip_cache.rects[clip_rect_index], &void_rect, sizeof(void_rect)) != 0)
    {
        if (memcmp(&s_clip_cache.rects[clip_rect_index], clip, sizeof(*clip)) == 0)
        {
            s_clip_cache.current_index = clip_rect_index;
            s_clip_cache.count++;
            return;
        }

        clip_rect_index++;

        if (clip_rect_index >= CLIP_RECT_CAPACITY)
            clip_rect_index = 0;
        if (clip_rect_index == start_index)
            Assert(0); // hash table is full
    }

    memcpy(&s_clip_cache.rects[clip_rect_index], clip, sizeof(*clip));
    s_clip_cache.current_index = clip_rect_index;
    s_clip_cache.count++;
}

static void renderer_push_rect(const Rect target_rect, const Rect texture_rect, const Color color,
                               const RectStyle style, const Rect* clip)
{
    Assert(s_vertex_cache.count != VERTEX_CAPACITY);

    /* Set clip */
    if (clip)
    {
        b32 use_last_clip = memcmp(&s_clip_cache.rects[s_clip_cache.current_index], clip, sizeof(*clip)) == 0;
        if (s_clip_cache.count == 0 || !use_last_clip)
            renderer_set_clip_rect(clip);
    }

    Vertex* vertex = &s_vertex_cache.data[s_vertex_cache.count];
    vertex->is_text = VERTEX_IS_NOT_TEXT;

    /* Update target rect */
    {
        vertex->target_rect.xmin = target_rect.xmin;
        vertex->target_rect.ymin = target_rect.ymin;
        vertex->target_rect.xmax = target_rect.xmax;
        vertex->target_rect.ymax = target_rect.ymax;
    }

    /* Update texture rect */
    {
        vertex->texture_rect.xmin = texture_rect.xmin / (f32)GLYPH_ATLAS_WIDTH;
        vertex->texture_rect.ymin = texture_rect.ymin / (f32)GLYPH_ATLAS_HEIGHT;
        vertex->texture_rect.xmax = texture_rect.xmax / (f32)GLYPH_ATLAS_WIDTH;
        vertex->texture_rect.ymax = texture_rect.ymax / (f32)GLYPH_ATLAS_HEIGHT;
    }

    /* Update color & border color */
    vertex->color = color_srgb_to_linear(color);
    vertex->border_color = color_srgb_to_linear(style.border_color);

    /* Update style parameters */
    vertex->style_params.corner_radius = style.corner_radius;
    vertex->style_params.border_thickness = style.border_thickness;
    vertex->style_params.shadow_offset.x = style.shadow_offset.x;
    vertex->style_params.shadow_offset.y = style.shadow_offset.y;

    /* Update clip rect index */
    vertex->clip_rect_index = clip ? s_clip_cache.current_index : CLIP_INDEX_SKIP;

    s_vertex_cache.count++;
}

//
// text width & height
//

static void renderer_update_glyph(GlyphCache* glyph_cache, GlyphInfo* glyph_info, const Font* font, const f32 font_size,
                                  const u32 codepoint, const u32 dpi)
{
    GlyphAtlas* atlas = &glyph_cache->atlas;
    u8* glyph_bitmap =
        glyph_rasterize(glyph_cache->dwrite, &glyph_cache->arena, glyph_info, codepoint, font, font_size, dpi);
    if (codepoint != ' ')
    {
        atlas_insert_glyph(atlas, glyph_info, glyph_bitmap);
        renderer_upload_glyph(&glyph_cache->atlas, glyph_info);
    }
}

f32 renderer_get_text_width_for_dpi(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                    const u32 dpi)
{
    f32 text_width = 0;
    u32 codepoint = 0;
    byte* p = text.data;
    while (p - text.data < text.len)
    {
        p = utf8_decode(p, &codepoint);

        GlyphFindOrInsertResult result = glyph_find_or_insert(glyph_cache, codepoint, font, font_size);
        if (result.signal == LRU_SIGNAL_TOINSERT || result.signal == LRU_SIGNAL_TOEVICT)
            renderer_update_glyph(glyph_cache, result.info, font, font_size, codepoint, dpi);

        text_width += result.info->xadvance;
    }
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    return text_width / dpi_scale;
}

// TODO: Future support for multiple fonts per line is planned. This will require calculating text height
// based on varying font line spaces rather than relying on a single font's line space.
f32 renderer_get_text_height_for_dpi(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                     const u32 dpi)
{
    u32 codepoint = 0;
    utf8_decode(text.data, &codepoint); // Get first glyph codepoint of the text
    Assert(codepoint);

    GlyphFindOrInsertResult result = glyph_find_or_insert(glyph_cache, codepoint, font, font_size);
    if (result.signal == LRU_SIGNAL_TOINSERT || result.signal == LRU_SIGNAL_TOEVICT)
        renderer_update_glyph(glyph_cache, result.info, font, font_size, codepoint, dpi);

    // TODO: Should we just return `font_size`?
    return font_size;
}

//
// draw
//

void renderer_draw_rect(const GlyphCache* glyph_cache, const Rect rect, const Color color, const RectStyle style,
                        const Rect* clip)
{
    /* Calculate expanded rect */
    Rect expanded_rect = rect;
    if (style.shadow_offset.x || style.shadow_offset.y)
    {
        f32 shadow_sigma = 4;
        f32 shadow_radius = 3.0f * shadow_sigma; // covers 99.73% size
        expanded_rect.xmin -= shadow_radius + max(0, -style.shadow_offset.x);
        expanded_rect.xmax += shadow_radius + max(0, style.shadow_offset.x);
        expanded_rect.ymin -= shadow_radius + max(0, -style.shadow_offset.y);
        expanded_rect.ymax += shadow_radius + max(0, style.shadow_offset.y);
    }

    GlyphInfo* glyph_white = (GlyphInfo*)glyph_cache->lru_cache.values_buf;
    Rect glyph_white_rect = {
        .xmin = (f32)glyph_white->atlas_x,
        .ymin = (f32)glyph_white->atlas_y,
        .xmax = (f32)(glyph_white->atlas_x + glyph_white->w),
        .ymax = (f32)(glyph_white->atlas_y + glyph_white->h),
    };
    renderer_push_rect(expanded_rect, glyph_white_rect, color, style, clip);
}

void renderer_draw_text(GlyphCache* glyph_cache, String text, const Position position, const Color color,
                        const Font* font, const f32 font_size, const u32 dpi, const Rect* clip)
{
    f32 next_position_x = position.x;

    /* Get physical pixel position of y */
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    f32 position_y = position.y + renderer_get_text_height_for_dpi(glyph_cache, text, font, font_size, dpi) * dpi_scale;

    u32 codepoint = 0;
    byte* p = text.data;
    while (p - text.data < text.len)
    {
        p = utf8_decode(p, &codepoint);

        GlyphFindOrInsertResult result = glyph_find_or_insert(glyph_cache, codepoint, font, font_size);
        if (result.signal == LRU_SIGNAL_TOINSERT || result.signal == LRU_SIGNAL_TOEVICT)
            renderer_update_glyph(glyph_cache, result.info, font, font_size, codepoint, dpi);

        Rect target_rect = {
            .xmin = next_position_x + (f32)result.info->xoff,
            .ymin = position_y + (f32)result.info->yoff,
            .xmax = next_position_x + (f32)result.info->xoff + (f32)result.info->w,
            .ymax = position_y + (f32)result.info->yoff + (f32)result.info->h,
        };
        Rect texture_rect = {
            .xmin = (f32)result.info->atlas_x,
            .ymin = (f32)result.info->atlas_y,
            .xmax = (f32)(result.info->atlas_x + result.info->w),
            .ymax = (f32)(result.info->atlas_y + result.info->h),
        };
        RectStyle rect_style = { 0 };
        renderer_push_rect(target_rect, texture_rect, color, rect_style, clip);

        /* Mark this as text rendering */
        Vertex* vertex = &s_vertex_cache.data[s_vertex_cache.count - 1];
        vertex->is_text = VERTEX_IS_TEXT;

        /* Update x position for next char */
        next_position_x += (f32)result.info->xadvance;
    }
}
