#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "lib.h"
#include "renderer.h"
#include "string.h"
#include "ui.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <winuser.h>

///

#define CLIENT_WIDTH  800
#define CLIENT_HEIGHT 400

#define MAX_WINDOW_TITLE_LENGTH 64

///

typedef struct
{
    UIContext* ui_context;
} AppContext;

///

GlyphCache* g_glyph_cache;

static wchar_t s_window_title[MAX_WINDOW_TITLE_LENGTH] = L"windows title";

/// Temp

RectStyle background_rect_style = {
    .border_color = { 0, 0, 0, 0 }, .corner_radius = 0, .border_thickness = 0, .enable_shadow = false
};

RectStyle normal_rect_style = {
    .border_color = { 0, 0, 0, 0 }, .corner_radius = 12, .border_thickness = 0, .enable_shadow = false
};

RectStyle full_rect_style = {
    .border_color = { 255, 255, 255, 255 }, .corner_radius = 12, .border_thickness = 4, .enable_shadow = true
};

Color black = { 0, 0, 0, 255 };
Color grey = { 128, 128, 128, 255 };
Color white = { 255, 255, 255, 255 };
Color green = { 0, 255, 0, 255 };
Color red = { 255, 0, 0, 255 };
Color blue = { 0, 0, 255, 255 };

Padding padding_bigger = { 50, 50, 50, 50 };
Padding padding_big = { 30, 30, 30, 30 };
Padding padding_medium = { 20, 20, 20, 20 };
Padding padding_small = { 10, 10, 10, 10 };

f32 child_gap_bigger = 30;
f32 child_gap_big = 20;
f32 child_gap_medium = 10;
f32 child_gap_small = 5;

///

static void process_frame(UIContext* ui_context)
{
    ui_reset(ui_context);

    ui_box({ .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
             .color = red,
             .rect_style = background_rect_style,
             .padding = padding_bigger,
             .child_gap = child_gap_bigger,
             .direction = LAYOUT_LEFT_TO_RIGHT })
    {
        ui_box({ .sizing = { fit_grow(0), fit_grow(0) },
                 .color = green,
                 .rect_style = normal_rect_style,
                 .padding = padding_big,
                 .child_gap = child_gap_big,
                 .direction = LAYOUT_LEFT_TO_RIGHT })
        {
            ui_box({ .sizing = { fixed(200), fit_grow(0) }, .color = blue, .rect_style = full_rect_style }) { }
            ui_box({ .sizing = { fit(0), fit(0) },
                     .color = white,
                     .rect_style = normal_rect_style,
                     .padding = padding_medium,
                     .child_gap = 0,
                     .direction = LAYOUT_TOP_TO_BOTTOM })
            {
                ui_box({ .sizing = { fit_grow(0), fixed(2) }, .color = grey, .rect_style = normal_rect_style, .padding = { 0 }, .child_gap = 0 }) { }
                ui_text(ui_context, str("Here's to you, Nicola and Bart"), &(TextConfig){ .color = black, .line_height = 0.f });
                ui_box({ .sizing = { fit_grow(0), fixed(2) }, .color = grey, .rect_style = normal_rect_style, .padding = { 0 }, .child_gap = 0 }) { }
            }
            ui_box({ .sizing = { fixed(50), fit_grow(0) }, .color = blue, .rect_style = normal_rect_style }) { }
        }
    }

    //

    UIBox* root = ui_box_get_root();
    ui_calculate_layout(ui_context, root);
    ui_generate_render_commands(ui_context, root);

    // Draw
    for (i32 i = 0; i < ui_context->ui_command_queue.count; i++)
    {
        UICommand* cmd = &ui_context->ui_command_queue.items[i];
        switch (cmd->type)
        {
            case UI_COMMAND_RECT:
                renderer_draw_rect(cmd->rect.rect, cmd->rect.color, cmd->rect.style);
                break;
            case UI_COMMAND_TEXT:
                renderer_draw_text(cmd->text.content, cmd->text.position, cmd->text.color, ui_context->dpi);
                break;
            default:
                Assert(0);
        }
    }
}

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    UIContext* ui_context = NULL;
    {
        AppContext* app_context = NULL;
        if (message == WM_CREATE)
        {
            CREATESTRUCT* create = (CREATESTRUCT*)(lparam);
            app_context = (AppContext*)(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app_context);
        }
        else
        {
            LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
            app_context = (AppContext*)ptr;
        }

        if (app_context)
            ui_context = app_context->ui_context;
    }

    switch (message)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(window, &ps);
            {
#ifndef NDEBUG
                LARGE_INTEGER starting_time, ending_time, elapsed_microseconds;
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                QueryPerformanceCounter(&starting_time);
#endif
                process_frame(ui_context);

                f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
                u32 physical_client_width = (u32)(ui_context->client_width * dpi_scale);
                u32 physical_client_height = (u32)(ui_context->client_height * dpi_scale);
                renderer_flush_and_present(physical_client_width, physical_client_height);
#ifndef NDEBUG
                QueryPerformanceCounter(&ending_time);
                elapsed_microseconds.QuadPart = ending_time.QuadPart - starting_time.QuadPart;
                elapsed_microseconds.QuadPart *= 1000000;
                elapsed_microseconds.QuadPart /= frequency.QuadPart;

                // Set window title to show frame time
                swprintf(s_window_title, MAX_WINDOW_TITLE_LENGTH, L"Frame Time: %lld μs",
                         elapsed_microseconds.QuadPart);
                SetWindowTextW(window, s_window_title);
#endif
            }
            EndPaint(window, &ps);
        } return 0;

        case WM_SIZE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_client_width = LOWORD(lparam);
            u32 physical_client_height = HIWORD(lparam);

            ui_context->client_width = (u32)ceil(physical_client_width / dpi_scale);
            ui_context->client_height = (u32)ceil(physical_client_height / dpi_scale);
            if (ui_context->client_width > 0 && ui_context->client_height > 0)
            {
                ui_context->on_resize(physical_client_width, physical_client_height);
                // Force an immediate repaint of entire client area to ensure the updated content is rendered promptly
                InvalidateRect(window, NULL, FALSE);
            }
        } return 0;

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
                DestroyWindow(window);
        } return 0;

        case WM_DPICHANGED:
        {
            ui_context->dpi = GetDpiForWindow(window);

            // Reinit glyph cache
            glyph_cache_deinit(g_glyph_cache);
            g_glyph_cache = malloc(sizeof(GlyphCache));
            glyph_cache_init_and_fill(window, g_glyph_cache, L"Segoe UI Symbol", ui_context->dpi);

            // Recreate glyph atlas texture
            renderer_recreate_glyph_atlas_texture();

            // Set new window
            RECT* const suggested_rect = (RECT*)lparam;
            SetWindowPos(window, NULL, suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } return 0;

        case WM_DESTROY:
        {
            PostQuitMessage(0);
        } return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

i32 WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, i32 nShowCmd)
{
    // Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Init ui_context
    UIContext* ui_context = malloc(sizeof(UIContext));
    {
        ui_context->dpi = GetDpiForSystem();
        ui_context->client_width = CLIENT_WIDTH;
        ui_context->client_height = CLIENT_HEIGHT;
        ui_context->on_resize = swapchain_resize;
        ui_context->get_text_width = renderer_get_text_width_for_dpi;
        ui_context->get_text_height = renderer_get_text_height_for_dpi;
        memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
    }

    // Create window
    HWND window;
    {
        f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
        u32 physical_client_width = (u32)(ui_context->client_width * dpi_scale);
        u32 physical_client_height = (u32)(ui_context->client_height * dpi_scale);

        // Set the client position to screen center
        i32 screen_width = GetSystemMetrics(SM_CXSCREEN);
        i32 screen_height = GetSystemMetrics(SM_CYSCREEN);
        i32 x = (screen_width - physical_client_width) / 2;
        i32 y = (screen_height - physical_client_height) / 2;

        // Give the client area rectangle, get back the entire window rectangle
        RECT rect = { x, y, x + physical_client_width, y + physical_client_height };
        DWORD window_style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRectEx(&rect, window_style, 0, 0);

        // Register window class
        WNDCLASSW wc = {
            .lpfnWndProc = window_procedure,
            .hInstance = GetModuleHandleW(NULL),
            .lpszClassName = L"window class",
        };
        RegisterClassW(&wc);

        // Create window with user data
        AppContext* app_context = malloc(sizeof(AppContext));
        app_context->ui_context = ui_context;
        window = CreateWindowExW(0, wc.lpszClassName, s_window_title, window_style, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, app_context);
    }

    // Initialize glyph cache & renderer
    g_glyph_cache = malloc(sizeof(GlyphCache));
    glyph_cache_init_and_fill(window, g_glyph_cache, L"Segoe UI Symbol", ui_context->dpi);
    renderer_init(window);

    // Show window
    ShowWindow(window, SW_SHOWDEFAULT);

    // Run message loop
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Clean
    renderer_deinit();
    glyph_cache_deinit(g_glyph_cache);
    free(ui_context);

    return 0;
}
