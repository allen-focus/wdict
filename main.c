#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")

#include "glyph_cache.h"
#include "palette.h"
#include "renderer.h"
#include "ui.h"
#include "utils.h"
#include "win32_helper.h"

#include <math.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define CLIENT_WIDTH  600
#define CLIENT_HEIGHT 600

#define MAX_TITLE_LENGTH 64
#define TEXT_BUFFER_SIZE 1024
#define MAX_WINDOWS      16

typedef enum
{
    FONT_INDEX_UI,
    FONT_INDEX_ZH,
    FONT_INDEX_MONO,
    FONT_INDEX_ICON,
    FONT_CAPACITY
} FontIndex;

typedef struct
{
    Color bg_base;
    Color bg_surface;
    Color bg_overlay;

    Color fg_primary;
    Color fg_secondary;
    Color fg_disabled;

    Color border_normal;
    Color border_focus;

    Color accent;
    Color accent_hover;
    Color accent_press;
    Color accent_fg;

    Color danger;
    Color success;
    Color warning;

    Color cursor;
    Color selection;
    Color selection_flash;
    Color cursor_trail;

    Color scrollbar_thumb;
    Color scrollbar_track;

    Color shadow;
} Theme;

typedef struct WindowContext WindowContext;

typedef struct
{
    DWriteContext dwrite;
    Font fonts[FONT_CAPACITY];
    Theme theme;
    GlyphRasterCache raster_cache;
    RendererShared renderer_shared;
    HCURSOR cursors[3];
    WindowContext* first_window;
    WindowContext* last_window;
} AppShared;

struct WindowContext
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    Renderer renderer;
    AppShared* shared;
    WindowContext* next;

    u8 text_buf_1[TEXT_BUFFER_SIZE];
    u8 text_buf_2[TEXT_BUFFER_SIZE];
    TextEditState text_edit_1;
    TextEditState text_edit_2;
};

static u16 s_utf16_pending_high = 0;

//
// Static Variables: Theme
//

// clang-format off
static const Theme s_theme_light = {
    .bg_base         = { LIGHT_1,  255 },
    .bg_surface      = { LIGHT_2,  255 },
    .bg_overlay      = { LIGHT_3,  255 },

    .fg_primary      = { DARK_5,   255 },
    .fg_secondary    = { DARK_1,   255 },
    .fg_disabled     = { LIGHT_5,  255 },

    .border_normal   = { LIGHT_4,  255 },
    .border_focus    = { BLUE_3,   255 },

    .accent          = { BLUE_2,   255 },
    .accent_hover    = { BLUE_3,   255 },
    .accent_press    = { BLUE_4,   255 },
    .accent_fg       = { LIGHT_1,  255 },

    .danger          = { RED_3,    255 },
    .success         = { GREEN_4,  255 },
    .warning         = { ORANGE_3, 255 },

    .cursor          = { DARK_5,   255 },
    .selection       = { BLUE_3,   128 },
    .selection_flash = { YELLOW_3, 255 },
    .cursor_trail    = { DARK_5,   110 },

    .scrollbar_thumb = { DARK_1,   240 },
    .scrollbar_track = { DARK_1,   96  },

    .shadow          = { DARK_5,   255 },
};

static const Theme s_theme_dark = {
    .bg_base         = { DARK_4,   255 },
    .bg_surface      = { DARK_3,   255 },
    .bg_overlay      = { DARK_2,   255 },

    .fg_primary      = { LIGHT_1,  255 },
    .fg_secondary    = { LIGHT_5,  255 },
    .fg_disabled     = { DARK_1,   255 },

    .border_normal   = { DARK_2,   255 },
    .border_focus    = { BLUE_2,   255 },

    .accent          = { BLUE_2,   255 },
    .accent_hover    = { BLUE_3,   255 },
    .accent_press    = { BLUE_4,   255 },
    .accent_fg       = { LIGHT_2,  255 },

    .danger          = { RED_1,    255 },
    .success         = { GREEN_2,  255 },
    .warning         = { ORANGE_1, 255 },

    .cursor          = { LIGHT_1,  255 },
    .selection       = { BLUE_2,   128 },
    .selection_flash = { YELLOW_2, 255 },
    .cursor_trail    = { LIGHT_1,  110 },

    .scrollbar_thumb = { LIGHT_5,  240 },
    .scrollbar_track = { LIGHT_5,  96  },

    .shadow          = { DARK_5,   255 },
};
// clang-format on

//
// Static Variables: Padding & Child Gap
//

// clang-format off
static Padding s_padding_big    = { 30, 30, 30, 30 };
static Padding s_padding_medium = { 20, 20, 20, 20 };
static Padding s_padding_small  = { 10, 10, 10, 10 };

static f32 s_child_gap_big    = 20;
static f32 s_child_gap_medium = 10;
static f32 s_child_gap_small  = 5;
// clang-format on

//
// Helpers
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

static void set_window_title(WindowContext* ctx, const String title)
{
    i32 written =
        MultiByteToWideChar(CP_UTF8, 0, (const char*)title.data, (int)title.len, ctx->title, MAX_TITLE_LENGTH - 1);
    ctx->title[written] = L'\0';
    SetWindowTextW(ctx->window, ctx->title);
}

static void window_list_add(AppShared* shared, WindowContext* ctx)
{
    ctx->next = NULL;
    if (!shared->first_window)
    {
        shared->first_window = shared->last_window = ctx;
    }
    else
    {
        shared->last_window->next = ctx;
        shared->last_window = ctx;
    }
}

static void window_list_remove(AppShared* shared, WindowContext* ctx)
{
    WindowContext** prev = &shared->first_window;
    while (*prev)
    {
        if (*prev == ctx)
        {
            *prev = ctx->next;
            break;
        }
        prev = &(*prev)->next;
    }
    if (!shared->first_window)
        shared->last_window = NULL;
    else
    {
        WindowContext* w = shared->first_window;
        while (w->next)
            w = w->next;
        shared->last_window = w;
    }
}

//
// Window creation
//

static void process_frame(WindowContext* ctx);

static WindowContext* window_create(AppShared* shared, const wchar_t* title, u32 width, u32 height)
{
    WindowContext* ctx = (WindowContext*)malloc(sizeof(WindowContext));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->shared = shared;

    /* Copy title */
    wcsncpy_s(ctx->title, MAX_TITLE_LENGTH, title, _TRUNCATE);

    /* Create window */
    RECT rect = get_screen_center_rect(width, height, GetDpiForSystem());
    DWORD window_style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&rect, window_style, 0, 0);
    ctx->window =
        CreateWindowExW(0, L"window class", ctx->title, window_style, rect.left, rect.top, rect.right - rect.left,
                        rect.bottom - rect.top, NULL, NULL, GetModuleHandleW(NULL), ctx);
    if (!ctx->window)
    {
        free(ctx);
        return NULL;
    }

    /* Init UI context */
    UIRenderFunc render_fn = {
        .flush_and_present = renderer_flush_and_present,
        .on_resize = renderer_resize,
        .wait_for_last_submitted_frame = renderer_wait_for_last_submitted_frame,
        .get_text_width = renderer_get_text_width_for_dpi,
        .get_text_height = renderer_get_text_height_for_dpi,
        .draw_rect = renderer_draw_rect,
        .draw_text = renderer_draw_text,
    };
    ui_init(ctx->window, &ctx->ui, &ctx->renderer, &shared->raster_cache, width, height, GetDpiForSystem(), render_fn);
    ctx->ui.clipboard_copy = win32_clipboard_copy;
    ctx->ui.clipboard_paste = win32_clipboard_paste;

    /* Init per-window renderer */
    renderer_init(&ctx->renderer, &shared->renderer_shared, ctx->window);

    /* Init per-window text edit state */
    ctx->text_edit_1.base = ctx->text_buf_1;
    ctx->text_edit_1.size = TEXT_BUFFER_SIZE;
    ctx->text_edit_2.base = ctx->text_buf_2;
    ctx->text_edit_2.size = TEXT_BUFFER_SIZE;

    /* Add to window list */
    window_list_add(shared, ctx);

    /* Render first frames to rasterize glyphs, then show */
    process_frame(ctx);
    process_frame(ctx);
    SetCursor(shared->cursors[ctx->ui.desired_cursor]);
    ShowWindow(ctx->window, SW_SHOWDEFAULT);

    return ctx;
}

//
// Process frame
//

static void process_frame(WindowContext* ctx)
{
    AppShared* shared = ctx->shared;
    UIContext* ui_context = &ctx->ui;
    const Theme* theme = &shared->theme;
    Font* font_ui = &shared->fonts[FONT_INDEX_UI];
    Font* font_zh = &shared->fonts[FONT_INDEX_ZH];
    Font* font_mono = &shared->fonts[FONT_INDEX_MONO];
    Font* font_symbol = &shared->fonts[FONT_INDEX_ICON];

    TracyCZone(ctx_frame, 1);
    isize arena_pos_backup = ui_frame_begin(ui_context);
    {
        ui_box({
            .sizing = { fixed((f32)ui_context->client_width), fixed((f32)ui_context->client_height) },
            .color = theme->bg_base,
            .padding = s_padding_big,
            .child_gap = s_child_gap_big,
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        })
        {
            ui_box({
                .sizing = { fixed(500), fixed(500) },
                .color = theme->accent,
                .padding = s_padding_medium,
            })
            {
                ui_scrollable_area({ str("scroll area"), (Sizing){ grow({}), grow({}) }, theme->bg_base,
                                     s_padding_small, theme->scrollbar_thumb })
                {

                    ui_box({ .sizing = { fixed(1000), fixed(2000) },
                             .color = theme->bg_surface,
                             .padding = s_padding_small,
                             .child_gap = s_child_gap_medium,
                             .direction = LAYOUT_TOP_TO_BOTTOM })
                    {
                        /* text feild */
                        ui_text_field(&ctx->text_edit_1, str("text##feild"), font_zh, 12, (SizingAxis)fixed(400),
                                      s_padding_small, theme->bg_overlay, theme->border_focus, theme->fg_primary,
                                      theme->scrollbar_thumb, theme->cursor_trail, theme->cursor, theme->selection,
                                      theme->selection_flash);
                        ui_text_field(&ctx->text_edit_2, str("text##feild2"), font_zh, 12, (SizingAxis)fixed(200),
                                      s_padding_small, theme->bg_overlay, theme->border_focus, theme->fg_primary,
                                      theme->scrollbar_thumb, theme->cursor_trail, theme->cursor, theme->selection,
                                      theme->selection_flash);

                        {
                            UISignalFlags flags =
                                ui_button(str("create a new window##new window"), font_mono, 12,
                                          (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent,
                                          theme->accent_fg, theme->accent_hover, theme->accent_press);
                            if (ui_lclicked(flags))
                                window_create(shared, L"New window", CLIENT_WIDTH, CLIENT_HEIGHT);
                        }

                        // clang-format off
                        /* button */
                        ui_box({ .sizing = { fit_grow({}), fit({}) }, .child_gap = 5 })
                        {
                            UISignalFlags flags = ui_button(str("hello##world"), font_zh, 12,
                                                            (Sizing){ fit_grow({ .max = 70 }), fit({}) }, s_padding_small,
                                                            theme->accent, theme->accent_fg, theme->accent_hover, theme->accent_press);
                            if (ui_hovered(flags))
                                ui_box({ .sizing = { fixed(30), fit_grow({}) }, .color = theme->danger }) {}
                            if (ui_lclicked(flags))
                                set_window_title(ctx, str("Left Click"));
                            if (ui_rclicked(flags))
                                set_window_title(ctx, str("Right Click"));
                        }

                        /* text */
                        ui_box({ .sizing = { fixed(400), fit({}) }, .child_gap = 20, .direction = LAYOUT_TOP_TO_BOTTOM })
                        {
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                            ui_text(str("Dream of the Red Chamber has a complicated textual history that scholars have long debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. Cao was a member of a prominent Chinese family that had served the Manchu emperors of the Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, and he may have written drafts of the remaining chapters. These hand-copied manuscripts circulated first among his personal friends and a growing circle of aficionados, then eventually on the open market where they sold for large sums of money."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                            ui_text(str("The first printed version of Dream of the Red Chamber, published by Cheng Weiyuan and Gao E in 1791, contains edits and revisions that Cao never authorized. It is possible that Cao destroyed the last chapters or that at least parts of Cao's original ending were incorporated into the 120 chapter Cheng-Gao versions, with Gao E's \"careful emendations\" of Cao's draft."), &(TextConfig){ .font = font_ui, .font_size = 12, .color = theme->fg_primary, .line_height = 24 });
                        }

                        /* switch box */
                        static b32 check = False;
                        UISignalFlags flags = ui_switchbox(
                            str("switch box"),
                            font_symbol,
                            &check,
                            theme->border_normal,
                            theme->accent_fg,
                            theme->shadow,
                            theme->accent
                        );
                        if (ui_lclicked(flags))
                            check = !check;
                        if (check)
                            ui_box({ .sizing = { fit_grow({}), fixed(50) }, .color = theme->success }) {}
                        // clang-format on
                    }
                }
            }
        }
    }
    ui_frame_end(arena_pos_backup);
    TracyCZoneEnd(ctx_frame);
}

//
// Window procedure
//

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    WindowContext* ctx = NULL;
    AppShared* shared = NULL;
    UIContext* ui_context = NULL;
    {
        if (message == WM_CREATE)
        {
            CREATESTRUCT* create = (CREATESTRUCT*)(lparam);
            ctx = (WindowContext*)(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)ctx);
        }
        else
        {
            LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
            ctx = (WindowContext*)ptr;
        }

        if (ctx)
        {
            shared = ctx->shared;
            ui_context = &ctx->ui;
        }
    }

    // Handle message
    switch (message)
    {
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
            f64 now = ui_context->current_time;
            i32 click_x = GET_X_LPARAM(lparam);
            i32 click_y = GET_Y_LPARAM(lparam);
            f64 double_click_sec = (f64)GetDoubleClickTime() / 1000.0;
            f32 dx = (f32)(click_x - ui_context->last_lclick_pos.x);
            f32 dy = (f32)(click_y - ui_context->last_lclick_pos.y);
            f32 dist = sqrtf(dx * dx + dy * dy);
            if (now - ui_context->last_lclick_time <= double_click_sec &&
                dist <= (f32)GetSystemMetrics(SM_CXDOUBLECLK) * 2.f)
                ui_context->mouse_double_click = True;
            ui_context->last_lclick_time = now;
            ui_context->last_lclick_pos.x = (f32)click_x;
            ui_context->last_lclick_pos.y = (f32)click_y;
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

        case WM_IME_SETCONTEXT:
        {
            if (wparam)
            {
                LPARAM flags = lparam;
                flags &= ~ISC_SHOWUICOMPOSITIONWINDOW;
                return DefWindowProcW(window, message, wparam, flags);
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_IME_STARTCOMPOSITION:
        {
            ui_context->ime_composing = True;
            return 0;
        }

        case WM_IME_COMPOSITION:
        {
            if (lparam & GCS_RESULTSTR)
            {
                String result = win32_ime_get_result(window, &ui_context->arena);
                const byte* p = result.data;
                while ((isize)(p - result.data) < result.len)
                {
                    UnicodeDecode dec = utf8_decode(p);
                    if (ui_context->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
                        ui_context->char_input_queue[ui_context->char_input_queue_count++] = dec.codepoint;
                    p = dec.next_p;
                }
            }
            if (lparam & GCS_COMPSTR)
                ui_context->ime_composition = win32_ime_get_composition(window, &ui_context->arena);
            return 0;
        }

        case WM_IME_ENDCOMPOSITION:
        {
            ui_context->ime_composing = False;
            ui_context->ime_composition = (String){ 0 };
            return 0;
        }

        case WM_IME_REQUEST:
        {
            if (wparam == IMR_CANDIDATEWINDOW)
            {
                CANDIDATEFORM* form = (CANDIDATEFORM*)lparam;
                form->dwIndex = 0;
                form->dwStyle = CFS_CANDIDATEPOS;
                Position pos = ui_context->ime_cursor_screen_pos;
                form->ptCurrentPos.x = (LONG)pos.x;
                form->ptCurrentPos.y = (LONG)pos.y;
                return 1;
            }
            return 0;
        }

        case WM_KEYDOWN:
        {
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(window);
                return 0;
            }

            TextAction action = { 0 };
            b32 ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            b32 shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl)
                action.flags |= TextActionFlag_WordScan;
            if (shift)
                action.flags |= TextActionFlag_KeepMark;

            // clang-format off
            switch (wparam)
            {
                case VK_LEFT:
                {
                    action.delta = -1;
                    if (!shift)
                        action.flags |= TextActionFlag_DeltaPicksSelectionSide;
                } break;
                case VK_RIGHT:
                {
                    action.delta = +1;
                    if (!shift)
                        action.flags |= TextActionFlag_DeltaPicksSelectionSide;
                } break;
                case VK_HOME:  action.delta = -INT64_MAX; break;
                case VK_END:   action.delta = +INT64_MAX; break;
                case VK_BACK:
                {
                    action.delta = -1;
                    action.flags |= TextActionFlag_Delete | TextActionFlag_ZeroDeltaWithSelection;
                } break;
                case VK_DELETE:
                {
                    action.delta = +1;
                    action.flags |= TextActionFlag_Delete | TextActionFlag_ZeroDeltaWithSelection;
                } break;
                case 'A':
                    if (ctrl) action.flags |= TextActionFlag_SelectAll;
                    else return DefWindowProcW(window, message, wparam, lparam); break;
                case 'C': 
                    if (ctrl) action.flags |= TextActionFlag_Copy; 
                    else return DefWindowProcW(window, message, wparam, lparam); break;
                case 'X': 
                    if (ctrl) action.flags |= TextActionFlag_Copy | TextActionFlag_Delete; 
                    else return DefWindowProcW(window, message, wparam, lparam); break;
                case 'V': 
                    if (ctrl) action.flags |= TextActionFlag_Paste; 
                    else return DefWindowProcW(window, message, wparam, lparam); break;
                default: 
                    return DefWindowProcW(window, message, wparam, lparam);
            }
            ui_context->text_action_queue[ui_context->text_action_queue_count++] = action;
            // clang-format on

            Assert(ui_context->text_action_queue_count < TEXT_ACTION_QUEUE_CAPACITY);

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
            if (codepoint >= 0x20 && codepoint != 127 && ui_context->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
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
                ui_context->render_fn.on_resize(ui_context->renderer, physical_client_width, physical_client_height);
            process_frame(ctx);
            SetCursor(shared->cursors[ui_context->desired_cursor]);
            return 0;
        }

        case WM_DPICHANGED:
        {
            ui_context->dpi = GetDpiForWindow(window);
            // Per-window atlas recreated; shared raster cache NOT reset — glyphs at
            // old DPI remain available for other windows while new DPI entries accumulate.
            renderer_recreate_glyph_atlas_texture(ui_context->renderer);

            RECT* const suggested_rect = (RECT*)lparam;
            SetWindowPos(window, NULL, suggested_rect->left, suggested_rect->top,
                         suggested_rect->right - suggested_rect->left, suggested_rect->bottom - suggested_rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_SETTINGCHANGE:
        {
            if (lparam && wcscmp((const wchar_t*)lparam, L"ImmersiveColorSet") == 0)
                shared->theme = win32_get_system_theme() == SYSTEM_THEME_LIGHT ? s_theme_light : s_theme_dark;
            return 0;
        }

        case WM_DESTROY:
        {
            if (shared && ctx)
            {
                // First disconnect the association, so subsequent trailing messages (WM_NCDESTROY, etc.) get NULL
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);

                window_list_remove(shared, ctx);
                ui_deinit(&ctx->ui);
                renderer_deinit(&ctx->renderer);
                free(ctx);
            }
            if (!shared || !shared->first_window)
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
    AppShared shared = { 0 };
    shared.theme = win32_get_system_theme() == SYSTEM_THEME_LIGHT ? s_theme_light : s_theme_dark;

    /* Load cursors */
    shared.cursors[UI_CURSOR_ARROW] = LoadCursor(NULL, IDC_ARROW);
    shared.cursors[UI_CURSOR_IBEAM] = LoadCursor(NULL, IDC_IBEAM);
    shared.cursors[UI_CURSOR_HAND] = LoadCursor(NULL, IDC_HAND);

    /* Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Register window class */
    WNDCLASSW wc = {
        .lpfnWndProc = window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"window class",
    };
    RegisterClassW(&wc);

    /* Initialize font rasterizer */
    dwrite_init(&shared.dwrite);

    /* Initialize shared raster cache */
    raster_cache_init(&shared.dwrite, &shared.raster_cache, GLYPHS_LENGTH);

    /* Initialize shared renderer state */
    renderer_shared_init(&shared.renderer_shared);

    /* Register fonts */
    font_register_from_system(&shared.dwrite, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              &shared.fonts[FONT_INDEX_UI]);
    font_register_from_system(&shared.dwrite, L"Microsoft YaHei", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              &shared.fonts[FONT_INDEX_ZH]);
    font_register_from_system(&shared.dwrite, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              &shared.fonts[FONT_INDEX_MONO]);
    font_register_from_resource(&shared.dwrite, L"ICON_FONT", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                &shared.fonts[FONT_INDEX_ICON]);

    /* Create first window */
    window_create(&shared, L"App Title", CLIENT_WIDTH, CLIENT_HEIGHT);

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
        for (WindowContext* w = shared.first_window; w; w = w->next)
        {
            process_frame(w);
            SetCursor(shared.cursors[w->ui.desired_cursor]);
        }
    }

    /* Clean up remaining windows */
    WindowContext* w = shared.first_window;
    while (w)
    {
        WindowContext* next = w->next;
        DestroyWindow(w->window);
        w = next;
    }

    font_unregister(&shared.fonts[FONT_INDEX_UI]);
    font_unregister(&shared.fonts[FONT_INDEX_ZH]);
    font_unregister(&shared.fonts[FONT_INDEX_MONO]);
    font_unregister(&shared.fonts[FONT_INDEX_ICON]);

    renderer_shared_deinit(&shared.renderer_shared);
    raster_cache_deinit(&shared.raster_cache);
    dwrite_deinit(&shared.dwrite);

    return 0;
}
