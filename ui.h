#pragma once
#include "utils.h"
#include "glyph_cache.h"

// clang-format off
#define ui_box(...) \
    for (UIBox* box = ui_box_start(&(BoxConfig)__VA_ARGS__); \
         box != NULL; \
         ui_box_end(box), box = NULL)
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

// TODO: Don't hard-code
#define CHILDREN_SIZE      16
#define COMMAND_QUEUE_SIZE 4096

///

// Command -----------------------------

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
} UICommandRect;

typedef struct
{
    UICommandBase base;
    Font* font;
    f32 font_size;
    String content;
    Color color;
    Position position;
} UICommandText;

typedef union
{
    Command type;
    UICommandBase base;
    UICommandRect rect;
    UICommandText text;
} UICommand;

// Sizing ------------------------------

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

// Box ------------------------------

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
    f32 child_gap;
    Alignment alignment;
    LayoutDirection direction;
} BoxConfig;

typedef struct
{
    Font* font;
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
    Font* font;
    f32 font_size;
    String content;
    Color color;
    Slice(String) wrapped_lines;
    isize line_count;
    f32 line_height;
    f32 half_leading;
} TextData;

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
    Position position;
    Size size;

    UIBox* parent;
    UIBox* prev;
    UIBox* next;
    UIBox* child_first;
    UIBox* child_last;
};

// Context -----------------------------

typedef struct
{
    Arena arena;
    u32 dpi;
    u32 client_width; // logic client width
    u32 client_height; // logic client height
    void (*on_resize)(const u32 client_width, const u32 client_height);
    f32 (*get_text_width)(const GlyphCache* glyph_cache, const String text, const u32 dpi, const Font* font, const f32 font_size);
    f32 (*get_text_height)(const GlyphCache* glyph_cache, const String text, const u32 dpi, const Font* font, const f32 font_size);
    Queue(UICommand, COMMAND_QUEUE_SIZE) command_queue;
} UIContext;

///

void ui_reset(UIContext* ui_context);
UIBox* ui_box_start(const BoxConfig* config);
void ui_box_end(UIBox* box);
UIBox* ui_box_get_root();

void ui_calculate_layout(UIContext* ui_context, const GlyphCache* glyph_cache, UIBox* box);
void ui_generate_render_commands(UIContext* ui_context, const UIBox* box);

UIBox* ui_text(const UIContext* ui_context, const GlyphCache* glyph_cache, const String text, const TextConfig* text_config);

///

extern UICommand* g_ui_command_queue;
