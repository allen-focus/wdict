#include "lib.h"
#include "renderer.h"
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

static Queue(UIBox, QUEUE_SIZE) ui_box_queue = { 0 };
static Stack(UIBox*, STACK_SIZE) ui_box_stack = { 0 };

///

static bool ui_box_has_fit_attribute(SizingAxis axis)
{
    return axis.mode == SIZING_MODE_FIT || axis.mode == SIZING_MODE_FIT_GROW;
}

// Recursively calculate sizes for boxes configured with 'fit' attribute (reverse pass)
void ui_box_calculate_fit_size(UIBox* box)
{
    // Recursively resolve child box sizes (depth-first, reverse order)
    for (int i = 0; i < box->data.container.children_count; i++)
    {
        UIBox* child = box->children[i];
        ui_box_calculate_fit_size(child);
    }

    // When sizing mode has a 'fit' attribute on an axis, adjust the box size by adding padding and child gap
    if (ui_box_has_fit_attribute(box->config.sizing.width))
        box->config.sizing.width.value += box->config.padding.left + box->config.padding.right;
    if (ui_box_has_fit_attribute(box->config.sizing.height))
        box->config.sizing.height.value += box->config.padding.top + box->config.padding.bottom;
    int child_gap_count = box->data.container.children_count - 1;
    if (child_gap_count > 0)
        switch (box->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                if (ui_box_has_fit_attribute(box->config.sizing.width))
                    box->config.sizing.width.value += box->config.child_gap * child_gap_count;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                if (ui_box_has_fit_attribute(box->config.sizing.height))
                    box->config.sizing.height.value += box->config.child_gap * child_gap_count;
                break;
        }

    // If the current box is a child, adjust its parent's size (if has a 'fit' attribute) based on box direction.
    // Note: Since recursion processes deepest children first, this box’s size has already been fully calculated
    // (including any children it may have), so no further size propagation is needed at this level.
    UIBox* parent = box->parent;
    if (parent)
        switch (parent->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                if (ui_box_has_fit_attribute(parent->config.sizing.width))
                    parent->config.sizing.width.value += box->config.sizing.width.value;
                if (ui_box_has_fit_attribute(parent->config.sizing.height))
                    parent->config.sizing.height.value = max(box->config.sizing.height.value, parent->config.sizing.height.value);
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                if (ui_box_has_fit_attribute(parent->config.sizing.height))
                    parent->config.sizing.height.value += box->config.sizing.height.value;
                if (ui_box_has_fit_attribute(parent->config.sizing.width))
                    parent->config.sizing.width.value = max(box->config.sizing.width.value, parent->config.sizing.width.value);
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

        // The amount to add to all children currently equal to 'smallest' before they reach 'second_smallest'.
        float to_add = 0;

        // First pass: find the smallest size and the next smallest size (to determine the increment).
        for (int i = 0; i < *growable_count; i++)
        {
            float child = *growable_children[i];
            if (child < smallest)
            {
                second_smallest = smallest;
                smallest = child;
                to_add = second_smallest - smallest;
            }
            else if (child > smallest)
            {
                second_smallest = min(child, second_smallest);
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


// Recursively calculate sizes for boxes configured with 'grow' attribute
void ui_box_grow_children(UIBox* box)
{
    if (box->type != BOX_TYPE_CONTAINER)
        return;

    float* remaining_width = &box->data.container.remaining_space.width;
    float* remaining_height = &box->data.container.remaining_space.height;
    int children_count = box->data.container.children_count;

    // Initialize remaining space
    // (which might be fixed or determined by a parent's 'grow' attribute in a previous pass)
    *remaining_width = box->config.sizing.width.value;
    *remaining_height = box->config.sizing.height.value;

    // Subtract padding
    *remaining_width -= box->config.padding.left + box->config.padding.right;
    *remaining_height -= box->config.padding.top + box->config.padding.bottom;

    // Subtract child gap
    int child_gap_count = children_count - 1;
    if (child_gap_count > 0)
        switch (box->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                *remaining_width -= box->config.child_gap * child_gap_count;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                *remaining_height -= box->config.child_gap * child_gap_count;
                break;
        }

    float* growable_children_widths[CHILDREN_SIZE] = { NULL };
    float* growable_children_heights[CHILDREN_SIZE] = { NULL };
    int growable_count_width = 0;
    int growable_count_height = 0;

    // Subtract the childrens' determined size from the parent's remaining space.
    for (int i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];
        if (child->config.sizing.width.mode == SIZING_MODE_FIT_GROW)
            growable_children_widths[growable_count_width++] = &child->config.sizing.width.value;
        if (child->config.sizing.height.mode == SIZING_MODE_FIT_GROW)
            growable_children_heights[growable_count_height++] = &child->config.sizing.height.value;
        switch (box->config.direction)
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
    switch (box->config.direction)
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
    for (int i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];
        ui_box_grow_children(child);
    }
}

void ui_box_resolve_position(UIBox* box)
{
    if (box->parent)
    {
        UIBox* parent = box->parent;
        ContainerData* parent_data = &parent->data.container;
        box->position.x += parent->position.x + parent->config.padding.left;
        box->position.y += parent->position.y + parent->config.padding.top;
        switch (parent->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                box->position.x += parent_data->next_child_offset_x;
                parent_data->next_child_offset_x += box->config.sizing.width.value + parent->config.child_gap;
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                box->position.y += parent_data->next_child_offset_y;
                parent_data->next_child_offset_y += box->config.sizing.height.value + parent->config.child_gap;
                break;
        }
    }

    for (int i = 0; i < box->data.container.children_count; i++)
        ui_box_resolve_position(box->children[i]);
}

// TODO: Currently we assume all render commands are Rect. Need to handle text etc in the future.
void ui_generate_render_commands(UIContext* ui_context, UIBox* box)
{
    UICommand* cmd = ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++;
    switch (box->type)
    {
        case BOX_TYPE_CONTAINER:
            cmd->base.type = UI_COMMAND_RECT;
            cmd->base.size = sizeof(UICommandRect);
            cmd->rect.rect = (Rect){ box->position.x, box->position.y, box->position.x + box->config.sizing.width.value,
                                box->position.y + box->config.sizing.height.value };
            cmd->rect.color = box->config.color;
            cmd->rect.style = box->config.rect_style;
            break;
        case BOX_TYPE_TEXT:
            cmd->base.type = UI_COMMAND_TEXT;
            cmd->base.size = sizeof(UICommandText);
            cmd->text.content = box->data.text.content;
            cmd->text.color = box->data.text.color;
            cmd->text.position = box->position;
            break;
    }

    for (int i = 0; i < box->data.container.children_count; i++)
        ui_generate_render_commands(ui_context, box->children[i]);
}

static UIBox* ui_box_new()
{
    return &ui_box_queue.items[ui_box_queue.count++];
}

static UIBox* ui_box_get_parent()
{
    return (ui_box_stack.depth > 0) ? ui_box_stack.items[ui_box_stack.depth - 1] : NULL;
}

UIBox* ui_box_get_root()
{
    Assert(ui_box_queue.count > 0);
    return &ui_box_queue.items[0];
}

UIBox* ui_box_start(BoxConfig* config)
{
    Assert(ui_box_stack.depth <= STACK_SIZE);
    Assert(ui_box_queue.count <= QUEUE_SIZE);

    UIBox* box = ui_box_new();
    UIBox* parent = ui_box_get_parent();

    // Set the parent of the current box and add the current box as a child of its parent
    if (parent)
    {
        box->parent = parent;
        for (int i = 0; i < CHILDREN_SIZE; i++)
        {
            if (parent->children[i] == NULL)
            {
                parent->children[i] = box;
                parent->data.container.children_count++;
                break;
            }
        }
    }

    ui_box_stack.items[ui_box_stack.depth++] = box;
    memcpy(&box->config, config, sizeof(*config));
    return box;
}

void ui_box_end()
{
    ui_box_stack.items[ui_box_stack.depth--] = NULL;
}

void ui_reset(UIContext* ui_context)
{
    Assert(ui_box_stack.depth == 0);
    memset(&ui_box_queue, 0, sizeof(ui_box_queue));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}

void ui_text(const char* text, TextConfig* text_config)
{
    BoxConfig box_config = {
        .sizing = {
            .width = { (float)renderer_get_text_width(text), SIZING_MODE_FIXED },
            .height = { (float)renderer_get_text_height(text), SIZING_MODE_FIXED }
        }
    };
    UIBox* ui_box = ui_box_start(&box_config);
    ui_box->type = BOX_TYPE_TEXT;
    ui_box->data.text.content = text;
    ui_box->data.text.color = text_config->color;
    ui_box_end();
}
