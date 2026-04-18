#pragma once

#include "utils.h"
#include "glyph_cache.h"

// clang-format off
#define ui_box(...)                                                                                                    \
    for (UIBox* box = ui_box_start(&(BoxConfig)__VA_ARGS__);                                                           \
         box != NULL;                                                                                                  \
         ui_box_end(box), box = NULL)

#define ui_scrollable_area(...)                                                                                                    \
    for (ScrollContext scroll_context = ui_scrollable_area_start(&(ScrollableAreaConfig)__VA_ARGS__);                                                           \
         scroll_context.area_box != NULL;                                                                                                  \
         ui_scrollable_area_end(scroll_context), scroll_context.area_box = NULL)
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

///

typedef enum
{
    // clang-format off
    UI_Signal_Flag_None     = 0,
    UI_Signal_Flag_Hovered  = (1 << 0),
    UI_Signal_Flag_LClicked = (1 << 1),
    UI_Signal_Flag_RClicked = (1 << 2),
    // clang-format on
} UISignalFlags;

#define ui_hovered(signal_flags)  (signal_flags & UI_Signal_Flag_Hovered)
#define ui_lclicked(signal_flags) (signal_flags & UI_Signal_Flag_LClicked)
#define ui_rclicked(signal_flags) (signal_flags & UI_Signal_Flag_RClicked)
#define ui_clicked(signal_flags)  (signal_flags & (UI_Signal_Flag_LClicked | UI_Signal_Flag_RClicked))

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
    Font font;
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
} BoxConfig;

typedef struct
{
    Font font;
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
    Font font;
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
    ANIMATION_IDLE,
    ANIMATION_FORWARD,
    ANIMATION_REVERSE,
} AnimationState;

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

    /* layout tree */
    UIBox* parent;
    UIBox* prev;
    UIBox* next;
    UIBox* child_first;
    UIBox* child_last;

    /* cached data */
    BoxKey key;
    u64 last_frame_index;

    Position position;
    Size size;
    Position scroll_delta;

    f32 hover_t; // `t` is transition, range: [0, 1]
    f32 press_t;
    AnimationState anim_state;
};

//
// Context
//

typedef void (*flush_and_present_fn)(const u32 client_width, const u32 client_height);
typedef void (*on_resize_fn)(const u32 client_width, const u32 client_height);
typedef void (*wait_for_last_submitted_frame_fn)();
typedef f32 (*get_text_width_fn)(GlyphCache* glyph_cache, const String text, const Font font, const f32 font_size,
                                 const u32 dpi);
typedef f32 (*get_text_height_fn)(GlyphCache* glyph_cache, const String text, const Font font, const f32 font_size,
                                  const u32 dpi);
typedef void (*draw_rect_fn)(const GlyphCache* glyph_cache, const Rect rect, const Color color, const RectStyle style,
                             const Rect* clip);
typedef void (*draw_text_fn)(GlyphCache* glyph_cache, String text, const Position position, const Color color,
                             const Font font, const f32 font_size, const u32 dpi, const Rect* clip);

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
    u64 frame_index;
    f32 frame_delta_time;

    /* window */
    u32 dpi;
    u32 client_width; // logic client width
    u32 client_height; // logic client height

    /* interaction */
    Position mouse_pos;
    b32 mouse_lclick;
    b32 mouse_rclick;
    Position mouse_delta;

    /* ui */
    UIBox* root;
    UIBoxCache box_cache;

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
    Position delta;
    UIBox* area_box;
    UIBoxFindResult area_result;
    UIBoxFindResult content_result;
    Color thumb_color;
} ScrollContext;

typedef struct
{
    String text_with_hash_str;
    Sizing sizing;
    Color bg_color;
    Padding padding;
    Color thumb_color;
} ScrollableAreaConfig;

///

extern UIContext* g_ui_context;

///

void ui_init(UIContext* ui_context, u32 width, u32 height, u32 dpi, IDWriteFactory3* dwrite_factory,
             UIRenderFunc render_fn);

void ui_reset();
UIBox* ui_box_start(const BoxConfig* config);
void ui_box_end(UIBox* box);
UIBox* ui_text(const String text, const TextConfig* text_config);

isize ui_begin_frame(UIContext* ui_context);
void ui_end_frame(isize arena_pos_backup);

UISignalFlags ui_button(const String text_with_hash_str, const Font font, const Sizing sizing, const Color bg_color,
                        const Color text_color, const Color bg_color_hover, const Color bg_color_press);
UISignalFlags ui_switchbox(const String text_with_hash_str, const Font font, b32* check, const Color bg_color,
                           const Color switch_button_color, const Color bg_color_active);

ScrollContext ui_scrollable_area_start(const ScrollableAreaConfig* config);
void ui_scrollable_area_end(ScrollContext scroll_ctx);
