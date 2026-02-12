#pragma once
#include "lib.h"

// NOTE: Suppress compiler warning C4068: unknown pragma 'clang'
#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
static int ui_box_macro_flag;
#pragma clang diagnostic pop

// clang-format off
#define ui_box(...)                                                                                                 \
    for (ui_box_macro_flag = (ui_box_start(&(BoxConfig)__VA_ARGS__), 0);                                      \
         ui_box_macro_flag < 1;                                                                                     \
         ui_box_macro_flag = 1, ui_box_end())
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
    UI_COMMAND_RECT = 1,
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
    char* text;
    Position position;
    Color color;
} UICommandText;

typedef union
{
    Command type;
    UICommandRect rect;
    UICommandText text;
} UICommand;

// Sizing ------------------------------

typedef struct {
    float width;
    float height;
} Size;

typedef enum
{
    SIZING_MODE_FIXED,
    SIZING_MODE_FIT,
    SIZING_MODE_FIT_GROW,
} SizingMode;

typedef struct
{
    float value;
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
    float child_gap;
    LayoutDirection direction;
} BoxConfig;

typedef struct UIBox UIBox;
struct UIBox
{
    Size remaining_space;
    Position position;
    float next_child_offset_x;
    float next_child_offset_y;
    BoxConfig config;
    UIBox* parent;
    UIBox* children[CHILDREN_SIZE];
    int children_count;
};

// Context -----------------------------

typedef struct
{
    uint16_t client_width;
    uint16_t client_height;
    void (*on_resize)(uint16_t, uint16_t);
    Queue(UICommand, COMMAND_QUEUE_SIZE) ui_command_queue;
} UIContext;

///

void ui_reset(UIContext* ui_context);

UIBox* ui_box_get_root();
UIBox* ui_box_start(BoxConfig* box_config);
void ui_box_end();

void ui_box_calculate_fit_size(UIBox* box);
void ui_box_grow_children(UIBox* box);
void ui_box_resolve_position(UIBox* box);

void ui_generate_render_commands(UIContext* ui_context, UIBox* box);

///

extern UICommand* g_ui_command_queue;
