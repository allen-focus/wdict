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

static Queue(UIBox, QUEUE_SIZE) ui_box_queue = { 0 };
static Stack(UIBox*, STACK_SIZE) ui_box_stack = { 0 };

///

static bool axis_has_fit_attribute(SizingAxis axis)
{
    return axis.mode == SIZING_MODE_FIT || axis.mode == SIZING_MODE_FIT_GROW;
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
                *growable_children_widths[i] = max(*growable_children_widths[i], *remaining_width);
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

void ui_box_end(UIBox* box)
{
    if (box->type == BOX_TYPE_CONTAINER)
    {
        // 'FIXED' sizing mode axis
        if (box->config.sizing.width.mode == SIZING_MODE_FIXED)
            box->min_size.width = box->config.sizing.width.value;
        if (box->config.sizing.height.mode == SIZING_MODE_FIXED)
            box->min_size.height = box->config.sizing.height.value;

        // 'FIT/FIT_GROW' sizing mode axis
        int child_gap_count = box->data.container.children_count - 1;
        if (axis_has_fit_attribute(box->config.sizing.width))
        {
            box->config.sizing.width.value += box->config.padding.left + box->config.padding.right;
            if (box->config.direction == LAYOUT_LEFT_TO_RIGHT)
                box->config.sizing.width.value += box->config.child_gap * child_gap_count;
        }
        if (axis_has_fit_attribute(box->config.sizing.height))
        {
            box->config.sizing.height.value += box->config.padding.top + box->config.padding.bottom;
            if (box->config.direction == LAYOUT_TOP_TO_BOTTOM)
                box->config.sizing.height.value += box->config.child_gap * child_gap_count;
        }
    }

    // If the current box is a child, adjust its parent's size based on box direction.
    UIBox* parent = box->parent;
    if (parent)
    {
        float* parent_width = &parent->config.sizing.width.value;
        float* parent_height = &parent->config.sizing.height.value;
        switch (parent->config.direction)
        {
            case LAYOUT_LEFT_TO_RIGHT:
                if (axis_has_fit_attribute(parent->config.sizing.width))
                {
                    *parent_width += box->config.sizing.width.value;
                    parent->min_size.width += box->min_size.width;
                }
                if (axis_has_fit_attribute(parent->config.sizing.height))
                {
                    *parent_height = max(box->config.sizing.height.value, *parent_height);
                    parent->min_size.height = max(box->min_size.height, parent->min_size.height);
                }
                break;
            case LAYOUT_TOP_TO_BOTTOM:
                if (axis_has_fit_attribute(parent->config.sizing.height))
                {
                    *parent_height += box->config.sizing.height.value;
                    parent->min_size.height += box->min_size.height;
                }
                if (axis_has_fit_attribute(parent->config.sizing.width))
                {
                    *parent_width = max(box->config.sizing.width.value, *parent_width);
                    parent->min_size.width = max(box->min_size.width, parent->min_size.width);
                }
                break;
        }
    }

    ui_box_stack.items[ui_box_stack.depth--] = NULL;
}

void ui_reset(UIContext* ui_context)
{
    Assert(ui_box_stack.depth == 0);
    memset(&ui_box_queue, 0, sizeof(ui_box_queue));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}

void ui_text(UIContext* ui_context, const char* text, TextConfig* text_config)
{
    float text_width = (float)ui_context->get_text_width(text);
    float text_height = (float)ui_context->get_text_height(text);

    BoxConfig box_config = { .sizing = { .width = { text_width, SIZING_MODE_FIXED },
                                         .height = { text_height, SIZING_MODE_FIXED } } };
    UIBox* box = ui_box_start(&box_config);
    {
        box->type = BOX_TYPE_TEXT;
        box->data.text.content = text;
        box->data.text.color = text_config->color;

        // Calculate ui_box->min_size.width by finding the width of the shortest word in the text.
        float min_width = INFINITY;
        {
            char* text_copy = strdup(text);
            char* token = strtok(text_copy, " ");
            while (token != NULL)
            {
                float word_width = (float)ui_context->get_text_width(token);
                if (word_width < min_width)
                    min_width = word_width;
                token = strtok(NULL, " ");
            }
            free(text_copy);
        }
        box->min_size.width = min_width;
    }
    ui_box_end(box);
}
