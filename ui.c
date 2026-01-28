#include "ui.h"
#include "lib.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define QUEUE_SIZE  256
#define STACK_SIZE 16

///

static Queue(UILayout, QUEUE_SIZE) ui_layout_queue = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

void ui_layout_resolve_size(UIContext* ui_context, UILayout* layout)
{
    // Recursively resolve layout size (process deepest children first)
    int child_count = 0;
    for (int i = 0; i < CHILDEN_SIZE; i++)
    {
        UILayout* child = layout->children[child_count];
        if (child)
        {
            child_count++;
            ui_layout_resolve_size(ui_context, child);
        }
    }

    // When sizing mode is set to 'fit', adjust the layout size by adding padding and child gap
    if (layout->config.sizing.mode == SIZE_STYLE_FIT)
    {
        // padding
        layout->config.sizing.value.width += layout->config.padding.left + layout->config.padding.right;
        layout->config.sizing.value.height += layout->config.padding.top + layout->config.padding.bottom;

        // child gap
        int child_gap_count = child_count - 1;
        switch (layout->config.direction)
        {
            case UI_LAYOUT_LEFT_TO_RIGHT:
                layout->config.sizing.value.width += layout->config.child_gap * child_gap_count;
                break;
            case UI_LAYOUT_TOP_TO_BOTTOM:
                layout->config.sizing.value.height += layout->config.child_gap * child_gap_count;
                break;
            default:
                Assert(0);
        }
    }

    // If the current layout is a child, adjust its parent's size based on layout direction.
    // Note: Since recursion processes deepest children first, this layout’s size has already been fully calculated
    // (including any children it may have), so no further size propagation is needed at this level.
    UILayout* parent = layout->parent;
    if (parent)
        if (parent->config.sizing.mode == SIZE_STYLE_FIT)
            switch (parent->config.direction)
            {
                case UI_LAYOUT_LEFT_TO_RIGHT:
                    parent->config.sizing.value.height = max(layout->config.sizing.value.height, parent->config.sizing.value.height);
                    parent->config.sizing.value.width += layout->config.sizing.value.width;
                    break;
                case UI_LAYOUT_TOP_TO_BOTTOM:
                    parent->config.sizing.value.width = max(layout->config.sizing.value.width, parent->config.sizing.value.width);
                    parent->config.sizing.value.height += layout->config.sizing.value.height;
                    break;
                default:
                    Assert(0);
            }
}

void ui_layout_resolve_position(UIContext* ui_context, UILayout* layout)
{
    if (layout->parent)
    {
        UILayout* parent = layout->parent;
        layout->position.x += parent->position.x + parent->config.padding.left;
        layout->position.y += parent->position.y + parent->config.padding.top;
        switch (parent->config.direction)
        {
            case UI_LAYOUT_LEFT_TO_RIGHT:
                layout->position.x += parent->next_child_offset_x;
                parent->next_child_offset_x += layout->config.sizing.value.width + parent->config.child_gap;
                break;
            case UI_LAYOUT_TOP_TO_BOTTOM:
                layout->position.y += parent->next_child_offset_y;
                parent->next_child_offset_y += layout->config.sizing.value.height + parent->config.child_gap;
                break;
            default:
                Assert(0);
        }
    }

    for (int i = 0; i < CHILDEN_SIZE; i++)
    {
        UILayout* child = layout->children[i];
        if (child == NULL)
            break;
        ui_layout_resolve_position(ui_context, child);
    }
}

// TODO: Currently we assume all render commands are Rect. Need to handle text etc in the future.
void ui_layout_generate_render_commands(UIContext* ui_context, UILayout* root)
{
    UICommandRect* cmd = (UICommandRect*)(ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++);
    cmd->base.type = UI_COMMAND_RECT;
    cmd->base.size = sizeof(UICommandRect);
    cmd->rect = (Rect){ root->position.x, root->position.y, root->position.x + root->config.sizing.value.width,
                        root->position.y + root->config.sizing.value.height };
    cmd->color = root->config.color;
    cmd->style = root->config.rect_style;

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
    return &ui_layout_queue.items[ui_layout_queue.count++];
}

static UILayout* ui_layout_get_parent()
{
    return (ui_layout_stack.depth > 0) ? ui_layout_stack.items[ui_layout_stack.depth - 1] : NULL;
}

UILayout* ui_layout_start(LayoutConfig* layout_style)
{
    Assert(ui_layout_stack.depth <= STACK_SIZE);
    Assert(ui_layout_queue.count <= QUEUE_SIZE);

    Assert(layout_style->sizing.mode != SIZE_STYLE_FIT ||
           (layout_style->sizing.value.width == 0 && layout_style->sizing.value.height == 0));

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
    memcpy(&layout->config, layout_style, sizeof(*layout_style));
    return layout;
}

void ui_layout_end()
{
    ui_layout_stack.items[ui_layout_stack.depth--] = NULL;
}

void ui_layout_config(UILayout* layout, LayoutConfig* style)
{
    memcpy(&layout->config, style, sizeof(*style));
}

void ui_reset(UIContext* ui_context)
{
    Assert(ui_layout_stack.depth == 0);
    memset(&ui_layout_queue, 0, sizeof(ui_layout_queue));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}
