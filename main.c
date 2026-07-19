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

#ifdef TRACY_ENABLE
#    define IDLE_WAKE_FRAMES 0x7FFFFFFF
#else
#    define IDLE_WAKE_FRAMES 4
#endif

#define CMD_ARENA_SIZE     KB(64)
#define SEARCH_DISPLAY_MAX 100

#define SEARCH_PALETTE_INPUT_HASH_STR "search palette input"

#define WM_TRAY_CALLBACK      (WM_APP + 1)
#define TRAY_ICON_ID          1
#define HOTKEY_QUICK_SEARCH   2
#define QUICK_SEARCH_WINDOW_W 540
#define QUICK_SEARCH_WINDOW_H 420
#define OCR_POPUP_WINDOW_W    320
#define OCR_POPUP_WINDOW_H    200

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
    PALETTE_MODE_WORD,
    PALETTE_MODE_DEF,
} SearchPaletteMode;

typedef enum
{
    PALETTE_VIEW_EMPTY,
    PALETTE_VIEW_LOADING,
    PALETTE_VIEW_RESULTS,
    PALETTE_VIEW_NO_MATCH,
} PaletteViewState;

typedef enum
{
    POS_DOMAIN_NAME,
    POS_DOMAIN_SLANG,
    POS_DOMAIN_CHEM,
    POS_DOMAIN_MED,
    POS_DOMAIN_ARCHAIC,
    POS_DOMAIN_BIBLE,
    POS_DOMAIN_GEO,
    POS_DOMAIN_ARCH,
    POS_DOMAIN_MECH,
    POS_DOMAIN_LAW,
    POS_DOMAIN_ELEC,
    POS_DOMAIN_ECON,
    POS_DOMAIN_NET,
    POS_DOMAIN_SLANG_US,
    POS_DOMAIN_COMP,
    POS_ADJ,
    POS_ABBR,
    POS_ADV,
    POS_ART,
    POS_AUX,
    POS_CONJ,
    POS_INTERJ,
    POS_NOUN,
    POS_NUM,
    POS_PL,
    POS_PREF,
    POS_PREP,
    POS_PRON,
    POS_SUFFIX,
    POS_VERB,
    POS_VBL,
    POS_VERB_INTRANSITIVE,
    POS_VERB_TRANSITIVE,
    POS_UNKNOWN = 0xFF,
} PosKind;

typedef struct
{
    PosKind kind;
    String def_text;
} DefLine;

static const char* s_pos_names[] = {
    [POS_DOMAIN_NAME] = "[人名]",
    [POS_DOMAIN_SLANG] = "[俚]",
    [POS_DOMAIN_CHEM] = "[化]",
    [POS_DOMAIN_MED] = "[医]",
    [POS_DOMAIN_ARCHAIC] = "[古]",
    [POS_DOMAIN_BIBLE] = "[圣经]",
    [POS_DOMAIN_GEO] = "[地名]",
    [POS_DOMAIN_ARCH] = "[建]",
    [POS_DOMAIN_MECH] = "[机]",
    [POS_DOMAIN_LAW] = "[法]",
    [POS_DOMAIN_ELEC] = "[电]",
    [POS_DOMAIN_ECON] = "[经]",
    [POS_DOMAIN_NET] = "[网络]",
    [POS_DOMAIN_SLANG_US] = "[美俚]",
    [POS_DOMAIN_COMP] = "[计]",
    [POS_ADJ] = "adj.",
    [POS_ABBR] = "abbr.",
    [POS_ADV] = "adv.",
    [POS_ART] = "art.",
    [POS_AUX] = "aux.",
    [POS_CONJ] = "conj.",
    [POS_INTERJ] = "interj.",
    [POS_NOUN] = "noun",
    [POS_NUM] = "num.",
    [POS_PL] = "pl.",
    [POS_PREF] = "pref.",
    [POS_PREP] = "prep.",
    [POS_PRON] = "pron.",
    [POS_SUFFIX] = "suf.",
    [POS_VERB] = "verb",
    [POS_VBL] = "vbl.",
    [POS_VERB_INTRANSITIVE] = "vi.",
    [POS_VERB_TRANSITIVE] = "vt.",
};

static const struct
{
    const char* tag;
    PosKind kind;
} s_pos_tag_table[] = {
    { "[人名]", POS_DOMAIN_NAME },
    { "[俚]", POS_DOMAIN_SLANG },
    { "[化]", POS_DOMAIN_CHEM },
    { "[医]", POS_DOMAIN_MED },
    { "[古]", POS_DOMAIN_ARCHAIC },
    { "[圣经]", POS_DOMAIN_BIBLE },
    { "[地名]", POS_DOMAIN_GEO },
    { "[建]", POS_DOMAIN_ARCH },
    { "[机]", POS_DOMAIN_MECH },
    { "[法]", POS_DOMAIN_LAW },
    { "[电]", POS_DOMAIN_ELEC },
    { "[经]", POS_DOMAIN_ECON },
    { "[网络]", POS_DOMAIN_NET },
    { "[美俚]", POS_DOMAIN_SLANG_US },
    { "[计]", POS_DOMAIN_COMP },
    { "a", POS_ADJ },
    { "abbr", POS_ABBR },
    { "adv", POS_ADV },
    { "art", POS_ART },
    { "aux", POS_AUX },
    { "conj", POS_CONJ },
    { "int", POS_INTERJ },
    { "interj", POS_INTERJ },
    { "n", POS_NOUN },
    { "num", POS_NUM },
    { "pl", POS_PL },
    { "pref", POS_PREF },
    { "prep", POS_PREP },
    { "pron", POS_PRON },
    { "su", POS_SUFFIX },
    { "v", POS_VERB },
    { "vbl", POS_VBL },
    { "vi", POS_VERB_INTRANSITIVE },
    { "vt", POS_VERB_TRANSITIVE },
};

static PosKind pos_kind_from_tag(const u8* tag, isize tag_len)
{
    for (u32 i = 0; i < countof(s_pos_tag_table); i++)
    {
        if ((isize)strlen(s_pos_tag_table[i].tag) == tag_len &&
            memcmp(s_pos_tag_table[i].tag, tag, (usize)tag_len) == 0)
            return s_pos_tag_table[i].kind;
    }
    return POS_UNKNOWN;
}

typedef struct
{
    b32 open;
    f32 t;
    Rect rect;
} OverlayPopup;

typedef struct
{
    b32 active;
    char word[256];
    char phonetic[256];
    DefLine def_lines[32];
    i32 def_line_count;
    i32 anchor_x, anchor_y;
    u32 anchor_w, anchor_h;
    Rect popup_rect;
} OcrPopupState;

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
} Theme;

typedef struct WindowContext WindowContext;

typedef struct
{
    DWriteContext dwrite;
    Font fonts[FONT_CAPACITY];
    Theme theme;
    GlyphRasterCache raster_cache;
    RendererShared renderer_shared;
    HCURSOR cursors[UI_CURSOR_COUNT];
    WindowContext* quick_search_window;
    WindowContext* ocr_popup_window;
    HWND tray_hwnd;
    Arena cfg_arena;
    Arena cmd_arena;
    CmdRegistry cmd_registry;
    ShortcutRegistry shortcuts;
    CmdQueue cmd_queue;
    SearchState palette_search;
    DictDB dict_db;
    Arena dict_arena;
    volatile LONG dict_ready;
    DWORD main_thread_id;
    DictSearchAuxEntry* search_aux;
    Arena search_aux_arena;

    Color accent_border_color;
    b32 has_accent_border;

    b32 ocr_async_active;
} AppShared;

struct WindowContext
{
    HWND window;
    UIContext ui;
    Renderer renderer;
    IDCompositionDevice* dcomp_device;
    IDCompositionTarget* dcomp_target;
    IDCompositionVisual* dcomp_visual;
    AppShared* shared;
    WindowContext* next;
    u32 id;

    u8 palette_text_buf[SEARCH_QUERY_BUF];
    TextEditState palette_text_edit;
    i32 palette_selected_index;
    i32 palette_prev_selected_index;
    isize palette_prev_query_len;
    b32 palette_activate_pending;
    SearchPaletteMode palette_search_mode;
    SearchPaletteMode palette_effective_mode;
    volatile LONG palette_switch_version;

    OverlayPopup palette_popup;

    /* IME */
    HIMC default_himc;

    b32 in_frame;
    b32 mouse_tracked;

    b32 is_quick_search;
    b32 quick_search_result_confirmed;
    b32 quick_search_closing;
    b32 dpi_repositioning;

    b32 is_ocr_popup;
    OcrPopupState ocr_popup;
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
};
// clang-format on

// Forward declarations for global lookup helpers used by handlers
static void process_frame(WindowContext* ctx);
static WindowContext* create_quick_search_window(AppShared* shared);
static WindowContext* find_quick_search_window(AppShared* shared);
static WindowContext* create_ocr_popup_window(AppShared* shared);
static WindowContext* find_ocr_popup_window(AppShared* shared);
static void ocr_popup_activate(AppShared* shared, const u8* word, OcrWordBbox* bbox);

static WindowContext* create_quick_search_window(AppShared* shared)
{
    WindowContext* ctx = (WindowContext*)calloc(1, sizeof(WindowContext));
    Assert(ctx);
    MEM_TRACK("[mem] calloc: quick-search WindowContext = %zu B\n", sizeof(WindowContext));
    ctx->shared = shared;
    ctx->id = s_window_next_id++;
    ctx->is_quick_search = True;

    /* Use the DPI of the primary monitor (no main window exists anymore). */
    HMONITOR mon = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
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

static WindowContext* create_ocr_popup_window(AppShared* shared)
{
    WindowContext* ctx = (WindowContext*)calloc(1, sizeof(WindowContext));
    Assert(ctx);
    MEM_TRACK("[mem] calloc: ocr-popup WindowContext = %zu B\n", sizeof(WindowContext));
    ctx->shared = shared;
    ctx->id = s_window_next_id++;
    ctx->is_ocr_popup = True;

    HMONITOR mon = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    {
        UINT dpi_x = 0, dpi_y = 0;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
        if (dpi_x > 0)
            dpi = dpi_x;
    }
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_w = (u32)(OCR_POPUP_WINDOW_W * dpi_scale);
    u32 physical_h = (u32)(OCR_POPUP_WINDOW_H * dpi_scale);

    ctx->window =
        CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"window class", L"", WS_POPUP, 0,
                        0, (i32)physical_w, (i32)physical_h, NULL, NULL, GetModuleHandleW(NULL), ctx);
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

    ctx->renderer.is_composition = True;
    ctx->renderer.initial_width = physical_w;
    ctx->renderer.initial_height = physical_h;

    renderer_init(&ctx->renderer, &shared->renderer_shared, ctx->window);
    renderer_init_dcomp(&ctx->renderer, ctx->window, &ctx->dcomp_device, &ctx->dcomp_target, &ctx->dcomp_visual);

    ctx->default_himc = ImmGetContext(ctx->window);
    ImmReleaseContext(ctx->window, ctx->default_himc);

    shared->ocr_popup_window = ctx;

    ShowWindow(ctx->window, SW_HIDE);

    return ctx;
}

//
// Window Dragging
//

static void quick_search_activate(AppShared* shared)
{
    if (!InterlockedOr(&shared->dict_ready, 0))
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

    /* DPI + position adaptation for primary monitor.
       Hidden WS_POPUP windows don't receive WM_DPICHANGED, so GetDpiForWindow
       returns stale values. Use GetDpiForMonitor on the primary monitor as ground truth. */
    {
        HMONITOR mon = MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
        UINT target_dpi = USER_DEFAULT_SCREEN_DPI;
        {
            UINT dx = 0, dy = 0;
            GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
            if (dx > 0)
                target_dpi = dx;
        }

        if (target_dpi != qs->ui.dpi)
        {
            qs->ui.dpi = target_dpi;
            renderer_recreate_glyph_atlas_texture(&qs->renderer);
        }

        f32 dpi_scale = (f32)target_dpi / USER_DEFAULT_SCREEN_DPI;
        u32 physical_w = (u32)(QUICK_SEARCH_WINDOW_W * dpi_scale);
        u32 physical_h = (u32)(QUICK_SEARCH_WINDOW_H * dpi_scale);

        MONITORINFO mi = { .cbSize = sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        i32 work_w = mi.rcWork.right - mi.rcWork.left;
        i32 work_h = mi.rcWork.bottom - mi.rcWork.top;
        i32 pos_x = mi.rcWork.left + (work_w - (i32)physical_w) / 2;
        i32 pos_y = mi.rcWork.top + (work_h - (i32)physical_h) / 2;

        /* Guard: skip GetDpiForWindow in WM_NCCALCSIZE (stale — window hasn't moved yet) */
        qs->dpi_repositioning = True;

        /* Show before SetWindowPos so WM_NCCALCSIZE/WM_SIZE fire while visible on target monitor */
        ShowWindow(qs->window, SW_SHOW);
        SetWindowPos(qs->window, NULL, pos_x, pos_y, (i32)physical_w, (i32)physical_h, SWP_NOZORDER | SWP_NOACTIVATE);

        /* Hidden WS_POPUP may not get WM_SIZE — manually update client dims + swapchain */
        {
            f32 s = (f32)qs->ui.dpi / USER_DEFAULT_SCREEN_DPI;
            qs->ui.client_width = (u32)ceil(physical_w / s);
            qs->ui.client_height = (u32)ceil(physical_h / s);
            if (qs->ui.client_width > 0 && qs->ui.client_height > 0)
                qs->ui.render_fn.on_resize(qs->ui.renderer, physical_w, physical_h);
        }

        qs->dpi_repositioning = False;
    }

    SetForegroundWindow(qs->window);
}

/* Activate the quick-search palette with a preset query (used by OCR).
   The query text is copied into the palette buffer so search_palette_render
   picks it up and auto-queries on the next frame. */
static void quick_search_open_with_query(AppShared* shared, const u8* query)
{
    if (!InterlockedOr(&shared->dict_ready, 0))
        return;

    WindowContext* qs = find_quick_search_window(shared);
    if (!qs)
    {
        qs = create_quick_search_window(shared);
        if (!qs)
            return;
    }

    isize qlen = (isize)strlen((const char*)query);
    if (qlen > SEARCH_QUERY_BUF - 1)
        qlen = SEARCH_QUERY_BUF - 1;

    /* All OCR words are normalised to lowercase before being placed in the
       quick-search input field. */
    u8 lower_buf[SEARCH_QUERY_BUF];
    for (isize i = 0; i < qlen; i++)
    {
        u8 c = query[i];
        if (c >= 'A' && c <= 'Z')
            c = (u8)(c + ('a' - 'A'));
        lower_buf[i] = c;
    }
    lower_buf[qlen] = 0;
    const u8* lq = lower_buf;

    /* If the OCR word is an inflected variant not present as a headword,
       resolve it to its base word and query that instead. */
    if (dict_lookup(&shared->dict_db, (const char*)lq) < 0)
    {
        i32 base_idx = dict_resolve(&shared->dict_db, (const char*)lq);
        if (base_idx >= 0)
        {
            const char* base = DICT_STR(&shared->dict_db, shared->dict_db.words[base_idx].word_stroff);
            isize blen = (isize)strlen(base);
            if (blen > SEARCH_QUERY_BUF - 1)
                blen = SEARCH_QUERY_BUF - 1;
            memcpy(qs->palette_text_buf, base, (usize)blen);
            qs->palette_text_buf[blen] = 0;
            qs->palette_text_edit.text_len = (i32)blen;
            qs->palette_text_edit.cursor = (i32)blen;
            qs->palette_text_edit.mark = 0;
            quick_search_activate(shared);
            return;
        }
    }

    memcpy(qs->palette_text_buf, lq, (usize)qlen);
    qs->palette_text_buf[qlen] = 0;
    qs->palette_text_edit.text_len = qlen;
    qs->palette_text_edit.cursor = (i32)qlen;
    qs->palette_text_edit.mark = 0;

    quick_search_activate(shared);
}

static void ocr_popup_activate(AppShared* shared, const u8* word, OcrWordBbox* bbox)
{
    if (!InterlockedOr(&shared->dict_ready, 0))
        return;

    WindowContext* popup = find_ocr_popup_window(shared);
    if (!popup)
    {
        popup = create_ocr_popup_window(shared);
        if (!popup)
            return;
    }

    popup->ocr_popup.active = False;

    isize wlen = (isize)strlen((const char*)word);
    if (wlen > 255)
        wlen = 255;

    u8 lower_buf[256];
    for (isize i = 0; i < wlen; i++)
    {
        u8 c = word[i];
        if (c >= 'A' && c <= 'Z')
            c = (u8)(c + ('a' - 'A'));
        lower_buf[i] = c;
    }
    lower_buf[wlen] = 0;

    const char* display_word = (const char*)lower_buf;
    i32 lookup_idx = dict_lookup(&shared->dict_db, (const char*)lower_buf);
    if (lookup_idx < 0)
    {
        i32 base_idx = dict_resolve(&shared->dict_db, (const char*)lower_buf);
        if (base_idx >= 0)
        {
            display_word = DICT_STR(&shared->dict_db, shared->dict_db.words[base_idx].word_stroff);
            lookup_idx = dict_lookup(&shared->dict_db, display_word);
        }
    }

    isize dwlen = (isize)strlen(display_word);
    if (dwlen > 255)
        dwlen = 255;
    memcpy(popup->ocr_popup.word, display_word, (usize)dwlen);
    popup->ocr_popup.word[dwlen] = 0;

    popup->ocr_popup.phonetic[0] = 0;
    popup->ocr_popup.def_line_count = 0;

    if (lookup_idx >= 0)
    {
        const u8* p = shared->dict_db.entdata + shared->dict_db.words[lookup_idx].entdata_off;
        u32 phon_off = dict_rd_u32(&p);
        dict_rd_u32(&p); /* def_off (English) */
        u32 tr_off = dict_rd_u32(&p);

        if (phon_off)
        {
            const char* s = DICT_STR(&shared->dict_db, phon_off);
            isize plen = (isize)strlen(s);
            if (plen > 255)
                plen = 255;
            memcpy(popup->ocr_popup.phonetic, s, (usize)plen);
            popup->ocr_popup.phonetic[plen] = 0;
        }

        if (tr_off)
        {
            const char* s = DICT_STR(&shared->dict_db, tr_off);
            isize slen = (isize)strlen(s);
            const char* line_start = s;
            i32 line_count = 0;

            for (isize i = 0; i <= slen && line_count < 32; i++)
            {
                if (s[i] == '\xef' && s[i + 1] == '\xbc' && s[i + 2] == '\x9b')
                {
                    goto emit;
                }
                else if (s[i] != 0)
                {
                    continue;
                }
            emit:
                if (i > (isize)(line_start - s))
                {
                    isize seg_len = s + i - line_start;
                    isize tag_len = 0;

                    if (line_start[0] == '[')
                    {
                        while (tag_len < seg_len && line_start[tag_len] != ']')
                            tag_len++;
                        if (tag_len < seg_len)
                            tag_len++;
                    }
                    else
                    {
                        while (tag_len < seg_len && ((u8)line_start[tag_len] < 0x80))
                            tag_len++;
                    }
                    while (tag_len > 0 && (line_start[tag_len - 1] == ' ' || line_start[tag_len - 1] == '.'))
                        tag_len--;

                    if (tag_len > 0 && tag_len < seg_len)
                    {
                        popup->ocr_popup.def_lines[line_count].kind = pos_kind_from_tag((const u8*)line_start, tag_len);
                        isize def_start = tag_len;
                        while (def_start < seg_len && (line_start[def_start] == ' ' || line_start[def_start] == '.'))
                            def_start++;
                        popup->ocr_popup.def_lines[line_count].def_text =
                            (String){ (u8*)(line_start + def_start), seg_len - def_start };
                    }
                    else
                    {
                        popup->ocr_popup.def_lines[line_count].kind = POS_UNKNOWN;
                        popup->ocr_popup.def_lines[line_count].def_text = (String){ (u8*)line_start, seg_len };
                    }
                    line_count++;
                }
                line_start = s + i + 3;
                i += 2;
                if (s[i - 2] == 0)
                    break;
            }
            popup->ocr_popup.def_line_count = line_count;
        }
    }

    i32 anchor_x, anchor_y;
    u32 anchor_w, anchor_h;
    if (bbox)
    {
        anchor_x = bbox->screen_x;
        anchor_y = bbox->screen_y;
        anchor_w = bbox->screen_w;
        anchor_h = bbox->screen_h;
    }
    else
    {
        POINT pt;
        GetCursorPos(&pt);
        anchor_x = pt.x;
        anchor_y = pt.y;
        anchor_w = 0;
        anchor_h = 0;
    }

    popup->ocr_popup.anchor_x = anchor_x;
    popup->ocr_popup.anchor_y = anchor_y;
    popup->ocr_popup.anchor_w = anchor_w;
    popup->ocr_popup.anchor_h = anchor_h;

    POINT anchor_pt = { anchor_x, anchor_y };
    HMONITOR mon = MonitorFromPoint(anchor_pt, MONITOR_DEFAULTTONEAREST);
    UINT target_dpi = USER_DEFAULT_SCREEN_DPI;
    {
        UINT dpi_x = 0, dpi_y = 0;
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
        if (dpi_x > 0)
            target_dpi = dpi_x;
    }

    if (target_dpi != popup->ui.dpi)
    {
        popup->ui.dpi = target_dpi;
        renderer_recreate_glyph_atlas_texture(&popup->renderer);
    }

    f32 dpi_scale = (f32)target_dpi / USER_DEFAULT_SCREEN_DPI;
    u32 physical_w = (u32)(OCR_POPUP_WINDOW_W * dpi_scale);
    u32 physical_h = (u32)(OCR_POPUP_WINDOW_H * dpi_scale);

    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    i32 gap = 8;
    i32 word_center_x = anchor_x + (i32)anchor_w / 2;
    i32 popup_x = word_center_x - (i32)physical_w / 2;
    i32 popup_y = anchor_y + (i32)anchor_h + gap;

    if (popup_x < mi.rcWork.left)
        popup_x = mi.rcWork.left;
    if (popup_x + (i32)physical_w > mi.rcWork.right)
        popup_x = mi.rcWork.right - (i32)physical_w;

    i32 popup_bottom = popup_y + (i32)physical_h;
    if (popup_bottom > mi.rcWork.bottom)
    {
        popup_y = anchor_y - (i32)physical_h - gap;
        if (popup_y < mi.rcWork.top)
            popup_y = mi.rcWork.top;
    }

    popup->dpi_repositioning = True;
    ShowWindow(popup->window, SW_SHOW);
    SetWindowPos(popup->window, NULL, popup_x, popup_y, (i32)physical_w, (i32)physical_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    {
        f32 s = (f32)popup->ui.dpi / USER_DEFAULT_SCREEN_DPI;
        popup->ui.client_width = (u32)ceil(physical_w / s);
        popup->ui.client_height = (u32)ceil(physical_h / s);
        if (popup->ui.client_width > 0 && popup->ui.client_height > 0)
            popup->ui.render_fn.on_resize(popup->ui.renderer, physical_w, physical_h);
    }

    popup->dpi_repositioning = False;
    popup->ocr_popup.active = True;
    popup->ui.requested_frames = IDLE_WAKE_FRAMES;
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
                    /* Toggle the quick-search palette */
                    if (!InterlockedOr(&shared->dict_ready, 0))
                        return 0;
                    WindowContext* qs = find_quick_search_window(shared);
                    if (qs && GetForegroundWindow() == qs->window)
                    {
                        qs->palette_popup.open = False;
                        qs->quick_search_result_confirmed = False;
                        qs->ui.requested_frames = IDLE_WAKE_FRAMES;
                    }
                    else
                    {
                        quick_search_activate(shared);
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
                        PostQuitMessage(0);
                    }
                }
            }
            return 0;
        }
        case WM_HOTKEY:
        {
            if (shared && wparam == HOTKEY_QUICK_SEARCH)
            {
                if (!InterlockedOr(&shared->dict_ready, 0))
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
            if (!shared || !InterlockedOr(&shared->dict_ready, 0) || shared->ocr_async_active)
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

            ocr_popup_activate(shared, text, bbox);

            free(text);
            if (bbox)
                free(bbox);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static WindowContext* find_quick_search_window(AppShared* shared)
{
    return shared->quick_search_window;
}

static WindowContext* find_ocr_popup_window(AppShared* shared)
{
    return shared->ocr_popup_window;
}

//
// Command
//

static void cmd_toggle_theme(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    shared->theme = (shared->theme.border.r == s_theme_dark.border.r) ? s_theme_light : s_theme_dark;
}

static void cmd_close_palette(void* userdata, String cmd_text)
{
    (void)cmd_text;
    AppShared* shared = (AppShared*)userdata;
    WindowContext* ctx = shared->quick_search_window;
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
    String phonetic; // pronunciation string (empty if none)
    String display_context; // def or translation snippet; zero-length if not applicable
    f32 score;
    i32 word_range_count;
    FuzzyRange word_ranges[FUZZY_MAX_RANGES];
    i32 ctx_range_count;
    FuzzyRange ctx_ranges[FUZZY_MAX_RANGES];
    b32 has_context;
} PaletteDisplayItem;

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
                                       PaletteDisplayItem* out, Arena* arena)
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

            /* Flat v4 EntryBlob: 4 x u32 offsets (phonetic, definition, translation, exchange) */
            {
                const u8* p = db->entdata + w->entdata_off;
                u32 phon_off = dict_rd_u32(&p);
                u32 def_off = dict_rd_u32(&p);
                u32 tr_off = dict_rd_u32(&p);
                dict_rd_u32(&p); /* exchange_off (unused in palette) */

                if (phon_off)
                {
                    const char* s = DICT_STR(db, phon_off);
                    item->phonetic = (String){ (u8*)s, (isize)strlen(s) };
                }

                if (tr_off)
                {
                    const char* s = DICT_STR(db, tr_off);
                    item->display_context = (String){ (u8*)s, (isize)strlen(s) };
                    item->has_context = True;
                    item->ctx_range_count = 0;
                }
            }
            continue;
        }

        /* ── DEF mode: dict_def_extract returns only the Chinese translation,
           so match ranges are already byte offsets relative to that segment.
           Just locate the AUXSEG_DEF_ZH segment and use it directly — no
           segment‑local normalisation needed. ── */
        if (mode == PALETTE_MODE_DEF)
        {
            const AuxSegment* zh_seg = NULL;
            for (i32 s = 0; s < aux[idx].def_seg_count; s++)
            {
                if (aux[idx].def_segs[s].kind == AUXSEG_DEF_ZH)
                {
                    zh_seg = &aux[idx].def_segs[s];
                    break;
                }
            }

            if (zh_seg && sr[i].range_count > 0)
            {
                item->display_context.data = (u8*)aux[idx].def_search_text + zh_seg->offset;
                item->display_context.len = zh_seg->len;
                item->has_context = True;

                // Ranges are byte offsets into the Chinese segment (the
                // text returned by dict_def_extract).  Copy directly,
                // clipping to segment bounds + snapping to UTF-8 boundaries.
                item->ctx_range_count = 0;
                for (i32 r = 0; r < sr[i].range_count && item->ctx_range_count < FUZZY_MAX_RANGES; r++)
                {
                    i32 r_start = sr[i].ranges[r].start;
                    i32 r_end = sr[i].ranges[r].end;
                    if (r_start < 0)
                        r_start = 0;
                    if (r_end > (i32)zh_seg->len)
                        r_end = (i32)zh_seg->len;
                    if (r_end == (i32)zh_seg->len && r_end > 0)
                        r_end--; /* never point at the terminating \0 */

                    r_start = utf8_back_to_char_start(item->display_context.data, r_start, 0);
                    r_end = utf8_fwd_to_char_end(item->display_context.data, r_end, zh_seg->len);
                    if (r_end > r_start)
                    {
                        item->ctx_ranges[item->ctx_range_count].start = r_start;
                        item->ctx_ranges[item->ctx_range_count].end = r_end;
                        item->ctx_range_count++;
                    }
                }
            }
            else
            {
                item->has_context = False;
            }
        }
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
    const DictSearchAuxEntry* aux = &g_search_aux[idx];

    // For definition search, return only the Chinese translation segment
    // (AUXSEG_DEF_ZH).  Concatenating English + Chinese produces overly
    // long candidate texts for common polysemous words (e.g. "run" has
    // 250+ bytes of English definitions listing every sense — "to move
    // quickly", "to manage", "to flow", … — before the Chinese
    // translation).  Searching only the Chinese side keeps candidates
    // focused and comparable in length.
    for (i32 i = 0; i < aux->def_seg_count; i++)
    {
        if (aux->def_segs[i].kind == AUXSEG_DEF_ZH)
        {
            return (String){ (u8*)aux->def_search_text + aux->def_segs[i].offset, aux->def_segs[i].len };
        }
    }
    return (String){ 0 };
}

static FieldDef s_dict_fields_def[] = {
    { "def", dict_def_extract, 1.0f },
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

// Like dict_freq_weight but with a higher blend weight for definition
// search.  Definition texts for common polysemous words (e.g. "run")
// can be 10‑50× longer than rare words (e.g. "lope"), producing raw
// fuzzy scores that are still spread across a wider range even after
// log‑normalisation.  A stronger frequency signal compensates for the
// remaining bias so that "跑步" → "run" ranks ahead of niche words.
static f32 dict_freq_weight_def(const void* entry, f32 raw)
{
    const DictWordIndex* w = (const DictWordIndex*)entry;
    u32 freq = w->freq;
    f32 freq_score;

    if (freq == 0xFFFFFFFF)
        freq_score = 5.0f;
    else
        freq_score = log2f(1.0f + (f32)freq) * 0.8f;

    const f32 freq_weight = 0.75f;
    return raw * (1.0f - freq_weight) + freq_score * freq_weight;
}

static FieldDef s_dict_fields[] = { { "word", dict_word_extract, 1.0f } };
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

static void render_dict_headline(String word_text, i32 word_range_count, const FuzzyRange* word_ranges,
                                 const TextConfig* word_cfg, const TextConfig* word_hl_cfg, String phonetic,
                                 const TextConfig* phon_cfg)
{
    UIBox* word_line = ui_box_begin(&(BoxConfig){
        .sizing = { fit_grow({}), fit({}) },
        .child_gap = 8.f,
    });
    {
        UIBox* word_inner = ui_box_begin(&(BoxConfig){
            .sizing = { fit({}), fit({}) },
            .direction = LAYOUT_LEFT_TO_RIGHT,
            .child_gap = 0.f,
        });
        {
            render_highlighted_text(word_text, word_range_count, word_ranges, word_cfg, word_hl_cfg);
        }
        ui_box_end(word_inner);

        if (phonetic.len > 0)
        {
            UIBox* phon_inner = ui_box_begin(&(BoxConfig){
                .sizing = { fit({}), fit({}) },
                .direction = LAYOUT_LEFT_TO_RIGHT,
                .child_gap = 0.f,
            });
            {
                ui_text(str("/"), phon_cfg);
                ui_text(phonetic, phon_cfg);
                ui_text(str("/"), phon_cfg);
            }
            ui_box_end(phon_inner);
        }
    }
    ui_box_end(word_line);
}

/* Returns byte length of the longest prefix of `text` whose rendered width plus
   the width of "..." fits within `max_w` (logical px). Used to truncate palette
   context lines at the visible right edge rather than at a fixed char count. */
static isize palette_fit_width(u8* data, isize len, f32 max_w, const Font* font, f32 font_size, u32 dpi,
                               Renderer* renderer, GlyphRasterCache* raster_cache)
{
    String dots = str("...");
    f32 dots_w = renderer_get_text_width_for_dpi(renderer, raster_cache, dots, font, font_size, dpi);
    f32 acc = 0;
    const u8* p = data;
    const u8* end = data + len;
    const u8* cut = data;
    while (p < end)
    {
        UnicodeDecode ud = utf8_decode(p);
        if (ud.codepoint == 0)
            break;
        f32 cw = renderer_get_text_width_for_dpi(renderer, raster_cache, (String){ (u8*)p, (isize)(ud.next_p - p) },
                                                 font, font_size, dpi);
        if (acc + cw + dots_w > max_w)
            break;
        acc += cw;
        cut = ud.next_p;
        p = ud.next_p;
    }
    return (isize)(cut - data);
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
        }
        // Set mode-specific score adjustment BEFORE search_reconfigure so
        // that workers spawned by reconfigure use the correct weighting
        // from the very first entry.  search_reconfigure() kills old
        // workers, spawns new ones, and immediately triggers a search
        // round — if score_adjust is changed afterwards the workers will
        // have already processed the query with the old weighting.
        shared->palette_search.score_adjust = (mode == PALETTE_MODE_DEF) ? dict_freq_weight_def : dict_freq_weight;

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
    i32 item_count = palette_build_display_items(sr, sr_count, mode, SEARCH_DISPLAY_MAX, items, &ctx->ui.arena);

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

                            /* Horizontal budget for the context line: palette width minus the
                               horizontal paddings of vbox(1) + scroll_container(1) + scroll area(5)
                               + result row(8), times two sides. */
                            f32 ctx_avail_w = popup_w - 30.f;

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
                                        TextConfig phon_cfg = {
                                            .font = &shared->fonts[FONT_INDEX_UI],
                                            .font_size = 11.f,
                                            .color = theme->dict_phonetic_fg,
                                        };
                                        render_dict_headline(item->word_text, item->word_range_count, item->word_ranges,
                                                             &normal_cfg, &highlight_cfg, item->phonetic, &phon_cfg);
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

                                        /* Truncate at the visible right edge (fit to available width) */
                                        {
                                            isize fit = palette_fit_width(
                                                ctx_text.data, ctx_text.len, ctx_avail_w, &shared->fonts[FONT_INDEX_ZH],
                                                11.f, ctx->ui.dpi, &ctx->renderer, &shared->raster_cache);
                                            if (fit < ctx_text.len)
                                            {
                                                trunc_byte = fit;
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

static void ocr_popup_render(WindowContext* ctx)
{
    if (!ctx->ocr_popup.active)
        return;

    AppShared* shared = ctx->shared;
    UIContext* ui_ctx = &ctx->ui;
    const Theme* theme = &shared->theme;

    f32 client_w = (f32)ui_ctx->client_width;
    f32 client_h = (f32)ui_ctx->client_height;
    f32 popup_w = client_w - 10.f;
    f32 popup_h = client_h - 10.f;
    f32 pad = 12.f;

    UIBox* popup = ui_box_begin(&(BoxConfig){
        .sizing = { fixed(popup_w), fixed(popup_h) },
        .flags = BoxFlag_Float,
        .float_offset = { 5, 5 },
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
        UIBoxInteractResult ir = ui_box_interact(popup, str("##ocr_popup"));
        if (ir.last_box)
        {
            ctx->ocr_popup.popup_rect = (Rect){
                ir.last_box->position.x,
                ir.last_box->position.y,
                ir.last_box->position.x + ir.last_box->size.width,
                ir.last_box->position.y + ir.last_box->size.height,
            };
        }

        UIBox* vbox = ui_box_begin(&(BoxConfig){
            .sizing = { grow({}), grow({}) },
            .padding = { pad, pad, pad, pad },
            .direction = LAYOUT_TOP_TO_BOTTOM,
            .child_gap = 8,
        });
        {
            {
                String word_str = { (u8*)ctx->ocr_popup.word, (isize)strlen(ctx->ocr_popup.word) };
                String phon_str = { (u8*)ctx->ocr_popup.phonetic, (isize)strlen(ctx->ocr_popup.phonetic) };
                TextConfig word_cfg = { .font = &shared->fonts[FONT_INDEX_UI],
                                        .font_size = 14.f,
                                        .color = theme->dict_word_fg };
                TextConfig word_hl_cfg = word_cfg;
                TextConfig phon_cfg = { .font = &shared->fonts[FONT_INDEX_UI],
                                        .font_size = 11.f,
                                        .color = theme->dict_phonetic_fg };

                render_dict_headline(word_str, 0, NULL, &word_cfg, &word_hl_cfg, phon_str, &phon_cfg);
            }

            /* separator */
            ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = theme->dict_separator }));

            if (ctx->ocr_popup.def_line_count > 0)
            {
                TextConfig pos_cfg = { .font = &shared->fonts[FONT_INDEX_ZH],
                                       .font_size = 10.f,
                                       .color = theme->accent_fg };
                f32 pos_pad = 6.f;
                f32 pos_box_w = 0.f;

                for (i32 i = 0; i < ctx->ocr_popup.def_line_count; i++)
                {
                    const char* name = NULL;
                    if ((u32)ctx->ocr_popup.def_lines[i].kind < countof(s_pos_names))
                        name = s_pos_names[ctx->ocr_popup.def_lines[i].kind];
                    if (!name)
                        name = "?";
                    String ns = { (u8*)name, (isize)strlen(name) };
                    f32 w = renderer_get_text_width_for_dpi(&ctx->renderer, &shared->raster_cache, ns, pos_cfg.font,
                                                            pos_cfg.font_size, ui_ctx->dpi);
                    if (w > pos_box_w)
                        pos_box_w = w;
                }
                pos_box_w += pos_pad * 2.f;

                ScrollContext scroll = ui_scrollable_area_begin(&(ScrollableAreaConfig){
                    .hash_str = str("##ocr_defs"),
                    .sizing = { grow({}), grow({}) },
                    .child_gap = 4.f,
                    .padding = { 2, 2, 2, 2 },
                    .bg = theme->palette_bg,
                    .thumb_color = theme->scrollbar_thumb,
                    .direction = LAYOUT_TOP_TO_BOTTOM,
                });
                {
                    TextConfig def_cfg = { .font = &shared->fonts[FONT_INDEX_ZH],
                                           .font_size = 11.f,
                                           .color = theme->dict_definition_fg,
                                           .line_height = 16.5f,
                                           .wrap = True };

                    for (i32 i = 0; i < ctx->ocr_popup.def_line_count; i++)
                    {
                        UIBox* line = ui_box_begin(&(BoxConfig){
                            .sizing = { grow({}), fit({}) },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .child_gap = 6,
                        });
                        {
                            UIBox* pos_box = ui_box_begin(&(BoxConfig){
                                .sizing = { fixed(pos_box_w), fit({}) },
                                .padding = { pos_pad, pos_pad, pos_pad, pos_pad },
                                .color = theme->accent_weak_bg,
                                .rect_style = { .corner_radius = 3 },
                                .alignment = { ALIGN_CENTER, ALIGN_CENTER },
                            });
                            {
                                const char* name = NULL;
                                if ((u32)ctx->ocr_popup.def_lines[i].kind < countof(s_pos_names))
                                    name = s_pos_names[ctx->ocr_popup.def_lines[i].kind];
                                if (!name)
                                    name = "?";
                                String ns = { (u8*)name, (isize)strlen(name) };
                                ui_text(ns, &pos_cfg);
                            }
                            ui_box_end(pos_box);

                            UIBox* def_box = ui_box_begin(&(BoxConfig){
                                .sizing = { fit_grow({}), fit({}) },
                                .padding = { 2, 0, 2, 0 },
                            });
                            {
                                ui_text(ctx->ocr_popup.def_lines[i].def_text, &def_cfg);
                            }
                            ui_box_end(def_box);
                        }
                        ui_box_end(line);
                    }
                }
                ui_scrollable_area_end(scroll);
            }
        }
        ui_box_end(vbox);
    }
    ui_box_end(popup);
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
            /* Confirmed or dismissed — hide and reset the palette for next use. */
            ctx->palette_popup.open = False;
            ctx->quick_search_result_confirmed = False;
            ctx->quick_search_closing = False;
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

    if (ctx->is_ocr_popup)
    {
        ImmAssociateContext(ctx->window, NULL);

        f32 client_w = (f32)ctx->ui.client_width;
        f32 client_h = (f32)ctx->ui.client_height;

        isize arena_pos = ui_frame_begin(&ctx->ui);
        UIBox* root = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(client_w), fixed(client_h) },
        });
        ocr_popup_render(ctx);
        ui_box_end(root);
        ui_frame_end(arena_pos);
        if (ctx->dcomp_device)
            IDCompositionDevice_Commit(ctx->dcomp_device);

        if (!ctx->ocr_popup.active)
        {
            ctx->ui.requested_frames = 0;
            ctx->in_frame = False;
            ShowWindow(ctx->window, SW_HIDE);
            return;
        }
        ctx->in_frame = False;
        return;
    }
}

//
// Main
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

    switch (message)
    {
        /* Compute client size from the (frame-less) window rect. */
        case WM_NCCALCSIZE:
        {
            if (!wparam)
                return DefWindowProcW(window, message, wparam, lparam);

            u32 dpi = ctx->dpi_repositioning ? ui_ctx->dpi : GetDpiForWindow(window);
            ui_ctx->dpi = dpi;
            f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
            NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)lparam;
            ui_ctx->client_width = (u32)ceil((params->rgrc[0].right - params->rgrc[0].left) / dpi_scale);
            ui_ctx->client_height = (u32)ceil((params->rgrc[0].bottom - params->rgrc[0].top) / dpi_scale);
            return 0;
        }

        case WM_ACTIVATE:
        {
            if (ctx && LOWORD(wparam) == WA_INACTIVE && !ctx->quick_search_closing)
            {
                if (ctx->is_quick_search)
                {
                    ctx->palette_popup.open = False;
                    ctx->quick_search_result_confirmed = False;
                }
                if (ctx->is_ocr_popup)
                    ctx->ocr_popup.active = False;
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            }
            if (ctx)
                ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_MOUSEMOVE:
        {
            ctx->ui.requested_frames = IDLE_WAKE_FRAMES;

            f32 dpi_scale = (f32)ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;
            Position mouse_pos_backup = ui_ctx->mouse_pos;
            ui_ctx->mouse_pos.x = GET_X_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_pos.y = GET_Y_LPARAM(lparam) / dpi_scale;
            ui_ctx->mouse_delta.x = ui_ctx->mouse_pos.x - mouse_pos_backup.x;
            ui_ctx->mouse_delta.y = ui_ctx->mouse_pos.y - mouse_pos_backup.y;

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

            /* Close palette when clicking outside its popup rect */
            {
                f32 dpi_scale = (f32)ctx->ui.dpi / USER_DEFAULT_SCREEN_DPI;
                f32 lx = click_x / dpi_scale;
                f32 ly = click_y / dpi_scale;

                if (ctx->is_quick_search)
                {
                    OverlayPopup* p = &ctx->palette_popup;
                    if (p->open && p->rect.xmax > p->rect.xmin)
                        if (!(lx >= p->rect.xmin && lx < p->rect.xmax && ly >= p->rect.ymin && ly < p->rect.ymax))
                        {
                            p->open = False;
                            ctx->palette_selected_index = -1;
                            ctx->palette_prev_selected_index = -1;
                            ctx->palette_activate_pending = False;
                        }
                }

                if (ctx->is_ocr_popup)
                {
                    OcrPopupState* p = &ctx->ocr_popup;
                    if (p->active && p->popup_rect.xmax > p->popup_rect.xmin)
                        if (!(lx >= p->popup_rect.xmin && lx < p->popup_rect.xmax && ly >= p->popup_rect.ymin &&
                              ly < p->popup_rect.ymax))
                            p->active = False;
                }
            }

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
                if (ctx && ctx->is_quick_search)
                {
                    ctx->palette_popup.open = False;
                    ctx->quick_search_result_confirmed = False;
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                }
                if (ctx && ctx->is_ocr_popup)
                {
                    ctx->ocr_popup.active = False;
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                }
                return 0;
            }

            if (wparam == VK_ESCAPE)
            {
                if (ctx && ctx->is_quick_search)
                {
                    ctx->palette_popup.open = False;
                    ctx->quick_search_result_confirmed = False;
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
                if (ctx && ctx->is_ocr_popup)
                {
                    ctx->ocr_popup.active = False;
                    ctx->ui.requested_frames = IDLE_WAKE_FRAMES;
                    return 0;
                }
                return 0;
            }

            /* Palette mode switching — intercept before shortcut lookup
               so Ctrl+W switches to word mode instead of closing a tab.
               Tab/Shift+Tab cycles forward/backward through modes. */
            if (ctx && ctx->palette_popup.open && ctx->ui.focused_hash == s_search_palette_input_hash)
            {
                SearchPaletteMode new_mode = ctx->palette_search_mode;
                b32 handled = True;

                if (ctrl)
                {
                    switch (wparam)
                    {
                        case 'D':
                            new_mode = PALETTE_MODE_DEF;
                            break;
                        case 'W':
                            new_mode = PALETTE_MODE_WORD;
                            break;
                        default:
                            handled = False;
                            break;
                    }
                }
                else if (wparam == VK_TAB)
                {
                    if (shift)
                    {
                        switch (ctx->palette_search_mode)
                        {
                            case PALETTE_MODE_DEF:
                                new_mode = PALETTE_MODE_WORD;
                                break;
                            default:
                                new_mode = PALETTE_MODE_DEF;
                                break;
                        }
                    }
                    else
                    {
                        switch (ctx->palette_search_mode)
                        {
                            case PALETTE_MODE_WORD:
                                new_mode = PALETTE_MODE_DEF;
                                break;
                            default:
                                new_mode = PALETTE_MODE_WORD;
                                break;
                        }
                    }
                }
                else
                {
                    handled = False;
                }

                if (handled)
                {
                    if (new_mode != ctx->palette_search_mode)
                    {
                        const FieldDef* fields;
                        i32 field_count;
                        switch (new_mode)
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
                        }
                        shared->palette_search.score_adjust =
                            (new_mode == PALETTE_MODE_DEF) ? dict_freq_weight_def : dict_freq_weight;
                        search_reconfigure(&shared->palette_search, fields, field_count);
                        ctx->palette_search_mode = new_mode;
                        ctx->palette_effective_mode = new_mode;
                        ctx->palette_switch_version = shared->palette_search.query_version;
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

            switch (wparam)
            {
                case VK_LEFT:
                {
                    action.delta = -1;
                    if (!shift)
                        action.flags |= TextActionFlag_DeltaPicksSelectionSide;
                }
                break;
                case VK_RIGHT:
                {
                    action.delta = +1;
                    if (!shift)
                        action.flags |= TextActionFlag_DeltaPicksSelectionSide;
                }
                break;
                case VK_HOME:
                    action.delta = -INT64_MAX;
                    break;
                case VK_END:
                    action.delta = +INT64_MAX;
                    break;
                case VK_BACK:
                {
                    action.delta = -1;
                    action.flags |= TextActionFlag_Delete | TextActionFlag_ZeroDeltaWithSelection;
                }
                break;
                case VK_DELETE:
                {
                    action.delta = +1;
                    action.flags |= TextActionFlag_Delete | TextActionFlag_ZeroDeltaWithSelection;
                }
                break;
                case 'A':
                    if (ctrl)
                        action.flags |= TextActionFlag_SelectAll;
                    else
                        return DefWindowProcW(window, message, wparam, lparam);
                    break;
                case 'C':
                    if (ctrl)
                        action.flags |= TextActionFlag_Copy;
                    else
                        return DefWindowProcW(window, message, wparam, lparam);
                    break;
                case 'X':
                    if (ctrl)
                        action.flags |= TextActionFlag_Copy | TextActionFlag_Delete;
                    else
                        return DefWindowProcW(window, message, wparam, lparam);
                    break;
                case 'V':
                    if (ctrl)
                        action.flags |= TextActionFlag_Paste;
                    else
                        return DefWindowProcW(window, message, wparam, lparam);
                    break;
                default:
                    if (message == WM_SYSKEYDOWN)
                        return 0;
                    return DefWindowProcW(window, message, wparam, lparam);
            }
            ui_ctx->text_action_queue[ui_ctx->text_action_queue_count++] = action;

            Assert(ui_ctx->text_action_queue_count < TEXT_ACTION_QUEUE_CAPACITY);

            return 0;
        }

        case WM_CHAR:
        {
            wchar_t c = (wchar_t)wparam;
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
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                if (ctx->is_quick_search)
                    shared->quick_search_window = NULL;
                if (ctx->is_ocr_popup)
                    shared->ocr_popup_window = NULL;
                ui_deinit(&ctx->ui);
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
                renderer_deinit(&ctx->renderer);
                free(ctx);
            }
            if (!ctx || !ctx->is_ocr_popup)
                PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

static b32 any_window_needs_frames(const AppShared* shared)
{
    if (shared->quick_search_window && shared->quick_search_window->ui.requested_frames > 0)
        return True;
    if (shared->ocr_popup_window && shared->ocr_popup_window->ui.requested_frames > 0)
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
        MEM_TRACK("[mem] dict_arena push: decompressed dict = %lld B (%lld MB)\n", (long long)dsize,
                  (long long)(dsize / (1024 * 1024)));
        arena_push(&shared->dict_arena, (isize)dsize, 1, 1);

        usize result = ZSTD_decompress(shared->dict_arena.base, (size_t)dsize, compressed, compressed_size);
        Assert(!ZSTD_isError(result));

        shared->dict_db = dict_open(shared->dict_arena.base);
        Assert(shared->dict_db.hdr);
        g_dict_db = &shared->dict_db;
    }

    MEM_TRACK("[mem] search_aux_arena: arena_new(MB(16), MB(8))\n");
    shared->search_aux_arena = arena_new(MB(16), MB(8));
    shared->search_aux = dict_build_search_aux(&shared->dict_db, &shared->search_aux_arena);
    Assert(shared->search_aux);
    g_search_aux = shared->search_aux;

    search_init(&shared->palette_search, shared->dict_db.words, (i32)shared->dict_db.hdr->word_count,
                sizeof(DictWordIndex), s_dict_fields, countof(s_dict_fields), dict_word_extract);
    shared->palette_search.score_adjust = dict_freq_weight;
    search_start(&shared->palette_search);

    InterlockedExchange(&shared->dict_ready, True);

    /* Wake rendering: if the quick-search window already exists, force a frame.
       Otherwise the next quick_search_activate will request frames itself. */
    if (shared->quick_search_window)
    {
        shared->quick_search_window->ui.requested_frames = IDLE_WAKE_FRAMES;
        PostMessageW(shared->quick_search_window->window, WM_NULL, 0, 0);
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
        cmd_register(&shared.cmd_registry, (CmdDef){ str("app.toggle_theme"),       str("Toggle Light/Dark Theme"),  str(""), cmd_toggle_theme,           &shared });
        cmd_register(&shared.cmd_registry, (CmdDef){ str("palette.close"),          str("Close Search Palette"),     str(""), cmd_close_palette,          &shared });

        /* Bind shortcuts */
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_NONE,                                         VK_F11 },       str("app.toggle_theme"));
        shortcut_bind(&shared.shortcuts, (Shortcut){ SHORTCUT_MOD_CTRL,                                         VK_OEM_4 },     str("palette.close")); // ctrl+[

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
        MEM_TRACK("[mem] CreateThread: startup_dict_thread (default stack = 1 MB reserved)\n");
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

    /* Create message-only window for tray icon callbacks (process-lifetime, independent of any real window) */
    shared.tray_hwnd =
        CreateWindowExW(0, L"TrayIconClass", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), &shared);
    b32 hotkey_ok = True;
    wchar_t hotkey_fail_buf[512] = L"";

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
        if (any_window_needs_frames(&shared))
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

        /* Execute queued commands (theme toggle / palette close) */
        cmd_queue_execute_all(&shared.cmd_queue, &shared.cmd_registry);

        if (!any_window_needs_frames(&shared))
            continue;

        TracyCZoneNC(ctx_main_loop, "MainLoop", TracyColor_Frame, TRACY_SUBSYSTEMS & TracySys_Frame);

        /* Quick-search and OCR popup windows need frames */
        {
            WindowContext* qs = shared.quick_search_window;
            if (qs && qs->ui.requested_frames > 0)
            {
                process_frame(qs);
                if (qs->ui.requested_frames > 0)
                    qs->ui.requested_frames--;
            }

            WindowContext* op = shared.ocr_popup_window;
            if (op && op->ui.requested_frames > 0)
            {
                process_frame(op);
                if (op->ui.requested_frames > 0)
                    op->ui.requested_frames--;
            }
        }

        TracyCZoneEnd(ctx_main_loop);
    }

    /* Clean up quick-search window if still alive */
    if (shared.quick_search_window)
        DestroyWindow(shared.quick_search_window->window);

    /* Clean up OCR popup window if still alive */
    if (shared.ocr_popup_window)
        DestroyWindow(shared.ocr_popup_window->window);

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
    UnregisterHotKey(shared.tray_hwnd, HOTKEY_QUICK_SEARCH);
    if (shared.tray_hwnd)
        DestroyWindow(shared.tray_hwnd);

    /* Uninstall mouse hook and deinit OCR */
    if (g_mouse_hook)
        UnhookWindowsHookEx(g_mouse_hook);
    ocr_deinit();

    ExitProcess(0);
}
