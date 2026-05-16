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

#define CLIENT_WIDTH  600
#define CLIENT_HEIGHT 600

#define MAX_TITLE_LENGTH 64
#define TEXT_BUFFER_SIZE 1024
#define MAX_WINDOWS      16
#define CMD_ARENA_SIZE   KB(64)

#define PANEL_BOUNDARY 3.0f
#define PANEL_PCT_MIN  0.05f
#define PANEL_PCT_MAX  0.95f

typedef struct WindowContext WindowContext;

typedef struct
{
    WindowContext* ctx;
    Panel* panel;
    Panel* to_panel;
    PanelTab* tab;
    i32 delta;
    Axis2 axis;
    u32 width;
    u32 height;
    String title;
    b32* toggle;
} CmdPayload;

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

    Panel* root_panel;
    Panel* hovered_panel;

    /* widget needed */
    b32 check;

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
        MultiByteToWideChar(CP_UTF8, 0, (const char*)title.data, (int)title.len, ctx->title, MAX_TITLE_LENGTH - 1);
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
    ctx->root_panel = (Panel*)calloc(1, sizeof(*ctx->root_panel));
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

static void cmd_create_window(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    create_window((AppShared*)userdata, L"Window", p->width, p->height);
}

static void cmd_set_window_title(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    set_window_title(p->ctx, p->title);
}

static void cmd_toggle_check(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->toggle)
        *p->toggle = !*p->toggle;
}

static void cmd_toggle_theme(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)context;
    (void)payload;
    (void)payload_size;
    AppShared* shared = (AppShared*)userdata;
    shared->theme = (shared->theme.bg_base.r == s_theme_dark.bg_base.r) ? s_theme_light : s_theme_dark;
}

static void cmd_split_panel_h(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel)
        panel_split(p->panel, Axis2_X);
}

static void cmd_split_panel_v(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel)
        panel_split(p->panel, Axis2_Y);
}

static void cmd_close_panel(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel)
    {
        p->panel->anim_state = PANEL_ANIM_CLOSING;
        p->panel->anim_to_pct = 0.0f;
    }
}

static void cmd_tab_new(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel)
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(p->panel, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(p->panel, (String){ name_buf, name_len });
    }
}

static void cmd_tab_close(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (!p->panel || !p->tab)
        return;
    /* If this is the only tab left, close the panel instead */
    if (p->panel->tab_first && !p->panel->tab_first->next && p->panel->tab_first == p->tab)
    {
        if (p->panel->parent)
        {
            p->panel->anim_state = PANEL_ANIM_CLOSING;
            p->panel->anim_to_pct = 0.0f;
        }
    }
    else
    {
        panel_tab_close(p->panel, p->tab);
    }
}

static void cmd_tab_move(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel && p->tab)
        panel_tab_move(p->panel, p->tab, p->delta);
}

static void cmd_tab_move_to_panel(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel && p->to_panel && p->tab)
        panel_tab_move_to_panel(p->panel, p->tab, p->to_panel);
}

static void cmd_tab_to_new_panel(void* userdata, void* context, void* payload, isize payload_size)
{
    (void)payload_size;
    CmdPayload* p = (CmdPayload*)payload;
    if (p->panel && p->to_panel && p->tab)
        panel_tab_to_new_panel(p->panel, p->tab, p->to_panel, p->axis);
}

//
// Panel rendering helpers
//

static void panel_render_boundary(const Panel* panel, const Rect panel_rect, const Theme* theme)
{
    Assert(panel->child_a);
    Assert(panel->child_b);

    Panel* child_a = panel->child_a;
    Panel* child_b = panel->child_b;

    Rect child_a_rect = panel_calc_rect_from_parent(child_a, panel_rect);
    Position bound_pos;
    f32 bound_w, bound_h;
    if (panel->split_axis == Axis2_X)
    {
        bound_pos.x = child_a_rect.xmax - (PANEL_BOUNDARY / 2);
        bound_pos.y = child_a_rect.ymin;
        bound_w = PANEL_BOUNDARY;
        bound_h = child_a_rect.ymax - child_a_rect.ymin;
    }
    else
    {
        bound_pos.x = child_a_rect.xmin;
        bound_pos.y = child_a_rect.ymax - (PANEL_BOUNDARY / 2);
        bound_w = child_a_rect.xmax - child_a_rect.xmin;
        bound_h = PANEL_BOUNDARY;
    }

    ui_box({ .sizing = { fixed(bound_w), fixed(bound_h) },
             .flags = BoxFlag_Float,
             .float_offset = bound_pos,
             .direction = (panel->split_axis == Axis2_X) ? LAYOUT_LEFT_TO_RIGHT : LAYOUT_TOP_TO_BOTTOM })
    {
        u8 key_buf[HASH_STR_MAX_LENGTH];
        i32 key_len = snprintf((char*)key_buf, sizeof(key_buf), "###panel_bound_%p", (void*)panel);
        String key = { key_buf, key_len };

        /* Handle interaction */
        UIBoxInteractResult result = ui_box_interact(box, key);
        UISignalFlags flags = result.flags;
        if (ui_hovered(flags))
        {
            Cursor cur = (panel->split_axis == Axis2_X) ? UI_CURSOR_HORIZONTAL : UI_CURSOR_VERTICAL;
            ui_set_desired_cursor(cur);
        }
        if (ui_lclicked(flags))
        {
            child_a->drag_saved_pct = child_a->pct_of_parent;
            child_a->drag_saved_partner_pct = child_b->pct_of_parent;
        }
        if (ui_dragging(flags))
        {
            Position delta = ui_box_drag_delta(result.last_box);
            f32 parent_dim = (panel->split_axis == Axis2_X) ? (panel_rect.xmax - panel_rect.xmin)
                                                            : (panel_rect.ymax - panel_rect.ymin);
            f32 dp = (panel->split_axis == Axis2_X ? delta.x : delta.y) / parent_dim;
            child_a->pct_of_parent = clamp(child_a->drag_saved_pct + dp, PANEL_PCT_MIN, PANEL_PCT_MAX);
            child_b->pct_of_parent = clamp(child_a->drag_saved_partner_pct - dp, PANEL_PCT_MIN, PANEL_PCT_MAX);
        }

        /* Handle transition */
        if (result.last_box)
        {
            update_transition(&result.last_box->hot_t, 20.f,
                              ui_hovered(flags) || ui_pressed(flags) || ui_dragging(flags) ? 1.f : 0.f);
            update_transition(&result.last_box->active_t, 20.f, ui_pressed(flags) || ui_dragging(flags) ? 1.f : 0.f);
        }
        f32 hot_t = result.last_box ? result.last_box->hot_t : 0.f;
        f32 active_t = result.last_box ? result.last_box->active_t : 0.f;
        Color pad_color = lerp_color((Color){ 0, 0, 0, 0 }, theme->border_focus, hot_t);
        Color line_color = lerp_color(theme->border_normal, theme->border_focus, hot_t);
        pad_color = lerp_color(pad_color, theme->border_press, active_t);
        line_color = lerp_color(line_color, theme->border_press, active_t);

        /* Draw split line */
        // clang-format off
        if (panel->split_axis == Axis2_X)
        {
            ui_box({ .sizing = { fixed(PANEL_BOUNDARY / 3), fixed(bound_h) }, .color = pad_color }) { }
            ui_box({ .sizing = { fixed(PANEL_BOUNDARY / 3), fixed(bound_h) }, .color = line_color }) { }
            ui_box({ .sizing = { fixed(PANEL_BOUNDARY / 3), fixed(bound_h) }, .color = pad_color }) { }
        }
        else
        {
            ui_box({ .sizing = { fixed(bound_w), fixed(PANEL_BOUNDARY / 3) }, .color = pad_color }) { }
            ui_box({ .sizing = { fixed(bound_w), fixed(PANEL_BOUNDARY / 3) }, .color = line_color }) { }
            ui_box({ .sizing = { fixed(bound_w), fixed(PANEL_BOUNDARY / 3) }, .color = pad_color }) { }
        }
        // clang-format on
    }
}

static void panel_render_boundaries(const Panel* root, const Rect root_rect, const Theme* theme)
{
    for (const Panel* p = root; p; p = panel_iter_next(p))
    {
        if (!p->child_a)
            continue;
        Rect panel_rect = panel_calc_rect(p, root_rect);
        panel_render_boundary(p, panel_rect, theme);
    }
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
    cmd_queue_execute_all(&shared->cmd_queue, &shared->cmd_registry, ctx);
    isize arena_pos_backup = ui_frame_begin(ui_context);
    {
        ctx->root_panel = panel_update_animations(ctx->root_panel, g_ui_context->current_time);
        f32 client_w = (f32)ui_context->client_width;
        f32 client_h = (f32)ui_context->client_height;
        Rect root_rect = { 0, 0, client_w, client_h };
        ctx->hovered_panel = NULL;

        ui_box({
            .sizing = { fixed(client_w), fixed(client_h) },
            .color = theme->bg_base,
        })
        {
            panel_render_boundaries(ctx->root_panel, root_rect, theme);

            for (Panel* p = ctx->root_panel; p; p = panel_iter_next(p))
            {
                if (p->child_a)
                    continue;
                Rect rect = panel_calc_rect(p, root_rect);
                f32 pad = 2.0f;
                Rect inner = { rect.xmin + pad, rect.ymin + pad, rect.xmax - pad, rect.ymax - pad };
                f32 iw = max(0.0f, inner.xmax - inner.xmin);
                f32 ih = max(0.0f, inner.ymax - inner.ymin);
                if (iw < 1.0f || ih < 1.0f)
                    continue;

                b32 mouse_in = ui_context->mouse_pos.x >= rect.xmin && ui_context->mouse_pos.x < rect.xmax &&
                               ui_context->mouse_pos.y >= rect.ymin && ui_context->mouse_pos.y < rect.ymax;
                if (!ctx->hovered_panel && mouse_in)
                    ctx->hovered_panel = p;

                /* Re-declare all existing tabs so they survive cleanup */
                for (PanelTab* tab = p->tab_first; tab; tab = tab->next)
                {
                    String tab_name = { tab->name, tab->name_len };
                    panel_tab_declare(p, tab_name);
                }

                ui_box({
                    .sizing = { fixed(iw), fixed(ih) },
                    .flags = BoxFlag_Float,
                    .float_offset = { inner.xmin, inner.ymin },
                    .color = theme->bg_surface,
                    .child_gap = s_child_gap_small,
                    .direction = LAYOUT_TOP_TO_BOTTOM,
                })
                {
                    u8 key_buf[HASH_STR_MAX_LENGTH];
                    i32 key_len = snprintf((char*)key_buf, sizeof(key_buf), "###panel_%p", (void*)p);
                    String key = { key_buf, key_len };

                    ui_box_interact(box, key);

                    /* Tab bar */
                    ui_box({
                        .sizing = { fit_grow({}), fit({}) },
                        .direction = LAYOUT_LEFT_TO_RIGHT,
                        .child_gap = 2,
                    })
                    {
                        PanelTab* active = panel_tab_get_active(p);

                        for (PanelTab* tab = p->tab_first; tab; tab = tab->next)
                        {
                            b32 is_active = (tab == active);

                            u8 tab_key[HASH_STR_MAX_LENGTH];
                            i32 tab_key_len = snprintf((char*)tab_key, sizeof(tab_key), "###tab_%p_%.*s", (void*)p,
                                                       (int)tab->name_len, tab->name);
                            String tab_key_str = { tab_key, tab_key_len };

                            Color bg_normal = is_active ? theme->bg_base : (Color){ 0, 0, 0, 0 };
                            Color bg_hover = is_active ? theme->bg_base : theme->bg_overlay;
                            Color fg = is_active ? theme->accent : theme->fg_secondary;
                            f32 font_sz = 12;

                            ui_box({
                                .sizing = { fit({}), fit({}) },
                                .padding = { 4, 10, 4, 10 },
                                .direction = LAYOUT_LEFT_TO_RIGHT,
                                .child_gap = 4,
                                .alignment = { ALIGN_START, ALIGN_CENTER },
                                .rect_style = { .corner_radius = 4 },
                            })
                            {
                                UIBoxInteractResult r = ui_box_interact(box, tab_key_str);
                                if (r.last_box)
                                    update_transition(&r.last_box->hot_t, 20.f, ui_hovered(r.flags) ? 1.f : 0.f);
                                f32 hot = r.last_box ? r.last_box->hot_t : 0.f;
                                box->config.color = lerp_color(bg_normal, bg_hover, hot);

                                if (ui_lclicked(r.flags))
                                    panel_tab_activate(p, tab);

                                ui_text((String){ tab->name, tab->name_len }, &(TextConfig){ .font = font_ui,
                                                                                             .font_size = font_sz,
                                                                                             .color = fg,
                                                                                             .line_height = font_sz });

                                /* Close button */
                                u8 ck[HASH_STR_MAX_LENGTH];
                                i32 cl = snprintf((char*)ck, sizeof(ck), "×##tc_%p_%.*s", (void*)p, (int)tab->name_len,
                                                  tab->name);
                                Color cb_normal = is_active ? theme->fg_secondary : (Color){ 0, 0, 0, 0 };
                                Color cb_hover = theme->fg_secondary;
                                UISignalFlags cf =
                                    ui_button((String){ ck, cl }, font_ui, 10, (Sizing){ fixed(14), fixed(14) },
                                              (Padding){ 0 }, cb_normal, cb_normal, cb_hover, cb_hover);
                                if (ui_lclicked(cf))
                                {
                                    cmd_queue_push(&shared->cmd_queue, str("tab.close"),
                                                   &(CmdPayload){ .ctx = ctx, .panel = p, .tab = tab },
                                                   sizeof(CmdPayload));
                                }
                            }
                        }

                        /* Spacer */
                        ui_box({ .sizing = { grow({}), fit({}) } })
                        {
                        }

                        /* New tab button */
                        u8 plus_key[HASH_STR_MAX_LENGTH];
                        i32 plus_len = snprintf((char*)plus_key, sizeof(plus_key), "+##tab_add_%p", (void*)p);
                        UISignalFlags plus_flags =
                            ui_button((String){ plus_key, plus_len }, font_ui, 12, (Sizing){ fixed(22), fixed(22) },
                                      (Padding){ 0 }, (Color){ 0, 0, 0, 0 }, theme->fg_secondary, theme->bg_overlay,
                                      theme->bg_base);
                        if (ui_lclicked(plus_flags))
                        {
                            cmd_queue_push(&shared->cmd_queue, str("tab.new"), &(CmdPayload){ .ctx = ctx, .panel = p },
                                           sizeof(CmdPayload));
                        }

                        /* Close panel button */
                        u8 close_key[HASH_STR_MAX_LENGTH];
                        i32 close_len = snprintf((char*)close_key, sizeof(close_key), "X##panel_close_%p", (void*)p);
                        String close_str = { close_key, close_len };
                        UISignalFlags close_flags =
                            ui_button(close_str, font_ui, 12, (Sizing){ fixed(22), fixed(22) }, (Padding){ 0 },
                                      (Color){ 0, 0, 0, 0 }, theme->fg_secondary, theme->danger, theme->danger);
                        if (p->parent && ui_lclicked(close_flags))
                        {
                            cmd_queue_push(&shared->cmd_queue, str("panel.close"),
                                           &(CmdPayload){ .ctx = ctx, .panel = p }, sizeof(CmdPayload));
                        }
                    }

                    /* Panel content — rendered only if an active tab exists */
                    PanelTab* active_tab = panel_tab_get_active(p);
                    if (active_tab)
                    {
                        u8 sa_key[HASH_STR_MAX_LENGTH];
                        i32 sa_len = snprintf((char*)sa_key, sizeof(sa_key), "panel_scroll_%p", (void*)p);
                        ui_scrollable_area({
                            (String){ sa_key, sa_len },
                            (Sizing){ grow({}), grow({}) },
                            theme->bg_base,
                            s_padding_small,
                            theme->scrollbar_thumb,
                        })
                        {
                            ui_box({
                                .sizing = { fixed(400), fit_grow({}) },
                                .color = theme->bg_surface,
                                .padding = s_padding_small,
                                .child_gap = s_child_gap_medium,
                                .direction = LAYOUT_TOP_TO_BOTTOM,
                            })
                            {
                                String tab_label = { active_tab->name, active_tab->name_len };
                                ui_text(tab_label, &(TextConfig){ .font = font_ui,
                                                                  .font_size = 14,
                                                                  .color = theme->accent,
                                                                  .line_height = 20 });

                                ui_text(str("Ctrl+Shift+H / Ctrl+Shift+V to split horizontally / vertically."),
                                        &(TextConfig){ .font = font_ui,
                                                       .font_size = 12,
                                                       .color = theme->fg_primary,
                                                       .line_height = 24 });
                                ui_text(str("Ctrl+T new tab. Ctrl+W close tab. F11 toggle theme."),
                                        &(TextConfig){ .font = font_ui,
                                                       .font_size = 12,
                                                       .color = theme->fg_secondary,
                                                       .line_height = 24 });
                                ui_text(str("Ctrl+Shift+Left/Right to reorder tabs."),
                                        &(TextConfig){ .font = font_ui,
                                                       .font_size = 12,
                                                       .color = theme->fg_secondary,
                                                       .line_height = 24 });
                                ui_text(str("Ctrl+Shift+N moves tab to next panel."),
                                        &(TextConfig){ .font = font_ui,
                                                       .font_size = 12,
                                                       .color = theme->fg_secondary,
                                                       .line_height = 24 });
                                ui_text(str("Ctrl+Shift+F/G to detach tab as new panel (H/V)."),
                                        &(TextConfig){ .font = font_ui,
                                                       .font_size = 12,
                                                       .color = theme->fg_secondary,
                                                       .line_height = 24 });

                                u8 button_key[HASH_STR_MAX_LENGTH];
                                i32 button_len = snprintf((char*)button_key, sizeof(button_key),
                                                          "New Window##panel_button_%p", (void*)p);
                                String button_str = { button_key, button_len };

                                UISignalFlags button_flags = ui_button(
                                    button_str, font_mono, 12, (Sizing){ fit({}), fit({}) }, s_padding_small,
                                    theme->accent, theme->accent_fg, theme->accent_hover, theme->accent_press);
                                if (ui_lclicked(button_flags))
                                {
                                    cmd_queue_push(&shared->cmd_queue, str("window.create"),
                                                   &(CmdPayload){ .width = CLIENT_WIDTH, .height = CLIENT_HEIGHT },
                                                   sizeof(CmdPayload));
                                }
                            }
                        }
                    }
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

        case WM_SETCURSOR:
        {
            if (LOWORD(lparam) == HTCLIENT)
            {
                SetCursor(shared->cursors[ui_context->desired_cursor]);
                return True;
            }
            return DefWindowProcW(window, message, wparam, lparam);
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
        {
            ui_context->mouse_released = True;
            ui_context->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_RBUTTONUP:
        {
            ui_context->mouse_press = False;
            ReleaseCapture();
            return 0;
        }

        case WM_CAPTURECHANGED:
        {
            ui_context->mouse_released = True;
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
                String cmd_id = shortcut_lookup(&shared->shortcuts, sc);
                if (cmd_id.len)
                {
                    /* Build payload — each handler only reads the fields it needs */
                    CmdPayload payload = { .ctx = ctx };
                    if (ctx && ctx->hovered_panel)
                    {
                        payload.panel = ctx->hovered_panel;
                        PanelTab* at = panel_tab_get_active(ctx->hovered_panel);
                        if (at)
                            payload.tab = at;
                    }
                    if (wparam == VK_LEFT)
                        payload.delta = -1;
                    if (wparam == VK_RIGHT)
                        payload.delta = +1;
                    payload.axis = (wparam == 'F') ? Axis2_X : Axis2_Y;

                    /* Resolve next leaf for move-to-panel */
                    if (str_compare(cmd_id, str("tab.move_to_panel")) && ctx && ctx->hovered_panel)
                    {
                        Panel* next = ctx->hovered_panel;
                        do
                        {
                            next = panel_iter_next(next);
                        } while (next && next->child_a);
                        if (next)
                            payload.to_panel = next;
                    }

                    /* Use hovered panel as anchor for to-new-panel */
                    if (str_compare(cmd_id, str("tab.to_new_panel")))
                        payload.to_panel = ctx ? ctx->hovered_panel : NULL;

                    cmd_queue_push(&shared->cmd_queue, cmd_id, &payload, sizeof(payload));
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
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move"),            str("Move Tab"),                      str(""), cmd_tab_move,            &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.move_to_panel"),   str("Move Tab To Next Panel"),        str(""), cmd_tab_move_to_panel,   &shared });
    cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.to_new_panel"),    str("Move Tab To New Panel"),         str(""), cmd_tab_to_new_panel,    &shared });

    /* Bind shortcuts */
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'T' },      str("tab.new"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                      'W' },      str("tab.close"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'H' },      str("panel.split_h"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'V' },      str("panel.split_v"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, VK_LEFT },  str("tab.move"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, VK_RIGHT }, str("tab.move"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'N' },      str("tab.move_to_panel"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'F' },      str("tab.to_new_panel"));
    shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT, 'G' },      str("tab.to_new_panel"));
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
