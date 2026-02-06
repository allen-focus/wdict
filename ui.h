#pragma once
#include "lib.h"

// NOTE: Suppress compiler warning C4068: unknown pragma 'clang'
#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
static int ui_layout_macro_flag;
#pragma clang diagnostic pop

// clang-format off
#define ui_layout(...)                                                                                                 \
    for (ui_layout_macro_flag = (ui_layout_start(&(LayoutConfig)__VA_ARGS__), 0);                                      \
         ui_layout_macro_flag < 1;                                                                                     \
         ui_layout_macro_flag = 1, ui_layout_end())
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

// Layout ------------------------------

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
} LayoutConfig;

typedef struct UILayout UILayout;
struct UILayout
{
    Size remaining_space;
    Position position;
    float next_child_offset_x;
    float next_child_offset_y;
    LayoutConfig config;
    UILayout* parent;
    UILayout* children[CHILDREN_SIZE];
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

UILayout* ui_layout_get_root();
UILayout* ui_layout_start(LayoutConfig* layout_style);
void ui_layout_end();

void ui_layout_calculate_fit_size(UILayout* layout);
void ui_layout_grow_children(UILayout* layout);
void ui_layout_resolve_position(UILayout* layout);
void ui_layout_generate_render_commands(UIContext* ui_context, UILayout* root);

///

extern UICommand* g_ui_command_queue;
