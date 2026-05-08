#pragma once

#include "utils.h"
#include "glyph_cache.h"

// clang-format off
#define ui_box(...)                                                                                                    \
    for (UIBox* box = ui_box_start(&(BoxConfig)__VA_ARGS__);                                                           \
         box != NULL;                                                                                                  \
         ui_box_end(box), box = NULL)

#define ui_scrollable_area(...)                                                                                                    \
    for (ScrollContext scroll_ctx = ui_scrollable_area_start(&(ScrollableAreaConfig)__VA_ARGS__);                                                           \
         scroll_ctx.area != NULL;                                                                                                  \
         ui_scrollable_area_end(scroll_ctx), scroll_ctx.area = NULL)
// clang-format on

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

#define COMMAND_QUEUE_CAPACITY 4096
#define HASH_STR_MAX_LENGTH    128

#define ICON_FONT_UTF8_OK     "\xEE\xA0\x80"
#define ICON_FONT_UTF8_CANCEL "\xEE\xA0\x81"

///

typedef enum
{
    // clang-format off
    UI_Signal_Flag_None     = 0,
    UI_Signal_Flag_Hovered  = (1 << 0),
    UI_Signal_Flag_LClicked = (1 << 1),
    UI_Signal_Flag_RClicked = (1 << 2),
    UI_Signal_Flag_Pressed    = (1 << 3),
    // clang-format on
} UISignalFlags;

#define ui_hovered(signal_flags)  (signal_flags & UI_Signal_Flag_Hovered)
#define ui_lclicked(signal_flags) (signal_flags & UI_Signal_Flag_LClicked)
#define ui_rclicked(signal_flags) (signal_flags & UI_Signal_Flag_RClicked)
#define ui_clicked(signal_flags)  (signal_flags & (UI_Signal_Flag_LClicked | UI_Signal_Flag_RClicked))
#define ui_pressed(signal_flags)  (signal_flags & UI_Signal_Flag_Pressed)

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
    b32 enable_clip;
    b32 is_float;
    Position float_offset;
} BoxConfig;

typedef struct
{
    const Font* font;
    f32 font_size;
    Color color;
    f32 line_height;
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
    const Font* font;
    f32 font_size;
    String content;
    Color color;
    Slice(String) wrapped_lines;
    isize line_count;
    f32 line_height;
    f32 half_leading;
} TextData;

typedef struct
{
    u8 str[HASH_STR_MAX_LENGTH];
    isize len;
} BoxKey;

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
    BoxConfig config;
    b32 is_float;

    /* layout tree */
    UIBox* parent;
    UIBox* prev;
    UIBox* next;
    UIBox* child_first;
    UIBox* child_last;

    /* cached data */
    BoxKey key;
    u64 last_frame_index;
    f64 idle_timer;

    Position position;
    Size size;
    Position scroll_delta;

    // NOTE:
    //   `_t` stands for transition value, ranging from [0, 1].
    //   The terms "hot" and "active" are context-dependent. For example:
    //     - Button: "hot" means hover, "active" means click.
    //     - Scroll Area: "hot" means nothing, whereas "active" means
    //       the mouse is moving continuously across frames.
    f32 hot_t;
    f32 active_t;
    TransitionState anim_state;
    TimedLerpAnimation scroll_anim_x;
    TimedLerpAnimation scroll_anim_y;
};

//
// Context
//

typedef void (*flush_and_present_fn)(const u32 client_width, const u32 client_height);
typedef void (*on_resize_fn)(const u32 client_width, const u32 client_height);
typedef void (*wait_for_last_submitted_frame_fn)();
typedef f32 (*get_text_width_fn)(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                 const u32 dpi);
typedef f32 (*get_text_height_fn)(GlyphCache* glyph_cache, const String text, const Font* font, const f32 font_size,
                                  const u32 dpi);
typedef void (*draw_rect_fn)(const GlyphCache* glyph_cache, const Rect rect, const Color color, const RectStyle style,
                             const Rect* clip);
typedef void (*draw_text_fn)(GlyphCache* glyph_cache, String text, const Position position, const Color color,
                             const Font* font, const f32 font_size, const u32 dpi, const Rect* clip);

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

typedef struct
{
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
    b32 mouse_press;
    u8 char_input_utf8[4];
    isize char_input_len;

    /* ui general */
    UIBox* root;
    UIBoxCache box_cache;
    BoxKey focused_box_key;

    /* scroll area */
    BoxKey last_pressed_scroll_thumb_x_key;
    BoxKey last_pressed_scroll_thumb_y_key;
    Position last_drag_anchor_mouse_pos;
    Position last_drag_anchor_mouse_scroll;

    /* render */
    GlyphCache glyph_cache;
    UIRenderFunc render_fn;
    Queue(UICommand, COMMAND_QUEUE_CAPACITY) command_queue;
} UIContext;

//
// Widget
//

typedef struct
{
    UIBox* box;
    b32 found;
} UIBoxFindResult;

typedef struct
{
    String hash_str;
    Position delta;
    Position max_delta;
    Position thumb_delta;
    Vec2F32 thumb_delta_scale;
    UIBox* area;
    UISignalFlags area_flags;
    UIBoxFindResult last_area_result;
    UIBoxFindResult last_content_result;
    Color thumb_color;
} ScrollContext;

typedef struct
{
    String hash_str;
    Sizing sizing;
    Color bg_color;
    Padding padding;
    Color thumb_color;
} ScrollableAreaConfig;

///

extern UIContext* g_ui_context;

///

void ui_init(const DWriteContext* dwrite, UIContext* ui_context, u32 width, u32 height, u32 dpi,
             UIRenderFunc render_fn);

UIBox* ui_box_start(const BoxConfig* config);
void ui_box_end(UIBox* box);
UIBox* ui_text(const String text, const TextConfig* text_config);

isize ui_frame_begin(UIContext* ui_context);
void ui_frame_end(isize arena_pos_backup);

ScrollContext ui_scrollable_area_start(const ScrollableAreaConfig* config);
void ui_scrollable_area_end(ScrollContext scroll_ctx);

UISignalFlags ui_button(const String text_with_hash_str, const Font* font, const f32 font_size, const Sizing sizing,
                        const Padding padding, const Color bg_color, const Color text_color, const Color bg_color_hover,
                        const Color bg_color_press);
UISignalFlags ui_switchbox(const String hash_str, const Font* font, b32* check, const Color bg_color,
                           const Color switch_button_color, const Color bg_color_active);
UISignalFlags ui_text_field(BufferCursor* buf_cursor, const String text_with_hash_str, const Font* font,
                            const f32 font_size, const SizingAxis sizing_x, const Padding padding, const Color bg_color,
                            const Color border_color, const Color text_color);
