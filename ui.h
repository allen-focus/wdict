#pragma once
#include "lib.h"

// TODO: Don't hard-code
#define CHILDEN_SIZE 16

// NOTE: Suppress `warning C4068: unknown pragma 'clang'`
#pragma warning(disable : 4068)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
static int ui_layout_macro_flag;
#pragma clang diagnostic pop

#define ui_layout(...)                                                                                                 \
    for (ui_layout_macro_flag = (ui_layout_config(ui_layout_start(), &(UILayoutStyle)__VA_ARGS__), 0);                 \
         ui_layout_macro_flag < 1; ui_layout_macro_flag = 1, ui_layout_end())

///

typedef struct
{
    Position position;
    Size size;
    Color color;
    RectStyle rect_style;
    Padding padding;
} UILayoutStyle;

typedef struct UILayout UILayout;
struct UILayout
{
    UILayoutStyle style;
    UILayout* parent;
    UILayout* children[CHILDEN_SIZE];
};

///

void ui_init();

UILayout* ui_layout_get_root();

UILayout* ui_layout_start();
void ui_layout_config(UILayout* layout, UILayoutStyle* style);
void ui_layout_end();

void ui_layout_draw(const UILayout* layout);
