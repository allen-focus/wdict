#pragma once
#include "lib.h"
#include "string.h"

// clang-format off
#define ui_box(...)                                                                                                 \
    for (UIBox* box = ui_box_start(&(BoxConfig)__VA_ARGS__);                                      \
         box != NULL;                                                                                     \
         ui_box_end(box), box = NULL)
// clang-format on

#define fixed(value) { value, SIZING_MODE_FIXED }
#define fit(value) { value, SIZING_MODE_FIT }
#define fit_grow(value) { value, SIZING_MODE_FIT_GROW }

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

typedef enum
{
    WIDTH,
    HEIGHT
} Axis;

typedef struct {
    f32 width;
    f32 height;
} Size;

typedef enum
{
    SIZING_MODE_FIXED,
    SIZING_MODE_FIT,
    SIZING_MODE_FIT_GROW,
} SizingMode;

typedef struct
{
    f32 value;
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
    Sizing sizing;
    Color color;
    RectStyle rect_style;
    Padding padding;
    f32 child_gap;
    LayoutDirection direction;
} BoxConfig;

typedef struct
{
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
    i32 child_count;
} ContainerData;

typedef struct
{
    String content;
    Color color;
    String* wrapped_lines;
    i32 line_count;
    f32 line_height;
    f32 half_leading;
    u32 (*get_text_width)(String text);
    u32 (*get_text_height)(String text);
} TextData;

typedef struct UIBox UIBox;
struct UIBox
{
    union {
        ContainerData container;
        TextData text;
    } data;
    BoxType type;
    BoxConfig config;
    Position position;
    Size min_size;
    UIBox* parent;
    UIBox* children[CHILDREN_SIZE];
};

// Context -----------------------------

typedef struct
{
    u16 client_width;
    u16 client_height;
    void (*on_resize)(const u16 client_width, const u16 client_height);
    u32 (*get_text_width)(String text);
    u32 (*get_text_height)(String text);
    Queue(UICommand, COMMAND_QUEUE_SIZE) ui_command_queue;
} UIContext;

///

void ui_reset(UIContext* ui_context);

UIBox* ui_box_get_root();
UIBox* ui_box_start(BoxConfig* config);
void ui_box_end(UIBox* box);

void ui_box_calculate_fit_axis(UIBox* box, Axis axis);
void ui_box_grow_shrink_children_axis(UIBox* box, Axis axis);
void ui_box_apply_text_wrapping(UIContext* ui_context, UIBox* box);
void ui_box_resolve_position(UIBox* box);

void ui_generate_render_commands(UIContext* ui_context, UIBox* box);

UIBox* ui_text(UIContext* ui_context, String text, TextConfig* text_config);

///

extern UICommand* g_ui_command_queue;
