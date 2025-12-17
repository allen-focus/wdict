#pragma once
#include "lib.h"

// TODO: Don't hard-code
#define CHILDEN_SIZE       16
#define COMMAND_QUEUE_SIZE 4096

// NOTE: Suppress compiler warning C4068: unknown pragma 'clang'
#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
static int ui_layout_macro_flag;
#pragma clang diagnostic pop
///

typedef enum
{
    UI_LAYOUT_LEFT_TO_RIGHT,
    UI_LAYOUT_TOP_TO_BOTTOM
} Direction;

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

typedef enum
{
    SIZE_STYLE_FIXED,
    SIZE_STYLE_FIT
} SizeStyleType;

typedef struct
{
    Size size;
    SizeStyleType type;
} SizeStyle;

typedef struct
{
    SizeStyle size_style;
    Color color;
    RectStyle rect_style;
    Padding padding;
    float child_gap;
    Direction direction;
} UILayoutStyle;

typedef struct UILayout UILayout;
struct UILayout
{
    Position position;
    float next_child_offset_x;
    float next_child_offset_y;
    UILayoutStyle style;
    UILayout* parent;
    UILayout* children[CHILDEN_SIZE];
};

typedef struct
{
    uint16_t client_width;
    uint16_t client_height;
    void (*on_resize)(uint16_t, uint16_t);
    Queue(UICommand, COMMAND_QUEUE_SIZE) ui_command_queue;
} UIContext;

///

void ui_reset(UIContext* ui_context);

UILayout* ui_layout_start(UILayoutStyle* layout_style);
void ui_layout_end();

void ui_layout_resolve(UIContext* ui_context, UILayout* layout);
void ui_layout_generate_render_commands(UIContext* ui_context, UILayout* root);

///

extern UICommand* g_ui_command_queue;
