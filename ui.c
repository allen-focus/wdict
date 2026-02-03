#include "ui.h"
#include "lib.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winuser.h>

#define QUEUE_SIZE  256
#define STACK_SIZE 16

///

static Queue(UILayout, QUEUE_SIZE) ui_layout_queue = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

static bool ui_layout_with_fit_attribute(UILayout* layout)
{
    SizingMode mode = layout->config.sizing.mode;
    return (mode == SIZING_MODE_FIT || mode == SIZING_MODE_FIT_GROW_WIDTH || mode == SIZING_MODE_FIT_GROW_HEIGHT ||
            mode == SIZING_MODE_FIT_GROW_BOTH);
}

static bool ui_layout_with_grow_attribute(UILayout* layout)
{
    SizingMode mode = layout->config.sizing.mode;
    return (mode == SIZING_MODE_FIT_GROW_WIDTH || mode == SIZING_MODE_FIT_GROW_HEIGHT || mode == SIZING_MODE_FIT_GROW_BOTH);
}

// Recursively calculate sizes for layouts configured with 'fit' attribute (reverse pass)
void ui_layout_calculate_fit_size(UILayout* layout)
{
    // Recursively resolve child layout sizes (depth-first, reverse order)
    for (int i = 0; i < layout->children_count; i++)
    {
        UILayout* child = layout->children[i];
        ui_layout_calculate_fit_size(child);
    }

    // When sizing mode has a 'fit' attribute, adjust the layout size by adding padding and child gap
    if (ui_layout_with_fit_attribute(layout))
    {
        // padding
        layout->config.sizing.value.width += layout->config.padding.left + layout->config.padding.right;
        layout->config.sizing.value.height += layout->config.padding.top + layout->config.padding.bottom;

        // child gap
        int child_gap_count = layout->children_count - 1;
        if (child_gap_count > 0)
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
        if (ui_layout_with_fit_attribute(parent))
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

// Recursively calculate sizes for layouts configured with 'grow' attribute
void ui_layout_grow_children(UILayout* layout)
{
    // Initialize remaining space with the layout's calculated size
    // (which might be fixed or determined by a parent's 'grow' attribute in a previous pass)
    layout->remaining_space.width = layout->config.sizing.value.width;
    layout->remaining_space.height = layout->config.sizing.value.height;

    // Subtract padding from the available space
    layout->remaining_space.width -= layout->config.padding.left + layout->config.padding.right;
    layout->remaining_space.height -= layout->config.padding.top + layout->config.padding.bottom;

    // Subtract child gap if there are multiple children
    int child_gap_count = layout->children_count - 1;
    if (child_gap_count > 0)
    {
        if (layout->config.direction == UI_LAYOUT_LEFT_TO_RIGHT)
            layout->remaining_space.width -= layout->config.child_gap * child_gap_count;
        if (layout->config.direction == UI_LAYOUT_TOP_TO_BOTTOM)
            layout->remaining_space.height -= layout->config.child_gap * child_gap_count;
    }

    // Subtract the childrens' determined size from the parent's remaining space.
    for (int i = 0; i < layout->children_count; i++)
        switch (layout->config.direction)
        {
            case UI_LAYOUT_LEFT_TO_RIGHT:
                layout->remaining_space.width -= layout->children[i]->config.sizing.value.width;
                break;
            case UI_LAYOUT_TOP_TO_BOTTOM:
                layout->remaining_space.height -= layout->children[i]->config.sizing.value.height;
                break;
            default:
                Assert(0);
        }

    // Distribute remaining space to children configured with 'grow' attributes
    for (int i = 0; i < layout->children_count; i++)
    {
        UILayout* child = layout->children[i];
        SizingMode child_sizing_mode = child->config.sizing.mode;
        if (ui_layout_with_grow_attribute(child))
        {
            if (child_sizing_mode == SIZING_MODE_FIT_GROW_WIDTH || child_sizing_mode == SIZING_MODE_FIT_GROW_BOTH)
            {
                if (layout->config.direction == UI_LAYOUT_LEFT_TO_RIGHT)
                {
                    if (layout->remaining_space.width > 0)
                    {
                        child->config.sizing.value.width += layout->remaining_space.width;
                        child->remaining_space.width = layout->remaining_space.width;
                    }
                }
                else
                {
                    float diff = layout->remaining_space.width - child->config.sizing.value.width;
                    child->config.sizing.value.width += (diff > 0) ? diff : 0;
                    child->remaining_space.width = (diff > 0) ? diff : 0;
                }
                layout->remaining_space.width = 0;
            }
            if (child_sizing_mode == SIZING_MODE_FIT_GROW_HEIGHT || child_sizing_mode == SIZING_MODE_FIT_GROW_BOTH)
            {
                if (layout->config.direction == UI_LAYOUT_TOP_TO_BOTTOM)
                {
                    if (layout->remaining_space.height > 0)
                    {
                        child->config.sizing.value.height += layout->remaining_space.height;
                        child->remaining_space.height = layout->remaining_space.height;
                    }
                }
                else
                {
                    float diff = layout->remaining_space.height - child->config.sizing.value.height;
                    child->config.sizing.value.height += (diff > 0) ? diff : 0;
                    child->remaining_space.height = (diff > 0) ? diff : 0;
                }
                layout->remaining_space.height = 0;
            }
        }
    }

    // Recursively resolve size (breadth first)
    for (int i = 0; i < layout->children_count; i++)
    {
        UILayout* child = layout->children[i];
        ui_layout_grow_children(child);
    }
}

void ui_layout_resolve_position(UILayout* layout)
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

    for (int i = 0; i < layout->children_count; i++)
        ui_layout_resolve_position(layout->children[i]);
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

    for (int i = 0; i < root->children_count; i++)
        ui_layout_generate_render_commands(ui_context, root->children[i]);
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

    UILayout* layout = ui_layout_new();
    UILayout* parent = ui_layout_get_parent();

    // Validate sizing configuration
    SizingMode sizing_mode = layout_style->sizing.mode;
    float w = layout_style->sizing.value.width;
    float h = layout_style->sizing.value.height;
    Assert(sizing_mode != SIZING_MODE_FIXED || (w >= 0 && h >= 0));
    Assert(sizing_mode != SIZING_MODE_FIT || (w == 0 && h == 0));
    Assert(sizing_mode != SIZING_MODE_FIT_GROW_WIDTH || (w == 0 && h >= 0));
    Assert(sizing_mode != SIZING_MODE_FIT_GROW_HEIGHT || (h == 0 && w >= 0));
    Assert(sizing_mode != SIZING_MODE_FIT_GROW_BOTH || (w == 0 && h == 0));

    // Set the parent of the current layout and add the current layout as a child of its parent
    if (parent)
    {
        layout->parent = parent;
        for (int i = 0; i < CHILDREN_SIZE; i++)
        {
            if (parent->children[i] == NULL)
            {
                parent->children[i] = layout;
                parent->children_count++;
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
