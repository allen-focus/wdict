#pragma comment(lib, "user32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxguid")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "glyph_cache.h"
#include "lib.h"
#include "renderer.h"
#include "ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

///

#define CLIENT_WIDTH  1080
#define CLIENT_HEIGHT 720

#define MAX_WINDOW_TITLE_LENGTH 64

///

typedef struct
{
    UIContext* ui_context;
} AppContext;

///

GlyphCache* g_glyph_cache;

static wchar_t s_window_title[MAX_WINDOW_TITLE_LENGTH] = L"windows title";

///

static void process_frame(UIContext* ui_context)
{
    // clang-format off
    RectStyle rect_style = {
        .border_color = { 0, 0, 0, 0 },
        .corner_radius = 0,
        .border_thickness = 0,
        .enable_shadow = false
    };
    // clang-format on

    Color blue = (Color){ 10, 110, 137, 255 };
    Color pink = (Color){ 252, 147, 144, 255 };
    Color yellow = (Color){ 254, 216, 77, 255 };

    // Test text
    {
        // const char* text = "Hello glyph";
        // Pos text_pos = (Pos){ 50, 50 };
        // renderer_draw_text(text, text_pos, (Color){ 0, 255, 0, 255 });

        // // Draw top/bottom bar to test text width and height
        // uint32_t text_width = renderer_get_text_width(text);
        // uint32_t text_height = renderer_get_text_height(text);

        // Rect text_top_bar = (Rect){ text_pos.x, text_pos.y - 1, text_pos.x + text_width, text_pos.y };
        // renderer_draw_rect(text_top_bar, (Color){ 255, 0, 0, 255 }, simple_rect_style);

        // Rect text_bottom_bar =
        //     (Rect){ text_pos.x, text_pos.y + text_height, text_pos.x + text_width, text_pos.y + text_height + 1
        //     };
        // renderer_draw_rect(text_bottom_bar, (Color){ 255, 0, 0, 255 }, simple_rect_style);
    }

    {
        ui_reset(ui_context);
        ui_layout({ .size_style = { { 960, 540 }, SIZE_STYLE_FIXED },
                    .color = blue,
                    .rect_style = rect_style,
                    .padding = { 32, 32, 32, 32 },
                    .child_gap = 16,
                    .direction = UI_LAYOUT_LEFT_TO_RIGHT })
        {
            ui_layout({ .size_style = { { 300, 300 }, SIZE_STYLE_FIXED },
                        .color = pink,
                        .rect_style = rect_style,
                        .padding = { 16, 16, 16, 16 },
                        .child_gap = 8,
                        .direction = UI_LAYOUT_TOP_TO_BOTTOM })
            {
                ui_layout({ .size_style = { { 100, 100 }, SIZE_STYLE_FIXED },
                            .color = yellow,
                            .rect_style = rect_style,
                            .padding = { 8, 8, 8, 8 },
                            .child_gap = 4,
                            .direction = UI_LAYOUT_LEFT_TO_RIGHT })
                {
                }
                ui_layout({ .size_style = { { 100, 100 }, SIZE_STYLE_FIXED },
                            .color = yellow,
                            .rect_style = rect_style,
                            .padding = { 8, 8, 8, 8 },
                            .child_gap = 4,
                            .direction = UI_LAYOUT_LEFT_TO_RIGHT })
                {
                }
            }
            ui_layout({ .size_style = { { 300, 300 }, SIZE_STYLE_FIXED },
                        .color = pink,
                        .rect_style = rect_style,
                        .padding = { 16, 16, 16, 16 },
                        .child_gap = 8,
                        .direction = UI_LAYOUT_LEFT_TO_RIGHT })
            {
            }
        }
        UILayout* layout_root = ui_layout_get_root();
        ui_layout_resolve(ui_context, layout_root);
        ui_layout_generate_commands(ui_context, layout_root);

        // Draw
        for (int i = 0; i < ui_context->ui_command_queue.count; i++)
        {
            UICommand* cmd = &ui_context->ui_command_queue.items[i];
            switch (cmd->type)
            {
                case UI_COMMAND_RECT:
                {
                    renderer_draw_rect(cmd->rect.rect, cmd->rect.color, cmd->rect.style);
                    break;
                }
                case UI_COMMAND_TEXT:
                {
                    renderer_draw_text(cmd->text.text, cmd->text.position, cmd->text.color);
                    break;
                }
            }
        }
    }
}

static LRESULT CALLBACK window_procedure(const HWND window, const UINT message, const WPARAM wparam,
                                         const LPARAM lparam)
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
        {
            ui_context = app_context->ui_context;
        }
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
                {
                    process_frame(ui_context);
                    renderer_flush_and_present(ui_context->client_width, ui_context->client_height);
                }
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
            return 0;
        }
        case WM_SIZE:
        {
            ui_context->client_width = (uint16_t)LOWORD(lparam);
            ui_context->client_height = (uint16_t)HIWORD(lparam);
            if (ui_context->client_width > 0 && ui_context->client_height > 0)
            {
                ui_context->on_resize(ui_context->client_width, ui_context->client_height);

                // Force an immediate repaint of entire client area to ensure the updated content is rendered promptly
                InvalidateRect(window, NULL, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(window);
            }
            return 0;
        }
        case WM_DESTROY:
        {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nShowCmd)
{
    // Set DPI awareness for better scaling on high DPI displays (Windows 10, v1607)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Allocate user data
    UIContext* ui_context = malloc(sizeof(UIContext));
    g_glyph_cache = malloc(sizeof(GlyphCache));

    // Create window
    HWND window;
    {
        // Set the client position to screen center
        int screen_width = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        int x = (screen_width - CLIENT_WIDTH) / 2;
        int y = (screen_height - CLIENT_HEIGHT) / 2;

        // Give the client area rectangle, get back the entire window rectangle
        RECT rect = { x, y, x + CLIENT_WIDTH, y + CLIENT_HEIGHT };
        DWORD window_style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRectEx(&rect, window_style, 0, 0);

        // Register window class
        WNDCLASSW wc = {};
        wc.lpfnWndProc = window_procedure;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"window class";
        RegisterClassW(&wc);

        // Create window with user data
        AppContext* app_context = malloc(sizeof(AppContext));
        app_context->ui_context = ui_context;
        window = CreateWindowExW(0, wc.lpszClassName, s_window_title, window_style, rect.left, rect.top,
                                 rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, app_context);
    }

    // Initialize ui context & glyph cache & renderer
    {
        ui_context->on_resize = swapchain_resize;
        ui_context->client_width = CLIENT_WIDTH;
        ui_context->client_height = CLIENT_HEIGHT;
        memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
    }
    glyph_cache_init_and_fill(window, g_glyph_cache, L"Segoe UI Symbol");
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
