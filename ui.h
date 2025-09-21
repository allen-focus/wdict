#pragma once
#include "lib.h"

// TODO: Don't hard-code
#define CHILDEN_SIZE 16

///

typedef struct UILayout UILayout;
struct UILayout
{
    // Basic
    Position position;
    Size size;
    Color color;
    RectStyle style;
    // Visual
    Padding padding;
    // Tree
    UILayout* parent;
    UILayout* children[CHILDEN_SIZE];
};

typedef struct
{
    uint16_t client_width;
    uint16_t client_height;
    void (*on_resize)(uint16_t, uint16_t);
} UIContext;

///

void ui_init();

UILayout* ui_layout_get_root();

UILayout* ui_layout_start();
void ui_layout_config(UILayout* layout, Position position, Size size, Color color, RectStyle style, Padding padding);
void ui_layout_end();

void ui_layout_draw(const UILayout* layout);
