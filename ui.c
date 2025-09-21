#include "ui.h"
#include "lib.h"
#include "renderer.h"

#define POOL_SIZE  256
#define STACK_SIZE 16

///

static Pool(UILayout, POOL_SIZE) ui_layout_pool = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

static void ui_calculate_absolute_position(Position* position, const UILayout* layout)
{
    if (layout->parent)
    {
        position->x += layout->parent->position.x;
        position->y += layout->parent->position.y;
        ui_calculate_absolute_position(position, layout->parent);
    }
}

void ui_layout_draw(const UILayout* layout)
{
    Position position = { layout->position.x, layout->position.y };
    ui_calculate_absolute_position(&position, layout);
    Rect rect = { position.x, position.y, position.x + layout->size.width, position.y + layout->size.height };
    renderer_draw_rect(rect, layout->color, layout->style);

    for (int i = 0; i < CHILDEN_SIZE; i++)
    {
        UILayout* child = layout->children[i];
        if (child)
        {
            ui_layout_draw(child);
        }
    }
}

static UILayout* ui_layout_new()
{
    return &ui_layout_pool.items[ui_layout_pool.count++];
}

UILayout* ui_layout_get_root()
{
    Assert(ui_layout_pool.count > 0);
    return &ui_layout_pool.items[0];
}

static UILayout* ui_layout_get_parent()
{
    return (ui_layout_stack.depth > 0) ? ui_layout_stack.items[ui_layout_stack.depth - 1] : NULL;
}

UILayout* ui_layout_start()
{
    Assert(ui_layout_stack.depth <= STACK_SIZE);
    Assert(ui_layout_pool.count <= POOL_SIZE);

    UILayout* layout = ui_layout_new();
    UILayout* parent = ui_layout_get_parent();
    if (parent)
    {
        layout->parent = parent;
        for (int i = 0; i < CHILDEN_SIZE; i++)
        {
            if (parent->children[i] == NULL)
            {
                parent->children[i] = layout;
                break;
            }
        }
    }
    ui_layout_stack.items[ui_layout_stack.depth++] = layout;
    return layout;
}

void ui_layout_end()
{
    ui_layout_stack.items[ui_layout_stack.depth--] = NULL;
}

void ui_layout_config(UILayout* layout, Position position, Size size, Color color, RectStyle style, Padding padding)
{
    memcpy(&layout->position, &position, sizeof(position));
    memcpy(&layout->size, &size, sizeof(size));
    memcpy(&layout->color, &color, sizeof(color));
    memcpy(&layout->style, &style, sizeof(style));
    memcpy(&layout->padding, &padding, sizeof(padding));
}

void ui_init()
{
    Assert(ui_layout_stack.depth == 0);
    memset(&ui_layout_pool, 0, sizeof(ui_layout_pool));
}
