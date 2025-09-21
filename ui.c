#include "ui.h"
#include "lib.h"
#include "renderer.h"

#define POOL_SIZE  256
#define STACK_SIZE 16

///

static Pool(UILayout, POOL_SIZE) ui_layout_pool = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

void ui_layout_draw(UILayout* layout)
{
    if (layout->parent)
    {
        UILayout* parent = layout->parent;
        layout->position.x += parent->position.x + parent->style.padding.left;
        layout->position.y += parent->position.y + parent->style.padding.top;
        if (parent->style.direction == UI_LAYOUT_LEFT_TO_RIGHT)
        {
            layout->position.x += parent->next_child_offset_x;
            parent->next_child_offset_x += layout->style.size.width;
        }
        else
        {
            layout->position.y += parent->next_child_offset_y;
            parent->next_child_offset_y += layout->style.size.height;
        }
    }

    Rect rect = { layout->position.x, layout->position.y, layout->position.x + layout->style.size.width,
                  layout->position.y + layout->style.size.height };
    renderer_draw_rect(rect, layout->style.color, layout->style.rect_style);

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

void ui_layout_config(UILayout* layout, UILayoutStyle* style)
{
    memcpy(&layout->style, style, sizeof(*style));
}

void ui_init()
{
    Assert(ui_layout_stack.depth == 0);
    memset(&ui_layout_pool, 0, sizeof(ui_layout_pool));
}
