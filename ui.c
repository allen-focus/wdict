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

// Distribute remaining space proportionally among children that are configured to grow.
static void grow_axis(float* remaining, float* growable[], const int growable_count)
{
    while (*remaining > EPSILON && growable_count > 0)
    {
        float smallest = *growable[0];
        float second_smallest = INFINITY;
        float to_add = 0;

        // Find the smallest size and the next smallest size (to determine the increment).
        for (int i = 0; i < growable_count; i++)
            if (*growable[i] < smallest)
            {
                second_smallest = smallest;
                smallest = *growable[i];
                to_add = second_smallest - smallest;
            }
            else if (*growable[i] > smallest)
            {
                second_smallest = min(*growable[i], second_smallest);
                to_add = second_smallest - smallest;
            }

        // If all growable children have the same size (second_smallest remains INFINITY),
        // distribute the remaining space equally among all growable children.
        if (second_smallest == INFINITY)
            to_add = *remaining / growable_count;

        // If distributing 'to_add' to all 'smallest_count' children would exceed the remaining space,
        // calculate the exact amount that can be distributed equally among them.
        int smallest_count = 0;
        for (int i = 0; i < growable_count; i++)
            if (fabsf(*growable[i] - smallest) < EPSILON)
                smallest_count++;
        if (to_add * smallest_count > *remaining)
            to_add = *remaining / smallest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'smallest', and update the remaining space.
        for (int i = 0; i < growable_count; i++)
            if (fabsf(*growable[i] - smallest) < EPSILON)
            {
                *growable[i] += to_add;
                *remaining -= to_add;
            }
    }
}

void remove_shrinkable(float* shrinkable[], float* shrinkable_mins[], int* shrinkable_count, int index)
{
    // Remove the shrinkable at the given index and update the count.
    shrinkable[index] = shrinkable[*shrinkable_count - 1];
    shrinkable_mins[index] = shrinkable_mins[*shrinkable_count - 1];
    shrinkable[*shrinkable_count - 1] = NULL;
    shrinkable_mins[*shrinkable_count - 1] = NULL;
    --(*shrinkable_count);
}

static void shrink_axis(float* remaining, float* shrinkable[], float* shrinkable_mins[], int* shrinkable_count)
{
    while (*remaining < -EPSILON && *shrinkable_count > 0)
    {
        float largest = *shrinkable[0];
        float second_largest = 0;
        float to_add = 0;

        // Find the largest size and the next largest size (to determine the decrement).
        for (int i = 0; i < *shrinkable_count; i++)
            if (*shrinkable[i] > largest)
            {
                second_largest = largest;
                largest = *shrinkable[i];
                to_add = second_largest - largest;
            }
            else if (*shrinkable[i] < largest)
            {
                second_largest = max(*shrinkable[i], second_largest);
                to_add = second_largest - largest;
            }

        // If all shrinkable children have the same size (second_largest remains 0),
        // distribute the remaining space equally among all shrinkable children.
        if (second_largest == 0)
            to_add = *remaining / *shrinkable_count;

        // If 'to_add' is less than the remaining space, calculate the exact amount
        // that can be distributed equally among them.
        int largest_count = 0;
        for (int i = 0; i < *shrinkable_count; i++)
            if (fabsf(*shrinkable[i] - largest) < EPSILON)
                largest_count++;
        if (to_add * largest_count < *remaining)
            to_add = *remaining / largest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'largest', and update the remaining space.
        for (int i = 0; i < *shrinkable_count; i++)
        {
            float width_backup = *shrinkable[i];
            float* child = shrinkable[i];
            if (fabsf(*child - largest) < EPSILON)
            {
                *child += to_add;
                if (*child <= *shrinkable_mins[i])
                {
                    *child = *shrinkable_mins[i];
                    remove_shrinkable(shrinkable, shrinkable_mins, shrinkable_count, i);
                }
                *remaining -= (*child - width_backup);
            }
        }
    }
}

// Recursively calculate sizes for boxes configured with 'grow' attribute
void ui_box_grow_shrink_children(UIBox* box)
{
    if (box->type != BOX_TYPE_CONTAINER)
        return;

    float* remaining_width = &box->data.container.remaining_space.width;
    float* remaining_height = &box->data.container.remaining_space.height;
    int children_count = box->data.container.child_count;

    float* growable_widths[CHILDREN_SIZE] = { 0 };
    float* growable_heights[CHILDREN_SIZE] = { 0 };
    int growable_width_count = 0;
    int growable_height_count = 0;

    float* shrinkable_widths[CHILDREN_SIZE] = { 0 };
    float* shrinkable_heights[CHILDREN_SIZE] = { 0 };
    float* shrinkable_width_mins[CHILDREN_SIZE] = { 0 };
    float* shrinkable_height_mins[CHILDREN_SIZE] = { 0 };
    int shrinkable_width_count = 0;
    int shrinkable_height_count = 0;

    // Initialize remaining space (which might be fixed or determined by a
    // parent's 'grow' attribute in a previous pass)
    *remaining_width = box->config.sizing.width.value;
    *remaining_height = box->config.sizing.height.value;

    // Subtract padding and child gap
    *remaining_width -= box->config.padding.left + box->config.padding.right;
    *remaining_height -= box->config.padding.top + box->config.padding.bottom;
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

    // Subtract the childrens' determined size from the parent's remaining space.
    for (int i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];

        if (child->config.sizing.width.mode == SIZING_MODE_FIT_GROW)
            growable_widths[growable_width_count++] = &child->config.sizing.width.value;
        if (child->config.sizing.height.mode == SIZING_MODE_FIT_GROW)
            growable_heights[growable_height_count++] = &child->config.sizing.height.value;

        if (child->config.sizing.width.value > child->min_size.width)
        {
            shrinkable_width_mins[shrinkable_width_count] = &child->min_size.width;
            shrinkable_widths[shrinkable_width_count++] = &child->config.sizing.width.value;
        }
        if (child->config.sizing.height.value > child->min_size.height)
        {
            shrinkable_height_mins[shrinkable_height_count] = &child->min_size.height;
            shrinkable_heights[shrinkable_height_count++] = &child->config.sizing.height.value;
        }

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
            // Main axis (width)
            if (*remaining_width > 0)
                grow_axis(remaining_width, growable_widths, growable_width_count);
            else if (*remaining_width < 0)
                shrink_axis(remaining_width, shrinkable_widths, shrinkable_width_mins, &shrinkable_width_count);

            // Cross axis (height)
            if (*remaining_height > 0)
                for (int i = 0; i < growable_height_count; i++)
                    *growable_heights[i] = max(*growable_heights[i], *remaining_height);
            else if (*remaining_height < 0)
                for (int i = 0; i < shrinkable_height_count; i++)
                    *shrinkable_heights[i] = max(*shrinkable_height_mins[i], *shrinkable_heights[i] + *remaining_height);
            break;
        case LAYOUT_TOP_TO_BOTTOM:
            // Main axis (height)
            if (*remaining_height > 0)
                grow_axis(remaining_height, growable_heights, growable_height_count);
            else if (*remaining_height < 0)
                shrink_axis(remaining_height, shrinkable_heights, shrinkable_height_mins, &shrinkable_height_count);

            // Cross axis (width)
            if (*remaining_width > 0)
                for (int i = 0; i < growable_width_count; i++)
                    *growable_widths[i] = max(*growable_widths[i], *remaining_width);
            else if (*remaining_width < 0)
                for (int i = 0; i < shrinkable_width_count; i++)
                    *shrinkable_widths[i] = max(*shrinkable_width_mins[i], *shrinkable_widths[i] + *remaining_width);
            break;
    }

    // Recursively resolve size (breadth first)
    for (int i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];
        ui_box_grow_shrink_children(child);
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

    for (int i = 0; i < box->data.container.child_count; i++)
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

    for (int i = 0; i < box->data.container.child_count; i++)
        ui_generate_render_commands(ui_context, box->children[i]);
}

static UIBox* ui_box_new()
{
    Assert(ui_box_queue.count < QUEUE_SIZE);
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
                parent->data.container.child_count++;
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
        int child_gap_count = box->data.container.child_count - 1;
        if (axis_has_fit_attribute(box->config.sizing.width))
        {
            box->config.sizing.width.value += box->config.padding.left + box->config.padding.right;
            box->min_size.width += box->config.padding.left + box->config.padding.right;
            if (box->config.direction == LAYOUT_LEFT_TO_RIGHT)
            {
                box->config.sizing.width.value += box->config.child_gap * child_gap_count;
                box->min_size.width += box->config.child_gap * child_gap_count;
            }
        }
        if (axis_has_fit_attribute(box->config.sizing.height))
        {
            box->config.sizing.height.value += box->config.padding.top + box->config.padding.bottom;
            box->min_size.height += box->config.padding.top + box->config.padding.bottom;
            if (box->config.direction == LAYOUT_TOP_TO_BOTTOM)
            {
                box->config.sizing.height.value += box->config.child_gap * child_gap_count;
                box->min_size.height += box->config.child_gap * child_gap_count;
            }
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

UIBox* ui_text(UIContext* ui_context, char* text, TextConfig* text_config)
{
    float text_width = (float)ui_context->get_text_width(text);
    float text_height = (float)ui_context->get_text_height(text);

    BoxConfig box_config = { .sizing = { .width = { text_width, SIZING_MODE_FIXED },
                                         .height = { text_height, SIZING_MODE_FIXED } } };
    UIBox* text_box = ui_box_start(&box_config);
    {
        text_box->type = BOX_TYPE_TEXT;
        text_box->data.text.content = _strdup(text);
        text_box->data.text.color = text_config->color;

        // Calculate ui_box->min_size.width by finding the width of the shortest word in the text.
        float min_width = 0;
        {
            char* text_copy = _strdup(text);
            char* token = strtok(text_copy, " ");
            while (token != NULL)
            {
                float word_width = (float)ui_context->get_text_width(token);
                if (word_width > min_width)
                    min_width = word_width;
                token = strtok(NULL, " ");
            }
            free(text_copy);
        }
        text_box->min_size.width = min_width;
        text_box->min_size.height = text_height;
    }
    ui_box_end(text_box);
    return text_box;
}

static void wrap_text(UIContext* ui_context, UIBox* text_box)
{
    int text_len = (int)strlen(text_box->data.text.content);
    bool wrapped = false;
    for (int i = text_len - 1; i >= 0; i--)
    {
        if (text_box->data.text.content[i] == ' ')
        {
            text_box->data.text.content[i] = '\0';
            uint32_t left_text_width = ui_context->get_text_width(text_box->data.text.content);
            if (left_text_width <= text_box->config.sizing.width.value)
            {
                text_box->config.sizing.width.value = (float)left_text_width;

                char* left_text = text_box->data.text.content;
                int left_text_len = (int)strlen(left_text);

                char* right_text = left_text + left_text_len + 1;
                for (int j = 0; j < text_len - left_text_len - 1; j++)
                    if (right_text[j] == '\0')
                        right_text[j] = ' ';

                UIBox* right_text_box = ui_box_new();
                memcpy(right_text_box, text_box, sizeof(*text_box));
                right_text_box->data.text.content = right_text;
                right_text_box->position.y += text_box->config.sizing.height.value;

                int* parent_child_count = &text_box->parent->data.container.child_count;
                Assert(*parent_child_count < CHILDREN_SIZE);
                text_box->parent->children[(*parent_child_count)++] = right_text_box;

                wrap_text(ui_context, right_text_box);
                wrapped = true;
                break;
            }
        }
    }

    // If no suitable wrapping point (space) was found, the current text segment is treated as a single unbroken unit.
    // It will not be broken further and may overflow the box's specified width.
    if (!wrapped && text_len != 0)
    {
        for (int i = 0; i < text_len; i++)
            if (text_box->data.text.content[i] == '\0')
                text_box->data.text.content[i] = ' ';
        for (int i = 0; i < text_len; i++)
            if (text_box->data.text.content[i] == ' ')
            {
                text_box->data.text.content[i] = '\0';

                char* right_text = text_box->data.text.content + i + 1;
                UIBox* right_text_box = ui_box_new();
                memcpy(right_text_box, text_box, sizeof(*text_box));
                right_text_box->data.text.content = right_text;
                right_text_box->position.y += text_box->config.sizing.height.value;

                int* parent_child_count = &text_box->parent->data.container.child_count;
                Assert(*parent_child_count < CHILDREN_SIZE);
                text_box->parent->children[(*parent_child_count)++] = right_text_box;

                wrap_text(ui_context, right_text_box);
                break;
            }
    }
}

void ui_box_wrap_text(UIContext* ui_context, UIBox* text_box_array[], int text_box_count)
{
    for (int i = 0; i < text_box_count; i++)
    {
        UIBox* text_box = text_box_array[i];
        if (ui_context->get_text_width(text_box->data.text.content) > text_box->config.sizing.width.value)
            wrap_text(ui_context, text_box_array[i]);
    }
}

void ui_box_free_text_content(UIBox* text_box_array[], int text_box_count)
{
    for (int i = 0; i < text_box_count; i++)
        free(text_box_array[i]->data.text.content);
}
