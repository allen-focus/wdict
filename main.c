#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")

#include "cmd.h"
#include "glyph_cache.h"
#include "palette.h"
#include "panel.h"
#include "renderer.h"
#include "shortcut.h"
#include "ui.h"
#include "utils.h"
#include "win32_helper.h"

#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>
#include <windowsx.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

///

#define CLIENT_WIDTH  1000
#define CLIENT_HEIGHT 750

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
    HCURSOR cursors[5];
    WindowContext* first_window;
    WindowContext* last_window;
    Arena cfg_arena;
    Arena cmd_arena;
    CmdRegistry cmd_registry;
    ShortcutRegistry shortcuts;
    CmdQueue cmd_queue;
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
    .bg_base         = { LIGHT_1,  255 },
    .bg_surface      = { LIGHT_2,  255 },
    .bg_overlay      = { LIGHT_3,  255 },

    .fg_primary      = { DARK_5,   255 },
    .fg_secondary    = { DARK_1,   255 },
    .fg_disabled     = { LIGHT_5,  255 },

    .border_normal   = { LIGHT_4,  255 },
    .border_focus    = { BLUE_3,   255 },
    .border_press    = { BLUE_5,   255 },

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
    .border_press    = { BLUE_4,   255 },

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
        MultiByteToWideChar(CP_UTF8, 0, (const char*)title.data, (i32)title.len, ctx->title, MAX_TITLE_LENGTH - 1);
    ctx->title[written] = L'\0';
    SetWindowTextW(ctx->window, ctx->title);
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
static WindowContext* create_window(AppShared* shared, const wchar_t* title, u32 width, u32 height)
{
    WindowContext* ctx = (WindowContext*)malloc(sizeof(WindowContext));
    if (!ctx)
        return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->shared = shared;
    ctx->id = s_window_next_id++;

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
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(ctx->root_panel, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(ctx->root_panel, (String){ name_buf, name_len });
    }

    /* Render first frames to rasterize glyphs, then show */
    process_frame(ctx);
    process_frame(ctx);
    SetCursor(shared->cursors[ctx->ui.desired_cursor]);
    ShowWindow(ctx->window, SW_SHOWDEFAULT);

    return ctx;
}

//
// Command Handlers
//

static void cmd_create_window(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    u32 w = cmd_parse_u32(cmd_text, str("w"), CLIENT_WIDTH);
    u32 h = cmd_parse_u32(cmd_text, str("h"), CLIENT_HEIGHT);
    create_window((AppShared*)userdata, L"Window", w, h);
}

static void cmd_toggle_theme(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload;
    (void)payload_size;
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    shared->theme = (shared->theme.bg_base.r == s_theme_dark.bg_base.r) ? s_theme_light : s_theme_dark;
}

static void cmd_split_panel_h(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)userdata;
    (void)payload_size;
    (void)cmd_text;
    CmdPayload* p = (CmdPayload*)payload;
    if (p && p->panel)
        panel_split(p->panel, Axis2_X);
}

static void cmd_split_panel_v(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)userdata;
    (void)payload_size;
    (void)cmd_text;
    CmdPayload* p = (CmdPayload*)payload;
    if (p && p->panel)
        panel_split(p->panel, Axis2_Y);
}

static void cmd_close_panel(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)userdata;
    (void)payload_size;
    (void)cmd_text;
    CmdPayload* p = (CmdPayload*)payload;
    if (p && p->panel)
    {
        p->panel->anim_state = PANEL_ANIM_CLOSING;
        p->panel->anim_to_pct = 0.0f;
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
    Panel* panel;
    PanelTab* tab;
} ResolvePanelTabResult;

static ResolvePanelTabResult resolve_panel_and_tab(AppShared* shared, CmdPayload* p, String cmd_text)
{
    ResolvePanelTabResult r = { 0 };
    u32 window_id = 0;

    if (p)
    {
        r.panel = p->panel;
        r.tab = p->tab;
        window_id = p->window_id;
    }

    if (!window_id)
        window_id = cmd_parse_u32(cmd_text, str("window"), 0);

    if (!r.panel)
    {
        u32 pid = cmd_parse_u32(cmd_text, str("panel"), 0);
        if (pid)
            r.panel = find_panel_globally(shared, window_id, pid);
    }
    if (!r.tab)
    {
        u32 tid = cmd_parse_u32(cmd_text, str("tab"), 0);
        if (tid)
            r.tab = find_tab_globally(shared, window_id, tid);
    }
    return r;
}

static Panel* resolve_panel_from_text(AppShared* shared, u32 window_id, Panel* from_payload, String cmd_text, String key)
{
    if (from_payload)
        return from_payload;
    u32 pid = cmd_parse_u32(cmd_text, key, 0);
    return pid ? find_panel_globally(shared, window_id, pid) : NULL;
}

static void cmd_tab_new(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, (CmdPayload*)payload, cmd_text);

    if (rt.panel)
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(rt.panel, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(rt.panel, (String){ name_buf, name_len });
    }
}

static void cmd_tab_close(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, (CmdPayload*)payload, cmd_text);

    if (!rt.panel || !rt.tab)
        return;

    /* If this is the only tab left, close the panel instead */
    if (rt.panel->tab_first && !rt.panel->tab_first->next && rt.panel->tab_first == rt.tab)
    {
        if (rt.panel->parent)
        {
            rt.panel->anim_state = PANEL_ANIM_CLOSING;
            rt.panel->anim_to_pct = 0.0f;
        }
    }
    else
    {
        panel_tab_close(rt.panel, rt.tab);
    }
}

static void cmd_tab_activate(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, (CmdPayload*)payload, cmd_text);

    if (rt.panel && rt.tab)
        panel_tab_activate(rt.panel, rt.tab);
}

static void cmd_tab_move(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, (CmdPayload*)payload, cmd_text);

    if (!rt.panel || !rt.tab)
        return;
    i32 delta = cmd_parse_i32(cmd_text, str("delta"), 0);
    panel_tab_move(rt.panel, rt.tab, delta);
}

static void cmd_tab_move_to_panel(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    CmdPayload* p = (CmdPayload*)payload;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, p, cmd_text);
    u32 window_id = p ? p->window_id : cmd_parse_u32(cmd_text, str("window"), 0);
    Panel* to_panel = resolve_panel_from_text(shared, window_id, (p && p->to_panel) ? p->to_panel : NULL, cmd_text,
                                              str("to_panel"));

    if (rt.panel && to_panel && rt.tab)
        panel_tab_move_to_panel(rt.panel, rt.tab, to_panel);
}

static void cmd_tab_to_new_panel(void* userdata, void* payload, isize payload_size, String cmd_text)
{
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    CmdPayload* p = (CmdPayload*)payload;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, p, cmd_text);
    u32 window_id = p ? p->window_id : cmd_parse_u32(cmd_text, str("window"), 0);
    Panel* to_panel = resolve_panel_from_text(shared, window_id, (p && p->to_panel) ? p->to_panel : NULL, cmd_text,
                                              str("to_panel"));

    if (!rt.panel || !to_panel || !rt.tab)
        return;
    Axis2 axis = (Axis2)cmd_parse_axis(cmd_text, str("axis"), Axis2_X);
    panel_tab_to_new_panel(rt.panel, rt.tab, to_panel, axis);
}

//
// Panel rendering
//

static void panel_container(WindowContext* ctx, const Rect root_rect);

static void process_frame(WindowContext* ctx)
{
    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;

    TracyCZone(ctx_frame, 1);

    isize arena_pos_backup = ui_frame_begin(ui_ctx);
    {
        ctx->root_panel = panel_update_animations(ctx->root_panel, g_ui_ctx->current_time);
        f32 client_w = (f32)ui_ctx->client_width;
        f32 client_h = (f32)ui_ctx->client_height;
        Rect root_rect = { 0, 0, client_w, client_h };

        ui_box({
            .sizing = { fixed(client_w), fixed(client_h) },
            .rect_style = { .border_color = theme->fg_disabled, .border_thickness = 1 },
            .color = theme->bg_base,
        })
        {
            panel_container(ctx, root_rect);
        }
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
        .panel_bg            = theme->bg_surface,
        .panel_border        = theme->fg_disabled,
        .tab_splitter        = theme->fg_disabled,
        .tab_bar             = theme->bg_surface,
        .tab_bg              = theme->bg_surface,
        .tab_fg              = theme->fg_secondary,
        .tab_active_bg       = theme->bg_base,
        .tab_active_fg       = theme->fg_primary,
        .tab_dragging_bg     = theme->accent,
        .tab_drag_target_bg  = theme->selection,
        .hover_bg            = theme->bg_overlay,
        .click_bg            = theme->bg_overlay,
        .splitter_idle       = theme->fg_disabled,
        .splitter_hover      = theme->border_focus,
        .splitter_drag       = theme->border_press,
        .scrollbar_thumb     = theme->scrollbar_thumb,
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
        ui_panel({
            .panel = p,
            .root_rect = rect,
            .theme = &pt,
            .font_ui = &shared->fonts[FONT_INDEX_UI],
            .font_size = 12,
            .cmd_queue = &shared->cmd_queue,
            .cmd_ctx = ctx,
            .window_id = ctx->id,
            .padding = s_padding_medium,
            .child_gap = s_child_gap_medium,
            .direction = LAYOUT_TOP_TO_BOTTOM,
        })
        {
            PanelTab* active = panel_tab_get_active(p);
            if (!active)
                continue;

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

            UIBox* box = ui_box_start(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                    .child_gap = s_child_gap_medium,
                                                    .direction = LAYOUT_LEFT_TO_RIGHT });
            {
                /* Button: create window */
                UISignalFlags cw_button_flags =
                    ui_button(panel_str("New Window##panel_cw_button", p->id), &shared->fonts[FONT_INDEX_UI], 12,
                              (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                              theme->accent_hover, theme->accent_press, True);
                if (ui_lclicked(cw_button_flags))
                    cmd_queue_push(&shared->cmd_queue, str("window.create w=600 h=600"), NULL, 0);

                /* Button: split horizontally */
                UISignalFlags sph_button_flags =
                    ui_button(panel_str("Split Horizontally##panel_sph_button", p->id), &shared->fonts[FONT_INDEX_UI],
                              12, (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                              theme->accent_hover, theme->accent_press, True);
                if (ui_lclicked(sph_button_flags))
                {
                    char buf[64];
                    i32 len = snprintf(buf, sizeof(buf), "panel.split_h panel=%u", p->id);
                    cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len },
                                   &(CmdPayload){ .ctx = ctx, .panel = p }, sizeof(CmdPayload));
                }

                /* Button: split vertically */
                UISignalFlags spv_button_flags =
                    ui_button(panel_str("Split Vertically##panel_spv_button", p->id), &shared->fonts[FONT_INDEX_UI], 12,
                              (Sizing){ fit({}), fit({}) }, s_padding_small, theme->accent, theme->accent_fg,
                              theme->accent_hover, theme->accent_press, True);
                if (ui_lclicked(spv_button_flags))
                {
                    char buf[64];
                    i32 len = snprintf(buf, sizeof(buf), "panel.split_v panel=%u", p->id);
                    cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len },
                                   &(CmdPayload){ .ctx = ctx, .panel = p }, sizeof(CmdPayload));
                }
            }
            ui_box_end(box);

            UIBox* box2 = ui_box_start(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
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
            f32 dpi_scale = (f32)ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;
            Position mouse_pos_backup = ui_ctx->mouse_pos;
            ui_ctx->mouse_pos.x = GET_X_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_pos.y = GET_Y_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_delta.x = ui_ctx->mouse_pos.x - mouse_pos_backup.x;
            ui_ctx->mouse_delta.y = ui_ctx->mouse_pos.y - mouse_pos_backup.y;
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
            ui_ctx->mouse_scroll_delta.x += GET_WHEEL_DELTA_WPARAM(wparam) / 10;
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            ui_ctx->mouse_scroll_delta.y += GET_WHEEL_DELTA_WPARAM(wparam) / -10;
            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            ui_ctx->mouse_lclick = True;
            ui_ctx->mouse_press = True;
            f64 now = ui_ctx->current_time;
            i32 click_x = GET_X_LPARAM(lparam);
            i32 click_y = GET_Y_LPARAM(lparam);
            f64 double_click_sec = (f64)GetDoubleClickTime() / 1000.0;
            f32 dx = (f32)(click_x - ui_ctx->last_lclick_pos.x);
            f32 dy = (f32)(click_y - ui_ctx->last_lclick_pos.y);
            f32 dist = sqrtf(dx * dx + dy * dy);
            if (now - ui_ctx->last_lclick_time <= double_click_sec &&
                dist <= (f32)GetSystemMetrics(SM_CXDOUBLECLK) * 2.f)
                ui_ctx->mouse_double_click = True;
            ui_ctx->last_lclick_time = now;
            ui_ctx->last_lclick_pos.x = (f32)click_x;
            ui_ctx->last_lclick_pos.y = (f32)click_y;
            SetCapture(window);
            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            ui_ctx->mouse_rclick = True;
            SetCapture(window);
            return 0;
        }

        case WM_LBUTTONUP:
        {
            ui_ctx->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_RBUTTONUP:
        {
            ui_ctx->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            ui_ctx->mouse_press = False;
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
                    /* Build pointer cache */
                    CmdPayload payload = { .ctx = ctx, .window_id = ctx ? ctx->id : 0 };
                    if (ctx && ctx->hovered_panel)
                    {
                        payload.panel = ctx->hovered_panel;
                        PanelTab* at = panel_tab_get_active(ctx->hovered_panel);
                        if (at)
                            payload.tab = at;
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
                                payload.to_panel = next;
                        }
                        if (str_compare(token, str("tab.to_new_panel")))
                            payload.to_panel = ctx ? ctx->hovered_panel : NULL;
                    }

                    if (payload.panel)
                    {
                        char buf[128];
                        i32 len = snprintf(buf, sizeof(buf), "%.*s panel=%u tab=%u to_panel=%u window=%u",
                                           (i32)cmd_text.len, cmd_text.data, payload.panel->id,
                                           (payload.tab ? payload.tab->id : 0),
                                           (payload.to_panel ? payload.to_panel->id : 0), payload.window_id);
                        cmd_queue_push(&shared->cmd_queue, (String){ (u8*)buf, len }, &payload, sizeof(payload));
                    }
                    else
                    {
                        cmd_queue_push(&shared->cmd_queue, cmd_text, &payload, sizeof(payload));
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

    /* Register commands */
    // clang-format off
    cmd_register(&shared.cmd_registry, (CmdDef){ str("window.create"),       str("New Window"),                    str(""), cmd_create_window,       &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("app.toggle_theme"),    str("Toggle Light/Dark Theme"),       str(""), cmd_toggle_theme,        &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_h"),       str("Split Panel Horizontally"),      str(""), cmd_split_panel_h,       &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_v"),       str("Split Panel Vertically"),        str(""), cmd_split_panel_v,       &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.close"),         str("Close Panel"),                   str(""), cmd_close_panel,         &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.new"),             str("New Tab"),                       str(""), cmd_tab_new,             &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.close"),           str("Close Tab"),                     str(""), cmd_tab_close,           &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.activate"),        str("Activate Tab"),                  str(""), cmd_tab_activate,        &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move"),            str("Move Tab"),                      str(""), cmd_tab_move,            &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move_to_panel"),   str("Move Tab To Next Panel"),        str(""), cmd_tab_move_to_panel,   &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.to_new_panel"),    str("Move Tab To New Panel"),         str(""), cmd_tab_to_new_panel,    &shared });

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
    // clang-format on

    /* Load cursors */
    shared.cursors[UI_CURSOR_ARROW] = LoadCursor(NULL, IDC_ARROW);
    shared.cursors[UI_CURSOR_IBEAM] = LoadCursor(NULL, IDC_IBEAM);
    shared.cursors[UI_CURSOR_HAND] = LoadCursor(NULL, IDC_HAND);
    shared.cursors[UI_CURSOR_HORIZONTAL] = LoadCursor(NULL, IDC_SIZEWE);
    shared.cursors[UI_CURSOR_VERTICAL] = LoadCursor(NULL, IDC_SIZENS);

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
    create_window(&shared, L"App Title", CLIENT_WIDTH, CLIENT_HEIGHT);

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
        cmd_queue_execute_all(&shared.cmd_queue, &shared.cmd_registry);

        /* Process frame */
        for (WindowContext* w = shared.first_window; w; w = w->next)
            process_frame(w);
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
    arena_release(&shared.cmd_arena);
    arena_release(&shared.cfg_arena);

    return 0;
}
