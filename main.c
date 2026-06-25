#pragma comment(lib, "kernel32")
#pragma comment(lib, "user32")
#pragma comment(lib, "shcore")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "shell32")

#include "cmd.h"
#include "dict.h"
#include "glyph_cache.h"
#include "ocr.h"
#include "overlay_dcomp.h"
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
#define DICT_MAX_POS                  32
#define DICT_TOKEN_BLOCK_MAX          256
#define DICT_TOKEN_TOTAL_MAX          32768

#define WM_TRAY_CALLBACK         (WM_APP + 1)
#define TRAY_ICON_ID             1
#define HOTKEY_TOGGLE_FOREGROUND 1
#define HOTKEY_QUICK_SEARCH      2
#define QUICK_SEARCH_WINDOW_W    540
#define QUICK_SEARCH_WINDOW_H    420

/*
 * DictWordToken — per-word metadata for interaction hit-test.
 * Built from TextData.words after each ui_text() call on English text blocks.
 */
typedef struct
{
    String text;
    f32 width;
    i32 byte_start;
    i32 byte_end;
    u32 dict_word_idx;
    i32 line_index;
    f32 x_on_line;
} DictWordToken;

typedef struct
{
    DictWordToken* tokens;
    i32 token_count;
    UIBox* text_box;
} DictWordBlock;

/*
 * DictWordDragPayload — 64-byte drag payload stored in UIContext.drag_payload_buf.
 */
typedef struct
{
    u32 drag_type;
    i32 content_data;
    u32 from_window_id;
    u32 _reserved;
    char title[48];
} DictWordDragPayload;

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
    PALETTE_VIEW_EMPTY,
    PALETTE_VIEW_LOADING,
    PALETTE_VIEW_RESULTS,
    PALETTE_VIEW_NO_MATCH,
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

    Color hint_bg;
    Color hint_fg;

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

    /* dict content */
    Color dict_word_fg;
    Color dict_phonetic_fg;
    Color dict_separator;
    Color dict_card_bg;
    Color dict_definition_fg;
    Color dict_example_bg;
    Color dict_example_fg;
    Color dict_example_zh_fg;
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
    WindowContext* tray_window;
    WindowContext* quick_search_window;
    WindowContext* last_active_main_window;
    HWND tray_hwnd;
    Arena cfg_arena;
    Arena cmd_arena;
    CmdRegistry cmd_registry;
    ShortcutRegistry shortcuts;
    CmdQueue cmd_queue;
    SearchState palette_search;
    DictDB dict_db;
    Arena dict_arena;
    volatile b32 dict_ready;
    DWORD main_thread_id;
    DictSearchAuxEntry* search_aux;
    Arena search_aux_arena;

    b32 cross_drag_active;
    u8 cross_drag_payload_buf[DRAG_PAYLOAD_MAX];
    isize cross_drag_payload_size;
    u32 cross_drag_source_window_id;

    DragPopup* drag_popup;

    Color accent_border_color;
    b32 has_accent_border;

    b32 ocr_async_active;
} AppShared;

struct WindowContext
{
    HWND window;
    wchar_t title[MAX_TITLE_LENGTH];
    UIContext ui;
    Renderer renderer;
    IDCompositionDevice* dcomp_device;
    IDCompositionTarget* dcomp_target;
    IDCompositionVisual* dcomp_visual;
    AppShared* shared;
    WindowContext* next;
    u32 id;

    Panel* root_panel;
    Panel* focused_panel;

    b32 check;

    u8 text_buf_1[TEXT_BUFFER_SIZE];
    u8 text_buf_2[TEXT_BUFFER_SIZE];
    TextEditState text_edit_1;
    TextEditState text_edit_2;

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

    OverlayPopup menu_popup;
    OverlayPopup palette_popup;

    u8 palette_text_buf[SEARCH_QUERY_BUF];
    TextEditState palette_text_edit;
    i32 palette_selected_index;
    i32 palette_prev_selected_index;
    isize palette_prev_query_len;
    b32 palette_activate_pending;
    SearchPaletteMode palette_search_mode;
    SearchPaletteMode palette_effective_mode;
    volatile LONG palette_switch_version;

    /* dict POS selector state */
    u8 dict_pos_count;
    u8 dict_pos_kinds[DICT_MAX_POS];
    u8 dict_cur_pos;
    b32 dict_content_active;

    /* dict word token arrays */
    Arena dict_token_arena;
    DictWordBlock* dict_word_blocks;
    i32 dict_word_block_count;
    i32 dict_word_total_tokens;

    /* IME */
    HIMC default_himc;

    b32 in_frame;
    b32 mouse_tracked;

    b32 is_quick_search;
    b32 quick_search_live_preview;
    i32 quick_search_confirmed_word_idx;
    b32 quick_search_result_confirmed;
    b32 quick_search_closing;
};

static DictDB* g_dict_db;
static DictSearchAuxEntry* g_search_aux;

static u64 s_search_palette_input_hash;
static u32 s_window_next_id = 1;
static u16 s_utf16_pending_high = 0;
static HHOOK g_mouse_hook;
static HWND g_ocr_tray_hwnd;

static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_LBUTTONDOWN)
    {
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
        {
            if (g_ocr_tray_hwnd)
            {
                MSLLHOOKSTRUCT* hs = (MSLLHOOKSTRUCT*)lParam;
                PostMessageW(g_ocr_tray_hwnd, WM_APP_OCR_TRIGGER, (WPARAM)hs->pt.x, (LPARAM)hs->pt.y);
            }
            return 1;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

//
// Theme
//

// clang-format off
static const Theme s_theme_light = {
    /* general */
    .accent_bg          = rgba(53,  132, 228, 255),
    .accent_fg          = rgba(255, 255, 255, 255),
    .accent_subtle_bg   = rgba(98,  160, 234, 255),
    .accent_subtle_fg   = rgba(255, 255, 255, 255),
    .accent_weak_bg     = rgba(153, 193, 241, 255),
    .accent_weak_fg     = rgba(255, 255, 255, 255),

    .hover_bg           = rgba(222, 222, 224, 255),
    .hover_fg           = rgba(61,  61,  61,  255),
    .active_bg          = rgba(216, 216, 219, 255),
    .active_fg          = rgba(34,  34,  34,  255),
    .press_bg           = rgba(205, 205, 207, 255),
    .press_fg           = rgba(0,   0,   0,   255),

    .destructive_bg     = rgba(224, 27,  36,  255),
    .destructive_fg     = rgba(255, 255, 255, 255),
    .success_bg         = rgba(46,  194, 126, 255),
    .success_fg         = rgba(255, 255, 255, 255),
    .warning_bg         = rgba(229, 165, 10,  255),
    .warning_fg         = rgba(0,   0,   0,   255),

    .hint_bg            = rgba(238, 238, 238, 255),
    .hint_fg            = rgba(119, 118, 123, 255),

    .border             = rgba(192, 191, 188, 255),
    .scrollbar_thumb    = rgba(94,  92,  100, 80 ),
    .scrollbar_track    = rgba(192, 191, 188, 80 ),
    .shadow             = rgba(192, 191, 188, 255),

    /* specific */
    .bar_bg             = rgba(228, 227, 227, 255),
    .bar_fg             = rgba(0,   0,   0,   255),

    .panel_bg           = rgba(255, 255, 255, 255),
    .panel_fg           = rgba(0,   0,   0,   255),

    .palette_bg         = rgba(246, 245, 244, 255),
    .palette_fg         = rgba(0,   0,   0,   255),

    .cursor_bar         = rgba(34,  34,  38,  255),
    .cursor_trail       = rgba(46,  46,  46,  255),
    .selection          = rgba(192, 191, 188, 255),
    .selection_flash    = rgba(144, 83,  0,   255),

    .dict_word_fg       = rgba(34,  34,  38,  255),
    .dict_phonetic_fg   = rgba(119, 118, 123, 255),
    .dict_separator     = rgba(192, 191, 188, 255),
    .dict_card_bg       = rgba(246, 245, 244, 255),
    .dict_definition_fg = rgba(34,  34,  38,  255),
    .dict_example_bg    = rgba(232, 232, 232, 255),
    .dict_example_fg    = rgba(90,  90,  90,  255),
    .dict_example_zh_fg = rgba(119, 118, 123, 255),
};

static const Theme s_theme_dark = {
    /* general */
    .accent_bg        = rgba(23,  102, 178, 255),
    .accent_fg        = rgba(255, 255, 255, 255),
    .accent_subtle_bg = rgba(38,  100, 154, 255),
    .accent_subtle_fg = rgba(255, 255, 255, 255),
    .accent_weak_bg   = rgba(102, 142, 190, 255),
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

    .hint_bg          = rgba(46,  46,  46,  255),
    .hint_fg          = rgba(154, 153, 150, 255),

    .border           = rgba(64,  64,  64,  255),
    .scrollbar_thumb  = rgba(192, 191, 188, 80 ),
    .scrollbar_track  = rgba(94,  92,  100, 80 ),
    .shadow           = rgba(10,  10,  10,  255),

    /* specific */
    .bar_bg           = rgba(35,  35,  35,  255),
    .bar_fg           = rgba(210, 210, 210, 255),

    .panel_bg         = rgba(19,  19,  19,  255),
    .panel_fg         = rgba(210, 210, 210, 255),

    .palette_bg       = rgba(46,  46,  47,  255),
    .palette_fg       = rgba(255, 255, 255, 255),

    .cursor_bar       = rgba(255, 255, 255, 255),
    .cursor_trail     = rgba(246, 245, 244, 255),
    .selection        = rgba(94,  92,  100, 255),
    .selection_flash  = rgba(255, 192, 87,  255),

    .dict_word_fg       = rgba(255, 255, 255, 255),
    .dict_phonetic_fg   = rgba(154, 153, 150, 255),
    .dict_separator     = rgba(94,  92,  100, 255),
    .dict_card_bg       = rgba(24,  24,  24,  255),
    .dict_definition_fg = rgba(246, 245, 244, 255),
    .dict_example_bg    = rgba(36,  36,  36,  255),
    .dict_example_fg    = rgba(192, 191, 188, 255),
    .dict_example_zh_fg = rgba(154, 153, 150, 255),
};
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
static WindowContext* create_quick_search_window(AppShared* shared);
static WindowContext* find_quick_search_window(AppShared* shared);

static WindowContext* create_window(AppShared* shared, const wchar_t* title, i32 pos_x, i32 pos_y, u32 width,
                                    u32 height, b32 add_default_tab)
{
    WindowContext* ctx = (WindowContext*)calloc(1, sizeof(WindowContext));
    Assert(ctx);
    ctx->shared = shared;
    ctx->id = s_window_next_id++;
    ctx->dict_token_arena = arena_new(MB(1), KB(64));

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

    /* Save default IME context for IME disable/enable control */
    ctx->default_himc = ImmGetContext(ctx->window);
    ImmReleaseContext(ctx->window, ctx->default_himc);

    /* Add to window list */
    window_list_add(shared, ctx);

    /* Restore hidden tray window when a new window is created */
    if (shared->tray_window)
    {
        ShowWindow(shared->tray_window->window, SW_SHOW);
        shared->tray_window = NULL;
    }

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

static WindowContext* create_quick_search_window(AppShared* shared)
{
    WindowContext* ctx = (WindowContext*)calloc(1, sizeof(WindowContext));
    Assert(ctx);
    ctx->shared = shared;
    ctx->id = s_window_next_id++;
    ctx->is_quick_search = True;

    /* Use the DPI of the monitor where the last active main window lives,
       not the system-wide DPI, so mixed-DPI multi-monitor setups work correctly. */
    HMONITOR mon = MonitorFromWindow(shared->last_active_main_window ? shared->last_active_main_window->window : NULL, MONITOR_DEFAULTTONEAREST);
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    {
        UINT dpi_x = 0, dpi_y = 0;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
        if (dpi_x > 0)
            dpi = dpi_x;
    }
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_w = (u32)(QUICK_SEARCH_WINDOW_W * dpi_scale);
    u32 physical_h = (u32)(QUICK_SEARCH_WINDOW_H * dpi_scale);
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    i32 work_w = mi.rcWork.right - mi.rcWork.left;
    i32 work_h = mi.rcWork.bottom - mi.rcWork.top;
    i32 pos_x = mi.rcWork.left + (work_w - (i32)physical_w) / 2;
    i32 pos_y = mi.rcWork.top + (work_h - (i32)physical_h) / 2;

    ctx->window =
        CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"window class", L"", WS_POPUP,
                        pos_x, pos_y, (i32)physical_w, (i32)physical_h, NULL, NULL, GetModuleHandleW(NULL), ctx);
    if (!ctx->window)
    {
        free(ctx);
        return NULL;
    }
    {
        BOOL use_dark = TRUE;
        DwmSetWindowAttribute(ctx->window, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
        SetPropW(ctx->window, L"NonRudeHWND", (HANDLE)TRUE);
    }

    /* Init UI context */
    UIRenderFunc render_fn = {
        .flush_and_present = renderer_flush_overlay_and_present,
        .on_resize = renderer_resize,
        .wait_for_last_submitted_frame = renderer_wait_for_last_submitted_frame,
        .get_text_width = renderer_get_text_width_for_dpi,
        .get_text_height = renderer_get_text_height_for_dpi,
        .draw_rect = renderer_draw_rect,
        .draw_text = renderer_draw_text,
    };

    ui_init(ctx->window, &ctx->ui, &ctx->renderer, &shared->raster_cache, ctx->ui.client_width, ctx->ui.client_height,
            dpi, render_fn);
    ctx->ui.clipboard_copy = win32_clipboard_copy;
    ctx->ui.clipboard_paste = win32_clipboard_paste;

    /* Configure renderer for DComp composition (must be set before renderer_init — see memset trap) */
    ctx->renderer.is_composition = True;
    ctx->renderer.initial_width = physical_w;
    ctx->renderer.initial_height = physical_h;

    /* Init per-window renderer */
    renderer_init(&ctx->renderer, &shared->renderer_shared, ctx->window);

    /* Init DComp device + visual tree */
    renderer_init_dcomp(&ctx->renderer, ctx->window, &ctx->dcomp_device, &ctx->dcomp_target, &ctx->dcomp_visual);

    /* Init palette text edit state */
    ctx->palette_text_edit.base = ctx->palette_text_buf;
    ctx->palette_text_edit.size = SEARCH_QUERY_BUF;
    ctx->palette_selected_index = -1;
    ctx->palette_prev_selected_index = -1;
    ctx->palette_prev_query_len = 0;
    ctx->palette_activate_pending = False;
    ctx->palette_search_mode = PALETTE_MODE_WORD;
    ctx->palette_effective_mode = PALETTE_MODE_WORD;
    ctx->palette_switch_version = 0;

    /* Save default IME context for IME enable/disable control */
    ctx->default_himc = ImmGetContext(ctx->window);
    ImmReleaseContext(ctx->window, ctx->default_himc);

    shared->quick_search_window = ctx;

    /* Palette opens automatically — set focus routing */
    ctx->palette_popup.open = True;
    ctx->ui.focused_hash = s_search_palette_input_hash;

    /* Stay hidden until first global hotkey activation */
    ShowWindow(ctx->window, SW_HIDE);

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

static void quick_search_activate(AppShared* shared)
{
    if (!shared->dict_ready)
        return;

    WindowContext* qs = find_quick_search_window(shared);
    if (!qs)
    {
        qs = create_quick_search_window(shared);
        if (!qs)
            return;
    }

    qs->palette_popup.open = True;
    qs->quick_search_result_confirmed = False;
    qs->quick_search_closing = False;
    qs->palette_selected_index = 0;
    qs->palette_prev_selected_index = -1;
    qs->palette_prev_query_len = 0;
    qs->palette_activate_pending = False;
    qs->palette_search_mode = PALETTE_MODE_WORD;

    /* Do NOT set palette_effective_mode — let render detect mismatch and reconfigure. */
    // qs->palette_effective_mode = PALETTE_MODE_WORD;

    qs->palette_switch_version = 0;

    /* Preserve previous input, auto-select-all */
    qs->palette_text_edit.cursor = qs->palette_text_edit.text_len;
    qs->palette_text_edit.mark = 0;
    qs->ui.focused_hash = s_search_palette_input_hash;
    qs->ui.requested_frames = IDLE_WAKE_FRAMES;

    /* Detect whether the foreground window before activation is one of our main windows.
       Only then do we permit live preview updates to the focused tab content
       (hidden/minimized/foreign windows are never foreground). */
    {
        qs->quick_search_live_preview = False;
        HWND prev_fg = GetForegroundWindow();
        if (prev_fg)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(prev_fg, &pid);
            if (pid == GetCurrentProcessId())
            {
                for (WindowContext* w = shared->first_window; w; w = w->next)
                {
                    if (w->window == prev_fg)
                    {
                        qs->quick_search_live_preview = True;
                        break;
                    }
                }
            }
        }
    }

    /* Re-detect DPI — hidden windows may have missed WM_DPICHANGED
       when monitor topology changed (undock, disconnect external display, etc.) */
    {
        UINT current_dpi = GetDpiForWindow(qs->window);
        if (current_dpi != qs->ui.dpi)
        {
            qs->ui.dpi = current_dpi;
            renderer_recreate_glyph_atlas_texture(&qs->renderer);

            f32 dpi_scale = (f32)current_dpi / USER_DEFAULT_SCREEN_DPI;
            u32 physical_w = (u32)(QUICK_SEARCH_WINDOW_W * dpi_scale);
            u32 physical_h = (u32)(QUICK_SEARCH_WINDOW_H * dpi_scale);

            /* Re-center on the monitor the window is currently on */
            HMONITOR mon = MonitorFromWindow(qs->window, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { .cbSize = sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            i32 work_w = mi.rcWork.right - mi.rcWork.left;
            i32 work_h = mi.rcWork.bottom - mi.rcWork.top;
            i32 pos_x = mi.rcWork.left + (work_w - (i32)physical_w) / 2;
            i32 pos_y = mi.rcWork.top + (work_h - (i32)physical_h) / 2;

            /* SetWindowPos triggers WM_SIZE → renderer_resize + logical dims update */
            SetWindowPos(qs->window, NULL, pos_x, pos_y, (i32)physical_w, (i32)physical_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    ShowWindow(qs->window, SW_SHOW);
    SetForegroundWindow(qs->window);
}

static LRESULT CALLBACK tray_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    AppShared* shared = NULL;
    LONG_PTR ptr = GetWindowLongPtrW(window, GWLP_USERDATA);
    if (ptr)
        shared = (AppShared*)ptr;

    switch (message)
    {
        case WM_CREATE:
        {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)(cs->lpCreateParams));
            return 0;
        }
        case WM_TRAY_CALLBACK:
        {
            if (shared && wparam == TRAY_ICON_ID)
            {
                if (lparam == WM_LBUTTONUP)
                {
                    if (shared->tray_window)
                    {
                        HWND hwnd = shared->tray_window->window;
                        shared->tray_window = NULL;
                        ShowWindow(hwnd, SW_SHOW);
                        SetForegroundWindow(hwnd);
                        if (IsIconic(hwnd))
                            ShowWindow(hwnd, SW_RESTORE);
                    }
                    else if (shared->first_window)
                    {
                        SetForegroundWindow(shared->first_window->window);
                    }
                }
                else if (lparam == WM_RBUTTONUP)
                {
                    HMENU menu = CreatePopupMenu();
                    UINT cmd = 0;
                    if (menu)
                    {
                        AppendMenuW(menu, MF_STRING, 1, L"Exit");
                        POINT pt;
                        GetCursorPos(&pt);
                        if (shared->tray_window)
                            SetForegroundWindow(shared->tray_window->window);
                        else if (shared->first_window)
                            SetForegroundWindow(shared->first_window->window);
                        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, window, NULL);
                        DestroyMenu(menu);
                    }
                    PostMessageW(window, WM_NULL, 0, 0);
                    if (cmd == 1)
                    {
                        NOTIFYICONDATAW nid = { 0 };
                        nid.cbSize = sizeof(NOTIFYICONDATAW);
                        nid.hWnd = shared->tray_hwnd;
                        nid.uID = TRAY_ICON_ID;
                        Shell_NotifyIconW(NIM_DELETE, &nid);

                        shared->tray_window = NULL;
                        while (shared->first_window)
                            DestroyWindow(shared->first_window->window);
                    }
                }
            }
            return 0;
        }
        case WM_HOTKEY:
        {
            if (shared && wparam == HOTKEY_TOGGLE_FOREGROUND)
            {
                HWND foreground = GetForegroundWindow();
                WindowContext* ctx = NULL;
                for (WindowContext* w = shared->first_window; w; w = w->next)
                    if (w->window == foreground)
                    {
                        ctx = w;
                        break;
                    }

                if (ctx)
                {
                    b32 is_last = (!ctx->next && shared->first_window == ctx);
                    if (is_last)
                    {
                        shared->tray_window = ctx;
                        ShowWindow(ctx->window, SW_HIDE);
                    }
                    else
                    {
                        DestroyWindow(ctx->window);
                    }
                }
                else
                {
                    HWND target = NULL;
                    if (shared->tray_window)
                    {
                        target = shared->tray_window->window;
                        shared->tray_window = NULL;
                    }
                    else if (shared->first_window)
                        target = shared->first_window->window;
                    if (target)
                    {
                        ShowWindow(target, SW_SHOW);
                        SetForegroundWindow(target);
                        if (IsIconic(target))
                            ShowWindow(target, SW_RESTORE);
                    }
                }
            }
            else if (shared && wparam == HOTKEY_QUICK_SEARCH)
            {
                if (!shared->dict_ready)
                    return 0;

                WindowContext* qs = find_quick_search_window(shared);

                /* Quick-search already focused → close (toggle) */
                if (qs && GetForegroundWindow() == qs->window)
                {
                    qs->palette_popup.open = False;
                    qs->quick_search_result_confirmed = False;
                    qs->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }

                quick_search_activate(shared);
            }
            return 0;
        }
        case WM_APP_OCR_TRIGGER:
        {
            if (!shared || !shared->dict_ready || shared->ocr_async_active)
                return 0;
            shared->ocr_async_active = True;

            POINT cursor;
            cursor.x = (i32)wparam;
            cursor.y = (i32)lparam;

            HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            UINT dpiX = USER_DEFAULT_SCREEN_DPI, dpiY = USER_DEFAULT_SCREEN_DPI;
            GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
            f32 scale = (f32)dpiX / USER_DEFAULT_SCREEN_DPI;
            i32 half = (i32)(200.0f * scale);
            i32 size = half * 2;

            ocr_recognize_region_async(cursor.x - half, cursor.y - half, (u32)size, (u32)size, cursor.x, cursor.y,
                                       window);
            return 0;
        }
        case WM_APP_OCR_RESULT:
        {
            if (!shared)
                return 0;
            shared->ocr_async_active = False;

            u8* text = (u8*)wparam;
            OcrWordBbox* bbox = (OcrWordBbox*)lparam;

            if (!text || !text[0])
            {
                if (text)
                    free(text);
                if (bbox)
                    free(bbox);
                return 0;
            }

            /* Strip leading/trailing punctuation so "--build" → "build", "apples." → "apples" */
            ascii_word_strip((char*)text, (isize)strlen((const char*)text));
            if (!text[0])
            {
                free(text);
                if (bbox)
                    free(bbox);
                return 0;
            }

            i32 word_idx = dict_resolve(&shared->dict_db, (const char*)text);

            if (word_idx >= 0)
            {
                /* Restore or find target main window */
                WindowContext* main = shared->tray_window;
                if (!main)
                    main = shared->last_active_main_window;
                if (!main)
                    main = shared->first_window;
                if (main)
                {
                    if (shared->tray_window == main)
                        shared->tray_window = NULL;
                    ShowWindow(main->window, SW_SHOW);
                    SetForegroundWindow(main->window);
                    if (IsIconic(main->window))
                        ShowWindow(main->window, SW_RESTORE);

                    cmd_queue_push(&shared->cmd_queue,
                                   str_fmt(CMD_STR_MAX_LENGTH,
                                           "tab.open_word content_data=%d window=%u replace_active=1",
                                           (i32)(word_idx << 8), main->id));
                    main->ui.requested_frames = IDLE_WAKE_FRAMES;
                }
            }

            free(text);
            if (bbox)
                free(bbox);
            return 0;
        }
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
    if (shared->quick_search_window && shared->quick_search_window->id == id)
        return shared->quick_search_window;
    return NULL;
}

static WindowContext* find_quick_search_window(AppShared* shared)
{
    return shared->quick_search_window;
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

static void cmd_window_create(void* userdata, String cmd_text)
{
    u32 w = cmd_parse_u32(cmd_text, str("w"), CLIENT_WIDTH);
    u32 h = cmd_parse_u32(cmd_text, str("h"), CLIENT_HEIGHT);
    i32 pos_x = CW_USEDEFAULT;
    i32 pos_y = CW_USEDEFAULT;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    if (window_id)
    {
        WindowContext* ctx = find_window_by_id((AppShared*)userdata, window_id);
        if (ctx)
        {
            RECT r;
            if (GetWindowRect(ctx->window, &r))
            {
                pos_x = r.left + 50;
                pos_y = r.top + 75;
            }
        }
    }
    create_window((AppShared*)userdata, L"Window", pos_x, pos_y, w, h, True);
}

static void cmd_window_destroy(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    WindowContext* ctx = find_window_by_id(shared, window_id);
    if (!ctx)
        return;
    DestroyWindow(ctx->window);
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
           that had it focused to child_b (the new empty panel). */
        for (WindowContext* w = shared->first_window; w; w = w->next)
            if (w->focused_panel == p)
                w->focused_panel = p->child_b;
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
                w->focused_panel = p->child_b;
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
        PanelTab* tab = panel_tab_declare(rt.panel, (String){ name_buf, name_len });
        panel_tab_activate(rt.panel, tab);
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

static void cmd_tab_activate_left(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);
    if (!rt.panel || !rt.tab)
        return;
    PanelTab* prev = NULL;
    for (PanelTab* t = rt.panel->tab_first; t; t = t->next)
    {
        if (t == rt.tab)
            break;
        prev = t;
    }
    if (prev)
        panel_tab_activate(rt.panel, prev);
}

static void cmd_tab_activate_right(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    ResolvePanelTabResult rt = resolve_panel_and_tab(shared, cmd_text);
    if (!rt.panel || !rt.tab)
        return;
    if (rt.tab->next)
        panel_tab_activate(rt.panel, rt.tab->next);
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

static void render_dict_content(const void* data, void* ctx);
static void render_dict_pos_selector(void* userdata);

static void cmd_dict_pos_select(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    i32 delta = cmd_parse_i32(cmd_text, str("delta"), 0);
    i32 target_pos = cmd_parse_i32(cmd_text, str("pos"), -1);

    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w || !w->focused_panel)
        return;

    PanelTab* active = panel_tab_get_active(w->focused_panel);
    if (!active || active->render_fn != render_dict_content)
        return;

    /* content_data encodes both word index and POS index:
       upper bits = word_idx, lower 8 bits = pos_idx */
    i32 combined = (i32)(isize)active->content_data;
    i32 word_idx = combined >> 8;
    u8 cur_pos = (u8)(combined & 0xFF);

    const DictDB* db = &shared->dict_db;
    const DictWordIndex* word = &db->words[word_idx];
    const u8* p = db->entdata + word->entdata_off;
    p += 4; // freq
    {
        u8 cnt = dict_rd_u8(&p);
        p += (usize)cnt * 4;
    }
    {
        u8 cnt = dict_rd_u8(&p);
        p += (usize)cnt * 4;
    }
    u8 pos_count = dict_rd_u8(&p);

    if (pos_count == 0)
        return;

    i32 new_pos = cur_pos;
    if (delta != 0)
        new_pos = (i32)cur_pos + delta;
    else if (target_pos >= 0)
        new_pos = target_pos;

    if (new_pos < 0)
        new_pos = 0;
    if (new_pos >= (i32)pos_count)
        new_pos = (i32)pos_count - 1;

    if ((u8)new_pos == cur_pos)
        return;

    /* update content_data with new pos index */
    active->content_data = (void*)(isize)((word_idx << 8) | (u8)new_pos);

    if (w->ui.requested_frames <= 0)
        w->ui.requested_frames = 1;
}

static void cmd_tab_open_word(void* userdata, String cmd_text)
{
    AppShared* shared = (AppShared*)userdata;
    i32 content_data = cmd_parse_i32(cmd_text, str("content_data"), -1);
    u32 window_id = cmd_parse_u32(cmd_text, str("window"), 0);
    u32 panel_id = cmd_parse_u32(cmd_text, str("panel"), 0);
    b32 replace_active = (b32)cmd_parse_i32(cmd_text, str("replace_active"), 0);

    if (content_data < 0)
        return;

    WindowContext* w = find_window_by_id(shared, window_id);
    if (!w)
        return;

    Panel* p = panel_id ? panel_find_by_id(w->root_panel, panel_id) : w->focused_panel;
    if (!p)
        p = panel_find_first_leaf(w->root_panel);
    if (!p)
        return;

    i32 word_idx = content_data >> 8;
    const char* word_str = DICT_STR(&shared->dict_db, shared->dict_db.words[word_idx].word_stroff);
    isize wlen = strlen(word_str);

    if (replace_active)
    {
        PanelTab* active = panel_tab_get_active(p);
        if (active && wlen < PANEL_TAB_NAME_MAX)
        {
            memcpy(active->name, word_str, wlen);
            active->name_len = wlen;
            active->content_data = (void*)(isize)content_data;
            active->render_fn = render_dict_content;
        }
        else
        {
            /* Fallback: no active tab, create new one */
            PanelTab* tab = panel_tab_declare(p, (String){ (u8*)word_str, wlen });
            if (tab)
            {
                tab->content_data = (void*)(isize)content_data;
                tab->render_fn = render_dict_content;
                panel_tab_activate(p, tab);
            }
        }
    }
    else
    {
        PanelTab* tab = panel_tab_declare(p, (String){ (u8*)word_str, wlen });
        if (tab)
        {
            tab->content_data = (void*)(isize)content_data;
            tab->render_fn = render_dict_content;
            panel_tab_activate(p, tab);
        }
    }

    if (w->ui.requested_frames <= 0)
        w->ui.requested_frames = 1;
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
    [POS_NOUN] = "noun",
    [POS_VERB] = "verb",
    [POS_NOUN_VERB] = "noun, verb",
    [POS_ADJ] = "adj.",
    [POS_ADV] = "adv.",
    [POS_ADJ_ADV] = "adj., adv.",
    [POS_CONJ] = "conj.",
    [POS_DET] = "det.",
    [POS_INDEF_ART] = "indef. art.",
    [POS_INTERJ] = "interj.",
    [POS_MODAL] = "modal verb",
    [POS_NUM] = "num.",
    [POS_PREDET] = "predet.",
    [POS_PREP] = "prep.",
    [POS_ADV_PREP] = "adv., prep.",
    [POS_PRON] = "pron.",
    [POS_SUFFIX] = "suf.",
    [POS_PREFIX] = "pref.",
    [POS_AUX_VERB] = "aux. verb",
    [POS_PHRASAL_VERB] = "phr. verb",
    [POS_DEF_ART] = "def. art.",
};

static void render_dict_content(const void* data, void* ctx);

static void dict_skip_pos(const u8** p)
{
    dict_rd_u8(p); // pos_kind
    dict_rd_u32(p); // pron_off
    u8 def_count = dict_rd_u8(p);
    for (u8 di = 0; di < def_count; di++)
    {
        *p += 8; // en_off(4) + zh_off(4)
        u8 ex_count = dict_rd_u8(p);
        *p += (usize)ex_count * 8; // ex_en(4) + ex_zh(4) per example
    }
}

static void dict_build_tokens_for_block(WindowContext* wctx, UIBox* text_box, const DictDB* db)
{
    if (!text_box || text_box->type != BOX_TYPE_TEXT)
        return;
    if (wctx->dict_word_block_count >= DICT_TOKEN_BLOCK_MAX)
        return;

    TextData* td = &text_box->data.text;
    isize word_count = td->words.len;
    if (word_count == 0 || word_count > DICT_TOKEN_TOTAL_MAX)
        return;
    if (wctx->dict_word_total_tokens + word_count > DICT_TOKEN_TOTAL_MAX)
        return;

    Arena* arena = &wctx->dict_token_arena;

    /* allocate blocks array on first use */
    if (wctx->dict_word_blocks == NULL)
        wctx->dict_word_blocks =
            arena_push(arena, sizeof(DictWordBlock) * DICT_TOKEN_BLOCK_MAX, _Alignof(DictWordBlock), 1);

    DictWordBlock* block = &wctx->dict_word_blocks[wctx->dict_word_block_count];
    block->tokens = arena_push(arena, sizeof(DictWordToken) * word_count, _Alignof(DictWordToken), 1);
    block->token_count = (i32)word_count;
    block->text_box = text_box;

    String full_text = td->content;
    WordBreak* words = td->words.data;

    for (isize wi = 0; wi < word_count; wi++)
    {
        WordBreak* wb = &words[wi];
        String word_text = { full_text.data + wb->byte_start, wb->byte_end - wb->byte_start };

        DictWordToken* tok = &block->tokens[wi];
        tok->text = word_text;
        tok->width = wb->width;
        tok->byte_start = (i32)wb->byte_start;
        tok->byte_end = (i32)wb->byte_end;
        tok->line_index = -1;
        tok->x_on_line = -1.f;

        /* dict_resolve requires null-terminated string */
        char lookup_buf[128];
        isize copy_len = word_text.len < 127 ? word_text.len : 127;
        memcpy(lookup_buf, word_text.data, (usize)copy_len);
        lookup_buf[copy_len] = '\0';

        /* Strip leading/trailing punctuation so "apple." → "apple" */
        copy_len = ascii_word_strip(lookup_buf, copy_len);

        i32 idx = dict_resolve(db, lookup_buf);
        tok->dict_word_idx = (idx >= 0) ? (u32)idx : 0;
    }

    wctx->dict_word_block_count++;
    wctx->dict_word_total_tokens += (i32)word_count;
}

static void dict_compute_token_positions(DictWordBlock* block, f32 box_width, f32 space_width)
{
    if (block->token_count == 0 || box_width <= 0.f)
        return;

    f32 line_x = 0;
    i32 line = 0;
    i32 line_word_start = 0;

    for (i32 i = 0; i < block->token_count; i++)
    {
        DictWordToken* tok = &block->tokens[i];
        f32 w = tok->width;

        if (i > line_word_start)
        {
            i32 gap_bytes = tok->byte_start - block->tokens[i - 1].byte_end;
            if (gap_bytes > 0)
                w += (f32)gap_bytes * space_width;
        }

        if (line_x + w > box_width && i > line_word_start)
        {
            line++;
            line_x = 0;
            w = tok->width;
            line_word_start = i;
        }

        tok->x_on_line = line_x;
        tok->line_index = line;
        line_x += w;
    }
}

static i32 dict_token_hit_test(const DictWordBlock* block, f32 local_x, f32 local_y, f32 line_height)
{
    if (block->token_count == 0 || line_height <= 0.f)
        return -1;

    i32 line = (i32)(local_y / line_height);
    if (line < 0)
        return -1;

    for (i32 i = 0; i < block->token_count; i++)
    {
        const DictWordToken* tok = &block->tokens[i];
        if (tok->line_index == line && local_x >= tok->x_on_line && local_x < tok->x_on_line + tok->width)
        {
            return i;
        }
    }
    return -1;
}

static void render_dict_content(const void* data, void* ctx)
{
    /* content_data encodes both word index and POS index:
       upper bits = word_idx (into db->words[]), lower 8 bits = pos_idx */
    i32 combined = (i32)(isize)data;
    i32 word_idx = combined >> 8;
    u8 pos_idx = (u8)(combined & 0xFF);

    WindowContext* wctx = (WindowContext*)ctx;
    AppShared* shared = wctx->shared;
    const DictDB* db = &shared->dict_db;
    if (!db->hdr)
        return;
    const Theme* theme = &shared->theme;
    const DictWordIndex* w = &db->words[word_idx];
    const char* word = DICT_STR(db, w->word_stroff);

    const u8* p = db->entdata + w->entdata_off;

    /* freq */
    p += 4;

    /* skip brief_en */
    {
        u8 cnt = dict_rd_u8(&p);
        p += (usize)cnt * 4;
    }

    /* skip brief_zh */
    {
        u8 cnt = dict_rd_u8(&p);
        p += (usize)cnt * 4;
    }

    u8 pos_count = dict_rd_u8(&p);

    /* write POS list into WindowContext so panel_container can render the selector */
    wctx->dict_content_active = True;
    wctx->dict_pos_count = pos_count < DICT_MAX_POS ? pos_count : DICT_MAX_POS;
    wctx->dict_cur_pos = pos_idx;

    if (pos_count == 0)
        return;

    /* scan all POS entries to capture pos_kind[] for the selector */
    {
        const u8* scan = p;
        for (u8 pi = 0; pi < wctx->dict_pos_count; pi++)
        {
            wctx->dict_pos_kinds[pi] = dict_rd_u8(&scan);
            dict_rd_u32(&scan); // pron_off
            u8 dc = dict_rd_u8(&scan);
            for (u8 di = 0; di < dc; di++)
            {
                scan += 8; // en_off(4) + zh_off(4)
                u8 ec = dict_rd_u8(&scan);
                scan += (usize)ec * 8; // ex_en(4) + ex_zh(4) per example
            }
        }
    }

    /* skip preceding POS entries to reach the selected one */
    if (pos_idx >= pos_count)
        pos_idx = (u8)(pos_count - 1);
    for (u8 pi = 0; pi < pos_idx; pi++)
        dict_skip_pos(&p);

    /* render selected POS */
    u8 pos_kind = dict_rd_u8(&p);
    u32 pron_off = dict_rd_u32(&p);

    UIBox* container = ui_box_begin(&(BoxConfig){
        .sizing = { fit_grow({}), fit({}) },
        .padding = { 20, 28, 32, 28 },
        .child_gap = 12,
        .direction = LAYOUT_TOP_TO_BOTTOM,
        .alignment = { ALIGN_START, ALIGN_START },
    });
    {
        /* ── header row: word + phonetic ── */
        {
            UIBox* header = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                       .direction = LAYOUT_LEFT_TO_RIGHT,
                                                       .child_gap = 10,
                                                       .alignment = { ALIGN_START, ALIGN_CENTER } });
            {
                ui_text((String){ (u8*)word, (isize)strlen(word) },
                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                       .font_size = 18,
                                       .color = theme->dict_word_fg,
                                       .line_height = 24 });

                if (pron_off)
                {
                    const char* pron = DICT_STR(db, pron_off);
                    ui_text((String){ (u8*)pron, (isize)strlen(pron) },
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                           .font_size = 13,
                                           .color = theme->dict_phonetic_fg,
                                           .line_height = 24 });
                }
            }
            ui_box_end(header);
        }

        /* ── separator ── */
        // ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = theme->dict_separator }));

        /* ── POS label + phonetic ── */
        // if (pos_kind < countof(s_pos_names) && s_pos_names[pos_kind])
        // {
        //     UIBox* pos_row = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fit({}) },
        //                                                 .direction = LAYOUT_LEFT_TO_RIGHT,
        //                                                 .child_gap = 8,
        //                                                 .padding = { 4, 0, 4, 0 } });
        //     {
        //         const char* label = s_pos_names[pos_kind];
        //         ui_text((String){ (u8*)label, (isize)strlen(label) },
        //                 &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
        //                                .font_size = 14,
        //                                .color = theme->dict_phonetic_fg,
        //                                .line_height = 20 });
        //
        //         if (pron_off)
        //             ui_text((String){ (u8*)DICT_STR(db, pron_off), (isize)strlen(DICT_STR(db, pron_off)) },
        //                     &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
        //                                    .font_size = 13,
        //                                    .color = theme->dict_phonetic_fg,
        //                                    .line_height = 20 });
        //     }
        //     ui_box_end(pos_row);
        // }

        /* ── definitions ── */
        u8 def_count = dict_rd_u8(&p);
        for (u8 di = 0; di < def_count; di++)
        {
            u32 en_off = dict_rd_u32(&p);
            u32 zh_off = dict_rd_u32(&p);

            /* definition card */
            UIBox* card = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                     .color = theme->dict_card_bg,
                                                     .rect_style = { .corner_radius = 4 },
                                                     .padding = { 8, 18, 8, 18 },
                                                     .child_gap = 8,
                                                     .direction = LAYOUT_TOP_TO_BOTTOM });
            {
                UIBox* def_container = ui_box_begin(
                    &(BoxConfig){ .sizing = { fit({}), fit({}) }, .child_gap = 2, .direction = LAYOUT_TOP_TO_BOTTOM });
                {
                    UIBox* en_def_box =
                        ui_text((String){ (u8*)DICT_STR(db, en_off), (isize)strlen(DICT_STR(db, en_off)) },
                                &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI],
                                               .font_size = 12,
                                               .color = theme->dict_definition_fg,
                                               .line_height = 20,
                                               .wrap = True });

                    i32 block_before = wctx->dict_word_block_count;
                    dict_build_tokens_for_block(wctx, en_def_box, db);
                    if (wctx->dict_word_block_count > block_before)
                    {
                        UIBoxInteractResult ir = ui_box_interact(
                            en_def_box, str_fmt(HASH_STR_MAX_LENGTH, "dict_blk_%u_%d", wctx->id, block_before));
                        if (ui_hovered(ir.flags) && wctx->dict_word_block_count > 0)
                        {
                            DictWordBlock* block = &wctx->dict_word_blocks[block_before];
                            f32 box_width = ir.last_box ? ir.last_box->size.width : 0.f;
                            if (box_width > 0.f)
                            {
                                f32 spw = en_def_box->data.text.space_width;
                                f32 lnh = en_def_box->data.text.line_height;
                                dict_compute_token_positions(block, box_width, spw);

                                f32 lx = wctx->ui.mouse_pos.x - ir.last_box->position.x;
                                f32 ly = wctx->ui.mouse_pos.y - ir.last_box->position.y;
                                i32 hit = dict_token_hit_test(block, lx, ly, lnh);

                                if (hit >= 0 && block->tokens[hit].dict_word_idx > 0)
                                {
                                    DictWordToken* tok = &block->tokens[hit];
                                    ui_set_desired_cursor(UI_CURSOR_HAND);

                                    if (ui_lclicked(ir.flags))
                                        cmd_queue_push(
                                            &shared->cmd_queue,
                                            str_fmt(CMD_STR_MAX_LENGTH, "tab.open_word content_data=%d window=%u",
                                                    (i32)((tok->dict_word_idx << 8) | (u32)pos_idx), wctx->id));

                                    i32 line_count = 0;
                                    for (i32 i = 0; i < block->token_count; i++)
                                        if (block->tokens[i].line_index + 1 > line_count)
                                            line_count = block->tokens[i].line_index + 1;

                                    f32 offset_y = (tok->line_index + 1 - line_count) * lnh - 1.f;
                                    f32 offset_x = tok->x_on_line + spw;
                                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(tok->width), fixed(1.5f) },
                                                                          .color = theme->accent_bg,
                                                                          .flags = BoxFlag_Float,
                                                                          .float_offset = { offset_x, offset_y } }));
                                }
                            }
                        }
                    }

                    ui_text((String){ (u8*)DICT_STR(db, zh_off), (isize)strlen(DICT_STR(db, zh_off)) },
                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                           .font_size = 11,
                                           .color = theme->dict_definition_fg,
                                           .line_height = 18,
                                           .wrap = True });
                }
                ui_box_end(def_container);

                UIBox* example_container = ui_box_begin(
                    &(BoxConfig){ .sizing = { fit({}), fit({}) }, .child_gap = 6, .direction = LAYOUT_TOP_TO_BOTTOM });
                {
                    /* examples */
                    u8 ex_count = dict_rd_u8(&p);
                    for (u8 ei = 0; ei < ex_count; ei++)
                    {
                        u32 ex_en = dict_rd_u32(&p);
                        u32 ex_zh = dict_rd_u32(&p);

                        UIBox* ex_container = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fit({}) } });
                        {
                            // left spacer
                            ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(10), grow({}) } }));

                            UIBox* ex_box = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit({}) },
                                                                       .color = theme->dict_example_bg,
                                                                       .rect_style = { .corner_radius = 3 },
                                                                       .flags = BoxFlag_Clip,
                                                                       .padding = { 5, 11, 5, 11 },
                                                                       .child_gap = 3,
                                                                       .direction = LAYOUT_TOP_TO_BOTTOM });
                            {
                                UIBox* ex_en_box =
                                    ui_text((String){ (u8*)DICT_STR(db, ex_en), (isize)strlen(DICT_STR(db, ex_en)) },
                                            &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                                           .font_size = 11,
                                                           .color = theme->dict_example_fg,
                                                           .line_height = 17,
                                                           .wrap = True });
                                i32 block_before = wctx->dict_word_block_count;
                                dict_build_tokens_for_block(wctx, ex_en_box, db);
                                if (wctx->dict_word_block_count > block_before)
                                {
                                    UIBoxInteractResult ir_ex =
                                        ui_box_interact(ex_en_box, str_fmt(HASH_STR_MAX_LENGTH, "dict_blk_%u_%d",
                                                                           wctx->id, block_before));

                                    if (ui_hovered(ir_ex.flags) && wctx->dict_word_block_count > 0)
                                    {
                                        DictWordBlock* block = &wctx->dict_word_blocks[block_before];
                                        f32 box_width = ir_ex.last_box ? ir_ex.last_box->size.width : 0.f;
                                        if (box_width > 0.f)
                                        {
                                            f32 spw = ex_en_box->data.text.space_width;
                                            f32 lnh = ex_en_box->data.text.line_height;
                                            dict_compute_token_positions(block, box_width, spw);

                                            f32 lx = wctx->ui.mouse_pos.x - ir_ex.last_box->position.x;
                                            f32 ly = wctx->ui.mouse_pos.y - ir_ex.last_box->position.y;
                                            i32 hit = dict_token_hit_test(block, lx, ly, lnh);

                                            if (hit >= 0 && block->tokens[hit].dict_word_idx > 0)
                                            {
                                                DictWordToken* tok = &block->tokens[hit];
                                                ui_set_desired_cursor(UI_CURSOR_HAND);

                                                if (ui_lclicked(ir_ex.flags))
                                                    cmd_queue_push(
                                                        &shared->cmd_queue,
                                                        str_fmt(CMD_STR_MAX_LENGTH,
                                                                "tab.open_word content_data=%d window=%u",
                                                                (i32)((tok->dict_word_idx << 8) | (u32)pos_idx),
                                                                wctx->id));

                                                i32 line_count = 0;
                                                for (i32 i = 0; i < block->token_count; i++)
                                                    if (block->tokens[i].line_index + 1 > line_count)
                                                        line_count = block->tokens[i].line_index + 1;

                                                f32 offset_y = (tok->line_index + 1 - line_count) * lnh - 1.f;
                                                f32 offset_x = tok->x_on_line + spw;
                                                ui_box_end(ui_box_begin(
                                                    &(BoxConfig){ .sizing = { fixed(tok->width), fixed(1.5f) },
                                                                  .color = theme->accent_bg,
                                                                  .flags = BoxFlag_Float,
                                                                  .float_offset = { offset_x, offset_y } }));
                                            }
                                        }
                                    }
                                }

                                ui_text((String){ (u8*)DICT_STR(db, ex_zh), (isize)strlen(DICT_STR(db, ex_zh)) },
                                        &(TextConfig){ .font = &shared->fonts[FONT_INDEX_ZH],
                                                       .font_size = 10,
                                                       .color = theme->dict_example_zh_fg,
                                                       .line_height = 16,
                                                       .wrap = True });
                            }
                            ui_box_end(ex_box);
                        }
                        ui_box_end(ex_container);
                    }
                }
                ui_box_end(example_container);
            }
            ui_box_end(card);
        }
    }
    ui_box_end(container);
}

//
// Frame Processing
//

static void render_dict_pos_selector(void* userdata)
{
    WindowContext* ctx = (WindowContext*)userdata;
    AppShared* shared = ctx->shared;
    const Theme* theme = &shared->theme;

    if (ctx->dict_pos_count < 1 || !ctx->dict_content_active)
        return;

    // spacer
    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fixed(1) } }));

    for (u8 i = 0; i < ctx->dict_pos_count; i++)
    {
        u8 pk = ctx->dict_pos_kinds[i];
        const char* label = (pk < countof(s_pos_names) && s_pos_names[pk]) ? s_pos_names[pk] : "?";

        b32 is_active = (i == ctx->dict_cur_pos);
        Color bg = is_active ? theme->accent_bg : (Color){ 0 };
        Color fg = is_active ? theme->accent_fg : theme->bar_fg;

        UIBox* btn = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fixed(24) },
                                                .padding = { 2, 6, 2, 6 },
                                                .rect_style = { .corner_radius = 3 },
                                                .color = bg,
                                                .alignment = { ALIGN_CENTER, ALIGN_CENTER } });
        {
            UIBoxInteractResult ir =
                ui_box_interact(btn, str_fmt(HASH_STR_MAX_LENGTH, "dict_pos_btn_%u_%u", ctx->id, i));

            if (ui_lclicked(ir.flags) && (u8)i != ctx->dict_cur_pos)
                cmd_queue_push(&shared->cmd_queue,
                               str_fmt(CMD_STR_MAX_LENGTH, "dict.pos_select pos=%d window=%u panel=%u", (i32)i, ctx->id,
                                       ctx->focused_panel ? ctx->focused_panel->id : 0));

            ui_text((String){ (u8*)label, (isize)strlen(label) },
                    &(TextConfig){ .font = &shared->fonts[FONT_INDEX_UI], .font_size = 12.f, .color = fg });
        }
        ui_box_end(btn);
    }
}

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
                ui_text(str(ICON_FONT_UTF8_HELP),
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
        f32 gap = 4;
        f32 popup_x = ctx->decoration_menu.xmin;
        f32 popup_y = ctx->decoration_menu.ymax + gap;

        UIBox* popup = ui_box_begin(&(BoxConfig){
            .sizing = { fit({}), fit({}) },
            .flags = BoxFlag_Float,
            .float_offset = { popup_x, -(client_h - 1) + popup_y },
            .color = theme->palette_bg,
            .padding = { 16, 16, 16, 16 },
            .child_gap = 6,
            .direction = LAYOUT_TOP_TO_BOTTOM,
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
            // clang-format off
            BoxConfig title_cnt_cfg = (BoxConfig){ .sizing = { fit_grow({}), fit({}) }, .padding = { 8, 0, 3, 0 }, .alignment = { ALIGN_CENTER, ALIGN_CENTER } };
            BoxConfig cnt_cfg = (BoxConfig){ .sizing = { fit({}), fit({}) }, .alignment = { ALIGN_CENTER, ALIGN_CENTER } };
            BoxConfig box_cfg = { .sizing = { fit({}), fit({}) }, .color = theme->hint_bg, .rect_style = { .corner_radius = 2 }, .padding = { 3, 3, 3, 3 } };
            TextConfig hint_title_cfg = { .font = &shared->fonts[FONT_INDEX_ZH], .font_size = 11, .color = theme->accent_bg };
            TextConfig hint_text_cfg = { .font = &shared->fonts[FONT_INDEX_ZH], .font_size = 10, .color = theme->bar_fg };
            UIBox* title_cnt = NULL;
            UIBox* cnt = NULL;
            UIBox* box = NULL;

            /// window ---------------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("窗口"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Shift+N"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 创建窗口"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Shift+W"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 销毁窗口"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Esc"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 或 "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("鼠标右键"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 销毁窗口（最后一个窗口将隐藏到系统托盘）"), &hint_text_cfg);
                ui_box_end(cnt);
            }

            /// panel ----------------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("面板"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Alt+Shift+-/+"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 分割面板"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Alt+H/J/K/L"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 切换面板焦点"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Alt+Shift+H/J/K/L"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 调整大小"), &hint_text_cfg);
                ui_box_end(cnt);
            }

            /// tab ------------------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("标签页"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                ui_text(str("标签页可拖拽到任意位置"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+T"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 创建标签页"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+W"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 关闭标签页（最后一个标签页将销毁窗口）"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("4. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Shift+H/L"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 聚焦左/右标签页"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("5. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Shift+Alt+H/L"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 向左/右移动标签页"), &hint_text_cfg);
                ui_box_end(cnt);
            }

            /// search palette -------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("搜索面板"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Alt+M"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 打开搜索面板"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Esc"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 或 "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+["), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 关闭搜索面板"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Tab"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 在搜索面板中切换搜索模式"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("4. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Up/Down"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 或 "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+P/N"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 在搜索面板中导航条目"), &hint_text_cfg);
                ui_box_end(cnt);
            }

            /// dictionary -----------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("词典"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                ui_text(str("释义和例句中的单词可点击"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Alt+H/L"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 切换词性"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("J/K"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 滚动"), &hint_text_cfg);
                ui_box_end(cnt);
            }

            /// Miscellany -----------------------

            title_cnt = ui_box_begin(&title_cnt_cfg);
            ui_text(str("其他"), &hint_title_cfg);
            ui_box_end(title_cnt);
            {
                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("1. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("F11"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 切换浅色/深色主题"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("2. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Alt+Shift+M"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 切换前台（多窗口时关闭，最后一个隐藏到托盘）"), &hint_text_cfg);
                ui_box_end(cnt);

                cnt = ui_box_begin(&cnt_cfg);
                ui_text(str("3. "), &hint_text_cfg);
                box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Alt+鼠标左键"), &hint_text_cfg); ui_box_end(box);
                ui_text(str(" 悬停时查词（未找到结果时无提示）"), &hint_text_cfg);
                ui_box_end(cnt);
            }
            cnt = ui_box_begin(&cnt_cfg);
            ui_text(str(" "), &hint_text_cfg);
            ui_box_end(cnt);

            ui_text(str("版本：2026.6.1"), &hint_text_cfg);
            // clang-format on
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
    {
        view_state = PALETTE_VIEW_LOADING;
        ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
    }
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
        .float_offset = { popup_x, ctx->is_quick_search ? popup_y : (-(client_h - 1) + popup_y) },
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
                        &shared->fonts[FONT_INDEX_ZH], font_size, (SizingAxis)fit_grow({}), (Padding){ 10, 10, 10, 10 },
                        theme->palette_bg, (Color){ 0 }, theme->panel_fg, theme->scrollbar_thumb, theme->cursor_bar,
                        theme->cursor_trail, theme->selection, theme->selection_flash, True);
                }
                ui_box_end(tf_container);

                /* mode indicator */
                {
                    UIBox* indicator = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fit_grow({}) },
                                                                  .padding = { 10, 10, 10, 10 },
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
                                                      .color = theme->hover_fg });
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
                                    .sizing = { grow({}), fit({}) },
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
                                    {
                                        WindowContext* tctx = NULL;
                                        if (i == ctx->palette_selected_index)
                                        {
                                            if (!ctx->is_quick_search)
                                                tctx = ctx;
                                            else if (ctx->quick_search_live_preview)
                                                tctx = shared->last_active_main_window;
                                        }
                                        if (tctx)
                                        {
                                            const DictWordIndex* w = item->entry;
                                            PanelTab* active = panel_tab_get_active(tctx->focused_panel);
                                            if (active)
                                            {
                                                const char* wrd = DICT_STR(&shared->dict_db, w->word_stroff);
                                                isize wlen = strlen(wrd);
                                                memcpy(active->name, wrd, wlen);
                                                active->name_len = wlen;
                                                /* content_data encodes word_idx (upper bits) | pos_idx (lower 8 bits).
                                                   Start at pos 0 when selecting a new word from palette. */
                                                active->content_data =
                                                    (void*)(isize)(((w - shared->dict_db.words) << 8) | 0);
                                                active->render_fn = render_dict_content;
                                                if (tctx != ctx)
                                                    tctx->ui.requested_frames = 1;
                                            }
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
                            if (ctx->is_quick_search && ctx->palette_selected_index >= 0 &&
                                ctx->palette_selected_index < item_count)
                            {
                                const DictWordIndex* w = items[ctx->palette_selected_index].entry;
                                ctx->quick_search_confirmed_word_idx = (i32)(w - shared->dict_db.words);
                                ctx->quick_search_result_confirmed = True;
                            }
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
                                                                    .color = theme->hover_fg });
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
                                                                  .color = theme->hover_fg });
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

    ctx->dict_pos_count = 0;
    ctx->dict_word_block_count = 0;
    ctx->dict_word_total_tokens = 0;
    ctx->dict_word_blocks = NULL;
    arena_pop_to(&ctx->dict_token_arena, 0);

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
        .tab_active_fg     = theme->bar_fg,
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

        ctx->dict_pos_count = 0;

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
            .alignment = { ALIGN_CENTER, ALIGN_START },
            .show_bottom_bar = (p == ctx->focused_panel),
            .bottom_bar_render_fn = (p == ctx->focused_panel) ? render_dict_pos_selector : NULL,
            .bottom_bar_userdata = ctx,
        });
        {
            if (ui_lclicked(panel.interact.flags))
                ctx->focused_panel = panel.panel;

            PanelTab* active = panel_tab_get_active(p);
            if (active)
            {
                if (active->render_fn)
                {
                    active->render_fn(active->content_data, ctx);
                }
                else
                {
                    // clang-format off
                    TextConfig hint_text_cfg = { .font = &shared->fonts[FONT_INDEX_UI], .font_size = 11, .color = theme->hint_fg };
                    UIBox* box = NULL;
                    BoxConfig box_cfg = { .sizing = { fit({}), fit({}) }, .color = theme->hint_bg, .rect_style = { .corner_radius = 2 }, .padding = { 3, 3, 3, 3 } };
                    BoxConfig hint_cnt_cfg = { .sizing = { fit({}), fit({}) }, .alignment = { ALIGN_CENTER, ALIGN_CENTER } };
                    UIBox* hint_cnt = NULL;

                    UIBox* cnt = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), grow({}) }, .child_gap = 6, .direction = LAYOUT_TOP_TO_BOTTOM, .alignment = { ALIGN_CENTER, ALIGN_CENTER } });
                    {
                        hint_cnt = ui_box_begin(&hint_cnt_cfg);
                        {
                            box = ui_box_begin(&box_cfg); ui_text(str("Ctrl+Alt+M"), &hint_text_cfg); ui_box_end(box);
                            ui_text(str(" to search"), &hint_text_cfg);
                        }
                        ui_box_end(hint_cnt);

                        hint_cnt = ui_box_begin(&hint_cnt_cfg);
                        {
                            box = ui_box_begin(&box_cfg); ui_text(str("F1"), &hint_text_cfg); ui_box_end(box);
                            ui_text(str(" to toggle help"), &hint_text_cfg);
                        }
                        ui_box_end(hint_cnt);
                    }
                    ui_box_end(cnt);
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

    /* Derive dict_content_active from the focused panel's active tab */
    {
        PanelTab* active = panel_tab_get_active(ctx->focused_panel);
        ctx->dict_content_active = (active && active->render_fn == render_dict_content);
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

    /* Quick-search overlay: render only the search palette, no panels */
    if (ctx->is_quick_search)
    {
        AppShared* shared = ctx->shared;
        if (ctx->palette_popup.open)
            ImmAssociateContext(ctx->window, ctx->default_himc);
        else
            ImmAssociateContext(ctx->window, NULL);

        f32 client_w = (f32)ctx->ui.client_width;
        f32 client_h = (f32)ctx->ui.client_height;

        isize arena_pos = ui_frame_begin(&ctx->ui);
        UIBox* root = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(client_w), fixed(client_h) },
        });
        search_palette_render(ctx);
        ui_box_end(root);
        ui_frame_end(arena_pos);
        if (ctx->dcomp_device)
            IDCompositionDevice_Commit(ctx->dcomp_device);

        if (!ctx->palette_popup.open)
        {
            if (ctx->quick_search_result_confirmed)
            {
                WindowContext* main = shared->last_active_main_window;
                if (!main)
                    main = shared->first_window;
                if (!main && shared->tray_window)
                    main = shared->tray_window;
                if (main)
                {
                    if (shared->tray_window == main)
                        shared->tray_window = NULL;
                    ShowWindow(main->window, SW_SHOW);
                    SetForegroundWindow(main->window);
                    if (IsIconic(main->window))
                        ShowWindow(main->window, SW_RESTORE);
                    /* Replace focused tab content with confirmed word */
                    Panel* panel = main->focused_panel;
                    if (!panel)
                        panel = panel_find_first_leaf(main->root_panel);
                    if (panel)
                    {
                        PanelTab* active = panel_tab_get_active(panel);
                        if (active)
                        {
                            const DictWordIndex* w = &shared->dict_db.words[ctx->quick_search_confirmed_word_idx];
                            const char* wrd = DICT_STR(&shared->dict_db, w->word_stroff);
                            isize wlen = strlen(wrd);
                            if (wlen < PANEL_TAB_NAME_MAX)
                            {
                                memcpy(active->name, wrd, wlen);
                                active->name_len = wlen;
                            }
                            active->content_data = (void*)(isize)(((w - shared->dict_db.words) << 8) | 0);
                            active->render_fn = render_dict_content;
                        }
                    }
                    main->ui.requested_frames = IDLE_WAKE_FRAMES;
                }
            }
            /* Hide and reset for next use */
            ctx->palette_popup.open = False;
            ctx->quick_search_result_confirmed = False;
            ctx->quick_search_closing = False;
            ctx->quick_search_live_preview = False;
            ctx->palette_selected_index = -1;
            ctx->palette_prev_selected_index = -1;
            ctx->palette_prev_query_len = 0;
            ctx->palette_activate_pending = False;
            ctx->palette_search_mode = PALETTE_MODE_WORD;
            ctx->palette_switch_version = 0;
            ctx->ui.requested_frames = 0;
            ctx->in_frame = False;
            ShowWindow(ctx->window, SW_HIDE);
            return;
        }
        ctx->in_frame = False;
        return;
    }

    /* IME control: disable when palette closed, re-enable when palette open */
    if (ctx->palette_popup.open)
        ImmAssociateContext(ctx->window, ctx->default_himc);
    else
        ImmAssociateContext(ctx->window, NULL);

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

            if (ctx && ctx->is_quick_search)
            {
                NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lparam;
                u32 dpi = GetDpiForWindow(window);
                ui_ctx->dpi = dpi;
                f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
                ui_ctx->client_width = (u32)ceil((params->rgrc[0].right - params->rgrc[0].left) / dpi_scale);
                ui_ctx->client_height = (u32)ceil((params->rgrc[0].bottom - params->rgrc[0].top) / dpi_scale);
                return 0;
            }

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
            if (ctx && ctx->is_quick_search && LOWORD(wparam) == WA_INACTIVE && !ctx->quick_search_closing)
            {
                ctx->palette_popup.open = False;
                ctx->quick_search_result_confirmed = False;
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                return 0;
            }
            if (ctx && !ctx->is_quick_search && LOWORD(wparam) != WA_INACTIVE)
                shared->last_active_main_window = ctx;
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
                        quick_search_activate(ctx->shared);
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

            // EXPERIMENT: Hide the window when right click
            if (shared)
            {
                b32 is_last = (shared->first_window == shared->last_window && ctx == shared->first_window);
                if (is_last)
                {
                    shared->tray_window = ctx;
                    ShowWindow(window, SW_HIDE);
                }
                else
                {
                    DestroyWindow(window);
                }
            }
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

            if (wparam == VK_F4 && alt && message == WM_SYSKEYDOWN)
            {
                if (shared && ctx)
                {
                    if (ctx->is_quick_search)
                    {
                        ctx->palette_popup.open = False;
                        ctx->quick_search_result_confirmed = False;
                        ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    }
                    else
                        DestroyWindow(window);
                }
                return 0;
            }

            if (wparam == VK_ESCAPE)
            {
                /* Quick-search: close overlay and let process_frame hide the window */
                if (ctx && ctx->is_quick_search)
                {
                    ctx->palette_popup.open = False;
                    ctx->quick_search_result_confirmed = False;
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
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
                if (shared)
                {
                    b32 is_last = (shared->first_window == shared->last_window && ctx == shared->first_window);
                    if (is_last)
                    {
                        shared->tray_window = ctx;
                        ShowWindow(window, SW_HIDE);
                    }
                    else
                    {
                        DestroyWindow(window);
                    }
                }
                return 0;
            }

            /* Dict content keyboard scrolling (Up/Down/J/K).
               Only active when dict content is shown and no overlay is open. */
            if (ctx && ctx->dict_content_active && !ctx->palette_popup.open && !ctx->menu_popup.open && !ctrl && !alt)
            {
                b32 handled = True;
                f32 step = 8.f;
                switch (wparam)
                {
                    case VK_UP:
                    case 'K':
                        ctx->ui.keyboard_scroll_delta.y -= step;
                        break;
                    case VK_DOWN:
                    case 'J':
                        ctx->ui.keyboard_scroll_delta.y += step;
                        break;
                    default:
                        handled = False;
                        break;
                }
                if (handled)
                {
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
            }

            /* Palette mode switching — intercept before shortcut lookup
               so Ctrl+W switches to word mode instead of closing a tab.
               Tab/Shift+Tab cycles forward/backward through modes. */
            if (ctx && ctx->palette_popup.open && ctx->ui.focused_hash == s_search_palette_input_hash)
            {
                SearchPaletteMode new_mode = ctx->palette_search_mode;
                b32 handled = True;

                // clang-format off
                if (ctrl)
                {
                    switch (wparam)
                    {
                        case 'D': new_mode = PALETTE_MODE_DEF;     break;
                        case 'E': new_mode = PALETTE_MODE_EXAMPLE; break;
                        case 'W': new_mode = PALETTE_MODE_WORD;    break;
                        default:  handled = False;                 break;
                    }
                }
                else if (wparam == VK_TAB)
                {
                    if (shift)
                    {
                        switch (ctx->palette_search_mode)
                        {
                            case PALETTE_MODE_WORD:    new_mode = PALETTE_MODE_EXAMPLE; break;
                            case PALETTE_MODE_DEF:     new_mode = PALETTE_MODE_WORD;    break;
                            case PALETTE_MODE_EXAMPLE: new_mode = PALETTE_MODE_DEF;     break;
                            default:                   new_mode = PALETTE_MODE_WORD;    break;
                        }
                    }
                    else
                    {
                        switch (ctx->palette_search_mode)
                        {
                            case PALETTE_MODE_WORD:    new_mode = PALETTE_MODE_DEF;     break;
                            case PALETTE_MODE_DEF:     new_mode = PALETTE_MODE_EXAMPLE; break;
                            case PALETTE_MODE_EXAMPLE: new_mode = PALETTE_MODE_WORD;    break;
                            default:                   new_mode = PALETTE_MODE_WORD;    break;
                        }
                    }
                }
                else
                {
                    handled = False;
                }
                // clang-format on

                if (handled)
                {
                    if (new_mode != ctx->palette_search_mode)
                    {
                        const FieldDef* fields;
                        i32 field_count;
                        // clang-format off
                        switch (new_mode)
                        {
                            default:
                            case PALETTE_MODE_WORD:    fields = s_dict_fields;     field_count = countof(s_dict_fields);     break;
                            case PALETTE_MODE_DEF:     fields = s_dict_fields_def; field_count = countof(s_dict_fields_def); break;
                            case PALETTE_MODE_EXAMPLE: fields = s_dict_fields_ex;  field_count = countof(s_dict_fields_ex);  break;
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

            /* Dict content active: J/K are consumed as scroll keys in WM_KEYDOWN.
               Prevent them from falling through to char input or palette toggle. */
            if (ctx && ctx->dict_content_active && !ctx->palette_popup.open && !ctx->menu_popup.open)
                if (c == 'j' || c == 'J' || c == 'k' || c == 'K')
                    break;

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

        case WM_CLOSE:
        {
            if (shared && ctx)
            {
                b32 is_last = (shared->first_window == shared->last_window && ctx == shared->first_window);
                if (is_last)
                {
                    shared->tray_window = ctx;
                    ShowWindow(window, SW_HIDE);
                }
                else
                {
                    DestroyWindow(window);
                }
            }
            else
            {
                DestroyWindow(window);
            }
            return 0;
        }

        case WM_DESTROY:
        {
            if (shared && ctx)
            {
                // First disconnect the association, so subsequent trailing messages (WM_NCDESTROY, etc.) get NULL
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);

                if (!ctx->is_quick_search)
                {
                    window_list_remove(shared, ctx);
                    panel_free_tree(ctx->root_panel);
                }
                else
                {
                    shared->quick_search_window = NULL;
                }
                ui_deinit(&ctx->ui);
                if (ctx->is_quick_search)
                {
                    if (ctx->dcomp_visual)
                    {
                        IDCompositionVisual_Release(ctx->dcomp_visual);
                        ctx->dcomp_visual = NULL;
                    }
                    if (ctx->dcomp_target)
                    {
                        IDCompositionTarget_Release(ctx->dcomp_target);
                        ctx->dcomp_target = NULL;
                    }
                    if (ctx->dcomp_device)
                    {
                        IDCompositionDevice_Release(ctx->dcomp_device);
                        ctx->dcomp_device = NULL;
                    }
                }
                renderer_deinit(&ctx->renderer);
                arena_release(&ctx->dict_token_arena);
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
    if (shared->quick_search_window && shared->quick_search_window->ui.requested_frames > 0)
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

        shared->dict_arena = arena_new((isize)dsize, MB(8));
        arena_push(&shared->dict_arena, (isize)dsize, 1, 1);

        usize result = ZSTD_decompress(shared->dict_arena.base, (size_t)dsize, compressed, compressed_size);
        Assert(!ZSTD_isError(result));

        shared->dict_db = dict_open(shared->dict_arena.base);
        Assert(shared->dict_db.hdr);
        g_dict_db = &shared->dict_db;
    }

    shared->search_aux_arena = arena_new(MB(16), MB(8));
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
    shared.cfg_arena = arena_new(KB(8), KB(64));
    shared.cmd_arena = arena_new(CMD_ARENA_SIZE, KB(64));
    cmd_registry_init(&shared.cmd_registry, &shared.cfg_arena, 32);
    shortcut_registry_init(&shared.shortcuts, &shared.cfg_arena, 64);
    cmd_queue_init(&shared.cmd_queue, &shared.cmd_arena);

    // clang-format off
    {
        /* Register commands */
        cmd_register(&shared.cmd_registry, (CmdDef){ str("window.create"),          str("New Window"),               str(""), cmd_window_create,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("window.destroy"),         str("Close Window"),             str(""), cmd_window_destroy,         &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("app.toggle_theme"),       str("Toggle Light/Dark Theme"),  str(""), cmd_toggle_theme,           &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("menu.toggle"),            str("Toggle Menu Popup"),        str(""), cmd_toggle_menu,            &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("palette.close"),          str("Close Search Palette"),     str(""), cmd_close_palette,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_h"),          str("Split Panel Horizontally"), str(""), cmd_split_panel_h,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.split_v"),          str("Split Panel Vertically"),   str(""), cmd_split_panel_v,          &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("panel.close"),            str("Close Panel"),              str(""), cmd_close_panel,            &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.new"),                str("New Tab"),                  str(""), cmd_tab_new,                &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.close"),              str("Close Tab"),                str(""), cmd_tab_close,              &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.activate"),           str("Activate Tab"),             str(""), cmd_tab_activate,           &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.activate_left"),      str("Activate Left Tab"),        str(""), cmd_tab_activate_left,      &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.activate_right"),     str("Activate Right Tab"),       str(""), cmd_tab_activate_right,     &shared });
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
        cmd_register(&shared.cmd_registry, (CmdDef){ str("dict.pos_select"),       str("Select Part of Speech"),     str(""), cmd_dict_pos_select,        &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("tab.open_word"),         str("Open Word in Tab"),          str(""), cmd_tab_open_word,          &shared });

        /* Bind shortcuts */
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'N' },          str("window.create"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'W' },          str("window.destroy"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                                         'T' },          str("tab.new"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                                         'W' },          str("tab.close"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     VK_OEM_MINUS }, str("panel.split_h"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     VK_OEM_PLUS },  str("panel.split_v"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'H' },          str("tab.activate_left"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'L' },          str("tab.activate_right"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT | SHORTCUT_MOD_ALT, 'H' },          str("tab.move delta=-1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT | SHORTCUT_MOD_ALT, 'L' },          str("tab.move delta=+1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'F' },          str("tab.to_new_panel axis=X"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    'G' },          str("tab.to_new_panel axis=Y"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                                         VK_TAB },       str("panel.focus_next"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_SHIFT,                    VK_TAB },       str("panel.focus_prev"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,                      'H' },          str("panel.focus_left"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,                      'J' },          str("panel.focus_down"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,                      'K' },          str("panel.focus_up"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL | SHORTCUT_MOD_ALT,                      'L' },          str("panel.focus_right"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     'H' },          str("panel.resize_left"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     'J' },          str("panel.resize_down"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     'K' },          str("panel.resize_up"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT | SHORTCUT_MOD_SHIFT,                     'L' },          str("panel.resize_right"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT,                                          'H' },          str("dict.pos_select delta=-1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_ALT,                                          'L' },          str("dict.pos_select delta=+1"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_NONE,                                         VK_F11 },       str("app.toggle_theme"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_NONE,                                         VK_F1 },        str("menu.toggle"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                                         VK_OEM_4 },     str("palette.close")); // ctrl+[
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
    WNDCLASSEXW wc = {
        .cbSize = sizeof(wc),
        .lpfnWndProc = window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .hIcon = LoadIconW(GetModuleHandleW(NULL), L"MAIN_ICON"),
        .hIconSm = (HICON)LoadImageW(GetModuleHandleW(NULL), L"MAIN_ICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR),
        .lpszClassName = L"window class",
    };
    RegisterClassExW(&wc);

    /* Register drag popup window class */
    WNDCLASSW dp_wc = {
        .lpfnWndProc = drag_popup_window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"UIDragPopup",
    };
    RegisterClassW(&dp_wc);

    /* Register tray icon window class (message-only, lives for process lifetime) */
    WNDCLASSW tray_wc = {
        .lpfnWndProc = tray_window_procedure,
        .hInstance = GetModuleHandleW(NULL),
        .lpszClassName = L"TrayIconClass",
    };
    RegisterClassW(&tray_wc);

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
    /* Create message-only window for tray icon callbacks (process-lifetime, independent of any real window) */
    shared.tray_hwnd =
        CreateWindowExW(0, L"TrayIconClass", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), &shared);
    b32 hotkey_ok = True;
    wchar_t hotkey_fail_buf[512] = L"";

    if (!RegisterHotKey(shared.tray_hwnd, HOTKEY_TOGGLE_FOREGROUND, MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'M'))
    {
        hotkey_ok = False;
        wcscat_s(hotkey_fail_buf, 512, L"Ctrl+Shift+Alt+M (toggle main window)\n");
    }
    if (!RegisterHotKey(shared.tray_hwnd, HOTKEY_QUICK_SEARCH, MOD_CONTROL | MOD_ALT, 'M'))
    {
        hotkey_ok = False;
        wcscat_s(hotkey_fail_buf, 512, L"Ctrl+Alt+M (quick search palette)\n");
    }

    /* Init WinRT OCR and install low-level mouse hook for Ctrl+Alt+Click word lookup */
    ocr_init();
    g_ocr_tray_hwnd = shared.tray_hwnd;
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc, NULL, 0);
    if (!g_mouse_hook)
    {
        hotkey_ok = False;
        wcscat_s(hotkey_fail_buf, 512, L"Ctrl+Alt+MouseLeft (word lookup)\n");
    }

    if (!hotkey_ok)
    {
        wchar_t msg[640];
        swprintf_s(msg, 640,
                   L"The following global hotkeys failed to register:\n\n%ls\nPlease check for conflicts with other "
                   L"applications.",
                   hotkey_fail_buf);
        MessageBoxW(NULL, msg, L"Hotkey Registration Warning", MB_ICONWARNING | MB_OK);
    }

    /* Add system tray icon (persists until process exit) */
    {
        NOTIFYICONDATAW nid = { 0 };
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = shared.tray_hwnd;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY_CALLBACK;
        nid.hIcon = LoadIconW(GetModuleHandleW(NULL), L"MAIN_ICON");
        wcscpy_s(nid.szTip, 128, L"wdict");
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

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

        /* Quick-search window (tracked separately, not in main window list) */
        {
            WindowContext* qs = shared.quick_search_window;
            if (qs && qs->ui.requested_frames > 0)
            {
                process_frame(qs);
                if (qs->ui.requested_frames > 0)
                    qs->ui.requested_frames--;
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

    /* Clean up quick-search window if still alive */
    if (shared.quick_search_window)
        DestroyWindow(shared.quick_search_window->window);

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

    /* Remove system tray icon */
    {
        NOTIFYICONDATAW nid = { 0 };
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = shared.tray_hwnd;
        nid.uID = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    /* Unregister hotkeys and destroy message-only tray window */
    UnregisterHotKey(shared.tray_hwnd, HOTKEY_TOGGLE_FOREGROUND);
    UnregisterHotKey(shared.tray_hwnd, HOTKEY_QUICK_SEARCH);
    if (shared.tray_hwnd)
        DestroyWindow(shared.tray_hwnd);

    /* Uninstall mouse hook and deinit OCR */
    if (g_mouse_hook)
        UnhookWindowsHookEx(g_mouse_hook);
    ocr_deinit();

    ExitProcess(0);
}
