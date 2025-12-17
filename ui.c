#include "ui.h"
#include "lib.h"

#include <string.h>

#define POOL_SIZE  256
#define STACK_SIZE 16

///

static Pool(UILayout, POOL_SIZE) ui_layout_pool = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

void ui_layout_resolve(UIContext* ui_context, UILayout* layout)
{
    if (layout->parent)
    {
        UILayout* parent = layout->parent;
        layout->position.x += parent->position.x + parent->style.padding.left;
        layout->position.y += parent->position.y + parent->style.padding.top;
        if (parent->style.direction == UI_LAYOUT_LEFT_TO_RIGHT)
        {
            layout->position.x += parent->next_child_offset_x;
            parent->next_child_offset_x += layout->style.size_style.size.width + parent->style.child_gap;
        }
        else
        {
            layout->position.y += parent->next_child_offset_y;
            parent->next_child_offset_y += layout->style.size_style.size.height + parent->style.child_gap;
        }
    }

    for (int i = 0; i < CHILDEN_SIZE; i++)
    {
        UILayout* child = layout->children[i];
        if (child == NULL)
            break;
        ui_layout_resolve(ui_context, child);
    }
}

void ui_layout_generate_render_commands(UIContext* ui_context, UILayout* root)
{
    UICommandRect* cmd = (UICommandRect*)(ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++);
    cmd->base.type = UI_COMMAND_RECT;
    cmd->base.size = sizeof(UICommandRect);
    cmd->rect = (Rect){ root->position.x, root->position.y, root->position.x + root->style.size_style.size.width,
                        root->position.y + root->style.size_style.size.height };
    cmd->color = root->style.color;
    cmd->style = root->style.rect_style;

    for (int i = 0; i < CHILDEN_SIZE; i++)
    {
        UILayout* child = root->children[i];
        if (child == NULL)
            break;
        ui_layout_generate_render_commands(ui_context, child);
    }
}

static UILayout* ui_layout_new()
{
    return &ui_layout_pool.items[ui_layout_pool.count++];
}

static UILayout* ui_layout_get_parent()
{
    return (ui_layout_stack.depth > 0) ? ui_layout_stack.items[ui_layout_stack.depth - 1] : NULL;
}

UILayout* ui_layout_start(UILayoutStyle* layout_style)
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
    memcpy(&layout->style, layout_style, sizeof(*layout_style));
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

void ui_reset(UIContext* ui_context)
{
    Assert(ui_layout_stack.depth == 0);
    memset(&ui_layout_pool, 0, sizeof(ui_layout_pool));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}
