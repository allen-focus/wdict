#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")
#pragma comment(lib, "shcore")

#include "cmd.h"
#include "glyph_cache.h"
#include "palette.h"
#include "panel.h"
#include "renderer.h"
#include "shortcut.h"
#include "ui.h"
#include "utils.h"
#include "win32_helper.h"

#include <ShellScalingApi.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define CLIENT_WIDTH  1000
#define CLIENT_HEIGHT 750

#ifdef TRACY_ENABLE
#    define IDLE_WAKE_FRAMES 0x7FFFFFFF
#else
#    define IDLE_WAKE_FRAMES 4
#endif

#define MAX_TITLE_LENGTH 64
#define TEXT_BUFFER_SIZE 1024
#define MAX_WINDOWS      16
#define CMD_ARENA_SIZE   KB(64)

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
    Color border_press;

    Color accent;
    Color accent_hover;
    Color accent_press;
    Color accent_fg;

    Color danger;
    Color success;
    Color warning;

    Color cursor;
    Color selection_preview;
    Color selection;
    Color selection_flash;
    Color cursor_trail;

    Color scrollbar_thumb;
    Color scrollbar_track;

    Color shadow;
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

    /* cross-window drag-drop */
    b32 cross_drag_active;
    u8 cross_drag_payload_buf[DRAG_PAYLOAD_MAX];
    isize cross_drag_payload_size;
    u32 cross_drag_source_window_id;

    /* drag hint popup (heap-allocated — UIContext + Renderer are too large for the stack) */
    DragPopup* drag_popup;
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
    Panel* hovered_panel;

    /* widget needed */
    b32 check;

    u8 text_buf_1[TEXT_BUFFER_SIZE];
    u8 text_buf_2[TEXT_BUFFER_SIZE];
    TextEditState text_edit_1;
    TextEditState text_edit_2;
};

static u32 s_window_next_id = 1; /* 0 reserved for none */
static u16 s_utf16_pending_high = 0;

//
// Static Variables: Theme
//

// clang-format off
static const Theme s_theme_light = {
    .bg_base           = { LIGHT_1,  255 },
    .bg_surface        = { LIGHT_2,  255 },
    .bg_overlay        = { LIGHT_3,  255 },

    .fg_primary        = { DARK_5,   255 },
    .fg_secondary      = { DARK_1,   255 },
    .fg_disabled       = { LIGHT_5,  255 },

    .border_normal     = { LIGHT_4,  255 },
    .border_focus      = { BLUE_3,   255 },
    .border_press      = { BLUE_5,   255 },

    .accent            = { BLUE_2,   255 },
    .accent_hover      = { BLUE_3,   255 },
    .accent_press      = { BLUE_4,   255 },
    .accent_fg         = { LIGHT_1,  255 },

    .danger            = { RED_3,    255 },
    .success           = { GREEN_4,  255 },
    .warning           = { ORANGE_3, 255 },

    .cursor            = { DARK_5,   255 },
    .selection_preview = { BLUE_3,   64  },
    .selection         = { BLUE_3,   128 },
    .selection_flash   = { YELLOW_3, 255 },
    .cursor_trail      = { DARK_5,   110 },

    .scrollbar_thumb   = { DARK_1,   240 },
    .scrollbar_track   = { DARK_1,   96  },

    .shadow            = { DARK_5,   255 },
};

static const Theme s_theme_dark = {
    .bg_base           = { DARK_4,   255 },
    .bg_surface        = { DARK_3,   255 },
    .bg_overlay        = { DARK_2,   255 },

    .fg_primary        = { LIGHT_1,  255 },
    .fg_secondary      = { LIGHT_5,  255 },
    .fg_disabled       = { DARK_1,   255 },

    .border_normal     = { DARK_2,   255 },
    .border_focus      = { BLUE_2,   255 },
    .border_press      = { BLUE_4,   255 },

    .accent            = { BLUE_2,   255 },
    .accent_hover      = { BLUE_3,   255 },
    .accent_press      = { BLUE_4,   255 },
    .accent_fg         = { LIGHT_2,  255 },

    .danger            = { RED_1,    255 },
    .success           = { GREEN_2,  255 },
    .warning           = { ORANGE_1, 255 },

    .cursor            = { LIGHT_1,  255 },
    .selection_preview = { BLUE_2,   64  },
    .selection         = { BLUE_2,   128 },
    .selection_flash   = { YELLOW_2, 255 },
    .cursor_trail      = { LIGHT_1,  110 },

    .scrollbar_thumb   = { LIGHT_5,  240 },
    .scrollbar_track   = { LIGHT_5,  96  },

    .shadow            = { DARK_5,   255 },
};
// clang-format on

//
// Static Variables: Padding & Child Gap
//

// clang-format off
static Padding s_padding_medium = { 20, 20, 20, 20 };
static Padding s_padding_small  = { 10, 10, 10, 10 };

static f32 s_child_gap_big    = 20;
static f32 s_child_gap_medium = 10;
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

//
// Window creation
//

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

static void process_frame(WindowContext* ctx);

static WindowContext* find_window_by_id(AppShared* shared, u32 id)
{
    for (WindowContext* w = shared->first_window; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

static b32 any_window_needs_frames(const AppShared* shared)
{
    for (const WindowContext* w = shared->first_window; w; w = w->next)
        if (w->ui.requested_frames > 0)
            return True;
    return False;
}

static void cross_window_sync_to(AppShared* shared, WindowContext* ctx, const POINT cursor_pt)
{
    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
    ctx->ui.mouse_press = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    ctx->ui.drag_active = True;
    memcpy(ctx->ui.drag_payload_buf, shared->cross_drag_payload_buf, shared->cross_drag_payload_size);
    ctx->ui.drag_payload_size = shared->cross_drag_payload_size;
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
            .color = shared->theme.bg_base,
            .rect_style = { .border_color = shared->theme.border_normal, .border_thickness = 1 },
            .alignment = { ALIGN_CENTER, ALIGN_CENTER },
        });
        {
            if (title.data)
                ui_text(title, &(TextConfig){
                                   .font = &shared->fonts[FONT_INDEX_UI],
                                   .font_size = 12.f,
                                   .color = shared->theme.fg_primary,
                                   .line_height = ch,
                               });
        }
        ui_box_end(box);
    }
    ui_frame_end(arena_pos);
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
}

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
    ui_init(ctx->window, &ctx->ui, &ctx->renderer, &shared->raster_cache, width, height, dpi, render_fn);
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

    /* Render first frames to rasterize glyphs, then show */
    process_frame(ctx);
    process_frame(ctx);
    ShowWindow(ctx->window, SW_SHOWDEFAULT);

    return ctx;
}

//
// Command Handlers
//

/* Forward declarations for global lookup helpers used by handlers */
static Panel* find_panel_globally(AppShared* shared, u32 window_id, u32 panel_id);
static PanelTab* find_tab_globally(AppShared* shared, u32 window_id, u32 tab_id);

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
    shared->theme = (shared->theme.bg_base.r == s_theme_dark.bg_base.r) ? s_theme_light : s_theme_dark;
}

static void cmd_split_panel_h(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    Panel* p = find_panel_globally(shared, window_id, panel_id);
    if (p)
        panel_split(p, Axis2_X, True);
}

static void cmd_split_panel_v(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    Panel* p = find_panel_globally(shared, window_id, panel_id);
    if (p)
        panel_split(p, Axis2_Y, True);
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

//
// Global lookup helpers (for text-based command resolution across all windows)
//

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

//
// Panel rendering
//

static void panel_container(WindowContext* ctx, const Rect root_rect);

static void process_frame(WindowContext* ctx)
{
    if (IsIconic(ctx->window) || !IsWindowVisible(ctx->window))
        return;

    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;

    TracyCZone(ctx_frame, 1);

    isize arena_pos_backup = ui_frame_begin(ui_ctx);
    {
        ctx->root_panel = panel_process_pending_removes(ctx->root_panel);
        f32 client_w = (f32)ui_ctx->client_width;
        f32 client_h = (f32)ui_ctx->client_height;
        Rect root_rect = { 0, 0, client_w, client_h };

        UIBox* root_box = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(client_w), fixed(client_h) },
            .rect_style = { .border_color = theme->fg_disabled, .border_thickness = 1 },
            .color = theme->bg_base,
        });
        {
            String root_hash = str("###window_bg");
            UIBoxInteractResult rb = ui_box_interact(root_box, root_hash);

            panel_container(ctx, root_rect);

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
                    char buf[128];
                    i32 len =
                        snprintf(buf, sizeof(buf), "tab.move_to_new_window panel=%u tab=%u window=%u pos_x=%d pos_y=%d",
                                 payload->from_panel_id, payload->from_tab_id, payload->from_window_id,
                                 (i32)cursor_pt.x, (i32)cursor_pt.y);
                    cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len });
                }
            }
        }
        ui_box_end(root_box);
    }
    ui_frame_end(arena_pos_backup);
    TracyCZoneEnd(ctx_frame);
}

static void panel_container(WindowContext* ctx, const Rect rect)
{
    AppShared* shared = ctx->shared;
    const Theme* theme = &shared->theme;

    // clang-format off
    PanelTheme pt = {
        .panel_bg                  = theme->bg_surface,
        .panel_border              = theme->fg_disabled,
        .tab_splitter              = theme->fg_disabled,
        .tab_bar                   = theme->bg_surface,
        .tab_bg                    = theme->bg_surface,
        .tab_fg                    = theme->fg_secondary,
        .tab_active_bg             = theme->bg_base,
        .tab_active_fg             = theme->fg_primary,
        .tab_dragging_bg           = theme->accent,
        .tab_drag_target_bg        = theme->selection_preview,
        .tab_drag_target_bg_accent = theme->selection,
        .hover_bg                  = theme->bg_overlay,
        .click_bg                  = theme->bg_overlay,
        .splitter_idle             = theme->fg_disabled,
        .splitter_hover            = theme->border_focus,
        .splitter_drag             = theme->border_press,
        .scrollbar_thumb           = theme->scrollbar_thumb,
    };
    // clang-format on

    ui_panel_draw_boundaries(ctx->root_panel, rect, &pt);

    /* Find hovered panel for keyboard shortcuts */
    ctx->hovered_panel = NULL;
    for (Panel* hp = ctx->root_panel; hp; hp = panel_iter_next(hp))
    {
        if (hp->child_a)
            continue;
        Rect r = panel_calc_rect(hp, rect);
        if (ctx->ui.mouse_pos.x >= r.xmin && ctx->ui.mouse_pos.x < r.xmax && ctx->ui.mouse_pos.y >= r.ymin &&
            ctx->ui.mouse_pos.y < r.ymax)
        {
            ctx->hovered_panel = hp;
            break;
        }
    }

    for (Panel* p = ctx->root_panel; p; p = panel_iter_next(p))
    {
        /* Only handle leaf nodes. Internal nodes are not real UI containers and should be skipped */
        if (p->child_a)
            continue;

        PanelContext panel = ui_panel_begin(&(PanelConfig){
            .panel = p,
            .root_rect = rect,
            .theme = &pt,
            .font_ui = &shared->fonts[FONT_INDEX_UI],
            .font_size = 12,
            .cmd_queue = &shared->cmd_queue,
            .window_id = ctx->id,
            .padding = s_padding_medium,
            .child_gap = s_child_gap_medium,
            .direction = LAYOUT_TOP_TO_BOTTOM,
        });
        {
            PanelTab* active = panel_tab_get_active(p);
            if (active)
            {
                String tab_label = { active->name, active->name_len };
                ui_text(tab_label, &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                                  .font_size = 14,
                                                  .color = theme->accent,
                                                  .line_height = 20 });

                // clang-format off
                ui_text(str("Ctrl+Shift+H / Ctrl+Shift+V to split horizontally / vertically."),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 11,
                                       .color = theme->fg_primary,
                                       .line_height = 16 });
                ui_text(str("Ctrl+T new tab. Ctrl+W close tab. F11 toggle theme."),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 11,
                                       .color = theme->fg_primary,
                                       .line_height = 16 });
                ui_text(str("Ctrl+Shift+Left/Right to reorder tabs."),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 11,
                                       .color = theme->fg_primary,
                                       .line_height = 16 });
                ui_text(str("Ctrl+Shift+N moves tab to next panel."),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 11,
                                       .color = theme->fg_primary,
                                       .line_height = 16 });
                ui_text(str("Ctrl+Shift+F/G to detach tab as new panel (H/V)."),
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 11,
                                       .color = theme->fg_primary,
                                       .line_height = 16 });
                // clang-format on

                UIBox* box = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                        .child_gap = s_child_gap_medium,
                                                        .direction = LAYOUT_LEFT_TO_RIGHT });
                {
                    /* Button: create window */
                    UISignalFlags cw_button_flags =
                        ui_button(panel_str("New Window##panel_cw_button", p->id), &shared->fonts[FONT_INDEX_UI], 12,
                                  (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                                  theme->accent_hover, theme->accent_press, True);
                    if (ui_lclicked(cw_button_flags))
                        cmd_queue_push(&shared->cmd_queue, str("window.create w=600 h=600"));

                    /* Button: split horizontally */
                    UISignalFlags sph_button_flags =
                        ui_button(panel_str("Split Horizontally##panel_sph_button", p->id),
                                  &shared->fonts[FONT_INDEX_UI], 12, (Sizing){ fit({}), fit({}) }, s_padding_small,
                                  theme->accent, theme->accent_fg, theme->accent_hover, theme->accent_press, True);
                    if (ui_lclicked(sph_button_flags))
                    {
                        char buf[64];
                        i32 len = snprintf(buf, sizeof(buf), "panel.split_h panel=%u", p->id);
                        cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len });
                    }

                    /* Button: split vertically */
                    UISignalFlags spv_button_flags =
                        ui_button(panel_str("Split Vertically##panel_spv_button", p->id), &shared->fonts[FONT_INDEX_UI],
                                  12, (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                                  theme->accent_hover, theme->accent_press, True);
                    if (ui_lclicked(spv_button_flags))
                    {
                        char buf[64];
                        i32 len = snprintf(buf, sizeof(buf), "panel.split_v panel=%u", p->id);
                        cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len });
                    }
                }
                ui_box_end(box);

                UIBox* box2 = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                         .child_gap = s_child_gap_big,
                                                         .alignment = { ALIGN_START, ALIGN_CENTER } });
                {
                    ui_button(panel_str("nothing##world", p->id), &shared->fonts[FONT_INDEX_MONO], 11,
                              (Sizing){ fixed(80), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                              theme->accent_hover, theme->accent_press, True);
                    ui_text_field(&ctx->text_edit_1, panel_str("placeholder##text_field", p->id),
                                  &shared->fonts[FONT_INDEX_ZH], 12, (SizingAxis)fixed(250), s_padding_small,
                                  theme->bg_overlay, theme->border_focus, theme->fg_primary, theme->scrollbar_thumb,
                                  theme->cursor_trail, theme->cursor, theme->selection, theme->selection_flash);
                    UISignalFlags flags =
                        ui_switchbox(panel_str("switch box", p->id), &shared->fonts[FONT_INDEX_ICON], &ctx->check,
                                     theme->border_normal, theme->accent_fg, theme->shadow, theme->accent);
                    if (ui_lclicked(flags))
                        ctx->check = !ctx->check;
                }
                ui_box_end(box2);
            }
        }
        ui_panel_end(&panel);
    }

    /* Clean up tabs not declared this frame */
    for (Panel* p = ctx->root_panel; p; p = panel_iter_next(p))
    {
        if (p->child_a)
            continue;
        panel_tabs_cleanup(p);
    }
}

//
// Window procedure
//

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
        case WM_MOUSEMOVE:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;

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
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_lclick = True;
            ui_ctx->mouse_press = True;
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
            SetCapture(window);
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            ui_ctx->mouse_rclick = True;
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

        case WM_KEYDOWN:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            b32 ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            b32 shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            b32 alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(window);
                return 0;
            }

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

                    if (ctx && ctx->hovered_panel)
                    {
                        panel_id = ctx->hovered_panel->id;
                        PanelTab* at = panel_tab_get_active(ctx->hovered_panel);
                        if (at)
                            tab_id = at->id;
                    }

                    /* First-token matching for commands that need extra pointer ctx */
                    {
                        isize end = 0;
                        while (end < cmd_text.len && cmd_text.data[end] != ' ')
                            end++;
                        String token = { cmd_text.data, end };

                        if (str_compare(token, str("tab.move_to_panel")) && ctx && ctx->hovered_panel)
                        {
                            Panel* next = ctx->hovered_panel;
                            do
                            {
                                next = panel_iter_next(next);
                            } while (next && next->child_a);
                            if (next)
                                to_panel_id = next->id;
                        }
                        if (str_compare(token, str("tab.to_new_panel")) && ctx && ctx->hovered_panel)
                            to_panel_id = ctx->hovered_panel->id;
                    }

                    if (panel_id)
                    {
                        char buf[128];
                        i32 len = snprintf(buf, sizeof(buf), "%.*s panel=%u tab=%u to_panel=%u window=%u",
                                           (i32)cmd_text.len, cmd_text.data, panel_id, tab_id, to_panel_id, window_id);
                        cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len });
                    }
                    else
                    {
                        cmd_queue_push(&shared->cmd_queue, cmd_text);
                    }
                    return 0;
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
                    return DefWindowProcW(window, message, wparam, lparam);
            }
            ui_ctx->text_action_queue[ui_ctx->text_action_queue_count++] = action;
            // clang-format on

            Assert(ui_ctx->text_action_queue_count < TEXT_ACTION_QUEUE_CAPACITY);

            return 0;
        }

        case WM_CHAR:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
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
            if (codepoint >= 0x20 && codepoint != 127 && ui_ctx->char_input_queue_count < CHAR_INPUT_QUEUE_CAPACITY)
                ui_ctx->char_input_queue[ui_ctx->char_input_queue_count++] = codepoint;
            return 0;
        }

        case WM_SIZE:
        {
            if (wparam == SIZE_MINIMIZED)
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

        /* Bind shortcuts */
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'T' },      str("tab.new"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'W' },      str("tab.close"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'H' },      str("panel.split_h"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'V' },      str("panel.split_v"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, VK_LEFT },  str("tab.move delta=-1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, VK_RIGHT }, str("tab.move delta=+1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'N' },      str("tab.move_to_panel"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'F' },      str("tab.to_new_panel axis=X"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'G' },      str("tab.to_new_panel axis=Y"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_NONE,                       VK_F11 },  str("app.toggle_theme"));
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
                            char buf[128];
                            i32 len = snprintf(buf, sizeof(buf),
                                               "tab.move_to_new_window panel=%u tab=%u window=%u pos_x=%d pos_y=%d",
                                               pld->from_panel_id, pld->from_tab_id, pld->from_window_id,
                                               (i32)cursor_pt.x, (i32)cursor_pt.y);
                            cmd_queue_push(&shared.cmd_queue, (String){ (u8*)buf, len });
                        }
                    }

                    shared.cross_drag_active = False;
                    shared.cross_drag_payload_size = 0;
                    shared.cross_drag_source_window_id = 0;
                }
            }
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

    font_unregister(&shared.fonts[FONT_INDEX_UI]);
    font_unregister(&shared.fonts[FONT_INDEX_ZH]);
    font_unregister(&shared.fonts[FONT_INDEX_MONO]);
    font_unregister(&shared.fonts[FONT_INDEX_ICON]);

    renderer_shared_deinit(&shared.renderer_shared);
    raster_cache_deinit(&shared.raster_cache);
    dwrite_deinit(&shared.dwrite);
    arena_release(&shared.cmd_arena);
    arena_release(&shared.cfg_arena);

    return 0;
}
