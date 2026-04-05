#include "glyph_cache.h"
#include "renderer.h"
#include "ui.h"
#include "utils.h"

#include <math.h>
#include <wchar.h>
#include <windows.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define CLIENT_WIDTH  400
#define CLIENT_HEIGHT 400

#define MAX_TITLE_LENGTH 64

#define FONT_INDEX_UI   0
#define FONT_INDEX_ZH   1
#define FONT_INDEX_MONO 2
#define FONT_CAPACITY   3

///

typedef struct
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    IDWriteFactory3* dwrite_factory;
    Font fonts[FONT_CAPACITY];
} AppContext;

// clang-format off
static Color s_black = { 0,   0,   0,   255 };
static Color s_grey  = { 209, 233, 229, 255 };
static Color s_white = { 255, 255, 255, 255 };
static Color s_red   = { 251, 147, 143, 255 };
static Color s_green = { 253, 216, 77,  255 };
static Color s_blue  = { 94,  203, 228, 255 };

static RectStyle s_round_border_shadow = {
    .border_color = { 253, 216, 77,  255 },
    .corner_radius = 16,
    .border_thickness = 8,
    .enable_shadow = True
};

static Padding s_padding_big    = { 30, 30, 30, 30 };
static Padding s_padding_medium = { 20, 20, 20, 20 };
static Padding s_padding_small  = { 10, 10, 10, 10 };

static f32 s_child_gap_big    = 20;
static f32 s_child_gap_medium = 10;
static f32 s_child_gap_small  = 5;
// clang-format on

//
// Helper
//

static RECT get_screen_center_rect(u32 width, u32 height, u32 dpi)
{
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_width = (u32)(width * dpi_scale);
    u32 physical_height = (u32)(height * dpi_scale);

    i32 screen_width = GetSystemMetrics(SM_CXSCREEN);
    i32 screen_height = GetSystemMetrics(SM_CYSCREEN);
    i32 x = (screen_width - physical_width) / 2;
    i32 y = (screen_height - physical_height) / 2;
    RECT rect = { x, y, x + physical_width, y + physical_height };
    return rect;
}

static void set_app_title(AppContext* app_context, const String title)
{
    i32 written = MultiByteToWideChar(CP_UTF8, 0, (const char*)title.data, (int)title.len, app_context->title,
                                      MAX_TITLE_LENGTH - 1);
    app_context->title[written] = L'\0';
    SetWindowTextW(app_context->window, app_context->title);
}

//
// Process frame
//

static void process_frame(AppContext* app_context)
{

    UIContext* ui_context = &app_context->ui;
    Font font_zh = app_context->fonts[FONT_INDEX_ZH];

    TracyCZone(ctx, 1);
    isize arena_pos_backup = ui_begin_frame(ui_context);
    {
        ui_box({
            .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
            .color = s_white,
            .padding = s_padding_big,
            .child_gap = s_child_gap_big,
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        })
        {
            ui_box({ .sizing = { fixed(300), fixed(200) },
                     .color = s_blue,
                     .padding = s_padding_medium,
                     .enable_clip = True })
            {
                ui_box({ .sizing = { fixed(200), fixed(300) },
                         .color = s_grey,
                         .padding = s_padding_small,
                         .child_gap = s_child_gap_medium,
                         .direction = LAYOUT_TOP_TO_BOTTOM })
                {
                    ui_box({ .sizing = { fit({}), fit({}) }, .child_gap = s_child_gap_small })
                    {
                        UISignalFlags flags = ui_button(str("hello##world"), font_zh);
                        if (ui_hovered(flags))
                            ui_box({ .sizing = { fixed(30), fit_grow({}) }, .color = s_red })
                            {
                            }
                        if (ui_lclicked(flags))
                            set_app_title(app_context, str("Left Click"));
                        if (ui_rclicked(flags))
                            set_app_title(app_context, str("Right Click"));
                    }

                    static b32 check = False;
                    ui_checkbox(str("good##bye"), &check);
                    if (check)
                        ui_box({ .sizing = { fit_grow({}), fixed(50) }, .color = s_green })
                        {
                        }

                    ui_box(
                        { .sizing = { fixed(300), fixed(500) }, .color = s_black, .rect_style = s_round_border_shadow })
                    {
                    }
                }
            }
        }
    }
    ui_end_frame(arena_pos_backup);
    TracyCZoneEnd(ctx);
}

//
// Window procedure
//

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    // Read passing data from param
    AppContext* app_context = NULL;
    UIContext* ui_context = NULL;
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
            ui_context = &app_context->ui;
    }

    // Handle message
    switch (message)
    {
        case WM_MOUSEMOVE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            ui_context->mouse_pos.x = LOWORD(lparam) / dpi_scale;
            ui_context->mouse_pos.y = HIWORD(lparam) / dpi_scale;
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            ui_context->mouse_lclick = True;
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            ui_context->mouse_rclick = True;
            return 0;
        }

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
                DestroyWindow(window);
            return 0;
        }

        case WM_SIZE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_client_width = LOWORD(lparam);
            u32 physical_client_height = HIWORD(lparam);

            ui_context->client_width = (u32)ceil(physical_client_width / dpi_scale);
            ui_context->client_height = (u32)ceil(physical_client_height / dpi_scale);
            if (ui_context->client_width > 0 && ui_context->client_height > 0)
                ui_context->render_fn.on_resize(physical_client_width, physical_client_height);
            process_frame(app_context);
            return 0;
        }

        case WM_DPICHANGED:
        {
            ui_context->dpi = GetDpiForWindow(window);
            glyph_cache_deinit(&ui_context->glyph_cache);
            glyph_cache_init(&ui_context->glyph_cache, GLYPHS_LENGTH, app_context->dwrite_factory);
            renderer_recreate_glyph_atlas_texture(&ui_context->glyph_cache.atlas);

            // Set new window
            RECT* const suggested_rect = (RECT*)lparam;
            SetWindowPos(window, NULL, suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
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

//
// Main
//

#if !defined(NDEBUG) || defined(TRACY_ENABLE)
i32 WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, i32 nShowCmd)
#else
#    pragma comment(lib, "libvcruntime")
#    pragma comment(lib, "ucrt")
i32 WinMainCRTStartup()
#endif
{
    // Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Init context
    AppContext app_context = { .title = L"App Title" };
    UIRenderFunc render_fn = {
        .flush_and_present = renderer_flush_and_present,
        .on_resize = renderer_resize,
        .wait_for_last_submitted_frame = renderer_wait_for_last_submitted_frame,
        .get_text_width = renderer_get_text_width_for_dpi,
        .get_text_height = renderer_get_text_height_for_dpi,
        .draw_rect = renderer_draw_rect,
        .draw_text = renderer_draw_text,
    };
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (void**)&app_context.dwrite_factory);
    font_register(&app_context.fonts[FONT_INDEX_UI], app_context.dwrite_factory, L"Segoe UI");
    font_register(&app_context.fonts[FONT_INDEX_ZH], app_context.dwrite_factory, L"Microsoft YaHei");
    font_register(&app_context.fonts[FONT_INDEX_MONO], app_context.dwrite_factory, L"Consolas");
    ui_init(&app_context.ui, CLIENT_WIDTH, CLIENT_HEIGHT, GetDpiForSystem(), app_context.dwrite_factory, render_fn);

    // Create window
    RECT rect = get_screen_center_rect(app_context.ui.client_width, app_context.ui.client_height, app_context.ui.dpi);
    DWORD window_style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rect, window_style, 0, 0); // set the client position to screen center
    WNDCLASSW wc = {
        .lpfnWndProc = window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"window class",
    };
    RegisterClassW(&wc);
    app_context.window =
        CreateWindowExW(0, wc.lpszClassName, app_context.title, window_style, rect.left, rect.top,
                        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, &app_context);

    // Initialize renderer
    renderer_init(app_context.window, &app_context.ui.glyph_cache.atlas);

    // Render first frame before showing window
    process_frame(&app_context); // rasterize needed glyphs
    process_frame(&app_context);
    ShowWindow(app_context.window, SW_SHOWDEFAULT);

    // Run message loop
    MSG message;
    while (True)
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
    font_unregister(&app_context.fonts[FONT_INDEX_UI]);
    font_unregister(&app_context.fonts[FONT_INDEX_ZH]);
    font_unregister(&app_context.fonts[FONT_INDEX_MONO]);
    IDWriteFactory3_Release(app_context.dwrite_factory);

    return 0;
}
