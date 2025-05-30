#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>

#include <stdbool.h>
#include <stdint.h>

#include "lib.h"
#include "shaders/d3d11_pshader.h"
#include "shaders/d3d11_vshader.h"

///

#define BUFFER_SIZE 1024

///

typedef struct {
    float rect[4];
    uint8_t color[4];
} Vertex;

typedef struct {
    ID3D11Device*           device;
    ID3D11DeviceContext*    context;
    IDXGISwapChain1*        swapchain;
    ID3D11Buffer*           vertex_buffer;
    ID3D11Buffer*           constant_buffer;
    ID3D11InputLayout*      layout;
    ID3D11VertexShader*     vertex_shader;
    ID3D11PixelShader*      pixel_shader;
    ID3D11RenderTargetView* render_target_view;
    // For mitigating the input latency issue
    IDXGISwapChain2*        swapchain2;
    HANDLE                  frame_latency_waitable_object;
} RendererState;

///

static RendererState renderer_state;
static Vertex vertex_data[BUFFER_SIZE * 4];
static uint16_t buffer_index = 0;

///

void swapchain_resize(const uint16_t client_width, const uint16_t client_height)
{
    // Release old swapchain buffers
    ID3D11DeviceContext_ClearState(renderer_state.context);
    ID3D11RenderTargetView_Release(renderer_state.render_target_view);

    // Resize swapchain
    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#ifndef NDEBUG
    flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
#endif
    IDXGISwapChain1_ResizeBuffers(renderer_state.swapchain, 0, client_width, client_height, DXGI_FORMAT_UNKNOWN, flags);

    // Create render target view for new backbuffer texture
    ID3D11Texture2D* texture;
    IDXGISwapChain1_GetBuffer(renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
    ID3D11Device_CreateRenderTargetView(renderer_state.device, (ID3D11Resource*)texture, NULL, &renderer_state.render_target_view);
    ID3D11Texture2D_Release(texture);
}

///

void renderer_create(const HWND window)
{
    // Create device and context
    {
        UINT flags = 0;
#ifndef NDEBUG
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, ARRAYSIZE(levels),
                          D3D11_SDK_VERSION, &renderer_state.device, NULL, &renderer_state.context);
    }

#ifndef NDEBUG
    // Enable debug information for D3D11
    {
        ID3D11InfoQueue* info;
        ID3D11Device_QueryInterface(renderer_state.device, &IID_ID3D11InfoQueue, (void**)&info);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        ID3D11InfoQueue_Release(info);
    }

    // Enable debug information for DXGI
    {
        IDXGIInfoQueue* dxgiInfo;
        DXGIGetDebugInterface1(0, &IID_IDXGIInfoQueue, (void**)&dxgiInfo);
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE);
        IDXGIInfoQueue_Release(dxgiInfo);
    }
#endif

    // Create swapchain
    {
        IDXGIFactory2* factory;
        {
            IDXGIDevice* dxgi_device;
            ID3D11Device_QueryInterface(renderer_state.device, &IID_IDXGIDevice, (void**)&dxgi_device);
            IDXGIAdapter* dxgi_adapter;
            IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter);
            IDXGIAdapter_GetParent(dxgi_adapter, &IID_IDXGIFactory2, (void**)&factory);
            IDXGIAdapter_Release(dxgi_adapter);
            IDXGIDevice_Release(dxgi_device);
        }
        DXGI_SWAP_CHAIN_DESC1 desc =
        {
            .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = 2,
            .Scaling = DXGI_SCALING_NONE,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
        };
#ifndef NDEBUG
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
#endif
        IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown*)renderer_state.device, window, &desc, NULL, NULL, &renderer_state.swapchain);
        IDXGIFactory_MakeWindowAssociation(factory, window, DXGI_MWA_NO_ALT_ENTER);
        IDXGIFactory2_Release(factory);
        IDXGISwapChain1_QueryInterface(renderer_state.swapchain, &IID_IDXGISwapChain2, (void**)&renderer_state.swapchain2);
        renderer_state.frame_latency_waitable_object = IDXGISwapChain2_GetFrameLatencyWaitableObject(renderer_state.swapchain2);
    }

    // Create render target view for backbuffer texture
    {
        ID3D11Texture2D* texture;
        IDXGISwapChain1_GetBuffer(renderer_state.swapchain, 0, &IID_ID3D11Texture2D, (void**)&texture);
        ID3D11Device_CreateRenderTargetView(renderer_state.device, (ID3D11Resource*)texture, NULL, &renderer_state.render_target_view);
        ID3D11Texture2D_Release(texture);
    }

    // Create vertex buffer
    {
        D3D11_BUFFER_DESC desc =
        {
            .ByteWidth = sizeof(vertex_data),
            .Usage          = D3D11_USAGE_DYNAMIC,
            .BindFlags      = D3D11_BIND_VERTEX_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        D3D11_SUBRESOURCE_DATA initial = { .pSysMem = vertex_data };
        ID3D11Device_CreateBuffer(renderer_state.device, &desc, &initial, &renderer_state.vertex_buffer);
    }

    // Create constant buffer for delivering MVP (Model View Projection)
    {
        D3D11_BUFFER_DESC desc =
        {
            .ByteWidth = sizeof(float) * 4 * 4, // float mvp[4][4]
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        ID3D11Device_CreateBuffer(renderer_state.device, &desc, NULL, &renderer_state.constant_buffer);
    }

    // Create input layout, vertex shader, pixel shader
    {
        D3D11_INPUT_ELEMENT_DESC desc[] =
        {
            // SemanticName, SemanticIndex, Format,                         InputSlot, AlignedByteOffset,       InputSlotClass,              InstanceDataStepRate
            { "RECT",        0,             DXGI_FORMAT_R32G32B32A32_FLOAT, 0,         offsetof(Vertex, rect),  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "COLOR",       0,             DXGI_FORMAT_R8G8B8A8_UNORM,     0,         offsetof(Vertex, color), D3D11_INPUT_PER_INSTANCE_DATA, 1 }
        };
        ID3D11Device_CreateVertexShader(renderer_state.device, d3d11_vshader, sizeof(d3d11_vshader), NULL, &renderer_state.vertex_shader);
        ID3D11Device_CreatePixelShader(renderer_state.device, d3d11_pshader, sizeof(d3d11_pshader), NULL, &renderer_state.pixel_shader);
        ID3D11Device_CreateInputLayout(renderer_state.device, desc, ARRAYSIZE(desc), d3d11_vshader, sizeof(d3d11_vshader), &renderer_state.layout);
    }
}

void renderer_flush_and_present(const uint16_t client_width, const uint16_t client_height)
{
    // Map vertex buffer
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(renderer_state.context, (ID3D11Resource*)renderer_state.vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, vertex_data, sizeof(Vertex) * buffer_index);
        ID3D11DeviceContext_Unmap(renderer_state.context, (ID3D11Resource*)renderer_state.vertex_buffer, 0);
    }

    // Map orthographic projection matrix to cbuffer
    {
        float L = 0.f;
        float R = (float)client_width;
        float T = 0.f;
        float B = (float)client_height;
        float mvp[4][4] = {
            { 2.0f / (R - L),    0.0f,              0.0f, 0.0f },
            { 0.0f,              2.0f / (T - B),    0.0f, 0.0f },
            { 0.0f,              0.0f,              0.5f, 0.0f },
            { (R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f },
        };
        D3D11_MAPPED_SUBRESOURCE mapped;
        ID3D11DeviceContext_Map(renderer_state.context, (ID3D11Resource*)renderer_state.constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        memcpy(mapped.pData, mvp, sizeof(mvp));
        ID3D11DeviceContext_Unmap(renderer_state.context, (ID3D11Resource*)renderer_state.constant_buffer, 0);
    }

    // Set viewport
    D3D11_VIEWPORT viewport =
    {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = (FLOAT)client_width,
        .Height = (FLOAT)client_height,
        .MinDepth = 0,
        .MaxDepth = 1,
    };

    // Clear screen
    FLOAT color[4] = { 0.3f, 0.6f, 0.9f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(renderer_state.context, renderer_state.render_target_view, color);

    // IA-VS-RS-PS-OM, Draw
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11DeviceContext_IASetInputLayout(renderer_state.context, renderer_state.layout); // IA: Input Assembly
    ID3D11DeviceContext_IASetPrimitiveTopology(renderer_state.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetVertexBuffers(renderer_state.context, 0, 1, &renderer_state.vertex_buffer, &stride, &offset);
    ID3D11DeviceContext_VSSetConstantBuffers(renderer_state.context, 0, 1, &renderer_state.constant_buffer);
    ID3D11DeviceContext_VSSetShader(renderer_state.context, renderer_state.vertex_shader, NULL, 0); // VS: Vertex Shader
    ID3D11DeviceContext_RSSetViewports(renderer_state.context, 1, &viewport); // RS: Rasterizer Stage
    ID3D11DeviceContext_PSSetShader(renderer_state.context, renderer_state.pixel_shader, NULL, 0); // PS: Pixel Shader
    ID3D11DeviceContext_OMSetRenderTargets(renderer_state.context, 1, &renderer_state.render_target_view, NULL); // OM: Output Merger
    ID3D11DeviceContext_DrawInstanced(renderer_state.context, 4, buffer_index, 0, 0);

    // Present
    // TODO: Need investigate the input latency issue more
    // https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
    // (1000 is the timeout fallback; should never trigger under normal rendering)
    WaitForSingleObjectEx(renderer_state.frame_latency_waitable_object, 1000, true);
    bool vsync = true;
    UINT flags = 0;
#ifndef NDEBUG
    vsync = false;
    flags |= DXGI_PRESENT_ALLOW_TEARING;
#endif
    IDXGISwapChain1_Present(renderer_state.swapchain, vsync, flags);

    // Reset buffer index
    buffer_index = 0;
}

void renderer_destroy()
{
    ID3D11RenderTargetView_Release(renderer_state.render_target_view);
    ID3D11Buffer_Release(renderer_state.vertex_buffer);
    ID3D11Buffer_Release(renderer_state.constant_buffer);
    ID3D11InputLayout_Release(renderer_state.layout);
    ID3D11VertexShader_Release(renderer_state.vertex_shader);
    ID3D11PixelShader_Release(renderer_state.pixel_shader);
    IDXGISwapChain2_Release(renderer_state.swapchain2);
    IDXGISwapChain1_Release(renderer_state.swapchain);
    ID3D11DeviceContext_Release(renderer_state.context);
    ID3D11Device_Release(renderer_state.device);
}

void renderer_rect_push(const Rect destination, const Color color)
{
    Assert(buffer_index != BUFFER_SIZE);
    vertex_data[buffer_index].rect[0] = destination.x0;
    vertex_data[buffer_index].rect[1] = destination.y0;
    vertex_data[buffer_index].rect[2] = destination.x1;
    vertex_data[buffer_index].rect[3] = destination.y1;
    memcpy(vertex_data[buffer_index].color, &color, sizeof(color));
    buffer_index++;
}
