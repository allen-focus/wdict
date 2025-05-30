#pragma comment(lib, "user32")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxguid")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lib.h"
#include "renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

///

typedef struct {
    uint16_t client_width;
    uint16_t client_height;
    void (*on_resize)(uint16_t, uint16_t);
} UI_Context;

///

#define MAX_WINDOW_TITLE_LENGTH 64
wchar_t window_title[MAX_WINDOW_TITLE_LENGTH] = L"windows title";

///

static void process_frame()
{
    renderer_rect_push((Rect){ 50, 50, 150, 150 }, (Color){ 255, 0, 0, 255 });
    renderer_rect_push((Rect){ 250, 100, 350, 200 }, (Color){ 0, 255, 0, 255 });
}

static LRESULT CALLBACK window_procedure(const HWND window, const UINT message, const WPARAM wparam, const LPARAM lparam)
{
    UI_Context* ui_context;
    if (message == WM_CREATE)
    {
        CREATESTRUCT *create = (CREATESTRUCT*)(lparam);
        ui_context = (UI_Context*)(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)ui_context);
    }
    else
    {
        LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
        ui_context = (UI_Context*)ptr;
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
                process_frame();
                renderer_flush_and_present(ui_context->client_width, ui_context->client_height);
#ifndef NDEBUG
                QueryPerformanceCounter(&ending_time);
                elapsed_microseconds.QuadPart = ending_time.QuadPart - starting_time.QuadPart;
                elapsed_microseconds.QuadPart *= 1000000;
                elapsed_microseconds.QuadPart /= frequency.QuadPart;

                // Set window title to show frame time
                swprintf(window_title, MAX_WINDOW_TITLE_LENGTH, L"Frame Time: %lld μs", elapsed_microseconds.QuadPart);
                SetWindowTextW(window, window_title);
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
                // InvalidateRect(window, NULL, FALSE);
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

    // Create UI context
    UI_Context* ui_context = (UI_Context*)malloc(sizeof(UI_Context));
    ui_context->on_resize = swapchain_resize;
    ui_context->client_width = 600;
    ui_context->client_height = 400;

    // Create window
    HWND window;
    {
        // Set the client position to screen center
        int screen_width  = GetSystemMetrics(SM_CXSCREEN);
        int screen_height = GetSystemMetrics(SM_CYSCREEN);
        int x             = (screen_width - ui_context->client_width) / 2;
        int y             = (screen_height - ui_context->client_height) / 2;

        // Give the client area rectangle, get back the entire window rectangle
        RECT rect         = { x, y, x + ui_context->client_width, y + ui_context->client_height };
        DWORD window_style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRectEx(&rect, window_style, 0, 0);

        // Register window class then create window
        WNDCLASSW wc = {};
        wc.lpfnWndProc = window_procedure;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"window class";
        RegisterClassW(&wc);
        window = CreateWindowExW(0, wc.lpszClassName, window_title, window_style,
                                 rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                                 NULL, NULL, wc.hInstance, ui_context);
    }

    // Initialize renderer
    renderer_create(window);

    // Show window
    ShowWindow(window, SW_SHOWDEFAULT);

    // Run message and render loop
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    // Clean
    renderer_destroy();
    free(ui_context);

    return 0;
}
