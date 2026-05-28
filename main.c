#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")
#pragma comment(lib, "shcore")
#pragma comment(lib, "dwmapi")

#include "cmd.h"
#include "dict.h"
#include "glyph_cache.h"
#include "panel.h"
#include "renderer.h"
#include "search.h"
#include "shortcut.h"
#include "thirdparty/zstd/zstd.h"
#include "ui.h"
#include "utils.h"
#include "win32_helper.h"

#include <ShellScalingApi.h>
#include <dwmapi.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

///

#define CLIENT_WIDTH      1000
#define CLIENT_HEIGHT     750
#define MIN_WINDOW_WIDTH  320
#define MIN_WINDOW_HEIGHT 200

#ifdef TRACY_ENABLE
#    define IDLE_WAKE_FRAMES 0x7FFFFFFF
#else
#    define IDLE_WAKE_FRAMES 4
#endif

#define MAX_TITLE_LENGTH      64
#define TEXT_BUFFER_SIZE      1024
#define MAX_WINDOWS           16
#define MAX_DECORATION_SPACER 16
#define CMD_ARENA_SIZE        KB(64)
#define SEARCH_DISPLAY_MAX    100

#define SEARCH_PALETTE_INPUT_HASH_STR "search palette input"
#define PALETTE_CONTEXT_MAX_CHARS     64

typedef enum
{
    FONT_INDEX_UI,
    FONT_INDEX_ZH,
    FONT_INDEX_MONO,
    FONT_INDEX_MDL,
    FONT_INDEX_ICON,
    FONT_CAPACITY
} FontIndex;

typedef enum
{
    TitleBarHot_None,
    TitleBarHot_Minimize,
    TitleBarHot_Maximize,
    TitleBarHot_Close,
    TitleBarHot_Menu,
    TitleBarHot_Search,
} TitleBarHot;

typedef enum
{
    PALETTE_MODE_WORD,
    PALETTE_MODE_DEF,
    PALETTE_MODE_EXAMPLE,
} SearchPaletteMode;

typedef enum
{
    PALETTE_VIEW_EMPTY, /* query is empty */
    PALETTE_VIEW_LOADING, /* mode switching or waiting for worker results */
    PALETTE_VIEW_RESULTS, /* search complete, results available */
    PALETTE_VIEW_NO_MATCH, /* query non-empty but 0 results */
} PaletteViewState;

typedef struct
{
    b32 open;
    f32 t;
    Rect rect;
} OverlayPopup;

typedef struct
{
    /* general */
    Color accent_bg;
    Color accent_fg;
    Color accent_subtle_bg;
    Color accent_subtle_fg;
    Color accent_weak_bg;
    Color accent_weak_fg;

    Color hover_bg;
    Color hover_fg;
    Color active_bg;
    Color active_fg;
    Color press_bg;
    Color press_fg;

    Color destructive_bg;
    Color destructive_fg;
    Color success_bg;
    Color success_fg;
    Color warning_bg;
    Color warning_fg;

    Color hint;
    Color border;
    Color scrollbar_thumb;
    Color scrollbar_track;
    Color shadow;

    /* spacific */
    Color bar_bg;
    Color bar_fg;

    Color panel_bg;
    Color panel_fg;

    Color palette_bg;
    Color palette_fg;

    Color cursor_bar;
    Color cursor_trail;
    Color selection;
    Color selection_flash;
} Theme;

typedef struct WindowContext WindowContext;

typedef struct DragPopup
{
    HWND window;
    UIContext ui;
    Renderer renderer;
} DragPopup;

typedef struct
{
    DWriteContext dwrite;
    Font fonts[FONT_CAPACITY];
    Theme theme;
    GlyphRasterCache raster_cache;
    RendererShared renderer_shared;
    HCURSOR cursors[UI_CURSOR_COUNT];
    WindowContext* first_window;
    WindowContext* last_window;
    Arena cfg_arena;
    Arena cmd_arena;
    CmdRegistry cmd_registry;
    ShortcutRegistry shortcuts;
    CmdQueue cmd_queue;
    SearchState palette_search;
    DictDB dict_db;
    Arena dict_arena;
    volatile b32 dict_ready; // True = dict + search aux + search states fully initialized
    DWORD main_thread_id; // used by startup_dict_thread to PostMessage wake-ups
    DictSearchAuxEntry* search_aux;
    Arena search_aux_arena;

    /* cross-window drag-drop */
    b32 cross_drag_active;
    u8 cross_drag_payload_buf[DRAG_PAYLOAD_MAX];
    isize cross_drag_payload_size;
    u32 cross_drag_source_window_id;

    /* drag hint popup (heap-allocated — UIContext + Renderer are too large for the stack) */
    DragPopup* drag_popup;

    /* accent border color (registry-derived, updated on WM_DWMCOLORIZATIONCOLORCHANGED) */
    Color accent_border_color;
    b32 has_accent_border;
} AppShared;

struct WindowContext
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    Renderer renderer;
    AppShared* shared;
    WindowContext* next;
    u32 id;

    /* panel */
    Panel* root_panel;
    Panel* focused_panel;

    /* widget needed */
    b32 check;

    u8 text_buf_1[TEXT_BUFFER_SIZE];
    u8 text_buf_2[TEXT_BUFFER_SIZE];
    TextEditState text_edit_1;
    TextEditState text_edit_2;

    /* decoration (window-level overlay) */
    Rect decoration_minimize;
    Rect decoration_maximize;
    Rect decoration_close;
    Rect decoration_menu;
    Rect decoration_search;
    f32 decoration_buttons_width;
    f32 decoration_left_buttons_width;
    Rect decoration_spacer_rects[MAX_DECORATION_SPACER];
    isize decoration_spacer_count;
    TitleBarHot tb_hovered_button;

    /* overlay popups */
    OverlayPopup menu_popup;
    OverlayPopup palette_popup;

    /* palette state */
    u8 palette_text_buf[SEARCH_QUERY_BUF];
    TextEditState palette_text_edit;
    i32 palette_selected_index;
    i32 palette_prev_selected_index;
    isize palette_prev_query_len;
    b32 palette_activate_pending;
    SearchPaletteMode palette_search_mode;
    SearchPaletteMode palette_effective_mode;
    volatile LONG palette_switch_version;

    /* guards */
    b32 in_frame;
    b32 mouse_tracked;
};

static DictDB* g_dict_db; /* set at startup, used by extract callbacks */
static DictSearchAuxEntry* g_search_aux; /* set at startup, used by extract callbacks */

static u64 s_search_palette_input_hash;
static u32 s_window_next_id = 1; /* 0 reserved for none */
static u16 s_utf16_pending_high = 0;

//
// Theme
//

// clang-format off
static const Theme s_theme_light = {
    /* general */
    .accent_bg        = rgba(53,  132, 228, 255),
    .accent_fg        = rgba(255, 255, 255, 255),
    .accent_subtle_bg = rgba(98,  160, 234, 255),
    .accent_subtle_fg = rgba(255, 255, 255, 255),
    .accent_weak_bg   = rgba(153, 193, 241, 255),
    .accent_weak_fg   = rgba(255, 255, 255, 255),

    .hover_bg         = rgba(222, 222, 224, 255),
    .hover_fg         = rgba(61,  61,  61,  255),
    .active_bg        = rgba(216, 216, 219, 255),
    .active_fg        = rgba(34,  34,  34,  255),
    .press_bg         = rgba(205, 205, 207, 255),
    .press_fg         = rgba(0,   0,   0,   255),

    .destructive_bg   = rgba(224, 27,  36,  255),
    .destructive_fg   = rgba(255, 255, 255, 255),
    .success_bg       = rgba(46,  194, 126, 255),
    .success_fg       = rgba(255, 255, 255, 255),
    .warning_bg       = rgba(229, 165, 10,  255),
    .warning_fg       = rgba(0,   0,   0,   255),

    .hint             = rgba(119, 118, 123, 255),
    .border           = rgba(192, 191, 188, 255),
    .scrollbar_thumb  = rgba(94,  92,  100, 80 ),
    .scrollbar_track  = rgba(192, 191, 188, 80 ),
    .shadow           = rgba(192, 191, 188, 255),

    /* specific */
    .bar_bg           = rgba(222, 221, 218, 255),
    .bar_fg           = rgba(0,   0,   0,   255),

    .panel_bg         = rgba(255, 255, 255, 255),
    .panel_fg         = rgba(0,   0,   0,   255),

    .palette_bg       = rgba(246, 245, 244, 255),
    .palette_fg       = rgba(0,   0,   0,   255),

    .cursor_bar       = rgba(34,  34,  38,  255),
    .cursor_trail     = rgba(46,  46,  46,  255),
    .selection        = rgba(192, 191, 188, 255),
    .selection_flash  = rgba(144, 83,  0,   255),
};

static const Theme s_theme_dark = {
    /* general */
    .accent_bg        = rgba(53,  132, 228, 255),
    .accent_fg        = rgba(255, 255, 255, 255),
    .accent_subtle_bg = rgba(98,  160, 234, 255),
    .accent_subtle_fg = rgba(255, 255, 255, 255),
    .accent_weak_bg   = rgba(153, 193, 241, 255),
    .accent_weak_fg   = rgba(255, 255, 255, 255),

    .hover_bg         = rgba(61,  61,  64,  255),
    .hover_fg         = rgba(222, 221, 218, 255),
    .active_bg        = rgba(67,  67,  70,  255),
    .active_fg        = rgba(246, 245, 244, 255),
    .press_bg         = rgba(79,  79,  83,  255),
    .press_fg         = rgba(255, 255, 255, 255),

    .destructive_bg   = rgba(192, 28,  40,  255),
    .destructive_fg   = rgba(255, 255, 255, 255),
    .success_bg       = rgba(38,  162, 105, 255),
    .success_fg       = rgba(255, 255, 255, 255),
    .warning_bg       = rgba(205, 147, 9,   255),
    .warning_fg       = rgba(0,   0,   0,   255),

    .hint             = rgba(154, 153, 150, 255),
    .border           = rgba(94,  92,  100, 255),
    .scrollbar_thumb  = rgba(192, 191, 188, 80 ),
    .scrollbar_track  = rgba(94,  92,  100, 80 ),
    .shadow           = rgba(0,   0,   0,   255),

    /* specific */
    .bar_bg           = rgba(61,  61,  61,  255),
    .bar_fg           = rgba(255, 255, 255, 255),

    .panel_bg         = rgba(34,  34,  34,  255),
    .panel_fg         = rgba(255, 255, 255, 255),

    .palette_bg       = rgba(46,  46,  50,  255),
    .palette_fg       = rgba(255, 255, 255, 255),

    .cursor_bar       = rgba(255, 255, 255, 255),
    .cursor_trail     = rgba(246, 245, 244, 255),
    .selection        = rgba(94,  92,  100, 255),
    .selection_flash  = rgba(255, 192, 87,  255),
};

static Padding s_padding_big    = { 20, 20, 20, 20 };
static Padding s_padding_medium = { 10, 10, 10, 10 };

static f32 s_child_gap_big     = 20;
static f32 s_child_gap_medium  = 10;
// clang-format on

//
// Window Creation
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

// Forward declarations for global lookup helpers used by handlers
static void process_frame(WindowContext* ctx);

static WindowContext* create_window(AppShared* shared, const wchar_t* title, i32 pos_x, i32 pos_y, u32 width,
                                    u32 height, b32 add_default_tab)
{
    WindowContext* ctx = (WindowContext*)calloc(1, sizeof(WindowContext));
    Assert(ctx);
    ctx->shared = shared;
    ctx->id = s_window_next_id++;

    /* Copy title */
    wcsncpy_s(ctx->title, MAX_TITLE_LENGTH, title, _TRUNCATE);

    /* Determine target DPI — when creating from drag-drop desktop drop,
       the window may land on a different-DPI monitor. */
    UINT dpi;
    if (pos_x != CW_USEDEFAULT && pos_y != CW_USEDEFAULT)
    {
        HMONITOR hmon = MonitorFromPoint((POINT){ pos_x, pos_y }, MONITOR_DEFAULTTONEAREST);
        UINT dpi_x, dpi_y;
        if (SUCCEEDED(GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)))
            dpi = dpi_x;
        else
            dpi = GetDpiForSystem();
    }
    else
        dpi = GetDpiForSystem();

    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_width = (u32)(width * dpi_scale);
    u32 physical_height = (u32)(height * dpi_scale);

    RECT rect;
    if (pos_x != CW_USEDEFAULT && pos_y != CW_USEDEFAULT)
    {
        rect.left = pos_x;
        rect.top = pos_y;
        rect.right = pos_x + (i32)physical_width;
        rect.bottom = pos_y + (i32)physical_height;
    }
    else
        rect = get_screen_center_rect(width, height, dpi);

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
    /* Custom title bar needed things */
    {
        // Enable immersive dark mode — required to avoid white borders from DwmExtendFrameIntoClientArea.
        // On Windows 10, without it, there would be no border at all unless user enables accent color.
        BOOL use_dark = TRUE;
        DwmSetWindowAttribute(ctx->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));

        // For users with auto-hide taskbar: without this, moving mouse to screen edge won't reveal the taskbar because
        // maximized window blocks it. This property allows taskbar to overlay this window.
        SetPropW(ctx->window, L"NonRudeHWND", (HANDLE)TRUE);
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

    // NOTE: Client width/height have been initialized in WM_NCCALCSIZE, so we just pass it
    ui_init(ctx->window, &ctx->ui, &ctx->renderer, &shared->raster_cache, ctx->ui.client_width, ctx->ui.client_height,
            dpi, render_fn);
    ctx->ui.clipboard_copy = win32_clipboard_copy;
    ctx->ui.clipboard_paste = win32_clipboard_paste;
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;

    /* Init per-window renderer */
    renderer_init(&ctx->renderer, &shared->renderer_shared, ctx->window);

    /* Init per-window switch box check state */
    ctx->check = False;

    /* Init per-window text edit state */
    ctx->text_edit_1.base = ctx->text_buf_1;
    ctx->text_edit_1.size = TEXT_BUFFER_SIZE;
    ctx->text_edit_2.base = ctx->text_buf_2;
    ctx->text_edit_2.size = TEXT_BUFFER_SIZE;

    ctx->palette_text_edit.base = ctx->palette_text_buf;
    ctx->palette_text_edit.size = SEARCH_QUERY_BUF;
    ctx->palette_selected_index = -1;
    ctx->palette_prev_selected_index = -1;
    ctx->palette_prev_query_len = 0;
    ctx->palette_activate_pending = False;
    ctx->palette_search_mode = PALETTE_MODE_WORD;
    ctx->palette_effective_mode = PALETTE_MODE_WORD;
    ctx->palette_switch_version = 0;

    /* Add to window list */
    window_list_add(shared, ctx);

    /* Init per-window root panel */
    ctx->root_panel = panel_alloc();
    ctx->root_panel->pct_of_parent = 1.0f;
    if (add_default_tab)
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(ctx->root_panel, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(ctx->root_panel, (String){ name_buf, name_len });
    }
    ctx->focused_panel = ctx->root_panel;

    /* Render first frames to rasterize glyphs, then show */
    process_frame(ctx);
    process_frame(ctx);
    ShowWindow(ctx->window, SW_SHOWDEFAULT);

    return ctx;
}

//
// Window Dragging
//

static void cross_window_sync_to(AppShared* shared, WindowContext* ctx, const POINT cursor_pt)
{
    TracyCZoneNC(ctx_cws, "CrossWindowSync", TracyColor_Drag, TRACY_SUBSYSTEMS & TracySys_Drag);
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
    ctx->ui.mouse_press = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    ctx->ui.drag_active = True;
    memcpy(ctx->ui.drag_payload_buf, shared->cross_drag_payload_buf, shared->cross_drag_payload_size);
    ctx->ui.drag_payload_size = shared->cross_drag_payload_size;
    TracyCZoneEnd(ctx_cws);
}

static LRESULT CALLBACK drag_popup_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE)
    {
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static void drag_popup_render(AppShared* shared)
{
    TracyCZoneNC(ctx_dpr, "DragPopupRender", TracyColor_Drag, TRACY_SUBSYSTEMS & TracySys_Drag);
    DragPopup* popup = shared->drag_popup;
    UIContext* ui = &popup->ui;
    String title = { 0 };

    if (shared->cross_drag_active && shared->cross_drag_payload_size >= (isize)sizeof(TabDragPayload))
    {
        TabDragPayload* pld = (TabDragPayload*)shared->cross_drag_payload_buf;
        if (pld->drag_type == DRAG_TYPE_TAB && pld->title[0])
            title = (String){ (u8*)pld->title, (isize)strlen(pld->title) };
    }

    isize arena_pos = ui_frame_begin(ui);
    {
        f32 cw = (f32)ui->client_width;
        f32 ch = (f32)ui->client_height;

        UIBox* box = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(cw), fixed(ch) },
            .color = shared->theme.panel_bg,
            .rect_style = { .border_color = shared->theme.border, .border_thickness = 1 },
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        });
        {
            if (title.data)
                ui_text(title, &(TextConfig){
                                   .font = &shared->fonts[FONT_INDEX_UI],
                                   .font_size = 12.f,
                                   .color = shared->theme.panel_fg,
                                   .line_height = ch,
                               });
        }
        ui_box_end(box);
    }
    ui_frame_end(arena_pos);
    TracyCZoneEnd(ctx_dpr);
}

static void drag_popup_apply_size(DragPopup* popup, const u32 logical_w, const u32 logical_h, const u32 dpi)
{
    if (logical_w == (u32)popup->ui.client_width && logical_h == (u32)popup->ui.client_height && dpi == popup->ui.dpi)
        return;

    popup->ui.client_width = logical_w;
    popup->ui.client_height = logical_h;
    popup->ui.dpi = dpi;

    f32 scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    u32 phys_w = (u32)(logical_w * scale);
    u32 phys_h = (u32)(logical_h * scale);
    SetWindowPos(popup->window, NULL, 0, 0, (i32)phys_w, (i32)phys_h,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
    renderer_resize(&popup->renderer, phys_w, phys_h);
}

static void drag_popup_update(AppShared* shared)
{
    TracyCZoneNC(ctx_dpu, "DragPopupUpdate", TracyColor_Drag, TRACY_SUBSYSTEMS & TracySys_Drag);
    b32 should_show = shared->cross_drag_active && shared->cross_drag_payload_size >= (isize)sizeof(TabDragPayload) &&
                      ((TabDragPayload*)shared->cross_drag_payload_buf)->drag_type == DRAG_TYPE_TAB;

    if (should_show)
    {
        POINT pt;
        GetCursorPos(&pt);

        TabDragPayload* pld = (TabDragPayload*)shared->cross_drag_payload_buf;
        String title = { 0 };
        if (pld->title[0])
            title = (String){ (u8*)pld->title, (isize)strlen(pld->title) };

        /* Compute popup logical size matching the source tab's box layout:
           padding = 10 left + 10 right, child_gap = 4, close button ≈ 13 px */
        f32 text_w = 0.f;
        if (title.data)
        {
            Font* font = &shared->fonts[FONT_INDEX_UI];
            const f32 font_size = 12.f;
            u32 dpi = shared->drag_popup ? shared->drag_popup->ui.dpi : GetDpiForSystem();
            text_w = renderer_get_text_width_for_dpi(NULL, &shared->raster_cache, title, font, font_size, dpi);
        }
        u32 logical_w = (u32)(text_w + 10.f + 10.f + 4.f + 13.f + 0.5f);
        u32 logical_h = 29;

        /* Create the popup on first use */
        if (!shared->drag_popup)
        {
            DragPopup* popup = (DragPopup*)calloc(1, sizeof(DragPopup));
            Assert(popup);
            shared->drag_popup = popup;

            u32 dpi = GetDpiForSystem();
            f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;

            popup->window =
                CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"UIDragPopup",
                                L"", WS_POPUP, pt.x + 12, pt.y + 12, (i32)(logical_w * dpi_scale),
                                (i32)(logical_h * dpi_scale), NULL, NULL, GetModuleHandleW(NULL), shared);
            Assert(popup->window);

            UIRenderFunc render_fn = {
                .flush_and_present = renderer_flush_and_present,
                .on_resize = renderer_resize,
                .wait_for_last_submitted_frame = renderer_wait_for_last_submitted_frame,
                .get_text_width = renderer_get_text_width_for_dpi,
                .get_text_height = renderer_get_text_height_for_dpi,
                .draw_rect = renderer_draw_rect,
                .draw_text = renderer_draw_text,
            };
            ui_init(popup->window, &popup->ui, &popup->renderer, &shared->raster_cache, logical_w, logical_h, dpi,
                    render_fn);
            renderer_init(&popup->renderer, &shared->renderer_shared, popup->window);
        }

        DragPopup* popup = shared->drag_popup;

        /* Reposition to cursor + offset */
        SetWindowPos(popup->window, HWND_TOPMOST, pt.x + 12, pt.y + 12, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);

        /* DPI may have changed if the popup crossed monitors */
        u32 dpi = GetDpiForWindow(popup->window);

        /* Apply DPI awared size and render */
        drag_popup_apply_size(popup, logical_w, logical_h, dpi);
        drag_popup_render(shared);
    }
    else if (shared->drag_popup)
        ShowWindow(shared->drag_popup->window, SW_HIDE);
    TracyCZoneEnd(ctx_dpu);
}

//
// Find (window, panel, tab)
//

static WindowContext* find_window_by_id(AppShared* shared, u32 id)
{
    for (WindowContext* w = shared->first_window; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

static Panel* find_panel_globally(AppShared* shared, u32 window_id, u32 panel_id)
{
    for (WindowContext* w = shared->first_window; w; w = w->next)
    {
        if (window_id && w->id != window_id)
            continue;
        Panel* p = panel_find_by_id(w->root_panel, panel_id);
        if (p)
            return p;
    }
    return NULL;
}

static PanelTab* find_tab_globally(AppShared* shared, u32 window_id, u32 tab_id)
{
    for (WindowContext* w = shared->first_window; w; w = w->next)
    {
        if (window_id && w->id != window_id)
            continue;
        PanelTab* t = panel_find_tab_by_id(w->root_panel, tab_id);
        if (t)
            return t;
    }
    return NULL;
}

//
// Command
//

static void cmd_create_window(void* userdata, String cmd_text)
{
    u32 w = cmd_parse_u32(cmd_text, str("w"), CLIENT_WIDTH);
    u32 h = cmd_parse_u32(cmd_text, str("h"), CLIENT_HEIGHT);
    create_window((AppShared*)userdata, L"Window", CW_USEDEFAULT, CW_USEDEFAULT, w, h, True);
}

static void cmd_toggle_theme(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    shared->theme = (shared->theme.border.r == s_theme_dark.border.r) ? s_theme_light : s_theme_dark;
}

static void cmd_toggle_menu(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* ctx = find_window_by_id(shared, window_id);
    if (!ctx)
        return;

    ctx->menu_popup.open = !ctx->menu_popup.open;
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
}

static void cmd_toggle_palette(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    if (!shared->dict_ready)
        return;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* ctx = find_window_by_id(shared, window_id);
    if (!ctx)
        return;

    ctx->palette_popup.open = !ctx->palette_popup.open;
    if (ctx->palette_popup.open)
    {
        ctx->ui.focused_hash = s_search_palette_input_hash;
        ctx->palette_selected_index = 0;
        ctx->palette_search_mode = PALETTE_MODE_WORD;
        ctx->palette_switch_version = 0;

        // Auto select all text
        ctx->palette_text_edit.cursor = ctx->palette_text_edit.text_len;
        ctx->palette_text_edit.mark = 0;
    }
    else
    {
        ctx->ui.focused_hash = 0;
        ctx->palette_selected_index = -1;
        ctx->palette_prev_selected_index = -1;
        ctx->palette_activate_pending = False;
        ctx->palette_search_mode = PALETTE_MODE_WORD;
        ctx->palette_switch_version = 0;
    }
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
}

static void cmd_close_palette(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;

    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* ctx = find_window_by_id(shared, window_id);
    if (!ctx)
        return;

    ctx->palette_popup.open = False;
    ctx->ui.focused_hash = 0;
    ctx->palette_selected_index = -1;
    ctx->palette_prev_selected_index = -1;
    ctx->palette_activate_pending = False;
    ctx->palette_search_mode = PALETTE_MODE_WORD;
    ctx->palette_switch_version = 0;
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
}

static void cmd_split_panel_h(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    Panel* p = find_panel_globally(shared, window_id, panel_id);
    if (p && panel_split(p, Axis2_Y, True))
    {
        /* The split panel is now an internal node — redirect any window
           that had it focused to child_a (which inherited the original tabs). */
        for (WindowContext* w = shared->first_window; w; w = w->next)
            if (w->focused_panel == p)
                w->focused_panel = p->child_a;
    }
}

static void cmd_split_panel_v(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    Panel* p = find_panel_globally(shared, window_id, panel_id);
    if (p && panel_split(p, Axis2_X, True))
    {
        for (WindowContext* w = shared->first_window; w; w = w->next)
            if (w->focused_panel == p)
                w->focused_panel = p->child_a;
    }
}

static void cmd_close_panel(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    Panel* p = find_panel_globally(shared, window_id, panel_id);
    if (p)
    {
        if (!p->parent)
        {
            WindowContext* w = find_window_by_id(shared, window_id);
            if (w)
                DestroyWindow(w->window);
            return;
        }
        p->pending_remove = True;
    }
}

typedef struct
{
    u32 window_id;
    Panel* panel;
    PanelTab* tab;
} ResolvePanelTabResult;

static ResolvePanelTabResult resolve_panel_and_tab(AppShared* shared, String cmd_text)
{
    ResolvePanelTabResult r = { 0 };
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    if (window_id)
        r.window_id = window_id;
    u32 pid = cmd_parse_u32(cmd_text, str("panel"), 0);
    if (pid)
        r.panel = find_panel_globally(shared, window_id, pid);
    u32 tid = cmd_parse_u32(cmd_text, str("tab"), 0);
    if (tid)
        r.tab = find_tab_globally(shared, window_id, tid);
    return r;
}

static Panel* resolve_target_panel(AppShared* shared, String cmd_text)
{
    u32 window_id = cmd_parse_u32(cmd_text, str("to_window"), cmd_parse_u32(cmd_text, str("window"), 0));
    u32 pid = cmd_parse_u32(cmd_text, str("to_panel"), 0);
    return pid ? find_panel_globally(shared, window_id, pid) : NULL;
}

static void cmd_tab_new(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);

    if (rt.panel)
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(rt.panel, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(rt.panel, (String){ name_buf, name_len });
    }
}

static void cmd_tab_close(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);

    if (!rt.panel || !rt.tab)
        return;

    /* If this is the last remaining tab in the panel, determine whether
       to close the panel or destroy the window */
    if (rt.panel->tab_first && !rt.panel->tab_first->next && rt.panel->tab_first == rt.tab)
    {
        if (!rt.panel->parent)
        {
            WindowContext* w = find_window_by_id(shared, rt.window_id);
            if (w)
                DestroyWindow(w->window);
            return;
        }

        rt.panel->pending_remove = True;
    }
    else
    {
        panel_tab_close(rt.panel, rt.tab);
    }
}

static void cmd_tab_activate(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);

    if (rt.panel && rt.tab)
        panel_tab_activate(rt.panel, rt.tab);
}

static void cmd_tab_move(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);

    if (!rt.panel || !rt.tab)
        return;
    i32 delta = cmd_parse_i32(cmd_text, str("delta"), 0);
    panel_tab_move(rt.panel, rt.tab, delta);
}

static void cmd_tab_move_to_panel(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);
    Panel* to_panel = resolve_target_panel(shared, cmd_text);
    i32 to_idx = cmd_parse_i32(cmd_text, str("to_idx"), -1);

    if (!rt.panel || !to_panel || !rt.tab)
        return;
    panel_tab_move_to_panel(rt.panel, rt.tab, to_panel, to_idx);

    /* If the source panel was the root panel and is now empty,
       the window has no tabs left — close it */
    if (!rt.panel->parent && !rt.panel->tab_first)
    {
        u32 from_window_id = cmd_parse_u32(cmd_text, str("window"), 0);
        if (from_window_id)
        {
            WindowContext* from_win = find_window_by_id(shared, from_window_id);
            if (from_win)
                DestroyWindow(from_win->window);
        }
    }
}

static void cmd_tab_move_to_new_window(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);

    if (!rt.panel || !rt.tab)
        return;

    /* Parse optional position & size */
    i32 pos_x = cmd_parse_i32(cmd_text, str("pos_x"), CW_USEDEFAULT);
    i32 pos_y = cmd_parse_i32(cmd_text, str("pos_y"), CW_USEDEFAULT);
    u32 w = cmd_parse_u32(cmd_text, str("w"), CLIENT_WIDTH);
    u32 h = cmd_parse_u32(cmd_text, str("h"), CLIENT_HEIGHT);

    WindowContext* new_win = create_window(shared, L"Window", pos_x, pos_y, w, h, False);
    if (!new_win)
        return;

    panel_tab_move_to_panel(rt.panel, rt.tab, new_win->root_panel, -1);

    /* Close source window if it became empty */
    if (!rt.panel->parent && !rt.panel->tab_first)
    {
        u32 from_window_id = cmd_parse_u32(cmd_text, str("window"), 0);
        if (from_window_id)
        {
            WindowContext* from_win = find_window_by_id(shared, from_window_id);
            if (from_win)
                DestroyWindow(from_win->window);
        }
    }
}

static void cmd_tab_to_new_panel(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);
    Panel* to_panel = resolve_target_panel(shared, cmd_text);

    if (!rt.panel || !to_panel || !rt.tab)
        return;
    Axis2 axis = (Axis2)cmd_parse_axis(cmd_text, str("axis"), Axis2_X);
    String side_str = cmd_parse_string(cmd_text, str("side"), str("after"));
    PanelDockSide side = str_compare(side_str, str("before")) ? PanelDockSide_Before : PanelDockSide_After;
    panel_tab_to_new_panel(rt.panel, rt.tab, to_panel, axis, side);

    /* The anchor panel (to_panel) is now an internal node — redirect any window
       that had it focused to the child that retained the original tabs. */
    for (WindowContext* w = shared->first_window; w; w = w->next)
    {
        if (w->focused_panel == to_panel)
        {
            w->focused_panel = (side == PanelDockSide_Before) ? to_panel->child_b : to_panel->child_a;
        }
    }

    /* If the source was the root leaf panel and is now empty, close the window */
    if (!rt.panel->child_a && !rt.panel->parent && !rt.panel->tab_first)
    {
        u32 from_window_id = cmd_parse_u32(cmd_text, str("window"), 0);
        if (from_window_id)
        {
            WindowContext* from_win = find_window_by_id(shared, from_window_id);
            if (from_win)
                DestroyWindow(from_win->window);
        }
    }
}

static void cmd_panel_focus_next(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel)
        return;

    if (w->focused_panel)
        w->focused_panel = panel_find_next_leaf(w->root_panel, w->focused_panel);
    else
        w->focused_panel = panel_find_first_leaf(w->root_panel);
}

static void cmd_panel_focus_prev(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel)
        return;

    if (w->focused_panel)
        w->focused_panel = panel_find_prev_leaf(w->root_panel, w->focused_panel);
    else
        w->focused_panel = panel_find_first_leaf(w->root_panel);
}

static void cmd_panel_focus_left(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel || !w->focused_panel)
        return;
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    Panel* next = panel_find_spatial(w->root_panel, w->focused_panel, root_rect, PanelSpatial_Left);
    if (next)
        w->focused_panel = next;
}

static void cmd_panel_focus_down(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel || !w->focused_panel)
        return;
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    Panel* next = panel_find_spatial(w->root_panel, w->focused_panel, root_rect, PanelSpatial_Down);
    if (next)
        w->focused_panel = next;
}

static void cmd_panel_focus_up(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel || !w->focused_panel)
        return;
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    Panel* next = panel_find_spatial(w->root_panel, w->focused_panel, root_rect, PanelSpatial_Up);
    if (next)
        w->focused_panel = next;
}

static void cmd_panel_focus_right(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->root_panel || !w->focused_panel)
        return;
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    Panel* next = panel_find_spatial(w->root_panel, w->focused_panel, root_rect, PanelSpatial_Right);
    if (next)
        w->focused_panel = next;
}

static void cmd_panel_resize_left(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->focused_panel)
        return;
    f32 step = (f32)cmd_parse_i32(cmd_text, str("step"), 20);
    step = clamp(step, 1.f, 500.f);
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    panel_resize_pixel(w->root_panel, root_rect, w->focused_panel, PanelSpatial_Left, step);
}

static void cmd_panel_resize_down(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->focused_panel)
        return;
    f32 step = (f32)cmd_parse_i32(cmd_text, str("step"), 20);
    step = clamp(step, 1.f, 500.f);
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    panel_resize_pixel(w->root_panel, root_rect, w->focused_panel, PanelSpatial_Down, step);
}

static void cmd_panel_resize_up(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->focused_panel)
        return;
    f32 step = (f32)cmd_parse_i32(cmd_text, str("step"), 20);
    step = clamp(step, 1.f, 500.f);
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    panel_resize_pixel(w->root_panel, root_rect, w->focused_panel, PanelSpatial_Up, step);
}

static void cmd_panel_resize_right(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->focused_panel)
        return;
    f32 step = (f32)cmd_parse_i32(cmd_text, str("step"), 20);
    step = clamp(step, 1.f, 500.f);
    Rect root_rect = { 0, 0, (f32)w->ui.client_width, (f32)w->ui.client_height - 1 };
    panel_resize_pixel(w->root_panel, root_rect, w->focused_panel, PanelSpatial_Right, step);
}

//
// Palette navigation
//

typedef enum
{
    PALETTE_NAV_NONE,
    PALETTE_NAV_UP,
    PALETTE_NAV_DOWN,
    PALETTE_NAV_CONFIRM,
} PaletteNavAction;

static PaletteNavAction palette_nav_from_key(u32 vk, b32 ctrl)
{
    switch (vk)
    {
        case VK_UP:
            return PALETTE_NAV_UP;
        case VK_DOWN:
            return PALETTE_NAV_DOWN;
        case VK_RETURN:
            return PALETTE_NAV_CONFIRM;
        case 'P':
            if (ctrl)
                return PALETTE_NAV_UP;
            break;
        case 'N':
            if (ctrl)
                return PALETTE_NAV_DOWN;
            break;
    }
    return PALETTE_NAV_NONE;
}

static PaletteNavAction palette_nav_from_scroll(f32 scroll_delta_y, b32 hovered)
{
    if (scroll_delta_y != 0 && hovered)
        return (scroll_delta_y > 0) ? PALETTE_NAV_DOWN : PALETTE_NAV_UP;
    return PALETTE_NAV_NONE;
}

static i32 palette_nav_apply(i32 selected, i32 count, PaletteNavAction action, b32* out_activate_pending)
{
    if (action == PALETTE_NAV_NONE)
        return selected;
    if (action == PALETTE_NAV_CONFIRM)
    {
        if (selected >= 0 && out_activate_pending)
            *out_activate_pending = True;
        return selected;
    }
    selected += (action == PALETTE_NAV_DOWN) ? 1 : -1;
    if (count > 0)
        return (selected % count + count) % count;
    return selected;
}

//
// Palette Row Interaction
//

typedef struct
{
    b32 hovered;
    b32 clicked;
    f32 height;
} PaletteRowResult;

static PaletteRowResult palette_row_interact(UIBox* row, String hash_str, const Theme* theme, b32 is_selected)
{
    UIBoxInteractResult rir = ui_box_interact(row, hash_str);
    PaletteRowResult result = { 0 };
    result.hovered = ui_hovered(rir.flags);
    result.clicked = ui_lclicked(rir.flags);
    if (rir.last_box)
        result.height = rir.last_box->size.height;

    if (result.hovered)
        row->cfg.color = theme->hover_bg;
    else if (is_selected)
        row->cfg.color = theme->active_bg;

    return result;
}

//
// Palette Display Item
//

typedef struct
{
    const DictWordIndex* entry;
    String word_text;
    String display_context; // def or example snippet; zero-length if not applicable
    f32 score;
    i32 word_range_count;
    FuzzyRange word_ranges[FUZZY_MAX_RANGES];
    i32 ctx_range_count;
    FuzzyRange ctx_ranges[FUZZY_MAX_RANGES];
    b32 has_context;
} PaletteDisplayItem;

//
// Locate which AuxSegment contains a given byte offset (binary search).
// Returns the index, or -1 if the offset falls outside all segments.
// If offset lands in a separator gap between segments, returns the segment
// immediately before it so the caller still gets a meaningful fallback.
//
static i32 aux_segment_locate(const AuxSegment* segs, i32 seg_count, u32 offset)
{
    if (seg_count == 0)
        return -1;
    i32 lo = 0, hi = seg_count - 1;
    while (lo <= hi)
    {
        i32 mid = (lo + hi) / 2;
        u32 seg_start = segs[mid].offset;
        u32 seg_end = segs[mid].offset + segs[mid].len;
        if (offset < seg_start)
            hi = mid - 1;
        else if (offset >= seg_end)
            lo = mid + 1;
        else
            return mid;
    }
    /* offset falls in a separator gap or beyond boundaries */
    if (hi >= 0)
        return hi; /* segment immediately before gap */
    if (lo < seg_count)
        return lo; /* segment immediately after (rare) */
    return -1;
}

//
// Snap a byte offset backward to the start of the UTF-8 character it
// falls in.  Bytes 0x80-0xBF are continuation bytes — walk left until a
// start byte.
//
static i32 utf8_back_to_char_start(const u8* data, i32 offset, i32 min_bound)
{
    while (offset > min_bound && (data[offset] & 0xC0) == 0x80)
        offset--;
    return offset;
}

//
// Snap a byte offset forward past continuation bytes to the next character
// boundary.  Already at a start byte → return unchanged.
//
static i32 utf8_fwd_to_char_end(const u8* data, i32 offset, i32 max_bound)
{
    while (offset < max_bound && (data[offset] & 0xC0) == 0x80)
        offset++;
    return offset;
}

//
// Convert raw SearchResult[] into rich PaletteDisplayItem[]
// using the pre‑built DictSearchAux segment tables.
//
// For WORD mode  → word_text = entry's word, no context.
// For DEF/EX/ALL → locate the matching segment, normalise ranges,
//                  derive display_context.
// No allocation — all strings point into pre‑existing memory.
//
static i32 palette_build_display_items(const SearchResult* sr, i32 sr_count, SearchPaletteMode mode, i32 max_items,
                                       PaletteDisplayItem* out)
{
    const DictDB* db = g_dict_db;
    const DictSearchAuxEntry* aux = g_search_aux;
    i32 count = sr_count < max_items ? sr_count : max_items;

    for (i32 i = 0; i < count; i++)
    {
        const DictWordIndex* w = (const DictWordIndex*)sr[i].entry;
        i32 idx = (i32)(w - db->words);
        PaletteDisplayItem* item = &out[i];
        memset(item, 0, sizeof(*item));

        item->entry = w;
        item->score = sr[i].score;

        const char* wrd = DICT_STR(db, w->word_stroff);
        item->word_text = (String){ (u8*)wrd, (isize)strlen(wrd) };

        if (mode == PALETTE_MODE_WORD)
        {
            /* ranges are byte offsets into the word string */
            item->word_range_count = sr[i].range_count;
            memcpy(item->word_ranges, sr[i].ranges, (usize)sr[i].range_count * sizeof(FuzzyRange));

            /* Set display_context to first brief_zh entry if available */
            {
                const u8* p = db->entdata + w->entdata_off;
                p += 4; /* skip freq */
                dict_skip_brief_array(&p); /* skip brief_en */
                u8 zh_cnt = dict_rd_u8(&p);
                if (zh_cnt > 0)
                {
                    u32 zh_off = dict_rd_u32(&p);
                    const char* zh_str = DICT_STR(db, zh_off);
                    item->display_context = (String){ (u8*)zh_str, (isize)strlen(zh_str) };
                    item->has_context = True;
                    item->ctx_range_count = 0;
                }
            }
            continue;
        }

        /* ── DEF / EXAMPLE modes: determine which field matched, then locate context segment ── */
        const AuxSegment* segs;
        i32 seg_count;
        const u8* search_text;

        if (mode == PALETTE_MODE_DEF)
        {
            segs = aux[idx].def_segs;
            seg_count = aux[idx].def_seg_count;
            search_text = (const u8*)aux[idx].def_search_text;
        }
        else /* PALETTE_MODE_EXAMPLE */
        {
            segs = aux[idx].ex_segs;
            seg_count = aux[idx].ex_seg_count;
            search_text = (const u8*)aux[idx].ex_search_text;
        }

        /* Verify the result's ref_text matches what we expect. */
        if (sr[i].range_count == 0 || seg_count == 0)
        {
            item->has_context = False;
            continue;
        }

        /* Locate the segment that contains the first highlight range.
           If ranges[0] falls in a separator gap, try subsequent ranges. */
        i32 seg_idx = -1;
        for (i32 r = 0; r < sr[i].range_count; r++)
        {
            seg_idx = aux_segment_locate(segs, seg_count, (u32)sr[i].ranges[r].start);
            if (seg_idx >= 0)
                break;
        }

        if (seg_idx < 0)
        {
            /* No segment covers the hit — fallback: show word only */
            item->has_context = False;
            continue;
        }

        const AuxSegment* seg = &segs[seg_idx];
        item->display_context.data = (u8*)search_text + seg->offset;
        item->display_context.len = seg->len;
        item->has_context = True;

        /* Normalise ranges to be segment-local */
        item->ctx_range_count = 0;
        for (i32 r = 0; r < sr[i].range_count && item->ctx_range_count < FUZZY_MAX_RANGES; r++)
        {
            i32 r_start = sr[i].ranges[r].start;
            i32 r_end = sr[i].ranges[r].end;
            /* Only include ranges that overlap with this segment */
            if (r_end <= (i32)seg->offset || r_start >= (i32)(seg->offset + seg->len))
                continue;
            i32 local_start = r_start - (i32)seg->offset;
            i32 local_end = r_end - (i32)seg->offset;
            if (local_start < 0)
                local_start = 0;
            if (local_end > (i32)seg->len)
                local_end = (i32)seg->len;
            if (local_end == (i32)seg->len && local_end > 0)
                local_end--; /* never point at the terminating \0 */

            /* Snap to UTF-8 character boundaries — byte-level clipping may
               split a multi-byte codepoint (common with CJK in example text). */
            local_start = utf8_back_to_char_start(search_text + seg->offset, local_start, 0);
            local_end = utf8_fwd_to_char_end(search_text + seg->offset, local_end, seg->len);
            if (local_end > local_start)
            {
                item->ctx_ranges[item->ctx_range_count].start = local_start;
                item->ctx_ranges[item->ctx_range_count].end = local_end;
                item->ctx_range_count++;
            }
        }
        /* If no ranges survived normalisation, still show the context
           without highlights — better than an empty row. */
    }

    return count;
}

//
// Dictionary Callbacks
//

static String dict_word_extract(const void* entry)
{
    if (!g_dict_db)
        return (String){ 0 };
    const DictWordIndex* w = (const DictWordIndex*)entry;
    const char* s = DICT_STR(g_dict_db, w->word_stroff);
    return (String){ (u8*)s, (isize)strlen(s) };
}

static String dict_def_extract(const void* entry)
{
    if (!g_dict_db || !g_search_aux)
        return (String){ 0 };
    const DictWordIndex* w = (const DictWordIndex*)entry;
    i32 idx = (i32)(w - g_dict_db->words);
    return (String){ (u8*)g_search_aux[idx].def_search_text, g_search_aux[idx].def_len };
}

static String dict_ex_extract(const void* entry)
{
    if (!g_dict_db || !g_search_aux)
        return (String){ 0 };
    const DictWordIndex* w = (const DictWordIndex*)entry;
    i32 idx = (i32)(w - g_dict_db->words);
    return (String){ (u8*)g_search_aux[idx].ex_search_text, g_search_aux[idx].ex_len };
}

static FieldDef s_dict_fields[] = { { "word", dict_word_extract, 1.0f } };

static FieldDef s_dict_fields_def[] = {
    { "def", dict_def_extract, 1.0f },
};

static FieldDef s_dict_fields_ex[] = {
    { "ex", dict_ex_extract, 1.0f },
};

static f32 dict_freq_weight(const void* entry, f32 raw)
{
    const DictWordIndex* w = (const DictWordIndex*)entry;

    // freq is a rank: lower value = more frequent word.
    u32 freq = w->freq;
    f32 freq_score;

    // Missing frequency data → treat as extremely rare
    if (freq == 0xFFFFFFFF)
        freq_score = 5.0f;
    else
        freq_score = log2f(1.0f + (f32)freq) * 0.8f;

    const f32 freq_weight = 0.3f;
    return raw * (1.0f - freq_weight) + freq_score * freq_weight;
}

static const char* s_pos_names[] = {
    [POS_NOUN] = "n.",
    [POS_VERB] = "v.",
    [POS_NOUN_VERB] = "n., v.",
    [POS_ADJ] = "adj.",
    [POS_ADV] = "adv.",
    [POS_ADJ_ADV] = "adj., adv.",
    [POS_CONJ] = "conj.",
    [POS_DET] = "det.",
    [POS_INDEF_ART] = "indef. art.",
    [POS_INTERJ] = "interj.",
    [POS_MODAL] = "modal v.",
    [POS_NUM] = "num.",
    [POS_PREDET] = "predet.",
    [POS_PREP] = "prep.",
    [POS_ADV_PREP] = "adv., prep.",
    [POS_PRON] = "pron.",
    [POS_SUFFIX] = "suf.",
    [POS_PREFIX] = "pref.",
    [POS_AUX_VERB] = "aux. v.",
    [POS_PHRASAL_VERB] = "phr. v.",
    [POS_DEF_ART] = "def. art.",
};

static void render_dict_content(const void* data, void* ctx)
{
    i32 word_idx = (i32)(isize)data;
    AppShared* shared = (AppShared*)ctx;
    const DictDB* db = &shared->dict_db;
    if (!db->hdr)
        return;
    const Theme* theme = &shared->theme;
    const DictWordIndex* w = &db->words[word_idx];
    const char* word = DICT_STR(db, w->word_stroff);

    /* word title */
    ui_text((String){ (u8*)word, (isize)strlen(word) },
            &(TextConfig){
                .font = &shared->fonts[FONT_INDEX_UI], .font_size = 20, .color = theme->accent_bg, .line_height = 28 });

    const u8* p = db->entdata + w->entdata_off;

    /* freq */
    p += 4;

    /* brief_en */
    {
        u8 cnt = dict_rd_u8(&p);
        TextConfig cfg = {
            .font = &shared->fonts[FONT_INDEX_UI], .font_size = 13, .color = theme->panel_fg, .line_height = 20
        };
        for (u8 i = 0; i < cnt; i++)
        {
            u32 off = dict_rd_u32(&p);
            ui_text((String){ (u8*)DICT_STR(db, off), (isize)strlen(DICT_STR(db, off)) }, &cfg);
        }
    }

    /* brief_zh */
    {
        u8 cnt = dict_rd_u8(&p);
        TextConfig cfg = {
            .font = &shared->fonts[FONT_INDEX_ZH], .font_size = 13, .color = theme->panel_fg, .line_height = 20
        };
        for (u8 i = 0; i < cnt; i++)
        {
            u32 off = dict_rd_u32(&p);
            ui_text((String){ (u8*)DICT_STR(db, off), (isize)strlen(DICT_STR(db, off)) }, &cfg);
        }
    }

    /* POS sections */
    u8 pos_count = dict_rd_u8(&p);
    for (u8 pi = 0; pi < pos_count; pi++)
    {
        u8 pos_kind = dict_rd_u8(&p);
        u32 pron_off = dict_rd_u32(&p);

        /* POS label + pronunciation on one line */
        if (pos_kind < countof(s_pos_names) && s_pos_names[pos_kind])
        {
            UIBox* row = ui_box_begin(
                &(BoxConfig){ .sizing = { fit({}), fit({}) }, .direction = LAYOUT_LEFT_TO_RIGHT, .child_gap = 4 });
            {
                const char* label = s_pos_names[pos_kind];
                ui_text((String){ (u8*)label, (isize)strlen(label) },
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MONO],
                                       .font_size = 12,
                                       .color = theme->accent_subtle_bg,
                                       .line_height = 18 });
                if (pron_off)
                    ui_text((String){ (u8*)DICT_STR(db, pron_off), (isize)strlen(DICT_STR(db, pron_off)) },
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MONO],
                                           .font_size = 12,
                                           .color = theme->accent_subtle_bg,
                                           .line_height = 18 });
            }
            ui_box_end(row);
        }

        u8 def_count = dict_rd_u8(&p);
        for (u8 di = 0; di < def_count; di++)
        {
            u32 en_off = dict_rd_u32(&p);
            u32 zh_off = dict_rd_u32(&p);

            ui_text((String){ (u8*)DICT_STR(db, en_off), (isize)strlen(DICT_STR(db, en_off)) },
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                   .font_size = 13,
                                   .color = theme->panel_fg,
                                   .line_height = 20 });
            ui_text((String){ (u8*)DICT_STR(db, zh_off), (isize)strlen(DICT_STR(db, zh_off)) },
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                   .font_size = 12,
                                   .color = theme->panel_fg,
                                   .line_height = 18 });

            u8 ex_count = dict_rd_u8(&p);
            for (u8 ei = 0; ei < ex_count; ei++)
            {
                u32 ex_en = dict_rd_u32(&p);
                u32 ex_zh = dict_rd_u32(&p);

                ui_text((String){ (u8*)DICT_STR(db, ex_en), (isize)strlen(DICT_STR(db, ex_en)) },
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                       .font_size = 11,
                                       .color = theme->panel_fg,
                                       .line_height = 18,
                                       .wrap = True });
                ui_text((String){ (u8*)DICT_STR(db, ex_zh), (isize)strlen(DICT_STR(db, ex_zh)) },
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                       .font_size = 10,
                                       .color = theme->panel_fg,
                                       .line_height = 16,
                                       .wrap = True });
            }
        }
    }
}

//
// Frame Processing
//

static void render_highlighted_text(String text, i32 range_count, const FuzzyRange* ranges,
                                    const TextConfig* normal_cfg, const TextConfig* highlight_cfg)
{
    if (range_count == 0)
    {
        ui_text(text, normal_cfg);
    }
    else
    {
        isize prev = 0;
        for (i32 r = 0; r < range_count; r++)
        {
            if (ranges[r].start > (i32)prev && prev < (i32)text.len)
            {
                String seg = { text.data + prev, ranges[r].start - (i32)prev };
                ui_text(seg, normal_cfg);
            }
            if (ranges[r].end > ranges[r].start && ranges[r].start < (i32)text.len)
            {
                String seg = { text.data + ranges[r].start, ranges[r].end - ranges[r].start };
                ui_text(seg, highlight_cfg);
            }
            prev = ranges[r].end;
        }
        if (prev < text.len)
        {
            String seg = { text.data + prev, text.len - prev };
            ui_text(seg, normal_cfg);
        }
    }
}

//
// Decoration overlay — window-level min/max/close buttons rendered as
// float-positioned boxes at the top-right corner.  Drawn AFTER all panel
// content so they always appear on top.
//
static void decoration_overlay(WindowContext* ctx)
{
    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;
    HWND window = ctx->window;
    f32 client_w = (f32)ui_ctx->client_width;
    f32 client_h = (f32)ui_ctx->client_height;

    f32 font_size = 12.f;
    f32 button_h = font_size * 2.5f + 2.f - 1;
    f32 button_w = button_h + 1; /* square-ish */
    f32 left_total_w = button_w * 2;
    f32 right_total_w = button_w * 3;
    f32 start_x = client_w - right_total_w;

    ctx->decoration_buttons_width = right_total_w;
    ctx->decoration_left_buttons_width = left_total_w;

    b32 is_active = GetActiveWindow() == window;
    b32 is_maximized = IsZoomed(window);
    Color fg = is_active ? theme->hover_fg : theme->active_fg;

    UIBox* left_buttons_container = ui_box_begin(&(BoxConfig){ .sizing = { fixed(left_total_w), fixed(button_h) },
                                                               .child_gap = font_size * 0.25f,
                                                               .alignment = { ALIGN_CENTER, ALIGN_CENTER },
                                                               .flags = BoxFlag_Float,
                                                               .float_offset = { 0, -(client_h - 1) } });
    {
        f32 left_button_w = button_w * 0.8f;
        f32 left_button_h = button_h * 0.8f;

        /* menu */
        {
            UIBox* btn = ui_box_begin(&(BoxConfig){
                .sizing = { fixed(left_button_w), fixed(left_button_h) },
                .rect_style = { .corner_radius = 4 },
                .alignment = { ALIGN_CENTER, ALIGN_CENTER },
            });
            {
                b32 is_hovered = (ctx->tb_hovered_button == TitleBarHot_Menu);
                UIBoxInteractResult ir = ui_box_interact(btn, str("decoration_menu"));
                if (ir.last_box)
                {
                    update_transition(&ctx->menu_popup.t, ctx->menu_popup.open ? 1.f : 0.f, 15.f);
                    Color base_color = is_hovered ? theme->press_bg : (Color){ 0, 0, 0, 0 };
                    btn->cfg.color = lerp_color(base_color, theme->accent_weak_bg, ctx->menu_popup.t);
                    ctx->decoration_menu = (Rect){ ir.last_box->position.x, ir.last_box->position.y,
                                                   ir.last_box->position.x + ir.last_box->size.width,
                                                   ir.last_box->position.y + ir.last_box->size.height };
                }
                ui_text(str(ICON_FONT_UTF8_MENU),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ICON], .font_size = 10, .color = fg });
            }
            ui_box_end(btn);
        }

        /* search */
        {
            UIBox* btn = ui_box_begin(&(BoxConfig){
                .sizing = { fixed(left_button_w), fixed(left_button_h) },
                .rect_style = { .corner_radius = 4 },
                .alignment = { ALIGN_CENTER, ALIGN_CENTER },
            });
            {
                b32 is_hovered = (ctx->tb_hovered_button == TitleBarHot_Search);
                UIBoxInteractResult ir = ui_box_interact(btn, str("decoration_search"));
                if (ir.last_box)
                {
                    update_transition(&ctx->palette_popup.t, ctx->palette_popup.open ? 1.f : 0.f, 15.f);
                    Color base_color = is_hovered ? theme->press_bg : (Color){ 0, 0, 0, 0 };
                    btn->cfg.color = lerp_color(base_color, theme->accent_weak_bg, ctx->palette_popup.t);
                    ctx->decoration_search = (Rect){ ir.last_box->position.x, ir.last_box->position.y,
                                                     ir.last_box->position.x + ir.last_box->size.width,
                                                     ir.last_box->position.y + ir.last_box->size.height };
                }
                ui_text(str(ICON_FONT_UTF8_SEARCH),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ICON], .font_size = 10, .color = fg });
            }
            ui_box_end(btn);
        }
    }
    ui_box_end(left_buttons_container);

    /* menu popup (context-menu-like panel below the menu button) */
    if (ctx->menu_popup.open)
    {
        f32 popup_w = 200;
        f32 popup_h = 150;
        f32 gap = 4;
        f32 popup_x = ctx->decoration_menu.xmin;
        f32 popup_y = ctx->decoration_menu.ymax + gap;

        UIBox* popup = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(popup_w), fixed(popup_h) },
            .flags = BoxFlag_Float,
            .float_offset = { popup_x, -(client_h - 1) + popup_y },
            .color = theme->palette_bg,
            .rect_style = {
                .corner_radius = 8,
                .shadow_color = theme->shadow,
                .shadow_offset = { 0, 2 },
                .shadow_sigma = 4,
                .border_color = theme->border,
                .border_thickness = 1,
            },
        });
        {
            UIBoxInteractResult ir = ui_box_interact(popup, str("##menu_popup"));
            if (ir.last_box)
            {
                ctx->menu_popup.rect = (Rect){
                    ir.last_box->position.x,
                    ir.last_box->position.y,
                    ir.last_box->position.x + ir.last_box->size.width,
                    ir.last_box->position.y + ir.last_box->size.height,
                };
            }
            /* TODO: popup content goes here */
        }
        ui_box_end(popup);
    }

    /* minimize */
    {
        UIBox* btn = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(button_w), fixed(button_h) },
            .flags = BoxFlag_Float,
            .float_offset = { start_x, -(client_h - 1) },
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        });
        {
            b32 is_hovered = (ctx->tb_hovered_button == TitleBarHot_Minimize);
            UIBoxInteractResult ir = ui_box_interact(btn, str("decoration_minimize"));
            if (ir.last_box)
            {
                btn->cfg.color = is_hovered ? theme->press_bg : (Color){ 0, 0, 0, 0 };
                ctx->decoration_minimize = (Rect){ ir.last_box->position.x, ir.last_box->position.y,
                                                   ir.last_box->position.x + ir.last_box->size.width,
                                                   ir.last_box->position.y + ir.last_box->size.height };
            }
            ui_text(str("\xEE\xA4\xA1"),
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MDL], .font_size = 10, .color = fg });
        }
        ui_box_end(btn);
    }

    /* maximize / restore */
    {
        UIBox* btn = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(button_w), fixed(button_h) },
            .flags = BoxFlag_Float,
            .float_offset = { start_x + button_w, -(client_h - 1) },
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        });
        {
            b32 is_hovered = (ctx->tb_hovered_button == TitleBarHot_Maximize);
            UIBoxInteractResult ir = ui_box_interact(btn, str("decoration_maximize"));
            if (ir.last_box)
            {
                btn->cfg.color = is_hovered ? theme->press_bg : (Color){ 0, 0, 0, 0 };
                ctx->decoration_maximize = (Rect){ ir.last_box->position.x, ir.last_box->position.y,
                                                   ir.last_box->position.x + ir.last_box->size.width,
                                                   ir.last_box->position.y + ir.last_box->size.height };
            }
            ui_text(is_maximized ? str("\xEE\xA4\xA3") : str("\xEE\xA4\xA2"),
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MDL], .font_size = 10, .color = fg });
        }
        ui_box_end(btn);
    }

    /* close */
    {
        UIBox* btn = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(button_w), fixed(button_h) },
            .flags = BoxFlag_Float,
            .float_offset = { start_x + button_w * 2, -(client_h - 1) },
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        });
        {
            b32 is_hovered = (ctx->tb_hovered_button == TitleBarHot_Close);
            UIBoxInteractResult ir = ui_box_interact(btn, str("decoration_close"));
            if (ir.last_box)
            {
                btn->cfg.color = is_hovered ? theme->destructive_bg : (Color){ 0, 0, 0, 0 };
                ctx->decoration_close = (Rect){ ir.last_box->position.x, ir.last_box->position.y,
                                                ir.last_box->position.x + ir.last_box->size.width,
                                                ir.last_box->position.y + ir.last_box->size.height };
            }
            ui_text(str("\xEE\xA2\xBB"),
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MDL], .font_size = 10, .color = fg });
        }
        ui_box_end(btn);
    }
}

static void search_palette_render(WindowContext* ctx)
{
    if (!ctx->palette_popup.open)
        return;

    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;

    f32 client_w = (f32)ui_ctx->client_width;
    f32 client_h = (f32)ui_ctx->client_height;
    f32 popup_w = 500.f;
    f32 popup_h = 380.f;
    f32 popup_x = (client_w - popup_w) / 2.f;
    f32 popup_y = (client_h - popup_h) / 2.f;
    f32 pad = 6.f;
    f32 font_size = 13.f;

    /* build query from text edit state */
    String query = { ctx->palette_text_buf, ctx->palette_text_edit.text_len };

    /* ── Mode (set by Ctrl+D/E/W shortcuts) ── */
    SearchPaletteMode mode = ctx->palette_search_mode;
    b32 mode_changed = (mode != ctx->palette_effective_mode);

    if (mode_changed)
    {
        const FieldDef* fields;
        i32 field_count;
        switch (mode)
        {
            default:
            case PALETTE_MODE_WORD:
                fields = s_dict_fields;
                field_count = countof(s_dict_fields);
                break;
            case PALETTE_MODE_DEF:
                fields = s_dict_fields_def;
                field_count = countof(s_dict_fields_def);
                break;
            case PALETTE_MODE_EXAMPLE:
                fields = s_dict_fields_ex;
                field_count = countof(s_dict_fields_ex);
                break;
        }
        search_reconfigure(&shared->palette_search, fields, field_count);
        ctx->palette_effective_mode = mode;
    }

    /* ── Run search (against palette_search) ── */
    SearchResult sr[SEARCH_DISPLAY_MAX];
    i32 sr_count = 0;

    if (query.len > 0)
    {
        search_set_query(&shared->palette_search, query);
        /* After a mode switch, record the query version so we can
           tell when the new round's results arrive. */
        if (mode_changed)
            ctx->palette_switch_version = shared->palette_search.query_version;
        sr_count = search_get_results(&shared->palette_search, sr, SEARCH_DISPLAY_MAX);
    }
    else
    {
        /* empty query: show first words in alphabetical order */
        u32 total = shared->dict_db.hdr->word_count;
        i32 n = total < SEARCH_DISPLAY_MAX ? (i32)total : SEARCH_DISPLAY_MAX;
        for (i32 i = 0; i < n; i++)
        {
            const DictWordIndex* w = &shared->dict_db.words[i];
            sr[i].entry = (void*)w;
            const char* word = DICT_STR(&shared->dict_db, w->word_stroff);
            sr[i].key = (String){ (u8*)word, (isize)strlen(word) };
            sr[i].text = sr[i].key;
            sr[i].score = 0.f;
            sr[i].range_count = 0;
        }
        sr_count = n;
    }

    /* Build rich display items (word + context, normalised ranges) */
    PaletteDisplayItem items[SEARCH_DISPLAY_MAX];
    i32 item_count = palette_build_display_items(sr, sr_count, mode, SEARCH_DISPLAY_MAX, items);

    /* ── Determine view state ── */
    PaletteViewState view_state;
    if (query.len == 0)
        view_state = PALETTE_VIEW_EMPTY;
    else if (shared->palette_search.published_version < ctx->palette_switch_version)
        view_state = PALETTE_VIEW_LOADING;
    else if (item_count > 0)
        view_state = PALETTE_VIEW_RESULTS;
    else
        view_state = PALETTE_VIEW_NO_MATCH;

    /* detect query change: reset to first match */
    b32 query_changed = (query.len != ctx->palette_prev_query_len);
    ctx->palette_prev_query_len = query.len;
    if (query_changed)
        ctx->palette_selected_index = 0;

    /* auto-select first item when results transition from empty to non-empty */
    if (item_count > 0 && ctx->palette_selected_index < 0)
        ctx->palette_selected_index = 0;

    /* wrap selected index */
    if (item_count > 0)
        ctx->palette_selected_index = (ctx->palette_selected_index % item_count + item_count) % item_count;
    else
        ctx->palette_selected_index = -1;

    UIBox* popup = ui_box_begin(&(BoxConfig){
        .sizing = { fixed(popup_w), fixed(popup_h) },
        .flags = BoxFlag_Float,
        .float_offset = { popup_x, -(client_h - 1) + popup_y },
        .color = theme->palette_bg,
        .rect_style = {
            .corner_radius = 8,
            .shadow_color = theme->shadow,
            .shadow_offset = { 0, 2 },
            .shadow_sigma = 4,
            .border_color = theme->border,
            .border_thickness = 1,
        },
    });
    {
        UIBoxInteractResult ir = ui_box_interact(popup, str("##palette_popup"));
        if (ir.last_box)
        {
            ctx->palette_popup.rect = (Rect){
                ir.last_box->position.x,
                ir.last_box->position.y,
                ir.last_box->position.x + ir.last_box->size.width,
                ir.last_box->position.y + ir.last_box->size.height,
            };
        }

        UIBox* vbox = ui_box_begin(&(BoxConfig){
            .sizing = { grow({}), grow({}) },
            .padding = { 1, 0, 1, 0 },
            .direction = LAYOUT_TOP_TO_BOTTOM,
        });
        {
            /* ── Header row: text field + mode indicator ── */
            UIBox* header = ui_box_begin(&(BoxConfig){
                .sizing = { grow({}), fit({}) },
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .padding = { pad, pad, pad, pad },
                .child_gap = pad,
            });
            {
                /* text field */
                UIBox* tf_container = ui_box_begin(
                    &(BoxConfig){ .sizing = { grow({}), fit({}) }, .alignment = { ALIGN_START, ALIGN_CENTER } });
                {
                    const char* placeholder;
                    // clang-format off
                    switch (ctx->palette_search_mode)
                    {
                        default:
                        case PALETTE_MODE_WORD:    placeholder = "Search word...";       break;
                        case PALETTE_MODE_DEF:     placeholder = "Search definition..."; break;
                        case PALETTE_MODE_EXAMPLE: placeholder = "Search example...";    break;
                    }
                    // clang-format on
                    ui_text_field(
                        &ctx->palette_text_edit, str_fmt(128, "%s###" SEARCH_PALETTE_INPUT_HASH_STR, placeholder),
                        &shared->fonts[FONT_INDEX_ZH], font_size, (SizingAxis)fit_grow({}), s_padding_medium,
                        theme->palette_bg, (Color){ 0 }, theme->panel_fg, theme->scrollbar_thumb, theme->cursor_bar,
                        theme->cursor_trail, theme->selection, theme->selection_flash, True);
                }
                ui_box_end(tf_container);

                /* mode indicator */
                {
                    UIBox* indicator = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fit_grow({}) },
                                                                  .padding = s_padding_medium,
                                                                  .color = theme->hover_bg,
                                                                  .rect_style = { .corner_radius = 4 },
                                                                  .alignment = { ALIGN_CENTER, ALIGN_CENTER } });
                    {
                        String label;
                        // clang-format off
                        switch (ctx->palette_search_mode)
                        {
                            case PALETTE_MODE_WORD:    label = str("Word");       break;
                            case PALETTE_MODE_DEF:     label = str("Definition"); break;
                            case PALETTE_MODE_EXAMPLE: label = str("Example");    break;
                            default:                   label = str("Word");       break;
                        }
                        // clang-format on
                        ui_text(label, &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                                      .font_size = 10.f,
                                                      .color = theme->accent_weak_fg });
                    }
                    ui_box_end(indicator);
                }
            }
            ui_box_end(header);

            /* splitter */
            ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = theme->border }));

            /* ── Result area, switched on view_state ── */
            switch (view_state)
            {
                case PALETTE_VIEW_EMPTY:
                case PALETTE_VIEW_RESULTS:
                {
                    if (item_count == 0)
                        break;

                    b32 result_clicked = False;

                    UIBox* scroll_container =
                        ui_box_begin(&(BoxConfig){ .sizing = { grow({}), grow({}) }, .padding = { 0, 1, 0, 1 } });
                    {
                        i32 selection_changed = (ctx->palette_selected_index != ctx->palette_prev_selected_index);
                        f32 row_h = 0;

                        ScrollContext scroll =
                            ui_scrollable_area_begin(&(ScrollableAreaConfig){ .hash_str = str("##palette_results"),
                                                                              .sizing = { grow({}), grow({}) },
                                                                              .corner_radius = 8,
                                                                              .padding = { 5, 5, 5, 5 },
                                                                              .bg = theme->palette_bg,
                                                                              .thumb_color = theme->scrollbar_thumb,
                                                                              .direction = LAYOUT_TOP_TO_BOTTOM,
                                                                              .scroll_margin = font_size * 2.f });
                        {
                            /* on query change: instantly snap scroll to top */
                            if (query_changed && scroll.last_area)
                            {
                                scroll.last_area->scroll_delta.y = 0.f;
                                scroll.last_area->scroll_anim_y.target = 0.f;
                                scroll.last_area->scroll_anim_y.start = 0.f;
                                scroll.last_area->scroll_anim_y.started_at = ui_ctx->current_time;
                            }

                            /* mouse wheel navigates items (fzf-style) */
                            {
                                PaletteNavAction scroll_action = palette_nav_from_scroll(ui_ctx->mouse_scroll_delta.y,
                                                                                         ui_hovered(scroll.area_flags));
                                if (scroll_action != PALETTE_NAV_NONE)
                                {
                                    ui_ctx->mouse_scroll_delta.y = 0;
                                    ctx->palette_selected_index =
                                        palette_nav_apply(ctx->palette_selected_index, item_count, scroll_action, NULL);
                                    selection_changed = 1;
                                }
                            }

                            f32 cumulative_y = 0.f;

                            TextConfig normal_cfg = { .font = &shared->fonts[FONT_INDEX_UI],
                                                      .font_size = font_size,
                                                      .color = theme->panel_fg };
                            TextConfig highlight_cfg = { .font = &shared->fonts[FONT_INDEX_UI],
                                                         .font_size = font_size,
                                                         .color = theme->accent_bg };
                            TextConfig context_cfg = { .font = &shared->fonts[FONT_INDEX_ZH],
                                                       .font_size = 11.f,
                                                       .color = theme->hover_fg };
                            TextConfig context_hl_cfg = { .font = &shared->fonts[FONT_INDEX_ZH],
                                                          .font_size = 11.f,
                                                          .color = theme->accent_bg };

                            for (i32 i = 0; i < item_count; i++)
                            {
                                PaletteDisplayItem* item = &items[i];

                                UIBox* row = ui_box_begin(&(BoxConfig){
                                    .sizing = { fit_grow({}), fit({}) },
                                    .direction = LAYOUT_TOP_TO_BOTTOM,
                                    .padding = { 8, 8, 8, 8 },
                                    .child_gap = 6,
                                    .color = (Color){ 0, 0, 0, 0 },
                                    .rect_style = { .corner_radius = 4 },
                                });
                                {
                                    PaletteRowResult row_res = palette_row_interact(
                                        row, str_fmt(HASH_STR_MAX_LENGTH, "palette_result_%u_%d", ctx->id, i), theme,
                                        i == ctx->palette_selected_index);
                                    row_h = row_res.height;

                                    /* preview: auto-update tab content for selected item */
                                    if (i == ctx->palette_selected_index)
                                    {
                                        const DictWordIndex* w = item->entry;
                                        PanelTab* active = panel_tab_get_active(ctx->focused_panel);
                                        if (active)
                                        {
                                            const char* wrd = DICT_STR(&shared->dict_db, w->word_stroff);
                                            isize wlen = strlen(wrd);
                                            memcpy(active->name, wrd, wlen);
                                            active->name_len = wlen;
                                            active->content_data = (void*)(isize)(w - shared->dict_db.words);
                                            active->render_fn = render_dict_content;
                                        }
                                    }

                                    /* commit: close popup on click or Enter */
                                    b32 commit_this = row_res.clicked;
                                    if (!commit_this && ctx->palette_activate_pending &&
                                        i == ctx->palette_selected_index)
                                    {
                                        ctx->palette_activate_pending = False;
                                        commit_this = True;
                                    }
                                    if (commit_this)
                                        result_clicked = True;

                                    /* word line — always shown, in horizontal box */
                                    {
                                        UIBox* word_line =
                                            ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) } });
                                        {
                                            render_highlighted_text(item->word_text, item->word_range_count,
                                                                    item->word_ranges, &normal_cfg, &highlight_cfg);
                                        }
                                        ui_box_end(word_line);
                                    }

                                    /* context line — shown for def/ex/all modes, truncated to 2 lines */
                                    if (item->has_context && item->display_context.len > 0)
                                    {
                                        String ctx_text = item->display_context;
                                        i32 ctx_rc = item->ctx_range_count;
                                        FuzzyRange ctx_r[FUZZY_MAX_RANGES];
                                        memcpy(ctx_r, item->ctx_ranges, (usize)ctx_rc * sizeof(FuzzyRange));
                                        b32 ctx_trunc = False;
                                        isize trunc_byte = ctx_text.len;

                                        /* Count UTF-8 characters; truncate at PALETTE_CONTEXT_MAX_CHARS */
                                        {
                                            const u8* p = ctx_text.data;
                                            const u8* end = ctx_text.data + ctx_text.len;
                                            isize char_count = 0;
                                            while (p < end && char_count < PALETTE_CONTEXT_MAX_CHARS)
                                            {
                                                UnicodeDecode ud = utf8_decode(p);
                                                if (ud.codepoint == 0)
                                                    break;
                                                p = (const u8*)ud.next_p;
                                                char_count++;
                                            }
                                            if (p < end)
                                            {
                                                trunc_byte = (isize)(p - ctx_text.data);
                                                ctx_trunc = True;
                                            }
                                        }

                                        /* Filter ranges past the truncation point */
                                        if (ctx_trunc)
                                        {
                                            i32 keep = 0;
                                            for (i32 r = 0; r < ctx_rc; r++)
                                            {
                                                if (ctx_r[r].start >= (i32)trunc_byte)
                                                    continue;
                                                if (ctx_r[r].end > (i32)trunc_byte)
                                                    ctx_r[r].end = (i32)trunc_byte;
                                                if (ctx_r[r].end > ctx_r[r].start)
                                                    ctx_r[keep++] = ctx_r[r];
                                            }
                                            ctx_rc = keep;
                                        }

                                        UIBox* ctx_line = ui_box_begin(&(BoxConfig){
                                            .sizing = { fit_grow({}), fit({}) }, .padding = { 2, 0, 2, 0 } });
                                        {
                                            String display = { ctx_text.data, trunc_byte };
                                            render_highlighted_text(display, ctx_rc, ctx_r, &context_cfg,
                                                                    &context_hl_cfg);
                                            if (ctx_trunc)
                                                ui_text(str("..."), &context_cfg);
                                        }
                                        ui_box_end(ctx_line);
                                    }
                                }
                                ui_box_end(row);

                                /* splitter */
                                if (item->has_context && item->display_context.len > 0)
                                    ui_box_end(ui_box_begin(
                                        &(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = theme->hover_bg }));

                                if (selection_changed && i == ctx->palette_selected_index)
                                {
                                    scroll.scroll_hint.y = cumulative_y;
                                    scroll.scroll_hint_h = row_h;
                                }

                                cumulative_y += row_h;
                            }
                        }

                        f32 delta_y_before = scroll.last_area->scroll_delta.y;
                        ui_scrollable_area_end(scroll);
                        f32 delta_y_after = scroll.last_area->scroll_delta.y;

                        /* sync viewport -> selected index (scrollbar drag only) */
                        {
                            b32 scrollbar_used = fabs(delta_y_after - delta_y_before) > 0.01f;
                            if (!selection_changed && scrollbar_used && item_count > 0 && row_h > 0)
                            {
                                f32 current_scroll = delta_y_after;
                                f32 viewport_h = scroll.last_area->size.height;
                                f32 item_top = (f32)ctx->palette_selected_index * row_h;
                                f32 item_bottom = item_top + row_h;
                                f32 visible_top = current_scroll;
                                f32 visible_bottom = current_scroll + viewport_h;
                                if (item_bottom <= visible_top || item_top >= visible_bottom)
                                {
                                    i32 new_idx;
                                    if (item_bottom <= visible_top)
                                        new_idx = (i32)((visible_top + row_h * 0.5f) / row_h);
                                    else
                                        new_idx = (i32)((visible_bottom - row_h * 0.5f) / row_h);
                                    new_idx = max(0, min(new_idx, item_count - 1));
                                    if (new_idx != ctx->palette_selected_index)
                                        ctx->palette_selected_index = new_idx;
                                }
                            }
                        }

                        ctx->palette_prev_selected_index = ctx->palette_selected_index;

                        /* close popup only if click wasn't consumed by scrollbar */
                        if (result_clicked && ui_ctx->mouse_captured_by_hash != scroll.hash)
                        {
                            ctx->palette_popup.open = False;
                            ctx->palette_selected_index = -1;
                            ctx->palette_prev_selected_index = -1;
                        }
                    }
                    ui_box_end(scroll_container);
                }
                break;

                case PALETTE_VIEW_LOADING:
                {
                    UIBox* loading_row = ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fit({}) },
                                                                    .alignment = { ALIGN_CENTER, ALIGN_CENTER },
                                                                    .padding = { 20, 20, 20, 20 } });
                    {
                        ui_text(str("Searching..."), &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                                                    .font_size = 12.f,
                                                                    .color = theme->accent_weak_fg });
                    }
                    ui_box_end(loading_row);
                }
                break;

                case PALETTE_VIEW_NO_MATCH:
                {
                    UIBox* no_match_row = ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fit({}) },
                                                                     .alignment = { ALIGN_CENTER, ALIGN_CENTER },
                                                                     .padding = { 20, 20, 20, 20 } });
                    {
                        ui_text(str("No matches"), &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                                                  .font_size = 12.f,
                                                                  .color = theme->accent_weak_fg });
                    }
                    ui_box_end(no_match_row);
                }
                break;
            }
        }
        ui_box_end(vbox);
    }
    ui_box_end(popup);
}

static void panel_container(WindowContext* ctx, const Rect rect)
{
    TracyCZoneNC(ctx_pc, "PanelContainer", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);
    AppShared* shared = ctx->shared;
    const Theme* theme = &shared->theme;

    // clang-format off
    PanelTheme pt = {
        .hover_bg          = theme->press_bg,
        .scrollbar_thumb   = theme->scrollbar_thumb,

        .panel_bg          = theme->panel_bg,
        .panel_border      = theme->border,

        .tab_border        = theme->border,
        .tab_bg            = theme->bar_bg,
        .tab_fg            = theme->bar_fg,
        .tab_active_bg     = theme->panel_bg,
        .tab_active_fg     = theme->panel_fg,
        .tab_accent        = theme->accent_bg,
        .tab_accent_subtle = theme->accent_subtle_bg,
        .tab_accent_weak   = theme->accent_weak_bg,
    };
    // clang-format on

    ctx->decoration_spacer_count = 0;
    f32 decoration_w = ctx->decoration_buttons_width;

    for (Panel* p = ctx->root_panel; p; p = panel_iter_next(p))
    {
        /* Only handle leaf nodes. Internal nodes are not real UI containers and should be skipped */
        if (p->child_a)
            continue;

        /* Pre-compute panel rect and reserve space for window caption buttons.
           Only the top-right leaf panel needs the right inset, otherwise tabs would
           render underneath the minimize/maximize/close buttons.
           Only the top-left leaf panel needs the left inset, otherwise tabs would
           render underneath the menu/search buttons. */
        Rect r = panel_calc_rect(p, rect);

        b32 touches_top = (r.ymin <= rect.ymin + 0.5f);
        b32 touches_right = (r.xmax >= rect.xmax - 0.5f);
        b32 touches_left = (r.xmin <= rect.xmin + 0.5f);
        f32 right_inset = 0;
        f32 left_inset = 0;
        {
            if (touches_top && touches_right)
                right_inset = decoration_w;
            if (touches_top && touches_left)
                left_inset = ctx->decoration_left_buttons_width;
        }

        PanelContext panel = ui_panel_begin(&(PanelConfig){
            .panel = p,
            .root_rect = rect,
            .panel_rect = r,
            .tab_bar_right_inset = right_inset,
            .tab_bar_left_inset = left_inset,
            .theme = &pt,
            .font_ui = &shared->fonts[FONT_INDEX_UI],
            .font_size = 12,
            .cmd_queue = &shared->cmd_queue,
            .window_id = ctx->id,
            .padding = s_padding_big,
            .child_gap = s_child_gap_medium,
            .direction = LAYOUT_TOP_TO_BOTTOM,
            .show_bottom_bar = (p == ctx->focused_panel),
        });
        {
            if (ui_lclicked(panel.interact.flags))
                ctx->focused_panel = panel.panel;

            PanelTab* active = panel_tab_get_active(p);
            if (active)
            {
                if (active->render_fn)
                {
                    active->render_fn(active->content_data, shared);
                }
                else
                {
                    String tab_label = { active->name, active->name_len };
                    ui_text(tab_label, &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                                      .font_size = 14,
                                                      .color = theme->accent_bg,
                                                      .line_height = 20 });

                    ui_text(str("   "), &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MDL],
                                                                   .font_size = 11,
                                                                   .color = theme->panel_fg,
                                                                   .line_height = 16 });
                    ui_text(str("Ctrl+Shift+H / Ctrl+Shift+V to split horizontally / vertically."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 16 });
                    ui_text(str("Ctrl+T new tab. Ctrl+W close tab. F11 toggle theme."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 16 });
                    ui_text(str("Ctrl+Shift+Left/Right to reorder tabs."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 16 });
                    ui_text(str("Ctrl+Shift+N moves tab to next panel."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 16 });
                    ui_text(str("Ctrl+Shift+F/G to detach tab as new panel (H/V)."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 16 });

                    UIBox* box = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                            .child_gap = s_child_gap_medium,
                                                            .direction = LAYOUT_LEFT_TO_RIGHT });
                    {
                        /* Button: create window */
                        UISignalFlags cw_button_flags =
                            ui_button(str_fmt(HASH_STR_MAX_LENGTH, "New Window##panel_cw_button_%u", p->id),
                                      &shared->fonts[FONT_INDEX_UI], 12, (Sizing){ fit({}), fit({}) }, s_padding_medium,
                                      (Color){ 0 }, theme->hover_fg, theme->hover_bg);
                        if (ui_lclicked(cw_button_flags))
                            cmd_queue_push(&shared->cmd_queue, str("window.create w=600 h=600"));

                        /* Button: split horizontally */
                        UISignalFlags sph_button_flags =
                            ui_button(str_fmt(HASH_STR_MAX_LENGTH, "Split Horizontally##panel_sph_button_%u", p->id),
                                      &shared->fonts[FONT_INDEX_UI], 12, (Sizing){ fit({}), fit({}) }, s_padding_medium,
                                      (Color){ 0 }, theme->hover_fg, theme->hover_bg);
                        if (ui_lclicked(sph_button_flags))
                            cmd_queue_push(&shared->cmd_queue,
                                           str_fmt(CMD_STR_MAX_LENGTH, "panel.split_h panel=%u", p->id));

                        /* Button: split vertically */
                        UISignalFlags spv_button_flags =
                            ui_button(str_fmt(HASH_STR_MAX_LENGTH, "Split Vertically##panel_spv_button_%u", p->id),
                                      &shared->fonts[FONT_INDEX_UI], 12, (Sizing){ fit({}), fit({}) }, s_padding_medium,
                                      (Color){ 0 }, theme->hover_fg, theme->hover_bg);
                        if (ui_lclicked(spv_button_flags))
                            cmd_queue_push(&shared->cmd_queue,
                                           str_fmt(CMD_STR_MAX_LENGTH, "panel.split_v panel=%u", p->id));
                    }
                    ui_box_end(box);

                    UIBox* box2 = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                             .child_gap = s_child_gap_big,
                                                             .alignment = { ALIGN_START, ALIGN_CENTER } });
                    {
                        ui_button(str_fmt(HASH_STR_MAX_LENGTH, "nothing##world_%u", p->id),
                                  &shared->fonts[FONT_INDEX_MONO], 11, (Sizing){ fixed(80), fit({}) }, s_padding_medium,
                                  (Color){ 0 }, theme->hover_fg, theme->hover_bg);
                        ui_text_field(
                            &ctx->text_edit_1, str_fmt(HASH_STR_MAX_LENGTH, "placeholder##text_field_%u", p->id),
                            &shared->fonts[FONT_INDEX_ZH], 12, (SizingAxis)fixed(250), s_padding_medium,
                            theme->palette_bg, theme->border, theme->panel_fg, theme->scrollbar_thumb,
                            theme->cursor_bar, theme->cursor_trail, theme->selection, theme->selection_flash, False);
                        UISignalFlags flags = ui_switchbox(
                            str_fmt(HASH_STR_MAX_LENGTH, "switch box_%u", p->id), &shared->fonts[FONT_INDEX_ICON],
                            &ctx->check, theme->hover_bg, theme->accent_bg, theme->accent_fg, theme->shadow);
                        if (ui_lclicked(flags))
                            ctx->check = !ctx->check;
                    }
                    ui_box_end(box2);

                    /* Text test */
                    // TODO(important):
                    //       The current text width calculation is VERY heavy (and probably shouldn't be called so many
                    //       times!). Just tried 10 rounds, and the FPS dropped to ~8 frames per second on my laptop.
                    //       With
                    //       `.wrap = False`, 10 rounds give ~70 FPS. With 0 rounds, ~110 FPS.
                    // clang-format off
                for (isize i = 0; i < 1; i++)
                {
                    ui_text(
                            str("《红楼梦》也称《石头记》，中国古典长篇章回小说，被视为四大小说名著之一。《红楼梦》书内提及"
                                "的别名，还有《情僧录》、《金玉缘》、《风月宝鉴》、《金陵十二钗》。故事是从女娲补天时所剩下"
                                "的一块石头讲起，因无才补天而随神瑛侍者入世，幻化为贾宝玉与降世时口衔的美玉（贾宝玉即“假宝"
                                "玉”）游历世间最后归彼大荒，因此又名《石头记》。乾隆四十九年甲辰（1784年）梦觉主人序本题为"
                                "《红楼梦》（甲辰梦序抄本）。1791年在第一次活字印刷（程甲本）后，《红楼梦》便取代《石头记》"
                                "成为通行的书名。"),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 20,
                                           .wrap = True });
                    ui_text(
                            str("Dream of the Red Chamber has a complicated textual history that scholars have long "
                                "debated. It is known with certainty that Cao Xueqin began writing the novel in the 1740s. "
                                "Cao was a member of a prominent Chinese family that had served the Manchu emperors of the "
                                "Qing dynasty but whose fortunes had begun to decline. By the time of Cao's death in 1763 "
                                "or 1764, hand-copied manuscripts of the novel's first 80 chapters had begun circulating, "
                                "and he may have written drafts of the remaining chapters. These hand-copied manuscripts "
                                "circulated first among his personal friends and a growing circle of aficionados, then "
                                "eventually on the open market where they sold for large sums of money."),
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_MONO],
                                           .font_size = 11,
                                           .color = theme->panel_fg,
                                           .line_height = 20,
                                           .wrap = True });
                }
                    // clang-format on
                }
            }
        }
        ui_panel_end(&panel);

        /* Store spacer rect for HTCAPTION hit-testing (one-frame delay) */
        if (touches_top && ctx->decoration_spacer_count < 16)
            ctx->decoration_spacer_rects[ctx->decoration_spacer_count++] = panel.tab_bar_spacer_rect;
        Assert(ctx->decoration_spacer_count <= ctx->decoration_spacer_count);
    }

    /* Draw panel boundary */
    ui_panel_boundaries(ctx->root_panel, rect, &pt);

    /* Clean up tabs not declared this frame */
    for (Panel* p = ctx->root_panel; p; p = panel_iter_next(p))
    {
        if (p->child_a)
            continue;
        panel_tabs_cleanup(p);
    }
    TracyCZoneEnd(ctx_pc);
}

static void process_frame(WindowContext* ctx)
{
    if (IsIconic(ctx->window))
        return;

    /* Guard against re-entrant ShowWindow calls (e.g., maximize/restore triggering
       WM_SIZE -> process_frame), which would corrupt outer frame state. */
    if (ctx->in_frame)
        return;
    ctx->in_frame = True;

    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;

    TracyCZoneNC(ctx_frame, "ProcessFrame", TracyColor_Frame, TRACY_SUBSYSTEMS & TracySys_Frame);

    isize arena_pos_backup = ui_frame_begin(ui_ctx);
    {
        /* Input pre-pass: if any open overlay popup covers the mouse,
           claim input for this frame.  During the panel pass we exile the
           mouse to (-FLT_MAX, -FLT_MAX) so that rect_contains_point fails
           for every box — panels and their widgets see no hover, click,
           drag-anchor, or scroll.  Mouse state is restored before the
           overlay rendering pass so popups see the real input.
           This keeps Core (ui.c) completely unaware of overlay Z-order. */
        b32 overlay_claims_input = False;
        {
            f32 mx = ui_ctx->mouse_pos.x;
            f32 my = ui_ctx->mouse_pos.y;
            OverlayPopup* popups[] = { &ctx->menu_popup, &ctx->palette_popup };
            for (isize i = 0; i < countof(popups); i++)
            {
                OverlayPopup* p = popups[i];
                if (p->open && p->rect.xmax > p->rect.xmin && mx >= p->rect.xmin && mx < p->rect.xmax &&
                    my >= p->rect.ymin && my < p->rect.ymax)
                {
                    overlay_claims_input = True;
                    break;
                }
            }
        }

        /* Per-frame snapshot of mouse input state, restored after the
           panel pass so overlay rendering sees the real mouse position. */
        Position saved_mouse_pos, saved_mouse_delta, saved_mouse_scroll;
        b32 saved_lclick, saved_rclick, saved_mclick, saved_press, saved_dclick;

        /* Save sibling ID before removal in case focused panel is removed */
        u32 focused_panel_sibling_id = 0;
        if (ctx->focused_panel && ctx->focused_panel->parent)
        {
            Panel* sib = (ctx->focused_panel->parent->child_a == ctx->focused_panel)
                             ? ctx->focused_panel->parent->child_b
                             : ctx->focused_panel->parent->child_a;
            Panel* leaf = panel_find_first_leaf(sib);
            if (leaf)
                focused_panel_sibling_id = leaf->id;
        }

        ctx->root_panel = panel_process_pending_removes(ctx->root_panel);

        /* If focused panel was removed, focus its sibling */
        if (ctx->focused_panel)
        {
            if (!panel_find_by_id(ctx->root_panel, ctx->focused_panel->id))
            {
                if (focused_panel_sibling_id)
                {
                    Panel* sib = panel_find_by_id(ctx->root_panel, focused_panel_sibling_id);
                    if (sib)
                        ctx->focused_panel = sib;
                    else
                        ctx->focused_panel = panel_find_first_leaf(ctx->root_panel);
                }
                else
                {
                    ctx->focused_panel = panel_find_first_leaf(ctx->root_panel);
                }
            }
        }

        f32 client_w = (f32)ui_ctx->client_width;
        f32 client_h = (f32)ui_ctx->client_height;

        /* When an overlay is open and covers the mouse, exile the mouse
           position to (-FLT_MAX, -FLT_MAX) for the panel pass.
           Every UI box's rect_contains_point check fails naturally —
           no per-widget guards required.  Scroll delta, drag delta, and
           all click/press flags are also cleared as defense-in-depth. */
        if (overlay_claims_input)
        {
            saved_mouse_pos = ui_ctx->mouse_pos;
            saved_mouse_delta = ui_ctx->mouse_delta;
            saved_mouse_scroll = ui_ctx->mouse_scroll_delta;
            saved_lclick = ui_ctx->mouse_lclick;
            saved_rclick = ui_ctx->mouse_rclick;
            saved_mclick = ui_ctx->mouse_mclick;
            saved_press = ui_ctx->mouse_press;
            saved_dclick = ui_ctx->mouse_double_click;

            ui_ctx->mouse_pos.x = -FLT_MAX;
            ui_ctx->mouse_pos.y = -FLT_MAX;
            ui_ctx->mouse_delta = (Position){ 0 };
            ui_ctx->mouse_scroll_delta = (Position){ 0 };
            ui_ctx->mouse_lclick = False;
            ui_ctx->mouse_rclick = False;
            ui_ctx->mouse_mclick = False;
            ui_ctx->mouse_press = False;
            ui_ctx->mouse_double_click = False;
        }

        UIBox* root_box_container = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(client_w), fixed(client_h) },
            .direction = LAYOUT_TOP_TO_BOTTOM,
        });
        {
            if (!IsZoomed(ctx->window))
            {
                /* 1px top border — rendered outside root_box so it sits at y=0.
                   Active: uses system accent border color when available,
                           otherwise a darkened theme-derived line.
                   Inactive: subtly fades into the title bar background. */
                Color top_border_color;
                if (GetActiveWindow() == ctx->window)
                {
                    if (shared->has_accent_border)
                        top_border_color = shared->accent_border_color;
                    else
                        top_border_color =
                            (Color){ (u8)((u32)theme->border.r * 2 / 5), (u8)((u32)theme->border.g * 2 / 5),
                                     (u8)((u32)theme->border.b * 2 / 5), 255 };
                }
                else
                    top_border_color = (Color){ (u8)((u32)theme->border.r * 3 / 4), (u8)((u32)theme->border.g * 3 / 4),
                                                (u8)((u32)theme->border.b * 3 / 4), 255 };

                UIBox* top_border = ui_box_begin(&(BoxConfig){
                    .sizing = { fixed(client_w), fixed(1) },
                    .color = top_border_color,
                });
                ui_box_end(top_border);
            }

            UIBox* root_box = ui_box_begin(&(BoxConfig){
                .sizing = { fixed(client_w), fixed(client_h - 1) },
                .direction = LAYOUT_TOP_TO_BOTTOM,
            });
            {
                /* content area — full height, panels own the top edge */
                UIBox* content = ui_box_begin(&(BoxConfig){
                    .sizing = { fixed(client_w), fixed(client_h - 1) },
                });
                {
                    String root_hash = str("###window_bg");
                    UIBoxInteractResult rb = ui_box_interact(content, root_hash);

                    Rect panel_rect = { 0, 0, client_w, client_h - 1 };
                    panel_container(ctx, panel_rect);

                    /* Unhandled drop on background: create a new window with the dragged tab */
                    if (ui_dropped(rb.flags) && !g_ui_ctx->drag_payload_consumed &&
                        g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
                    {
                        TabDragPayload* payload = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                        if (payload->drag_type == DRAG_TYPE_TAB)
                        {
                            g_ui_ctx->drag_payload_consumed = True;

                            POINT cursor_pt;
                            GetCursorPos(&cursor_pt);
                            cmd_queue_push(&shared->cmd_queue,
                                           str_fmt(CMD_STR_MAX_LENGTH,
                                                   "tab.move_to_new_window panel=%u tab=%u window=%u pos_x=%d pos_y=%d",
                                                   payload->from_panel_id, payload->from_tab_id,
                                                   payload->from_window_id, (i32)cursor_pt.x, (i32)cursor_pt.y));
                        }
                    }
                }
                ui_box_end(content);

                /* Restore real mouse state so overlay popups (search,
                   menu, context menu) can process input normally. */
                if (overlay_claims_input)
                {
                    ui_ctx->mouse_pos = saved_mouse_pos;
                    ui_ctx->mouse_delta = saved_mouse_delta;
                    ui_ctx->mouse_scroll_delta = saved_mouse_scroll;
                    ui_ctx->mouse_lclick = saved_lclick;
                    ui_ctx->mouse_rclick = saved_rclick;
                    ui_ctx->mouse_mclick = saved_mclick;
                    ui_ctx->mouse_press = saved_press;
                    ui_ctx->mouse_double_click = saved_dclick;
                }

                /* decoration buttons float on top of panels at top-right */
                decoration_overlay(ctx);

                /* search palette (centered overlay) */
                search_palette_render(ctx);
            }
            ui_box_end(root_box);
        }
        ui_box_end(root_box_container);
    }
    ui_frame_end(arena_pos_backup);
    ctx->in_frame = False;
    TracyCZoneEnd(ctx_frame);
}

//
// Main
//

static TitleBarHot titlebar_hit_test_button(const WindowContext* ctx, f32 logical_x, f32 logical_y)
{
    if (logical_x >= ctx->decoration_menu.xmin && logical_x < ctx->decoration_menu.xmax &&
        logical_y >= ctx->decoration_menu.ymin && logical_y < ctx->decoration_menu.ymax)
        return TitleBarHot_Menu;
    if (logical_x >= ctx->decoration_search.xmin && logical_x < ctx->decoration_search.xmax &&
        logical_y >= ctx->decoration_search.ymin && logical_y < ctx->decoration_search.ymax)
        return TitleBarHot_Search;
    if (logical_x >= ctx->decoration_close.xmin && logical_x < ctx->decoration_close.xmax &&
        logical_y >= ctx->decoration_close.ymin && logical_y < ctx->decoration_close.ymax)
        return TitleBarHot_Close;
    if (logical_x >= ctx->decoration_maximize.xmin && logical_x < ctx->decoration_maximize.xmax &&
        logical_y >= ctx->decoration_maximize.ymin && logical_y < ctx->decoration_maximize.ymax)
        return TitleBarHot_Maximize;
    if (logical_x >= ctx->decoration_minimize.xmin && logical_x < ctx->decoration_minimize.xmax &&
        logical_y >= ctx->decoration_minimize.ymin && logical_y < ctx->decoration_minimize.ymax)
        return TitleBarHot_Minimize;
    return TitleBarHot_None;
}

static LRESULT CALLBACK window_procedure(const HWND window, const u32 message, const WPARAM wparam, const LPARAM lparam)
{
    WindowContext* ctx = NULL;
    AppShared* shared = NULL;
    UIContext* ui_ctx = NULL;
    {
        if (message == WM_CREATE)
        {
            CREATESTRUCT* create = (CREATESTRUCT*)(lparam);
            ctx = (WindowContext*)(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)ctx);

            // Force redrawing with the new client area as we draw a custom title bar
            SetWindowPos(window, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        }
        else
        {
            LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
            ctx = (WindowContext*)ptr;
        }

        if (ctx)
        {
            shared = ctx->shared;
            ui_ctx = &ctx->ui;
        }
    }

    // Handle message
    switch (message)
    {
        // By handling WM_NCCALCSIZE and returning 0, we extend the client area
        // into the normal title bar region so the title bar can be rendered
        // entirely by the application.
        // See:
        //   https://kubyshkin.name/posts/win32-window-custom-title-bar-caption
        //   https://handmade.network/forums/articles/t/9073-custom_window_title_bar_and_almost_correctly_drawing_windows_10_borders
        case WM_NCCALCSIZE:
        {
            if (!wparam)
                return DefWindowProcW(window, message, wparam, lparam);

            u32 dpi = ui_ctx->dpi ? ui_ctx->dpi : GetDpiForWindow(window);

            /* Standard resize border thickness provided by Windows. */
            i32 frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi);
            i32 frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi);

            /* Extra invisible padding added by DWM for resizing/shadows/snap. */
            i32 padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            /* rc initially represents the full window rect, not the final client rect. */
            NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lparam;
            RECT* rc = params->rgrc;

            // NOTE:
            //   Shrink the client rect on the left/right/bottom so Windows keeps
            //   its invisible resize borders there.
            //
            //   We intentionally do NOT shrink the top edge in normal windowed mode.
            //   This allows our custom title bar to fully cover the top frame area.
            //
            //   At first this may seem wrong because the top resize border is also
            //   removed from the non-client area. However, top resizing is usually
            //   restored manually in WM_NCHITTEST by returning HTTOP.
            //
            //   When maximized, Windows places part of the frame outside the monitor.
            //   Without restoring the top inset here, the client area would extend
            //   off-screen and the top of the UI would be clipped.
            rc->right -= frame_x + padding;
            rc->left += frame_x + padding;
            rc->bottom -= frame_y + padding;
            if (IsZoomed(window))
                rc->top += frame_y + padding;

            /* Update ui context client width/height */
            f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
            ui_ctx->client_width = (u32)ceil((rc->right - rc->left) / dpi_scale);
            ui_ctx->client_height = (u32)ceil((rc->bottom - rc->top) / dpi_scale);

            // Returning 0 tells Windows that we fully handled the non-client size
            // calculation ourselves, so the default title bar and frame should not
            // be applied.
            return 0;
        }

        // Even though the title bar area was turned into client area in
        // WM_NCCALCSIZE, Windows still sends WM_NCHITTEST for mouse handling.
        //
        // WM_NCHITTEST does not define what is client vs non-client visually.
        // Instead, it defines how a region should behave when interacted with.
        //
        // This allows parts of the client area to behave like system caption
        // buttons, resize borders, or the title bar itself.
        case WM_NCHITTEST:
        {
            /* let default handle resize edges */
            LRESULT hit = DefWindowProcW(window, message, wparam, lparam);
            switch (hit)
            {
                case HTNOWHERE:
                case HTRIGHT:
                case HTLEFT:
                case HTTOPLEFT:
                case HTTOP:
                case HTTOPRIGHT:
                case HTBOTTOMRIGHT:
                case HTBOTTOM:
                case HTBOTTOMLEFT:
                    return hit;
            }

            if (ctx)
            {
                u32 dpi = ui_ctx->dpi;
                f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;

                POINT pt;
                pt.x = GET_X_LPARAM(lparam);
                pt.y = GET_Y_LPARAM(lparam);
                ScreenToClient(window, &pt);

                f32 logical_x = pt.x / dpi_scale;
                f32 logical_y = pt.y / dpi_scale;

                /* unified caption-button hit-test state machine */
                {
                    TitleBarHot prev = ctx->tb_hovered_button;
                    ctx->tb_hovered_button = titlebar_hit_test_button(ctx, logical_x, logical_y);
                    if (ctx->tb_hovered_button != prev)
                        ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                }

                if (ctx->tb_hovered_button == TitleBarHot_Maximize)
                    return HTMAXBUTTON;

                /* menu / search / minimize / close button rects → HTCAPTION (NC message routing) */
                if (ctx->tb_hovered_button == TitleBarHot_Menu || ctx->tb_hovered_button == TitleBarHot_Search ||
                    ctx->tb_hovered_button == TitleBarHot_Minimize || ctx->tb_hovered_button == TitleBarHot_Close)
                    return HTCAPTION;

                /* top resize zone — only for non-maximized windows */
                if (!IsZoomed(window))
                {
                    i32 frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi);
                    i32 padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    if (logical_y >= 0 && logical_y < (frame_y + padding) / dpi_scale)
                        return HTTOP;
                }

                /* tab bar spacer areas → HTCAPTION (window drag handle) */
                for (isize i = 0; i < ctx->decoration_spacer_count; i++)
                {
                    Rect* sr = &ctx->decoration_spacer_rects[i];
                    if (logical_x >= sr->xmin && logical_x < sr->xmax && logical_y >= sr->ymin && logical_y < sr->ymax)
                        return HTCAPTION;
                }
            }

            return HTCLIENT;
        }

        case WM_ACTIVATE:
        {
            if (ctx)
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_NCRBUTTONUP:
        {
            return 0;
        }

        case WM_NCLBUTTONDOWN:
        {
            if (ctx)
            {
                if (ctx->menu_popup.open && ctx->tb_hovered_button != TitleBarHot_Menu)
                    ctx->menu_popup.open = False;
                if (ctx->palette_popup.open && ctx->tb_hovered_button != TitleBarHot_None)
                {
                    ctx->palette_popup.open = False;
                    ctx->palette_selected_index = -1;
                    ctx->palette_prev_selected_index = -1;
                    ctx->palette_activate_pending = False;
                }
                if (ctx->tb_hovered_button != TitleBarHot_None)
                {
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_NCLBUTTONUP:
        {
            if (ctx)
            {
                TitleBarHot hot = ctx->tb_hovered_button;
                switch (hot)
                {
                    case TitleBarHot_Close:
                        PostMessageW(window, WM_CLOSE, 0, 0);
                        break;
                    case TitleBarHot_Maximize:
                        ShowWindow(window, IsZoomed(window) ? SW_NORMAL : SW_MAXIMIZE);
                        break;
                    case TitleBarHot_Minimize:
                        ShowWindow(window, SW_MINIMIZE);
                        break;
                    case TitleBarHot_Menu:
                        cmd_queue_push(&ctx->shared->cmd_queue,
                                       str_fmt(CMD_STR_MAX_LENGTH, "menu.toggle window=%u", ctx->id));
                        break;
                    case TitleBarHot_Search:
                        cmd_queue_push(&ctx->shared->cmd_queue,
                                       str_fmt(CMD_STR_MAX_LENGTH, "palette.toggle window=%u", ctx->id));
                        break;
                    case TitleBarHot_None:
                    default:
                        return DefWindowProcW(window, message, wparam, lparam);
                }
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                return 0;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_MOUSEMOVE:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ctx->tb_hovered_button = TitleBarHot_None;

            f32 dpi_scale = (f32)ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;
            Position mouse_pos_backup = ui_ctx->mouse_pos;
            ui_ctx->mouse_pos.x = GET_X_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_pos.y = GET_Y_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_delta.x = ui_ctx->mouse_pos.x - mouse_pos_backup.x;
            ui_ctx->mouse_delta.y = ui_ctx->mouse_pos.y - mouse_pos_backup.y;

            /* Cross-window drag: any window whose client area the cursor
               enters promotes itself to the foreground. */
            if (shared->cross_drag_active && GetForegroundWindow() != window)
            {
                SetForegroundWindow(window);
                SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }

            /* Transfer capture from current window to the window under cursor
               when cross-drag is active */
            if (shared->cross_drag_active)
                if (GetCapture() == window)
                {
                    POINT screen_pt;
                    GetCursorPos(&screen_pt);
                    HWND under = WindowFromPoint(screen_pt);
                    if (under && under != window && under != (shared->drag_popup ? shared->drag_popup->window : NULL) &&
                        GetWindowThreadProcessId(under, NULL) == GetCurrentThreadId())
                    {
                        ReleaseCapture();
                        SetCapture(under);
                    }
                }

            if (!ctx->mouse_tracked)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, window, 0 };
                TrackMouseEvent(&tme);
                ctx->mouse_tracked = True;
            }

            return 0;
        }

        case WM_MOUSELEAVE:
        {
            ctx->mouse_tracked = False;
            ui_ctx->mouse_pos.x = -FLT_MAX;
            ui_ctx->mouse_pos.y = -FLT_MAX;
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            return 0;
        }
        case WM_NCMOUSEMOVE:
        {
            ui_ctx->mouse_pos.x = -FLT_MAX;
            ui_ctx->mouse_pos.y = -FLT_MAX;
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, window, 0 };
            TrackMouseEvent(&tme);

            return 0;
        }
        case WM_NCMOUSELEAVE:
        {
            ctx->tb_hovered_button = TitleBarHot_None;
            ui_ctx->mouse_pos.x = -FLT_MAX;
            ui_ctx->mouse_pos.y = -FLT_MAX;
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            return 0;
        }

        case WM_SETCURSOR:
        {
            if (LOWORD(lparam) == HTCLIENT)
            {
                SetCursor(shared->cursors[ui_ctx->desired_cursor]);
                return True;
            }
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_MOUSEHWHEEL:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_scroll_delta.x += GET_WHEEL_DELTA_WPARAM(wparam) / 10;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_scroll_delta.y += GET_WHEEL_DELTA_WPARAM(wparam) / -10;
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            /* Determine double-click */
            f64 now = ui_ctx->current_time;
            i32 click_x = GET_X_LPARAM(lparam);
            i32 click_y = GET_Y_LPARAM(lparam);
            {
                f64 double_click_sec = (f64)GetDoubleClickTime() / 1000.0;
                f32 dx = (f32)(click_x - ui_ctx->last_lclick_pos.x);
                f32 dy = (f32)(click_y - ui_ctx->last_lclick_pos.y);
                f32 dist = sqrtf(dx * dx + dy * dy);
                if (now - ui_ctx->last_lclick_time <= double_click_sec &&
                    dist <= (f32)GetSystemMetrics(SM_CXDOUBLECLK) * 2.f)
                    ui_ctx->mouse_double_click = True;
            }
            ui_ctx->last_lclick_time = now;
            ui_ctx->last_lclick_pos.x = (f32)click_x;
            ui_ctx->last_lclick_pos.y = (f32)click_y;

            /* Handle Overlay Popup */
            OverlayPopup* popups[] = { &ctx->menu_popup, &ctx->palette_popup };
            f32 dpi_scale = (f32)ctx->ui.dpi / USER_DEFAULT_SCREEN_DPI;
            f32 lx = click_x / dpi_scale;
            f32 ly = click_y / dpi_scale;
            for (isize i = 0; i < (isize)countof(popups); i++)
            {
                OverlayPopup* p = popups[i];
                if (p->open && p->rect.xmax > p->rect.xmin)
                    if (!(lx >= p->rect.xmin && lx < p->rect.xmax && ly >= p->rect.ymin && ly < p->rect.ymax))
                    {
                        p->open = False;
                        if (p == &ctx->palette_popup)
                        {
                            ctx->palette_selected_index = -1;
                            ctx->palette_prev_selected_index = -1;
                            ctx->palette_activate_pending = False;
                        }
                        break;
                    }
            }

            /* Update */
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_lclick = True;
            ui_ctx->mouse_press = True;
            SetCapture(window);
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_rclick = True;
            return 0;
        }

        case WM_MBUTTONDOWN:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_mclick = True;
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_RBUTTONUP:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_press = False;
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            ui_ctx->mouse_press = False;
            break;
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
            ui_ctx->ime_composing = True;
            return 0;
        }

        case WM_IME_COMPOSITION:
        {
            if (lparam & GCS_RESULTSTR)
            {
                String result = win32_ime_get_result(window, &ui_ctx->arena);
                const byte* p = result.data;
                while ((isize)(p - result.data) < result.len)
                {
                    UnicodeDecode dec = utf8_decode(p);
                    if (ui_ctx->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
                        ui_ctx->char_input_queue[ui_ctx->char_input_queue_count++] = dec.codepoint;
                    p = dec.next_p;
                }
            }
            if (lparam & GCS_COMPSTR)
                ui_ctx->ime_composition = win32_ime_get_composition(window, &ui_ctx->arena);
            return 0;
        }

        case WM_IME_ENDCOMPOSITION:
        {
            ui_ctx->ime_composing = False;
            ui_ctx->ime_composition = (String){ 0 };
            return 0;
        }

        case WM_IME_REQUEST:
        {
            if (wparam == IMR_CANDIDATEWINDOW)
            {
                CANDIDATEFORM* form = (CANDIDATEFORM*)lparam;
                form->dwIndex = 0;
                form->dwStyle = CFS_CANDIDATEPOS;
                Position pos = ui_ctx->ime_cursor_screen_pos;
                form->ptCurrentPos.x = (LONG)pos.x;
                form->ptCurrentPos.y = (LONG)pos.y;
                return 1;
            }
            return 0;
        }

        case WM_SYSCHAR:
        {
            // Prevent Alt-based shortcuts from triggering the system menu beep.
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            b32 ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            b32 shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            b32 alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (wparam == VK_ESCAPE)
            {
                /* Close open overlays before destroying the window */
                if (ctx)
                {
                    OverlayPopup* popups[] = { &ctx->menu_popup, &ctx->palette_popup };
                    for (isize i = 0; i < (isize)countof(popups); i++)
                    {
                        if (popups[i]->open)
                        {
                            popups[i]->open = False;
                            cmd_queue_push(&ctx->shared->cmd_queue,
                                           str_fmt(CMD_STR_MAX_LENGTH, "palette.close window=%u", ctx->id));
                            return 0;
                        }
                    }
                }
                DestroyWindow(window);
                return 0;
            }

            /* Palette mode switching — intercept before shortcut lookup
               so Ctrl+W switches to word mode instead of closing a tab. */
            if (ctx && ctrl && ctx->palette_popup.open && ctx->ui.focused_hash == s_search_palette_input_hash)
            {
                SearchPaletteMode new_mode = ctx->palette_search_mode;
                // clang-format off
                switch (wparam)
                {
                    case 'D': new_mode = PALETTE_MODE_DEF;     break;
                    case 'E': new_mode = PALETTE_MODE_EXAMPLE; break;
                    case 'W': new_mode = PALETTE_MODE_WORD;    break;
                    default: goto check_shortcuts;
                }
                // clang-format on

                if (new_mode != ctx->palette_search_mode)
                {
                    const FieldDef* fields;
                    i32 field_count;
                    // clang-format off
                    switch (new_mode)
                    {
                        default:
                        case PALETTE_MODE_WORD:    fields  = s_dict_fields;     field_count = countof(s_dict_fields);     break;
                        case PALETTE_MODE_DEF:     fields  = s_dict_fields_def; field_count = countof(s_dict_fields_def); break;
                        case PALETTE_MODE_EXAMPLE: fields  = s_dict_fields_ex;  field_count = countof(s_dict_fields_ex);  break;
                    }
                    // clang-format on
                    search_reconfigure(&shared->palette_search, fields, field_count);
                    ctx->palette_search_mode = new_mode;
                    ctx->palette_effective_mode = new_mode;
                    /* Re-issue current query with new fields */
                    String cur_query = { ctx->palette_text_buf, ctx->palette_text_edit.text_len };
                    if (cur_query.len > 0)
                    {
                        search_set_query(&shared->palette_search, cur_query);
                        ctx->palette_switch_version = shared->palette_search.query_version;
                    }
                }
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                return 0;
            }
        check_shortcuts:

            /* Check registered shortcuts */
            {
                Modifiers mods = 0;
                if (ctrl)
                    mods |= SHORTCUT_MOD_CTRL;
                if (shift)
                    mods |= SHORTCUT_MOD_SHIFT;
                if (alt)
                    mods |= SHORTCUT_MOD_ALT;
                Shortcut sc = { mods, (u32)wparam };
                String cmd_text = shortcut_lookup(&shared->shortcuts, sc);
                if (cmd_text.len)
                {
                    /* Extract stable IDs from current UI context for text augmentation */
                    u32 panel_id = 0;
                    u32 tab_id = 0;
                    u32 to_panel_id = 0;
                    u32 window_id = ctx ? ctx->id : 0;

                    if (ctx && ctx->focused_panel)
                    {
                        panel_id = ctx->focused_panel->id;
                        PanelTab* at = panel_tab_get_active(ctx->focused_panel);
                        if (at)
                            tab_id = at->id;
                    }

                    /* First-token matching for commands that need extra pointer ctx */
                    {
                        isize end = 0;
                        while (end < cmd_text.len && cmd_text.data[end] != ' ')
                            end++;
                        String token = { cmd_text.data, end };

                        if (str_compare(token, str("tab.move_to_panel")) && ctx && ctx->focused_panel)
                        {
                            Panel* next = ctx->focused_panel;
                            do
                            {
                                next = panel_iter_next(next);
                            } while (next && next->child_a);
                            if (next)
                                to_panel_id = next->id;
                        }
                        if (str_compare(token, str("tab.to_new_panel")) && ctx && ctx->focused_panel)
                            to_panel_id = ctx->focused_panel->id;
                    }

                    if (panel_id)
                        cmd_queue_push(&shared->cmd_queue,
                                       str_fmt(CMD_STR_MAX_LENGTH, "%.*s panel=%u tab=%u to_panel=%u window=%u",
                                               (i32)cmd_text.len, cmd_text.data, panel_id, tab_id, to_panel_id,
                                               window_id));
                    else if (window_id)
                        cmd_queue_push(&shared->cmd_queue, str_fmt(CMD_STR_MAX_LENGTH, "%.*s window=%u",
                                                                   (i32)cmd_text.len, cmd_text.data, window_id));
                    else
                        cmd_queue_push(&shared->cmd_queue, cmd_text);
                    return 0;
                }
            }

            /* Popup navigation — active when a popup's text field has focus */
            {
                PaletteNavAction action = palette_nav_from_key((u32)wparam, ctrl);
                if (action != PALETTE_NAV_NONE)
                {
                    if (ctx && ctx->palette_popup.open && ctx->ui.focused_hash == s_search_palette_input_hash)
                    {
                        ctx->palette_selected_index =
                            palette_nav_apply(ctx->palette_selected_index, 0, action, &ctx->palette_activate_pending);
                        return 0;
                    }
                }
            }

            TextAction action = { 0 };
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
                    if (message == WM_SYSKEYDOWN)
                        return 0;
                    return DefWindowProcW(window, message, wparam, lparam);
            }
            ui_ctx->text_action_queue[ui_ctx->text_action_queue_count++] = action;
            // clang-format on

            Assert(ui_ctx->text_action_queue_count < TEXT_ACTION_QUEUE_CAPACITY);

            return 0;
        }

        case WM_CHAR:
        {
            wchar_t c = (wchar_t)wparam;

            /* '/' key opens or focuses the search palette */
            if (c == '/' && ctx)
            {
                if (!(ctx->palette_popup.open && ctx->ui.focused_hash == s_search_palette_input_hash))
                {
                    cmd_queue_push(&shared->cmd_queue,
                                   str_fmt(CMD_STR_MAX_LENGTH, "palette.toggle window=%u", ctx->id));
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
            }

            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
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
            if (codepoint >= 0x20 && codepoint != 127 && ui_ctx->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
                ui_ctx->char_input_queue[ui_ctx->char_input_queue_count++] = codepoint;
            return 0;
        }

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lparam;
            u32 dpi = GetDpiForWindow(window);
            f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
            i32 frame_x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            i32 frame_y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            mmi->ptMinTrackSize.x = (LONG)(MIN_WINDOW_WIDTH * dpi_scale) + frame_x * 2;
            mmi->ptMinTrackSize.y = (LONG)(MIN_WINDOW_HEIGHT * dpi_scale) + frame_y;
            return 0;
        }

        case WM_SIZE:
        {
            if (wparam == SIZE_MINIMIZED || !IsWindowVisible(ctx->window))
                return 0;

            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;

            f32 dpi_scale = (f32)ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_client_width = LOWORD(lparam);
            u32 physical_client_height = HIWORD(lparam);

            ui_ctx->client_width = (u32)ceil(physical_client_width / dpi_scale);
            ui_ctx->client_height = (u32)ceil(physical_client_height / dpi_scale);
            if (ui_ctx->client_width > 0 && ui_ctx->client_height > 0)
                ui_ctx->render_fn.on_resize(ui_ctx->renderer, physical_client_width, physical_client_height);
            process_frame(ctx);
            return 0;
        }

        case WM_DPICHANGED:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->dpi = GetDpiForWindow(window);

            // Per-window atlas recreated; shared raster cache NOT reset — glyphs at
            // old DPI remain available for other windows while new DPI entries accumulate.
            renderer_recreate_glyph_atlas_texture(ui_ctx->renderer);

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

        case WM_DWMCOLORIZATIONCOLORCHANGED:
        {
            if (ctx)
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            shared->has_accent_border = win32_get_accent_border_color(&shared->accent_border_color);
            return 0;
        }

        case WM_DESTROY:
        {
            if (shared && ctx)
            {
                // First disconnect the association, so subsequent trailing messages (WM_NCDESTROY, etc.) get NULL
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);

                window_list_remove(shared, ctx);
                panel_free_tree(ctx->root_panel);
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

static b32 any_window_needs_frames(const AppShared* shared)
{
    for (const WindowContext* w = shared->first_window; w; w = w->next)
        if (w->ui.requested_frames > 0)
            return True;
    return False;
}

typedef struct
{
    AppShared* shared;
} StartupThreadParam;

static DWORD WINAPI startup_dict_thread(LPVOID param)
{
    StartupThreadParam* p = (StartupThreadParam*)param;
    AppShared* shared = p->shared;

    /* Decompress dictionary from embedded zstd resource */
    {
        HRSRC h_res = FindResourceW(NULL, L"DICT_DATA", (LPCWSTR)RT_RCDATA);
        Assert(h_res);
        HGLOBAL h_global = LoadResource(NULL, h_res);
        Assert(h_global);
        const void* compressed = LockResource(h_global);
        DWORD compressed_size = SizeofResource(NULL, h_res);

        u64 dsize = ZSTD_getFrameContentSize(compressed, compressed_size);
        Assert(dsize != ZSTD_CONTENTSIZE_ERROR && dsize != ZSTD_CONTENTSIZE_UNKNOWN);

        shared->dict_arena = arena_new((isize)dsize);
        arena_push(&shared->dict_arena, (isize)dsize, 1, 1);

        usize result = ZSTD_decompress(shared->dict_arena.base, (size_t)dsize, compressed, compressed_size);
        Assert(!ZSTD_isError(result));

        shared->dict_db = dict_open(shared->dict_arena.base);
        Assert(shared->dict_db.hdr);
        g_dict_db = &shared->dict_db;
    }

    shared->search_aux_arena = arena_new(MB(20));
    shared->search_aux = dict_build_search_aux(&shared->dict_db, &shared->search_aux_arena);
    Assert(shared->search_aux);
    g_search_aux = shared->search_aux;

    search_init(&shared->palette_search, shared->dict_db.words, (i32)shared->dict_db.hdr->word_count,
                sizeof(DictWordIndex), s_dict_fields, countof(s_dict_fields), dict_word_extract);
    shared->palette_search.score_adjust = dict_freq_weight;
    search_start(&shared->palette_search);

    shared->dict_ready = True;

    /* Wake rendering: force frame requests on all windows.
       If the main thread is blocked on GetMessageW a mouse move or
       keyboard event will unblock it — the flag transitions within a
       single frame, so the loading screen is never stale for long. */
    for (WindowContext* w = shared->first_window; w; w = w->next)
    {
        w->ui.requested_frames = IDLE_WAKE_FRAMES;
        PostMessageW(w->window, WM_NULL, 0, 0);
    }

    return 0;
}

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
    shared.has_accent_border = win32_get_accent_border_color(&shared.accent_border_color);

    /* Init command infrastructure */
    shared.cfg_arena = arena_new(KB(8));
    shared.cmd_arena = arena_new(CMD_ARENA_SIZE);
    cmd_registry_init(&shared.cmd_registry, &shared.cfg_arena, 32);
    shortcut_registry_init(&shared.shortcuts, &shared.cfg_arena, 64);
    cmd_queue_init(&shared.cmd_queue, &shared.cmd_arena);

    // clang-format off
    {
        /* Register commands */
        cmd_register(&shared.cmd_registry, (CmdDef){ str("window.create"),          str("New Window"),               str(""), cmd_create_window,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("app.toggle_theme"),       str("Toggle Light/Dark Theme"),  str(""), cmd_toggle_theme,           &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("menu.toggle"),            str("Toggle Menu Popup"),        str(""), cmd_toggle_menu,            &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("palette.toggle"),         str("Toggle Search Palette"),    str(""), cmd_toggle_palette,         &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("palette.close"),          str("Close Search Palette"),     str(""), cmd_close_palette,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_h"),          str("Split Panel Horizontally"), str(""), cmd_split_panel_h,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_v"),          str("Split Panel Vertically"),   str(""), cmd_split_panel_v,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.close"),            str("Close Panel"),              str(""), cmd_close_panel,            &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.new"),                str("New Tab"),                  str(""), cmd_tab_new,                &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.close"),              str("Close Tab"),                str(""), cmd_tab_close,              &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.activate"),           str("Activate Tab"),             str(""), cmd_tab_activate,           &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move"),               str("Move Tab"),                 str(""), cmd_tab_move,               &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move_to_panel"),      str("Move Tab To Next Panel"),   str(""), cmd_tab_move_to_panel,      &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move_to_new_window"), str("Move Tab To New Window"),   str(""), cmd_tab_move_to_new_window, &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.to_new_panel"),       str("Move Tab To New Panel"),    str(""), cmd_tab_to_new_panel,       &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_next"),       str("Focus Next Panel"),         str(""), cmd_panel_focus_next,       &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_prev"),       str("Focus Previous Panel"),     str(""), cmd_panel_focus_prev,       &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_left"),       str("Focus Panel Left"),         str(""), cmd_panel_focus_left,       &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_down"),       str("Focus Panel Down"),         str(""), cmd_panel_focus_down,       &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_up"),         str("Focus Panel Up"),           str(""), cmd_panel_focus_up,         &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.focus_right"),      str("Focus Panel Right"),        str(""), cmd_panel_focus_right,      &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.resize_left"),      str("Resize Panel Left"),        str(""), cmd_panel_resize_left,      &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.resize_down"),      str("Resize Panel Down"),        str(""), cmd_panel_resize_down,      &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.resize_up"),        str("Resize Panel Up"),          str(""), cmd_panel_resize_up,        &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.resize_right"),     str("Resize Panel Right"),       str(""), cmd_panel_resize_right,     &shared });

        /* Bind shortcuts */
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'T' },          str("tab.new"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'W' },          str("tab.close"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  VK_OEM_MINUS }, str("panel.split_h"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  VK_OEM_PLUS },  str("panel.split_v"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'H' },          str("tab.move delta=-1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'L' },          str("tab.move delta=+1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'N' },          str("tab.move_to_panel"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'F' },          str("tab.to_new_panel axis=X"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'G' },          str("tab.to_new_panel axis=Y"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      VK_TAB },       str("panel.focus_next"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, VK_TAB },       str("panel.focus_prev"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,   'H' },          str("panel.focus_left"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,   'J' },          str("panel.focus_down"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,   'K' },          str("panel.focus_up"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,   'L' },          str("panel.focus_right"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  'H' },          str("panel.resize_left"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  'J' },          str("panel.resize_down"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  'K' },          str("panel.resize_up"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,  'L' },          str("panel.resize_right"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_NONE,                      VK_F11 },       str("app.toggle_theme"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      VK_OEM_4 },     str("palette.close")); // ctrl+[
        Assert(!shortcut_detect_conflicts(&shared.shortcuts));

        /* Load cursors */
        shared.cursors[UI_CURSOR_ARROW]      = LoadCursor(NULL, IDC_ARROW);
        shared.cursors[UI_CURSOR_IBEAM]      = LoadCursor(NULL, IDC_IBEAM);
        shared.cursors[UI_CURSOR_HAND]       = LoadCursor(NULL, IDC_HAND);
        shared.cursors[UI_CURSOR_HORIZONTAL] = LoadCursor(NULL, IDC_SIZEWE);
        shared.cursors[UI_CURSOR_VERTICAL]   = LoadCursor(NULL, IDC_SIZENS);
        shared.cursors[UI_CURSOR_MOVE]       = LoadCursor(NULL, IDC_SIZEALL);
    }
    // clang-format on

    /* Pre-compute focus hash for palette input routing */
    s_search_palette_input_hash = fnv1a_64(SEARCH_PALETTE_INPUT_HASH_STR, (isize)strlen(SEARCH_PALETTE_INPUT_HASH_STR));

    /* Tell the DWM not to perform any automatic DPI scaling (Windows 10, v1607) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Register window class */
    WNDCLASSW wc = {
        .lpfnWndProc = window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"window class",
    };
    RegisterClassW(&wc);

    /* Register drag popup window class */
    WNDCLASSW dp_wc = {
        .lpfnWndProc = drag_popup_window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"UIDragPopup",
    };
    RegisterClassW(&dp_wc);

    /* Launch dictionary thread (non-blocking — decompress + search init in background) */
    shared.main_thread_id = GetCurrentThreadId();
    {
        StartupThreadParam startup_param = { .shared = &shared };
        HANDLE h_dict = CreateThread(NULL, 0, startup_dict_thread, &startup_param, 0, NULL);
        CloseHandle(h_dict); /* detached — OS reclaims thread stack on exit */
    }

    /* Init raster renderer synchronously on main thread */
    dwrite_init(&shared.dwrite);
    raster_cache_init(&shared.dwrite, &shared.raster_cache, GLYPHS_LENGTH);
    renderer_shared_init(&shared.renderer_shared);

    // clang-format off
    font_register_system_fonts(
        &shared.dwrite,
        (FontRegEntry[]){
            { .file_path = L"C:\\Windows\\Fonts\\segoeui.ttf", .family_name = L"Segoe UI",          .weight = DWRITE_FONT_WEIGHT_NORMAL, .style = DWRITE_FONT_STYLE_NORMAL, .font = &shared.fonts[FONT_INDEX_UI]   },
            { .file_path = L"C:\\Windows\\Fonts\\msyh.ttc",    .family_name = L"Microsoft YaHei",   .weight = DWRITE_FONT_WEIGHT_NORMAL, .style = DWRITE_FONT_STYLE_NORMAL, .font = &shared.fonts[FONT_INDEX_ZH]   },
            { .file_path = L"C:\\Windows\\Fonts\\consola.ttf", .family_name = L"Consolas",          .weight = DWRITE_FONT_WEIGHT_NORMAL, .style = DWRITE_FONT_STYLE_NORMAL, .font = &shared.fonts[FONT_INDEX_MONO] },
            { .file_path = L"C:\\Windows\\Fonts\\segmdl2.ttf", .family_name = L"Segoe MDL2 Assets", .weight = DWRITE_FONT_WEIGHT_NORMAL, .style = DWRITE_FONT_STYLE_NORMAL, .font = &shared.fonts[FONT_INDEX_MDL]  },
        },
        4
    );
    // clang-format on
    font_register_from_resource(&shared.dwrite, L"ICON_FONT", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                &shared.fonts[FONT_INDEX_ICON]);

    /* Create first window (warmup frames will show "Loading dictionary…" if dict not yet ready) */
    WindowContext* first =
        create_window(&shared, L"App Title", CW_USEDEFAULT, CW_USEDEFAULT, CLIENT_WIDTH, CLIENT_HEIGHT, True);
    first->ui.requested_frames = IDLE_WAKE_FRAMES;

    /* Run message loop */
    MSG message;
    while (True)
    {
        if (any_window_needs_frames(&shared) || shared.cross_drag_active)
        {
            /* Poll: drain all pending messages without blocking */
            if (PeekMessageW(&message, 0, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                    break;
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }
        }
        else
        {
            /* Idle: block until any message arrives */
            if (!GetMessageW(&message, 0, 0, 0))
                break;
            TranslateMessage(&message);
            DispatchMessageW(&message);

            /* Drain any remaining messages */
            while (PeekMessageW(&message, 0, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                    break;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (message.message == WM_QUIT)
                break;
        }

        TracyCFrameMark;

        /* log every command's human-readable text */
#if !defined(NDEBUG) && !defined(TRACY_ENABLE)
        for (CmdQueueNode* cn = shared.cmd_queue.first; cn; cn = cn->next)
        {
            char buf[512];
            i32 len = snprintf(buf, sizeof(buf), "[cmd] %.*s\n", (i32)cn->cmd_text.len, (const char*)cn->cmd_text.data);
            if (len > 0)
                OutputDebugStringA(buf);
        }
#endif
        { /* Register command count before execution. If there are commands, force at least one frame on all
             windows to render the result (essential for cross-window drops to render on the target window). */
            u64 command_count = shared.cmd_queue.count;
            cmd_queue_execute_all(&shared.cmd_queue, &shared.cmd_registry);
            if (command_count > 0)
                for (WindowContext* wf = shared.first_window; wf; wf = wf->next)
                    if (wf->ui.requested_frames <= 0)
                        wf->ui.requested_frames = 1;
        }

        if (!any_window_needs_frames(&shared))
            continue;

        TracyCZoneNC(ctx_main_loop, "MainLoop", TracyColor_Frame, TRACY_SUBSYSTEMS & TracySys_Frame);

        /* Snapshot cursor position once — query WindowFromPoint only once */
        POINT cursor_pt;
        GetCursorPos(&cursor_pt);

        /* Position popup BEFORE WindowFromPoint so the cursor is never
           inside the popup rect (popup sits at cursor+12px offset). */
        drag_popup_update(&shared);

        HWND hwnd_under = shared.cross_drag_active ? WindowFromPoint(cursor_pt) : NULL;

        /* Process each window.  Sync drag state to whichever window the
           cursor is currently over — including the source window itself,
           because after ReleaseCapture() fires WM_CAPTURECHANGED the
           source's mouse_press/drag_active are stale. */
        for (WindowContext* w = shared.first_window; w; w = w->next)
        {
            if (shared.cross_drag_active)
            {
                if (w->window == hwnd_under)
                    cross_window_sync_to(&shared, w, cursor_pt);
                else if (w->id != shared.cross_drag_source_window_id)
                    w->ui.drag_active = False;
            }

            if (w->ui.requested_frames > 0)
            {
                process_frame(w);
                if (w->ui.requested_frames > 0)
                    w->ui.requested_frames--;
            }
        }

        /* Force cursor update during cross-window drag: WM_SETCURSOR
           is suppressed when another window captures mouse. Use
           authoritative source (cross-drag source or local drag-active
           window) to apply desired cursor. */
        {
            // clang-format off
            WindowContext* ac = NULL;
            if (shared.cross_drag_active)
                ac = find_window_by_id(&shared, shared.cross_drag_source_window_id);
            else
                for (WindowContext* w = shared.first_window; w; w = w->next)
                    if (w->ui.drag_active) { ac = w; break; }
            if (ac && ac->ui.drag_active)
                SetCursor(shared.cursors[ac->ui.desired_cursor]);
            // clang-format on
        }

        /* After all frames: either pick up a new drag or tear down a stale one */
        if (!shared.cross_drag_active)
        {
            for (WindowContext* w = shared.first_window; w; w = w->next)
            {
                if (w->ui.drag_active)
                {
                    shared.cross_drag_active = True;
                    memcpy(shared.cross_drag_payload_buf, w->ui.drag_payload_buf, w->ui.drag_payload_size);
                    shared.cross_drag_payload_size = w->ui.drag_payload_size;
                    shared.cross_drag_source_window_id = w->id;
                    break;
                }
            }
        }
        else
        {
            WindowContext* src = find_window_by_id(&shared, shared.cross_drag_source_window_id);
            if (!src || (!src->ui.mouse_press && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)))
            {
                /* Don't clear while a target window still holds the drop frame */
                b32 any_target_dragging = False;
                for (WindowContext* w = shared.first_window; w; w = w->next)
                    if (w->id != shared.cross_drag_source_window_id && w->ui.drag_active)
                    {
                        any_target_dragging = True;
                        break;
                    }
                if (!any_target_dragging)
                {
                    /* Desktop drop: if no window consumed the drag payload,
                       create a new window at the cursor position */
                    if (shared.cross_drag_payload_size >= (isize)sizeof(TabDragPayload) &&
                        ((TabDragPayload*)shared.cross_drag_payload_buf)->drag_type == DRAG_TYPE_TAB)
                    {
                        b32 payload_consumed = False;
                        for (WindowContext* wc = shared.first_window; wc; wc = wc->next)
                            if (wc->ui.drag_payload_consumed)
                            {
                                payload_consumed = True;
                                break;
                            }

                        if (!payload_consumed)
                        {
                            TabDragPayload* pld = (TabDragPayload*)shared.cross_drag_payload_buf;
                            POINT cursor_pt;
                            GetCursorPos(&cursor_pt);
                            cmd_queue_push(&shared.cmd_queue,
                                           str_fmt(CMD_STR_MAX_LENGTH,
                                                   "tab.move_to_new_window panel=%u tab=%u window=%u pos_x=%d pos_y=%d",
                                                   pld->from_panel_id, pld->from_tab_id, pld->from_window_id,
                                                   (i32)cursor_pt.x, (i32)cursor_pt.y));
                        }
                    }

                    shared.cross_drag_active = False;
                    shared.cross_drag_payload_size = 0;
                    shared.cross_drag_source_window_id = 0;
                }
            }
        }

        TracyCZoneEnd(ctx_main_loop);
    }

    /* Clean up remaining windows */
    WindowContext* w = shared.first_window;
    while (w)
    {
        WindowContext* next = w->next;
        DestroyWindow(w->window);
        w = next;
    }

    /* Clean up drag hint popup */
    if (shared.drag_popup)
    {
        DragPopup* popup = shared.drag_popup;
        ShowWindow(popup->window, SW_HIDE);
        DestroyWindow(popup->window);
        ui_deinit(&popup->ui);
        renderer_deinit(&popup->renderer);
        free(popup);
        shared.drag_popup = NULL;
    }

    /* Stop palette search worker */
    search_stop(&shared.palette_search);

    arena_release(&shared.search_aux_arena);
    if (shared.dict_arena.base)
        arena_release(&shared.dict_arena);

    font_unregister(&shared.fonts[FONT_INDEX_UI]);
    font_unregister(&shared.fonts[FONT_INDEX_ZH]);
    font_unregister(&shared.fonts[FONT_INDEX_MONO]);
    font_unregister(&shared.fonts[FONT_INDEX_MDL]);
    font_unregister(&shared.fonts[FONT_INDEX_ICON]);

    renderer_shared_deinit(&shared.renderer_shared);
    raster_cache_deinit(&shared.raster_cache);
    dwrite_deinit(&shared.dwrite);
    arena_release(&shared.cmd_arena);
    arena_release(&shared.cfg_arena);

    ExitProcess(0);
}
