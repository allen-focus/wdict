#include "pch.h" // IWYU pragma: keep
#include "glyph_cache.h"
#include "renderer.h"
#include "ui.h"
#include "utils.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"

#include <math.h>
#include <wchar.h>

///

#define CLIENT_WIDTH  400
#define CLIENT_HEIGHT 400

#define MAX_TITLE_LENGTH 64

#define FONT_INDEX_UI 0
#define FONT_INDEX_ZH 1
#define FONT_INDEX_MONO 2
#define FONT_CAPACITY 3

///

typedef struct
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    IDWriteFactory3* dwrite_factory;
    Font fonts[FONT_CAPACITY];
    GlyphCache glyph_cache;
    u64 frame_count;
} AppContext;

///

static void set_app_title(AppContext* app_context, const String title)
{
    i32 written = MultiByteToWideChar(CP_UTF8, 0, (const char*)title.data, (int)title.len, app_context->title,
                                      MAX_TITLE_LENGTH - 1);
    app_context->title[written] = L'\0';
    SetWindowTextW(app_context->window, app_context->title);
}

static void process_frame(AppContext* app_context)
{
    if (app_context->frame_count > 0)
        renderer_wait_for_last_submitted_frame();

    TracyCZone(ctx, 1);

    // Alias
    UIContext* ui_context = &app_context->ui;
    GlyphCache* glyph_cache = &app_context->glyph_cache;
    Font font_zh = app_context->fonts[FONT_INDEX_ZH];

    // Style
    Color black = { 0,   0,   0,   255 };
    Color grey  = { 209, 233, 229, 255 };
    Color white = { 255, 255, 255, 255 };
    Color red   = { 251, 147, 143, 255 };
    Color green = { 253, 216, 77,  255 };
    Color blue  = { 94,  203, 228, 255 };

    Padding padding_big     = { 30, 30, 30, 30 };
    Padding padding_medium  = { 20, 20, 20, 20 };

    f32 child_gap_big     = 20;
    f32 child_gap_medium  = 10;

    ///

    isize arena_pos_backup = ui_context->arena.pos;
    {
        ui_box({
            .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
            .color = white,
            .padding = padding_big,
            .child_gap = child_gap_big,
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        })
        {
            ui_box({
                .sizing = { fixed(200), fixed(300) },
                .color = grey,
                .padding = padding_medium,
                .child_gap = child_gap_medium,
                .direction = LAYOUT_TOP_TO_BOTTOM
            })
            {
                UISignalFlags flags = ui_button(ui_context, glyph_cache, str("hello"), blue, black, font_zh, 12);
                if (ui_hovered(flags))
                    ui_box({ .sizing = { fit_grow({}), fixed(50) }, .color = red }) {}
                if (ui_lclicked(flags))
                    set_app_title(app_context, str("Left Click"));
                if (ui_rclicked(flags))
                    set_app_title(app_context, str("Right Click"));
            }
        }

        ///

        ui_context->root = ui_box_get_root();
        ui_calculate_layout(ui_context, glyph_cache, ui_context->root);
        ui_generate_render_commands(ui_context, ui_context->root);

        // Draw
        for (isize i = 0; i < ui_context->command_queue.count; i++)
        {
            UICommand* cmd = &ui_context->command_queue.items[i];
            switch (cmd->type)
            {
                case UI_COMMAND_RECT:
                    renderer_draw_rect(glyph_cache, cmd->rect.rect, cmd->rect.color, cmd->rect.style);
                    break;
                case UI_COMMAND_TEXT:
                    renderer_draw_text(glyph_cache, cmd->text.content, cmd->text.position, cmd->text.color, cmd->text.font, cmd->text.font_size, ui_context->dpi);
                    break;
                default:
                    Assert(0);
            }
        }

        ui_reset(ui_context);
    }
    arena_pop_to(&ui_context->arena, arena_pos_backup);

    // Present
    f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_client_width = (u32)(ui_context->client_width * dpi_scale);
    u32 physical_client_height = (u32)(ui_context->client_height * dpi_scale);
    renderer_flush_and_present(physical_client_width, physical_client_height);

    app_context->frame_count++;

    TracyCZoneEnd(ctx);
}

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    // Read passing data from param
    AppContext* app_context = NULL;
    UIContext* ui_context = NULL;
    GlyphCache* glyph_cache = NULL;
    {
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
            ui_context = &app_context->ui;
            glyph_cache = &app_context->glyph_cache;
        }
    }

    // Handle message
    switch (message)
    {
        case WM_MOUSEMOVE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            ui_context->mouse_pos.x = LOWORD(lparam) / dpi_scale;
            ui_context->mouse_pos.y = HIWORD(lparam) / dpi_scale;
        } return 0;

        case WM_LBUTTONDOWN:
        {
            ui_context->mouse_lclick = True;
        } return 0;

        case WM_RBUTTONDOWN:
        {
            ui_context->mouse_rclick = True;
        } return 0;

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
                DestroyWindow(window);
        } return 0;

        case WM_SIZE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_client_width = LOWORD(lparam);
            u32 physical_client_height = HIWORD(lparam);

            ui_context->client_width = (u32)ceil(physical_client_width / dpi_scale);
            ui_context->client_height = (u32)ceil(physical_client_height / dpi_scale);
            if (ui_context->client_width > 0 && ui_context->client_height > 0)
                ui_context->on_resize(physical_client_width, physical_client_height);
        } return 0;

        case WM_DPICHANGED:
        {
            ui_context->dpi = GetDpiForWindow(window);
            glyph_cache_deinit(glyph_cache);
            glyph_cache_init(glyph_cache, GLYPHS_LENGTH, app_context->dwrite_factory);
            renderer_recreate_glyph_atlas_texture(&glyph_cache->atlas);

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

#if !defined(NDEBUG) || defined(TRACY_ENABLE)
i32 WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, i32 nShowCmd)
#else
#pragma comment (lib, "libvcruntime")
#pragma comment (lib, "ucrt")
i32 WinMainCRTStartup()
#endif
{
    // Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Init context
    AppContext app_context = {
        .title = L"App Title",
        .ui = {
            .arena = arena_new(MB(16)),
            .dpi = GetDpiForSystem(),
            .client_width = CLIENT_WIDTH,
            .client_height = CLIENT_HEIGHT,
            .box_cache_capacity = BOX_CACHE_CAPACITY,
            .on_resize = swapchain_resize,
            .get_text_width = renderer_get_text_width_for_dpi,
            .get_text_height = renderer_get_text_height_for_dpi,
        },
        .glyph_cache = {
            .arena = arena_new(MB(32)),
            .atlas = {
                .w = GLYPH_ATLAS_WIDTH,
                .h = GLYPH_ATLAS_HEIGHT,
            },
        },
    };
    app_context.ui.box_cache = arena_push(&app_context.ui.arena, sizeof(*app_context.ui.box_cache), _Alignof(UIBox),
                                          app_context.ui.box_cache_capacity);

    // Create window
    {
        f32 dpi_scale = (f32)app_context.ui.dpi / USER_DEFAULT_SCREEN_DPI;
        u32 physical_client_width = (u32)(app_context.ui.client_width * dpi_scale);
        u32 physical_client_height = (u32)(app_context.ui.client_height * dpi_scale);

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
        app_context.window = CreateWindowExW(0, wc.lpszClassName, app_context.title, window_style, rect.left, rect.top,
                                             rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance,
                                             &app_context);
    }

    // Initialize dwrite factory, font, glyph cache and renderer
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (void**)&app_context.dwrite_factory);
    font_register(&app_context.fonts[FONT_INDEX_UI], app_context.dwrite_factory, L"Segoe UI");
    font_register(&app_context.fonts[FONT_INDEX_ZH], app_context.dwrite_factory, L"Microsoft YaHei");
    font_register(&app_context.fonts[FONT_INDEX_MONO], app_context.dwrite_factory, L"Consolas");
    glyph_cache_init(&app_context.glyph_cache, GLYPHS_LENGTH, app_context.dwrite_factory);
    renderer_init(app_context.window, &app_context.glyph_cache.atlas);

    // Render first frame before showing window
    process_frame(&app_context); // Rasterize needed glyphs
    process_frame(&app_context);
    ShowWindow(app_context.window, SW_SHOWDEFAULT);

    // Run message loop
    MSG message;
    while(True)
    {
        if (PeekMessageW(&message, 0, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        TracyCFrameMark;
        process_frame(&app_context);
    }

    // Clean
    renderer_deinit();
    glyph_cache_deinit(&app_context.glyph_cache);
    font_unregister(&app_context.fonts[FONT_INDEX_UI]);
    font_unregister(&app_context.fonts[FONT_INDEX_ZH]);
    font_unregister(&app_context.fonts[FONT_INDEX_MONO]);
    IDWriteFactory3_Release(app_context.dwrite_factory);

    arena_release(&app_context.ui.arena);
    arena_release(&app_context.glyph_cache.arena);

    return 0;
}
