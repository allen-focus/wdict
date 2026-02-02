#pragma once
#include "lib.h"

// TODO: Don't hard-code
#define CHILDREN_SIZE       16
#define COMMAND_QUEUE_SIZE 4096

// NOTE: Suppress compiler warning C4068: unknown pragma 'clang'
#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
static int ui_layout_macro_flag;
#pragma clang diagnostic pop

// =========================================================
// Enum & Struct
// =========================================================

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

typedef enum
{
    FIT = 0,
    GROW = -1
} SizeSpecialValue;

typedef enum
{
    SIZING_MODE_FIXED,
    SIZING_MODE_FIT,
    SIZING_MODE_GROW
} SizingMode;

typedef struct
{
    float width, height;
} Size;

typedef struct
{
    Size value;
    SizingMode mode;
} Sizing;

// Layout ------------------------------

typedef enum
{
    UI_LAYOUT_LEFT_TO_RIGHT,
    UI_LAYOUT_TOP_TO_BOTTOM
} Direction;

typedef struct
{
    Sizing sizing;
    Color color;
    RectStyle rect_style;
    Padding padding;
    float child_gap;
    Direction direction;
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

// =========================================================
// Function
// =========================================================

void ui_reset(UIContext* ui_context);

UILayout* ui_layout_start(LayoutConfig* layout_style);
void ui_layout_end();

void ui_layout_resolve_size_reverse(UILayout* layout);
void ui_layout_resolve_size(UILayout* layout);
void ui_layout_resolve_position(UILayout* layout);
void ui_layout_generate_render_commands(UIContext* ui_context, UILayout* root);

///

extern UICommand* g_ui_command_queue;
