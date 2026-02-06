#include "lib.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winscard.h>
#include <winuser.h>

#define EPSILON 1e-4f
#define QUEUE_SIZE 256
#define STACK_SIZE 16

///

static Queue(UILayout, QUEUE_SIZE) ui_layout_queue = { 0 };
static Stack(UILayout*, STACK_SIZE) ui_layout_stack = { 0 };

///

static bool ui_layout_has_fit_attribute(SizingAxis axis)
{
    return axis.mode == SIZING_MODE_FIT || axis.mode == SIZING_MODE_FIT_GROW;
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

    // When sizing mode has a 'fit' attribute on an axis, adjust the layout size by adding padding and child gap
    if (ui_layout_has_fit_attribute(layout->config.sizing.width))
        layout->config.sizing.width.value += layout->config.padding.left + layout->config.padding.right;
    if (ui_layout_has_fit_attribute(layout->config.sizing.height))
        layout->config.sizing.height.value += layout->config.padding.top + layout->config.padding.bottom;
    int child_gap_count = layout->children_count - 1;
    if (child_gap_count > 0)
        switch (layout->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                if (ui_layout_has_fit_attribute(layout->config.sizing.width))
                    layout->config.sizing.width.value += layout->config.child_gap * child_gap_count;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                if (ui_layout_has_fit_attribute(layout->config.sizing.height))
                    layout->config.sizing.height.value += layout->config.child_gap * child_gap_count;
                break;
        }

    // If the current layout is a child, adjust its parent's size (if has a 'fit' attribute) based on layout direction.
    // Note: Since recursion processes deepest children first, this layout’s size has already been fully calculated
    // (including any children it may have), so no further size propagation is needed at this level.
    UILayout* parent = layout->parent;
    if (parent)
        switch (parent->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                if (ui_layout_has_fit_attribute(parent->config.sizing.width))
                {
                    parent->config.sizing.height.value = max(layout->config.sizing.height.value, parent->config.sizing.height.value);
                    parent->config.sizing.width.value += layout->config.sizing.width.value;
                }
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                if (ui_layout_has_fit_attribute(parent->config.sizing.height))
                {
                    parent->config.sizing.width.value = max(layout->config.sizing.width.value, parent->config.sizing.width.value);
                    parent->config.sizing.height.value += layout->config.sizing.height.value;
                }
                break;
        }
}

static void grow_axis(float* remaining, float* growable_children[], int* growable_count)
{
    // Distribute remaining space proportionally among children that are configured to grow.
    // This loop continues as long as there is remaining space and there are children to distribute to.
    while (*remaining > EPSILON && *growable_count > 0)
    {
        // Find the smallest current size among the growable children.
        float smallest = *growable_children[0];
        float second_smallest = INFINITY;
        float to_add = 0;

        // First pass: find the smallest size and the next smallest size (to determine the increment).
        for (int i = 0; i < *growable_count; i++)
        {
            float child = *growable_children[i];
            if (child < smallest)
            {
                second_smallest = smallest;
                smallest = child;
            }
            else if (child > smallest)
            {
                second_smallest = min(child, second_smallest);
                // The amount to add to all children currently equal to 'smallest' before they reach 'second_smallest'.
                to_add = second_smallest - smallest;
            }
        }

        // If all growable children have the same size (second_smallest remains INFINITY),
        // distribute the remaining space equally among all growable children.
        if (second_smallest == INFINITY)
            to_add = *remaining / *growable_count;

        // Count how many children currently have the 'smallest' size.
        int smallest_count = 0;
        for (int i = 0; i < *growable_count; i++)
            if (fabsf(*growable_children[i] - smallest) < EPSILON)
                smallest_count++;

        // If distributing 'to_add' to all 'smallest_count' children would exceed the remaining space,
        // calculate the exact amount that can be distributed equally among them.
        if (to_add * smallest_count > *remaining)
            to_add = *remaining / smallest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'smallest', and update the remaining space.
        for (int i = 0; i < *growable_count; i++)
            if (fabsf(*growable_children[i] - smallest) < EPSILON)
            {
                *growable_children[i] += to_add;
                *remaining -= to_add;
            }
    }
}


// Recursively calculate sizes for layouts configured with 'grow' attribute
void ui_layout_grow_children(UILayout* layout)
{
    float* remaining_width = &layout->remaining_space.width;
    float* remaining_height = &layout->remaining_space.height;

    // Initialize remaining space
    // (which might be fixed or determined by a parent's 'grow' attribute in a previous pass)
    *remaining_width = layout->config.sizing.width.value;
    *remaining_height = layout->config.sizing.height.value;

    // Subtract padding
    *remaining_width -= layout->config.padding.left + layout->config.padding.right;
    *remaining_height -= layout->config.padding.top + layout->config.padding.bottom;

    // Subtract child gap
    int child_gap_count = layout->children_count - 1;
    if (child_gap_count > 0)
        switch (layout->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                *remaining_width -= layout->config.child_gap * child_gap_count;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                *remaining_height -= layout->config.child_gap * child_gap_count;
                break;
        }

    float* growable_children_widths[CHILDREN_SIZE] = { NULL };
    float* growable_children_heights[CHILDREN_SIZE] = { NULL };
    int growable_count_width = 0;
    int growable_count_height = 0;

    // Subtract the childrens' determined size from the parent's remaining space.
    for (int i = 0; i < layout->children_count; i++)
    {
        UILayout* child = layout->children[i];
        if (child->config.sizing.width.mode == SIZING_MODE_FIT_GROW)
            growable_children_widths[growable_count_width++] = &child->config.sizing.width.value;
        if (child->config.sizing.height.mode == SIZING_MODE_FIT_GROW)
            growable_children_heights[growable_count_height++] = &child->config.sizing.height.value;
        switch (layout->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                *remaining_width -= child->config.sizing.width.value;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                *remaining_height -= child->config.sizing.height.value;
                break;
        }
    }

    // Distribute remaining space to children configured with 'grow' attributes
    switch (layout->config.direction)
    {
        case LAYOUT_LEFT_TO_RIGHT:
            grow_axis(remaining_width, growable_children_widths, &growable_count_width);
            for (int i = 0; i < growable_count_height; i++)
                *growable_children_heights[i] = max(*growable_children_heights[i], *remaining_height);
            break;
        case LAYOUT_TOP_TO_BOTTOM:
            grow_axis(remaining_height, growable_children_heights, &growable_count_height);
            for (int i = 0; i < growable_count_width; i++)
                *growable_children_heights[i] = max(*growable_children_heights[i], *remaining_width);
            break;
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
            case LAYOUT_LEFT_TO_RIGHT:
                layout->position.x += parent->next_child_offset_x;
                parent->next_child_offset_x += layout->config.sizing.width.value + parent->config.child_gap;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                layout->position.y += parent->next_child_offset_y;
                parent->next_child_offset_y += layout->config.sizing.height.value + parent->config.child_gap;
                break;
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
    cmd->rect = (Rect){ root->position.x, root->position.y, root->position.x + root->config.sizing.width.value,
                        root->position.y + root->config.sizing.height.value };
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

UILayout* ui_layout_get_root()
{
    Assert(ui_layout_queue.count > 0);
    return &ui_layout_queue.items[0];
}

UILayout* ui_layout_start(LayoutConfig* layout_style)
{
    Assert(ui_layout_stack.depth <= STACK_SIZE);
    Assert(ui_layout_queue.count <= QUEUE_SIZE);

    UILayout* layout = ui_layout_new();
    UILayout* parent = ui_layout_get_parent();

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

void ui_reset(UIContext* ui_context)
{
    Assert(ui_layout_stack.depth == 0);
    memset(&ui_layout_queue, 0, sizeof(ui_layout_queue));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}
