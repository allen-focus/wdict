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

// Axis-specific helper structure to eliminate code duplication
typedef struct {
    float* size;
    float* min_size;
    float* remaining;
    float padding_start;
    float padding_end;
    SizingAxis* sizing_axis;
    LayoutDirection main_direction;
} AxisContext;

static AxisContext get_axis_context(UIBox* box, Axis axis)
{
    AxisContext ctx = { 0 };
    if (axis == WIDTH)
    {
        ctx.size = &box->config.sizing.width.value;
        ctx.min_size = &box->min_size.width;
        ctx.remaining = &box->data.container.remaining_space.width;
        ctx.padding_start = box->config.padding.left;
        ctx.padding_end = box->config.padding.right;
        ctx.sizing_axis = &box->config.sizing.width;
        ctx.main_direction = LAYOUT_LEFT_TO_RIGHT;
    }
    else
    {
        ctx.size = &box->config.sizing.height.value;
        ctx.min_size = &box->min_size.height;
        ctx.remaining = &box->data.container.remaining_space.height;
        ctx.padding_start = box->config.padding.top;
        ctx.padding_end = box->config.padding.bottom;
        ctx.sizing_axis = &box->config.sizing.height;
        ctx.main_direction = LAYOUT_TOP_TO_BOTTOM;
    }
    return ctx;
}

///

static bool axis_has_fit_attribute(SizingAxis axis)
{
    return axis.mode == SIZING_MODE_FIT || axis.mode == SIZING_MODE_FIT_GROW;
}

// Recursively calculate sizes for boxes configured with 'fit' attribute (reverse pass)
void ui_box_calculate_fit_axis(UIBox* box, Axis axis)
{
    // Recursively resolve child box sizes (depth-first, reverse order)
    if (box->type == BOX_TYPE_CONTAINER)
        for (int i = 0; i < box->data.container.child_count; i++)
                ui_box_calculate_fit_axis(box->children[i], axis);

    // If the current box is a child, adjust its parent's size based on box direction.
    UIBox* parent = box->parent;
    if (parent)
    {
        AxisContext parent_ctx = get_axis_context(parent, axis);
        AxisContext box_ctx = get_axis_context(box, axis);

        if (axis_has_fit_attribute(*parent_ctx.sizing_axis))
        {
            if (parent->config.direction == parent_ctx.main_direction)
            {
                // Main axis: accumulate children sizes
                *parent_ctx.size += *box_ctx.size;
                *parent_ctx.min_size += *box_ctx.min_size;
            }
            else
            {
                // Cross axis: take maximum of children sizes
                float padding = parent_ctx.padding_start + parent_ctx.padding_end;
                *parent_ctx.size = max(*box_ctx.size + padding, *parent_ctx.size);
                *parent_ctx.min_size = max(*box_ctx.min_size + padding, *parent_ctx.min_size);
            }
        }
    }
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
void ui_box_grow_shrink_children_axis(UIBox* box, Axis axis)
{
    if (box->type != BOX_TYPE_CONTAINER)
        return;

    int children_count = box->data.container.child_count;
    AxisContext ctx = get_axis_context(box, axis);

    float* growable[CHILDREN_SIZE] = { 0 };
    int growable_count = 0;

    float* shrinkable[CHILDREN_SIZE] = { 0 };
    float* shrinkable_mins[CHILDREN_SIZE] = { 0 };
    int shrinkable_count = 0;

    // Initialize remaining space (which might be fixed or determined by a
    // parent's 'grow' attribute in a previous pass)
    *ctx.remaining = *ctx.size;

    // Subtract padding and child gap
    *ctx.remaining -= ctx.padding_start + ctx.padding_end;
    int child_gap_count = children_count - 1;
    if (box->config.direction == ctx.main_direction)
        *ctx.remaining -= box->config.child_gap * child_gap_count;

    // Subtract the childrens' determined size from the parent's remaining space.
    for (int i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];
        AxisContext child_ctx = get_axis_context(child, axis);

        if (child_ctx.sizing_axis->mode == SIZING_MODE_FIT_GROW)
            growable[growable_count++] = child_ctx.size;

        if (*child_ctx.size > *child_ctx.min_size)
        {
            if (axis == WIDTH && child->type == BOX_TYPE_TEXT)
                child->data.text.needs_wrapping = true;
            shrinkable_mins[shrinkable_count] = child_ctx.min_size;
            shrinkable[shrinkable_count++] = child_ctx.size;
        }

        if (box->config.direction == ctx.main_direction)
            *ctx.remaining -= *child_ctx.size;
    }

    // Distribute remaining space to children configured with 'grow' attributes
    if (box->config.direction == ctx.main_direction)
    {
        // Main axis
        if (*ctx.remaining > 0)
            grow_axis(ctx.remaining, growable, growable_count);
        else if (*ctx.remaining < 0)
            shrink_axis(ctx.remaining, shrinkable, shrinkable_mins, &shrinkable_count);
    }
    else
    {
        // Cross axis
        if (*ctx.remaining > 0)
            for (int i = 0; i < growable_count; i++)
                *growable[i] = max(*growable[i], *ctx.remaining);
        else if (*ctx.remaining < 0)
            for (int i = 0; i < shrinkable_count; i++)
                *shrinkable[i] = max(*shrinkable_mins[i], *shrinkable[i] + *ctx.remaining);
    }

    // Recursively resolve size (breadth first)
    if (box->type == BOX_TYPE_CONTAINER)
        for (int i = 0; i < children_count; i++)
            ui_box_grow_shrink_children_axis(box->children[i], axis);
}

void ui_box_resolve_position(UIBox* box)
{
    if (box->parent)
    {
        UIBox* parent = box->parent;
        ContainerData* parent_data = &parent->data.container;
        box->position.x += parent->position.x + parent->config.padding.left;
        box->position.y += parent->position.y + parent->config.padding.top;
        if (parent->config.direction == LAYOUT_LEFT_TO_RIGHT)
        {
                box->position.x += parent_data->next_child_offset_x;
                parent_data->next_child_offset_x += box->config.sizing.width.value + parent->config.child_gap;
        }
        else
        {
            box->position.y += parent_data->next_child_offset_y;
            parent_data->next_child_offset_y += box->config.sizing.height.value + parent->config.child_gap;
        }
    }

    if (box->type == BOX_TYPE_CONTAINER)
        for (int i = 0; i < box->data.container.child_count; i++)
            ui_box_resolve_position(box->children[i]);
}

void ui_generate_render_commands(UIContext* ui_context, UIBox* box)
{
    switch (box->type)
    {
        case BOX_TYPE_CONTAINER:
        {
            UICommand* cmd = ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++;
            cmd->rect.base.type = UI_COMMAND_RECT,
            cmd->rect.base.size = sizeof(UICommandRect),
            cmd->rect.rect = (Rect){ box->position.x, box->position.y, box->position.x + box->config.sizing.width.value,
                                     box->position.y + box->config.sizing.height.value };
            cmd->rect.color = box->config.color;
            cmd->rect.style = box->config.rect_style;
        }
        break;
        case BOX_TYPE_TEXT:
        {
            if (!box->data.text.wrapped_lines)
            {
                UICommand* cmd = ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++;
                cmd->text.base.type = UI_COMMAND_TEXT;
                cmd->text.base.size = sizeof(UICommandText);
                cmd->text.content = box->data.text.content;
                cmd->text.color = box->data.text.color;
                cmd->text.position = box->position;
            }
            else
            {
                uint32_t line_height = ui_context->get_text_height("A");
                for (int i = 0; i < box->data.text.line_count; i++)
                {
                    UICommand* cmd = ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++;
                    cmd->text.base.type = UI_COMMAND_TEXT;
                    cmd->text.base.size = sizeof(UICommandText);
                    cmd->text.content = box->data.text.wrapped_lines[i];
                    cmd->text.color = box->data.text.color;
                    cmd->text.position.x = box->position.x;
                    cmd->text.position.y = box->position.y + i * line_height;
                }
            }
        }
        break;
    }

    if (box->type == BOX_TYPE_CONTAINER)
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
    else if (box->type == BOX_TYPE_TEXT)
    {
        // Calculate box->min_size.width by finding the width of the shortest word in the text.
        float min_width = 0;
        char* text_copy = _strdup(box->data.text.content);
        char* token = strtok(text_copy, " ");
        while (token != NULL)
        {
            float word_width = (float)box->data.text.get_text_width(token);
            if (word_width > min_width)
                min_width = word_width;
            token = strtok(NULL, " ");
        }
        free(text_copy);
        box->min_size.width = min_width;
        box->min_size.height = (float)box->data.text.get_text_height(box->data.text.content);
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
    BoxConfig box_config = { .sizing = { .width = { (float)ui_context->get_text_width(text), SIZING_MODE_FIXED },
                                         .height = { (float)ui_context->get_text_height(text), SIZING_MODE_FIXED } } };
    UIBox* text_box = ui_box_start(&box_config);
    text_box->type = BOX_TYPE_TEXT;
    text_box->data.text.content = _strdup(text);
    text_box->data.text.color = text_config->color;
    text_box->data.text.get_text_width = ui_context->get_text_width;
    text_box->data.text.get_text_height = ui_context->get_text_height;
    ui_box_end(text_box);
    return text_box;
}

static void perform_text_wrapping(UIContext* ui_context, UIBox* text_box)
{
    if (!text_box->data.text.needs_wrapping)
        return;

    float max_width = text_box->config.sizing.width.value;
    char* text = text_box->data.text.content;
    size_t text_len = strlen(text);

    // Allocate space for wrapped lines (worst case: each word is a line)
    size_t max_lines = text_len / 2 + 1;
    text_box->data.text.wrapped_lines = malloc(max_lines * sizeof(char*));
    text_box->data.text.line_count = 0;

    char* line_start = text;
    char* current = text;
    char* last_space = NULL;

    while (*current != '\0')
    {
        if (*current == ' ')
            last_space = current;

        // Check if current segment exceeds max_width
        char saved_char = *(current + 1);
        *(current + 1) = '\0';
        uint32_t width = ui_context->get_text_width(line_start);
        *(current + 1) = saved_char;

        if (width > max_width)
        {
            // Break at last space, or force break if no space
            char* break_point = last_space ? last_space : current;

            // Store this line
            size_t line_len = break_point - line_start;
            text_box->data.text.wrapped_lines[text_box->data.text.line_count] = malloc(line_len + 1);
            strncpy(text_box->data.text.wrapped_lines[text_box->data.text.line_count], line_start, line_len);
            text_box->data.text.wrapped_lines[text_box->data.text.line_count][line_len] = '\0';
            text_box->data.text.line_count++;

            // Move to next line
            line_start = break_point + (last_space ? 1 : 0); // Skip space
            current = line_start;
            last_space = NULL;
        }
        else
        {
            current++;
        }
    }

    // Store final line
    if (*line_start != '\0')
    {
        text_box->data.text.wrapped_lines[text_box->data.text.line_count] = _strdup(line_start);
        text_box->data.text.line_count++;
    }

    // Update box dimensions
    float line_height = (float)ui_context->get_text_height("A");
    text_box->config.sizing.height.value = line_height * text_box->data.text.line_count;
    text_box->min_size.height = text_box->config.sizing.height.value;
    text_box->min_size.width = max_width;
}

void ui_box_apply_text_wrapping(UIContext* ui_context, UIBox* box)
{
    if (box->type == BOX_TYPE_TEXT)
        perform_text_wrapping(ui_context, box);
    else if (box->type == BOX_TYPE_CONTAINER)
        for (int i = 0; i < box->data.container.child_count; i++)
            ui_box_apply_text_wrapping(ui_context, box->children[i]);
}
