#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")

#include "glyph_cache.h"
#include "renderer.h"
#include "ui.h"
#include "utils.h"

#include <math.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define CLIENT_WIDTH  600
#define CLIENT_HEIGHT 600

#define MAX_TITLE_LENGTH 64

typedef enum
{
    FONT_INDEX_UI,
    FONT_INDEX_ZH,
    FONT_INDEX_MONO,
    FONT_INDEX_ICON,
    FONT_CAPACITY
} FontIndex;

///

typedef struct
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    DWriteContext dwrite;
    Font fonts[FONT_CAPACITY];
} AppContext;

static u16 s_utf16_pending_high = 0;

// clang-format off
static Color s_black = { 0,   0,   0,   255 };
static Color s_grey  = { 244, 244, 244, 255 };
static Color s_white = { 255, 255, 255, 255 };
static Color s_red   = { 251, 147, 143, 255 };
static Color s_green = { 253, 216, 77,  255 };
static Color s_blue  = { 94,  203, 228, 255 };

static Padding s_padding_big    = { 30, 30, 30, 30 };
static Padding s_padding_medium = { 20, 20, 20, 20 };
static Padding s_padding_small  = { 10, 10, 10, 10 };

static f32 s_child_gap_big    = 20;
static f32 s_child_gap_medium = 10;
static f32 s_child_gap_small  = 5;
// clang-format on

#define TEXT_BUFFER_SIZE 1024
static u8 s_buf_1[TEXT_BUFFER_SIZE] = { 0 };
static u8 s_buf_2[TEXT_BUFFER_SIZE] = { 0 };
static TextEditState s_text_edit_state_1 = { s_buf_1, TEXT_BUFFER_SIZE, 0, 0 };
static TextEditState s_text_edit_state_2 = { s_buf_2, TEXT_BUFFER_SIZE, 0, 0 };

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
    Font* font_ui = &app_context->fonts[FONT_INDEX_UI];
    Font* font_zh = &app_context->fonts[FONT_INDEX_ZH];
    // Font* font_mono = &app_context->fonts[FONT_INDEX_MONO];
    Font* font_symbol = &app_context->fonts[FONT_INDEX_ICON];

    TracyCZone(ctx, 1);
    isize arena_pos_backup = ui_frame_begin(ui_context);
    {
        ui_box({
            .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
            .color = s_white,
            .padding = s_padding_big,
            .child_gap = s_child_gap_big,
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        })
        {
            ui_box({
                .sizing = { fixed(500), fixed(500) },
                .color = s_blue,
                .padding = s_padding_medium,
            })
            {
                ui_scrollable_area({ str("scroll area"), (Sizing){ grow({}), grow({}) }, s_white, s_padding_small,
                                     (Color){ 140, 140, 140, 240 } })
                {

                    ui_box({ .sizing = { fixed(1000), fixed(2000) },
                             .color = s_grey,
                             .padding = s_padding_small,
                             .child_gap = s_child_gap_medium,
                             .direction = LAYOUT_TOP_TO_BOTTOM })
                    {
                        // clang-format off

                         /* text feild */
                         ui_text_field(&s_text_edit_state_1, str("text##feild"), font_zh, 12, (SizingAxis)fixed(400), s_padding_small, s_blue, s_red, s_black);
                         ui_text_field(&s_text_edit_state_2, str("text##feild2"), font_zh, 12, (SizingAxis)fixed(200), s_padding_small, s_blue, s_red, s_black);

                        /* button */
                        ui_box({ .sizing = { fit_grow({}), fit({}) }, .child_gap = s_child_gap_small })
                        {
                            UISignalFlags flags = ui_button(str("hello##world"), font_zh, 12,
                                                            (Sizing){ fit_grow({ .max = 70 }), fit({}) }, s_padding_small,
                                                            (Color){ 120,  220, 255, 255 }, s_black, (Color){ 81,  189, 255, 255 }, (Color){ 46, 143, 255, 255 });
                            if (ui_hovered(flags))
                                ui_box({ .sizing = { fixed(30), fit_grow({}) }, .color = s_red }) {}
                            if (ui_lclicked(flags))
                                set_app_title(app_context, str("Left Click"));
                            if (ui_rclicked(flags))
                                set_app_title(app_context, str("Right Click"));
                        }

                        /* text */
                        ui_box({ .sizing = { fixed(400), fit({}) }, .child_gap = 20, .direction = LAYOUT_TOP_TO_BOTTOM })
                        {
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = s_black, .line_height = 24 });
                        }

                        /* switch box */
                        static b32 check = False;
                        UISignalFlags flags = ui_switchbox(
                            str("switch box"),
                            font_symbol,
                            &check,
                            (Color){ 200, 200, 200, 255 },
                            s_white,
                            (Color){ 46, 143, 255, 255 }
                        );
                        if (ui_lclicked(flags))
                            check = !check;
                        if (check)
                            ui_box({ .sizing = { fit_grow({}), fixed(50) }, .color = s_green }) {}
                        // clang-format on
                    }
                }
            }
        }
    }
    ui_frame_end(arena_pos_backup);
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
        case WM_SETCURSOR:
        {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
            Position mouse_pos_backup = ui_context->mouse_pos;
            ui_context->mouse_pos.x = GET_X_LPARAM(lparam) / dpi_scale;
            ui_context->mouse_pos.y = GET_Y_LPARAM(lparam) / dpi_scale;
            ui_context->mouse_delta.x = ui_context->mouse_pos.x - mouse_pos_backup.x;
            ui_context->mouse_delta.y = ui_context->mouse_pos.y - mouse_pos_backup.y;
            return 0;
        }

        case WM_MOUSEHWHEEL:
        {
            ui_context->mouse_scroll_delta.x += GET_WHEEL_DELTA_WPARAM(wparam) / 10;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            ui_context->mouse_scroll_delta.y += GET_WHEEL_DELTA_WPARAM(wparam) / -10;
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            ui_context->mouse_lclick = True;
            ui_context->mouse_press = True;
            SetCapture(window);
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            ui_context->mouse_rclick = True;
            SetCapture(window);
            return 0;
        }

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        {
            ui_context->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            ui_context->mouse_press = False;
            ReleaseCapture();
        }

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
                DestroyWindow(window);
            return 0;
        }

        case WM_CHAR:
        {
            wchar_t c = (wchar_t)wparam;
            u32 codepoint = 0;
            if (is_high_surrogate(c))
            {
                s_utf16_pending_high = c;
            }
            else if (is_low_surrogate(c))
            {
                Assert(s_utf16_pending_high);
                u16 surrogate_pair[2] = { s_utf16_pending_high, c };
                codepoint = utf16_decode(surrogate_pair).codepoint;
            }
            else
            {
                codepoint = utf16_decode(&c).codepoint;
            }
            if (codepoint && ui_context->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
                ui_context->char_input_queue[ui_context->char_input_queue_count++] = codepoint;
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
            glyph_cache_init(&app_context->dwrite, &ui_context->glyph_cache, GLYPHS_LENGTH);
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
    /* Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Init context */
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

    dwrite_init(&app_context.dwrite);
    font_register_from_system(&app_context.dwrite, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              &app_context.fonts[FONT_INDEX_UI]);
    font_register_from_system(&app_context.dwrite, L"Microsoft YaHei", DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_FONT_STYLE_NORMAL, &app_context.fonts[FONT_INDEX_ZH]);
    font_register_from_system(&app_context.dwrite, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              &app_context.fonts[FONT_INDEX_MONO]);
    font_register_from_resource(&app_context.dwrite, L"ICON_FONT", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                &app_context.fonts[FONT_INDEX_ICON]);
    ui_init(&app_context.dwrite, &app_context.ui, CLIENT_WIDTH, CLIENT_HEIGHT, GetDpiForSystem(), render_fn);

    /* Create window */
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

    /* Initialize renderer */
    renderer_init(app_context.window, &app_context.ui.glyph_cache.atlas);

    /* Render first frame before showing window */
    process_frame(&app_context); // rasterize needed glyphs
    process_frame(&app_context);
    ShowWindow(app_context.window, SW_SHOWDEFAULT);

    /* Run message loop */
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

    /* Clean */
    renderer_deinit();
    font_unregister(&app_context.fonts[FONT_INDEX_UI]);
    font_unregister(&app_context.fonts[FONT_INDEX_ZH]);
    font_unregister(&app_context.fonts[FONT_INDEX_MONO]);
    font_unregister(&app_context.fonts[FONT_INDEX_ICON]);
    dwrite_deinit(&app_context.dwrite);

    return 0;
}
