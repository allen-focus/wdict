#include "pch.h"  // IWYU pragma: keep
#include "lib.h"
#include "string.h"
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPSILON 1e-4f
#define QUEUE_SIZE 256
#define STACK_SIZE 16

///

static Queue(UIBox, QUEUE_SIZE) ui_box_queue = { 0 };
static Stack(UIBox*, STACK_SIZE) ui_box_stack = { 0 };

///

// Axis-specific helper structure to eliminate code duplication
typedef struct {
    f32* size;
    f32* min_size;
    f32* remaining;
    f32 padding_start;
    f32 padding_end;
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

static b32 axis_has_fit_attribute(SizingAxis axis)
{
    return axis.mode == SIZING_MODE_FIT || axis.mode == SIZING_MODE_FIT_GROW;
}

// Recursively calculate sizes for boxes configured with 'fit' attribute
void ui_box_calculate_fit_axis(UIBox* box, Axis axis)
{
    // Recursively resolve child box sizes (depth-first, reverse order)
    if (box->type == BOX_TYPE_CONTAINER)
        for (i32 i = 0; i < box->data.container.child_count; i++)
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
                f32 padding = parent_ctx.padding_start + parent_ctx.padding_end;
                *parent_ctx.size = max(*box_ctx.size + padding, *parent_ctx.size);
                *parent_ctx.min_size = max(*box_ctx.min_size + padding, *parent_ctx.min_size);
            }
        }
    }
}

// Distribute remaining space proportionally among children that are configured to grow.
static void grow_axis(f32* remaining, f32* growable[], const i32 growable_count)
{
    while (*remaining > EPSILON && growable_count > 0)
    {
        f32 smallest = *growable[0];
        f32 second_smallest = INFINITY;
        f32 to_add = 0;

        // Find the smallest size and the next smallest size (to determine the increment).
        for (i32 i = 0; i < growable_count; i++)
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
        i32 smallest_count = 0;
        for (i32 i = 0; i < growable_count; i++)
            if (fabsf(*growable[i] - smallest) < EPSILON)
                smallest_count++;
        if (to_add * smallest_count > *remaining)
            to_add = *remaining / smallest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'smallest', and update the remaining space.
        for (i32 i = 0; i < growable_count; i++)
            if (fabsf(*growable[i] - smallest) < EPSILON)
            {
                *growable[i] += to_add;
                *remaining -= to_add;
            }
    }
}

void remove_shrinkable(f32* shrinkable[], f32* shrinkable_mins[], int* shrinkable_count, i32 index)
{
    // Remove the shrinkable at the given index and update the count.
    shrinkable[index] = shrinkable[*shrinkable_count - 1];
    shrinkable_mins[index] = shrinkable_mins[*shrinkable_count - 1];
    shrinkable[*shrinkable_count - 1] = NULL;
    shrinkable_mins[*shrinkable_count - 1] = NULL;
    --(*shrinkable_count);
}

static void shrink_axis(f32* remaining, f32* shrinkable[], f32* shrinkable_mins[], int* shrinkable_count)
{
    while (*remaining < -EPSILON && *shrinkable_count > 0)
    {
        f32 largest = *shrinkable[0];
        f32 second_largest = 0;
        f32 to_add = 0;

        // Find the largest size and the next largest size (to determine the decrement).
        for (i32 i = 0; i < *shrinkable_count; i++)
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
        i32 largest_count = 0;
        for (i32 i = 0; i < *shrinkable_count; i++)
            if (fabsf(*shrinkable[i] - largest) < EPSILON)
                largest_count++;
        if (to_add * largest_count < *remaining)
            to_add = *remaining / largest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'largest', and update the remaining space.
        for (i32 i = 0; i < *shrinkable_count; i++)
        {
            f32 width_backup = *shrinkable[i];
            f32* child = shrinkable[i];
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

    i32 children_count = box->data.container.child_count;
    AxisContext ctx = get_axis_context(box, axis);

    f32* growable[CHILDREN_SIZE] = { 0 };
    i32 growable_count = 0;

    f32* shrinkable[CHILDREN_SIZE] = { 0 };
    f32* shrinkable_mins[CHILDREN_SIZE] = { 0 };
    i32 shrinkable_count = 0;

    // Initialize remaining space (which might be fixed or determined by a
    // parent's 'grow' attribute in a previous pass)
    *ctx.remaining = *ctx.size;

    // Subtract padding and child gap
    *ctx.remaining -= ctx.padding_start + ctx.padding_end;
    i32 child_gap_count = children_count - 1;
    if (box->config.direction == ctx.main_direction)
        *ctx.remaining -= box->config.child_gap * child_gap_count;

    // Subtract the childrens' determined size from the parent's remaining space.
    for (i32 i = 0; i < children_count; i++)
    {
        UIBox* child = box->children[i];
        AxisContext child_ctx = get_axis_context(child, axis);

        if (child_ctx.sizing_axis->mode == SIZING_MODE_FIT_GROW)
            growable[growable_count++] = child_ctx.size;

        if (*child_ctx.size > *child_ctx.min_size)
        {
            shrinkable_mins[shrinkable_count] = child_ctx.min_size;
            shrinkable[shrinkable_count++] = child_ctx.size;
        }

        if (box->config.direction == ctx.main_direction)
            *ctx.remaining -= *child_ctx.size;
        else
            if (*ctx.remaining < *child_ctx.size && cross_axis_remainings_min > (*ctx.remaining - *child_ctx.size))
                cross_axis_remainings_min -= *child_ctx.size;
    }

    // Distribute remaining space to children
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
            for (i32 i = 0; i < growable_count; i++)
                *growable[i] = max(*growable[i], *ctx.remaining);
        else if (*ctx.remaining < 0)
            for (i32 i = 0; i < shrinkable_count; i++)
                *shrinkable[i] = max(*shrinkable_mins[i], *shrinkable[i] + *ctx.remaining);

        if (cross_axis_remainings_min < 0)
            for (i32 i = 0; i < shrinkable_count; i++)
                *shrinkable[i] = max(*shrinkable_mins[i], *shrinkable[i] + cross_axis_remainings_min);
    }

    // Recursively resolve size (breadth first)
    if (box->type == BOX_TYPE_CONTAINER)
        for (i32 i = 0; i < children_count; i++)
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
        for (i32 i = 0; i < box->data.container.child_count; i++)
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
            for (i32 i = 0; i < box->data.text.line_count; i++)
            {
                UICommand* cmd = ui_context->ui_command_queue.items + ui_context->ui_command_queue.count++;
                cmd->text.content = (box->data.text.line_count == 1) ? box->data.text.content : box->data.text.wrapped_lines[i];
                cmd->text.base.type = UI_COMMAND_TEXT;
                cmd->text.base.size = sizeof(UICommandText);
                cmd->text.color = box->data.text.color;
                cmd->text.position.x = box->position.x;
                cmd->text.position.y = box->position.y + box->data.text.half_leading + i * box->data.text.line_height;
            }
        }
        break;
    }

    if (box->type == BOX_TYPE_CONTAINER)
        for (i32 i = 0; i < box->data.container.child_count; i++)
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
        for (i32 i = 0; i < CHILDREN_SIZE; i++)
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
        i32 child_gap_count = box->data.container.child_count - 1;
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
        f32 min_width = INFINITY;
        {
            String s = box->data.text.content;
            isize start = 0;
            for (isize i = 0; i < s.len; i++)
                if (s.data[i] == ' ')
                {
                    String word = str_slice(s, start, i);
                    min_width = min(min_width, box->data.text.get_text_width(word));
                    start = i + 1;
                }
        }
        f32 whole_text_width = (f32)box->data.text.get_text_width(box->data.text.content);
        box->min_size.width = (min_width != INFINITY) ? min_width : whole_text_width;
        box->min_size.height = (f32)box->data.text.line_height;
    }

    ui_box_stack.items[ui_box_stack.depth--] = NULL;
}

void ui_reset(UIContext* ui_context)
{
    Assert(ui_box_stack.depth == 0);
    memset(&ui_box_queue, 0, sizeof(ui_box_queue));
    memset(&ui_context->ui_command_queue, 0, sizeof(ui_context->ui_command_queue));
}

UIBox* ui_text(UIContext* ui_context, String text, TextConfig* text_config)
{
    f32 base_line_height = (f32)ui_context->get_text_height(text);
    f32 effective_line_height = text_config->line_height > 0 ? text_config->line_height : base_line_height;
    BoxConfig box_config = { .sizing = { .width = { (f32)ui_context->get_text_width(text), SIZING_MODE_FIXED },
                                         .height = { effective_line_height, SIZING_MODE_FIXED } } };

    UIBox* text_box = ui_box_start(&box_config);
    {
        text_box->type = BOX_TYPE_TEXT;
        text_box->data.text.content = text;
        text_box->data.text.color = text_config->color;
        text_box->data.text.line_count = 1;
        text_box->data.text.line_height = effective_line_height;
        text_box->data.text.half_leading = (effective_line_height - base_line_height) / 2.0f;
        text_box->data.text.get_text_width = ui_context->get_text_width;
        text_box->data.text.get_text_height = ui_context->get_text_height;
    }
    ui_box_end(text_box);
    return text_box;
}

static void perform_text_wrapping(UIBox* text_box)
{
    String text = text_box->data.text.content;
    u32 text_width = text_box->data.text.get_text_width(text);
    f32 max_width = text_box->config.sizing.width.value;
    if ((f32)text_width <= max_width)
        return;

    // TODO: 1. Don't hard code 64;
    //       2. Have to free it.
    text_box->data.text.wrapped_lines = calloc(64, sizeof(text));

    isize new_line_idx = 0;
    isize distance_between_line_start_and_newest_space = 0;
    // TODO: don't calculate whole text width in every loop [performance]
    for (isize i = 0; i <= text.len; i++)
    {
        u32 current_line_width = text_box->data.text.get_text_width(str_slice(text, new_line_idx, i));
        if (current_line_width > max_width)
        {
            i32* line_count = &text_box->data.text.line_count;
            if (!distance_between_line_start_and_newest_space)
            {
                // The width of first word of current line has exceeded the maximum width, force a line break
                text_box->data.text.wrapped_lines[(*line_count)++ - 1] = str_slice(text, new_line_idx, --i);
                new_line_idx = i;
            }
            else
            {
                isize end = new_line_idx + distance_between_line_start_and_newest_space;
                text_box->data.text.wrapped_lines[(*line_count)++ - 1] = str_slice(text, new_line_idx, end);
                new_line_idx = end + 1;
            }
            distance_between_line_start_and_newest_space = 0;
        }
        if (text.data[i] == ' ')
            distance_between_line_start_and_newest_space = i - new_line_idx;
    }
    // Handle last line
    text_box->data.text.wrapped_lines[text_box->data.text.line_count - 1] = str_slice(text, new_line_idx, text.len);

    // Update box dimensions
    text_box->config.sizing.height.value = text_box->data.text.line_height * text_box->data.text.line_count;
    text_box->min_size.height = text_box->config.sizing.height.value;
    text_box->min_size.width = max_width;
}

void ui_box_apply_text_wrapping(UIContext* ui_context, UIBox* box)
{
    if (box->type == BOX_TYPE_TEXT)
        perform_text_wrapping(box);
    else if (box->type == BOX_TYPE_CONTAINER)
        for (i32 i = 0; i < box->data.container.child_count; i++)
            ui_box_apply_text_wrapping(ui_context, box->children[i]);
}
