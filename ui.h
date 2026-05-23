#pragma once

#include "utils.h"
#include "glyph_cache.h"
#include "panel.h"

// NOTE: In FIT mode, min/max constraints act as bounds on content wrapping:
// They define the range within which content can shrink or expand due to wrapping,
// but do not override the content's natural size if it falls outside this range.
//
// Example 1: Parent width = fit({ .min = 100, .max = 300 }), child is text with 400 width
//   → The parent can be sized down to 100 (text wraps at that point),
//     and can grow up to 300 before wrapping stops. The text's intrinsic width (400)
//     is constrained within this range for wrapping behavior.
//
// Example 2: Parent width = fit({ .min = 100, .max = 300 }), child is a fixed 50-width box
//   → The parent width becomes 50, ignoring the .min constraint,
//     because FIT mode adapts to the child's size when it's smaller than the min bound.
#define fixed(value)  { { value, value }, SIZING_MODE_FIXED }
#define fit(...)      { __VA_ARGS__, SIZING_MODE_FIT }
#define fit_grow(...) { __VA_ARGS__, SIZING_MODE_FIT_GROW }
#define grow(...)     { __VA_ARGS__, SIZING_MODE_GROW }

#define COMMAND_QUEUE_CAPACITY     4096
#define BOX_QUEUE_CAPACITY         2048
#define BOX_STACK_CAPACITY         32
#define HASH_STR_MAX_LENGTH        128
#define CHAR_INPUT_QUEUE_CAPACITY  64
#define TEXT_ACTION_QUEUE_CAPACITY 64
#define DRAG_PAYLOAD_MAX           64

#define ICON_FONT_UTF8_OK     "\xEE\xA0\x80"
#define ICON_FONT_UTF8_CANCEL "\xEE\xA0\x81"

///

typedef enum
{
    // clang-format off
    UI_Signal_Flag_None          = 0,
    UI_Signal_Flag_Hovered       = (1 << 0),
    UI_Signal_Flag_LClicked      = (1 << 1),
    UI_Signal_Flag_RClicked      = (1 << 2),
    UI_Signal_Flag_MClicked      = (1 << 3),
    UI_Signal_Flag_Pressed       = (1 << 4),
    UI_Signal_Flag_DoubleClicked = (1 << 5),
    UI_Signal_Flag_Released      = (1 << 6),
    UI_Signal_Flag_Dragging      = (1 << 7),
    UI_Signal_Flag_DragOver      = (1 << 8),
    UI_Signal_Flag_Dropped       = (1 << 9),
    // clang-format on
} UISignalFlags;

#define ui_hovered(signal_flags)        (signal_flags & UI_Signal_Flag_Hovered)
#define ui_lclicked(signal_flags)       (signal_flags & UI_Signal_Flag_LClicked)
#define ui_rclicked(signal_flags)       (signal_flags & UI_Signal_Flag_RClicked)
#define ui_mclicked(signal_flags)       (signal_flags & UI_Signal_Flag_MClicked)
#define ui_pressed(signal_flags)        (signal_flags & UI_Signal_Flag_Pressed)
#define ui_double_clicked(signal_flags) (signal_flags & UI_Signal_Flag_DoubleClicked)
#define ui_released(signal_flags)       (signal_flags & UI_Signal_Flag_Released)
#define ui_dragging(signal_flags)       (signal_flags & UI_Signal_Flag_Dragging)
#define ui_drag_over(signal_flags)      (signal_flags & UI_Signal_Flag_DragOver)
#define ui_dropped(signal_flags)        (signal_flags & UI_Signal_Flag_Dropped)

//
// Cursor
//

typedef enum
{
    UI_CURSOR_ARROW,
    UI_CURSOR_IBEAM,
    UI_CURSOR_HAND,
    UI_CURSOR_HORIZONTAL,
    UI_CURSOR_VERTICAL,
    UI_CURSOR_MOVE,
    UI_CURSOR_COUNT
} Cursor;

//
// Command
//

typedef enum
{
    UI_COMMAND_RECT,
    UI_COMMAND_TEXT
} Command;

typedef struct
{
    Command type;
    size_t size;
} UICommandBase;

typedef struct
{
    UICommandBase base;
    Rect rect;
    Color color;
    RectStyle style;
    Rect clip;
} UICommandRect;

typedef struct
{
    UICommandBase base;
    const Font* font;
    f32 font_size;
    String content;
    Color color;
    Position position;
    Rect clip;
} UICommandText;

typedef union
{
    Command type;
    UICommandBase base;
    UICommandRect rect;
    UICommandText text;
} UICommand;

//
// Sizing
//

typedef struct
{
    f32 width;
    f32 height;
} Size;

typedef struct
{
    f32 min;
    f32 max;
} MinMax;

typedef enum
{
    SIZING_MODE_FIXED,
    SIZING_MODE_FIT,
    SIZING_MODE_FIT_GROW,
    SIZING_MODE_GROW,
} SizingMode;

typedef struct
{
    MinMax min_max;
    SizingMode mode;
} SizingAxis;

typedef struct
{
    SizingAxis width;
    SizingAxis height;
} Sizing;

//
// Box
//

typedef enum
{
    BoxFlag_None = 0,
    BoxFlag_Clip = (1 << 0),
    BoxFlag_Float = (1 << 1),
} BoxFlags;

typedef enum
{
    LAYOUT_LEFT_TO_RIGHT,
    LAYOUT_TOP_TO_BOTTOM
} LayoutDirection;

typedef struct
{
    f32 top, right, bottom, left;
} Padding;

typedef enum
{
    ALIGN_START,
    ALIGN_CENTER,
    ALIGN_END,
} AlignPosition;

typedef struct
{
    AlignPosition x;
    AlignPosition y;
} Alignment;

typedef struct
{
    Sizing sizing;
    Color color;
    RectStyle rect_style;
    Padding padding;
    Position child_offset;
    f32 child_gap;
    Alignment alignment;
    LayoutDirection direction;
    u32 flags;
    Position float_offset;
} BoxConfig;

typedef struct
{
    const Font* font;
    f32 font_size;
    Color color;
    f32 line_height;
    b32 wrap;
} TextConfig;

typedef enum
{
    BOX_TYPE_CONTAINER,
    BOX_TYPE_TEXT
} BoxType;

typedef struct
{
    Size remaining_space;
    f32 next_child_offset_x;
    f32 next_child_offset_y;
    isize child_count;
} ContainerData;

typedef struct
{
    isize byte_start;
    isize byte_end;
    f32 width;
} WordBreak;

typedef Slice(WordBreak) WordBreakSlice;

#define TEXT_MEASURE_CACHE_SIZE 128

typedef struct
{
    u64 key;
    WordBreakSlice words;
    f32 full_text_width;
    f32 min_word_width;
    f32 space_width;
    isize word_count;
} TextMeasureSlot;

typedef struct
{
    const Font* font;
    f32 font_size;
    String content;
    Color color;
    Slice(String) wrapped_lines;
    isize line_count;
    f32 line_height;
    f32 half_leading;
    WordBreakSlice words;
    f32 space_width;
    f32 full_text_width;
} TextData;

typedef enum
{
    TRANSITION_IDLE,
    TRANSITION_FORWARD,
    TRANSITION_REVERSE,
} TransitionState;

typedef struct
{
    f32 start;
    f32 target;
    f64 started_at;
    f64 duration;
} TimedLerpAnimation;

typedef struct UIBox UIBox;
struct UIBox
{
    union
    {
        ContainerData container;
        TextData text;
    } data;

    BoxType type;
    BoxConfig cfg;
    u32 flags;

    /* layout tree */
    UIBox* parent;
    UIBox* prev;
    UIBox* next;
    UIBox* child_first;
    UIBox* child_last;

    /* cached data */
    u64 key_hash;
    u64 last_frame_index;
    f64 idle_timer;

    Position position;
    Size size;
    Rect clip;
    Position scroll_delta;

    // NOTE:
    //   `_t` stands for transition value, ranging from [0, 1].
    //   The terms "hot" and "active" are ctx-dependent. For example:
    //     - Button: "hot" means hover, "active" means click.
    //     - Scroll Area: "hot" means nothing, whereas "active" means
    //       the mouse is moving continuously across frames.
    f32 hot_t;
    f32 active_t;
    TransitionState anim_state;
    TimedLerpAnimation scroll_anim_x;
    TimedLerpAnimation scroll_anim_y;

    // Scroll-value anchor: recorded when thumb press starts.
    // Used by: scrollbar drag formula  new_scroll = anchor + mouse_delta * scale
    Position drag_scroll_anchor;

    // Mouse-position anchor: recorded by update_interaction_flags() on press.
    // Used by: ui_box_drag_delta() to compute mouse displacement since press.
    // Managed automatically — set on LClick, cleared on Release.
    Position drag_mouse_anchor;
};

typedef struct
{
    UISignalFlags flags;
    UIBox* last_box;
} UIBoxInteractResult;

//
// Text Edit
//

typedef struct
{
    byte* base;
    isize size;
    isize cursor;
    isize mark;
    isize text_len;
    f32 copy_t;
    f32 cursor_trail_x;
    f32 cursor_glide_x;
    isize composition_start;
    isize composition_len;
} TextEditState;

typedef enum
{
    // clang-format off
    TextActionFlag_WordScan                = (1 << 0), // Ctrl-held: move/dele by word boundary not codepoint
    TextActionFlag_KeepMark                = (1 << 1), // Shift-held: preserve mark for selection
    TextActionFlag_Delete                  = (1 << 2), // Del/Backspace: this action deletes, not navigates
    TextActionFlag_Copy                    = (1 << 3), // Ctrl+C
    TextActionFlag_Paste                   = (1 << 4), // Ctrl+V
    TextActionFlag_ZeroDeltaWithSelection  = (1 << 5), // If selection exists, treat delta as 0 (delete selection)
    TextActionFlag_DeltaPicksSelectionSide = (1 << 6), // On selection, jump to max/min side instead of moving by delta
    TextActionFlag_SelectAll               = (1 << 7), // Ctrl+A: select entire text
    // clang-format on
} TextActionFlags;

typedef struct
{
    TextActionFlags flags;
    isize delta;
    u32 codepoint;
} TextAction;

//
// Context
//

struct Renderer;

typedef void (*flush_and_present_fn)(struct Renderer* renderer, const u32 client_width, const u32 client_height);
typedef void (*on_resize_fn)(struct Renderer* renderer, const u32 client_width, const u32 client_height);
typedef void (*wait_for_last_submitted_frame_fn)(struct Renderer* renderer);
typedef f32 (*get_text_width_fn)(struct Renderer* renderer, GlyphRasterCache* raster_cache, const String text,
                                 const Font* font, const f32 font_size, const u32 dpi);
typedef f32 (*get_text_height_fn)(struct Renderer* renderer, GlyphRasterCache* raster_cache, const String text,
                                  const Font* font, const f32 font_size, const u32 dpi);
typedef void (*draw_rect_fn)(struct Renderer* renderer, const Rect rect, const Color color, const RectStyle style,
                             const Rect* clip);
typedef void (*draw_text_fn)(struct Renderer* renderer, GlyphRasterCache* raster_cache, String text,
                             const Position position, const Color color, const Font* font, const f32 font_size,
                             const u32 dpi, const Rect* clip);
typedef void (*clipboard_copy_fn)(const HWND window, const String text);
typedef String (*clipboard_paste_fn)(const HWND window, Arena* arena);

// NOTE:
//   Use LRUCache for its fixed-size hash table with linked-list chaining, and its
//   insertion order (new entries appended to tail). This allows per-frame cleanup
//   to traverse from the oldest until encountering an entry with a matching
//   `last_frame_index`, avoiding a full scan of all entries.
typedef struct
{
    Arena arena;
    LRUCache lru_cache;
} UIBoxCache;

typedef struct
{
    flush_and_present_fn flush_and_present;
    on_resize_fn on_resize;
    wait_for_last_submitted_frame_fn wait_for_last_submitted_frame;
    get_text_width_fn get_text_width;
    get_text_height_fn get_text_height;
    draw_rect_fn draw_rect;
    draw_text_fn draw_text;
} UIRenderFunc;

typedef struct UIContext UIContext;
struct UIContext
{
    /* OS specific */
    HWND window;

    /* renderer */
    struct Renderer* renderer;

    /* basic */
    Arena arena;
    f64 current_time;
    u64 frame_index;
    f32 frame_delta_time;

    /* window */
    u32 dpi;
    u32 client_width; // logic client width
    u32 client_height; // logic client height

    /* interaction */
    Position mouse_pos;
    Position mouse_delta;
    Position mouse_scroll_delta;
    b32 mouse_lclick;
    b32 mouse_rclick;
    b32 mouse_mclick;
    b32 mouse_press;
    b32 mouse_double_click;
    f64 last_lclick_time;
    Position last_lclick_pos;
    Cursor desired_cursor;
    u32 char_input_queue[CHAR_INPUT_QUEUE_CAPACITY];
    isize char_input_queue_count;
    TextAction text_action_queue[TEXT_ACTION_QUEUE_CAPACITY];
    isize text_action_queue_count;

    /* drag-drop */
    u8 drag_payload_buf[DRAG_PAYLOAD_MAX];
    isize drag_payload_size;
    b32 drag_active;
    UIBox* drag_source_box;
    b32 drag_payload_consumed;
    u64 mouse_captured_by_hash;

    /* idle */
    i32 requested_frames;

    /* box cache */
    UIBox* root;
    UIBoxCache box_cache;
    u64 focused_hash;
    Queue(UIBox, BOX_QUEUE_CAPACITY) box_queue;
    Stack(UIBox*, BOX_STACK_CAPACITY) box_stack;

    /* text cache */
    TextMeasureSlot text_measure_cache[TEXT_MEASURE_CACHE_SIZE];

    /* render */
    GlyphRasterCache* raster_cache;
    UIRenderFunc render_fn;
    Queue(UICommand, COMMAND_QUEUE_CAPACITY) command_queue;

    /* clipboard */
    clipboard_copy_fn clipboard_copy;
    clipboard_paste_fn clipboard_paste;

    /* IME */
    b32 ime_composing;
    String ime_composition;
    Position ime_cursor_screen_pos;

    /* nesting */
    UIContext* prev_ctx;
};

//
// Scroll Area
//

typedef struct
{
    u64 hash;
    Position delta;
    Position max_delta;
    Position thumb_delta;
    Vec2F32 thumb_delta_scale;
    UIBox* area;
    UISignalFlags area_flags;
    UIBox* last_area;
    UIBox* last_content;
    Color thumb_color;
    b32 fixed_track;
    f32 cursor_content_x;
    f32 scroll_margin;
} ScrollContext;

typedef struct
{
    String hash_str;
    Sizing sizing;
    Padding padding;
    f32 child_gap;
    Alignment alignment;
    LayoutDirection direction;
    Color bg;
    Color thumb_color;
    b32 fixed_track;
} ScrollableAreaConfig;

//
// Panel
//

typedef struct
{
    Color hover_bg;
    Color scrollbar_thumb;

    Color panel_bg;
    Color panel_border;

    Color tab_bg;
    Color tab_fg;
    Color tab_active_bg;
    Color tab_active_fg;
    Color tab_border;
    Color tab_accent;
    Color tab_accent_subtle;
    Color tab_accent_weak;
} PanelTheme;

typedef struct
{
    Panel* panel;
    Rect root_rect;
    Rect panel_rect; // pre-computed panel rect (set by caller; zero = compute internally)
    f32 tab_bar_right_inset; // pixels to trim from tab bar right edge (0 = full width)
    const PanelTheme* theme;
    const Font* font_ui;
    f32 font_size;
    CmdQueue* cmd_queue;
    u32 window_id;
    Padding padding;
    f32 child_gap;
    LayoutDirection direction;
} PanelConfig;

typedef struct
{
    Panel* panel;
    ScrollContext scroll_ctx;
    UIBox* outer_box;
    f32 panel_w;
    f32 panel_h;
    Rect tab_bar_spacer_rect; // output: absolute logical rect of tab bar spacer (filled by ui_panel_begin)
    u32 window_id;
    CmdQueue* cmd_queue;
    const PanelTheme* theme;
    f32 font_size;
} PanelContext;

//
// Functions
//

// basic
void ui_init(const HWND window, UIContext* ui_ctx, struct Renderer* renderer, GlyphRasterCache* raster_cache, u32 width,
             u32 height, u32 dpi, UIRenderFunc render_fn);
void ui_deinit(UIContext* ui_ctx);

isize ui_frame_begin(UIContext* ui_ctx);
void ui_frame_end(isize arena_pos_backup);

UIBox* ui_box_begin(const BoxConfig* cfg);
void ui_box_end(UIBox* box);
UIBox* ui_text(const String text, const TextConfig* text_cfg);

// interaction
UIBoxInteractResult ui_box_interact(UIBox* box, const String hash_str);

Position ui_box_drag_delta(const UIBox* box);
void ui_set_drag_payload(void* payload, isize size);

void ui_set_desired_cursor(Cursor shape);

// transition
Color lerp_color(const Color a, const Color b, const f32 t);
b32 update_transition(f32* transition, const f32 speed, const f32 target);
void ui_request_frames(void);

// scroll area
ScrollContext ui_scrollable_area_begin(const ScrollableAreaConfig* cfg);
void ui_scrollable_area_end(ScrollContext scroll_ctx);

// panel

void ui_panel_boundaries(const Panel* root, const Rect root_rect, const PanelTheme* theme);
PanelContext ui_panel_begin(const PanelConfig* cfg);
void ui_panel_end(PanelContext* pf);

// widget
UISignalFlags ui_button(const String text_with_hash_str, const Font* font, f32 font_size, Sizing sizing,
                        Padding padding, Color bg, Color fg, Color hover_bg);
UISignalFlags ui_switchbox(const String hash_str, const Font* font, b32* check, const Color bg, const Color active_bg,
                           const Color fg, const Color shadow_color);
UISignalFlags ui_text_field(TextEditState* state, const String text_with_hash_str, const Font* font,
                            const f32 font_size, const SizingAxis sizing_x, const Padding padding, const Color bg,
                            const Color border_color, const Color fg, const Color thumb_color,
                            const Color cursor_bar_color, const Color cursor_trail_color, const Color selection_color,
                            const Color selection_flash_color);

//
// Global Variables
//

extern UIContext* g_ui_ctx;
