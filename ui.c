#include "pch.h"  // IWYU pragma: keep
#include "arena.h"
#include "lib.h"
#include "slice.h"
#include "string.h"
#include "ui.h"

#include <debugapi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <winuser.h>

#define STACK_SIZE 16
#define EPSILON 1e-4f
#define QUEUE_SIZE 256

typedef Slice(f32*) F32PtrSlice;

///

static Queue(UIBox, QUEUE_SIZE) ui_box_queue = { 0 };
static Stack(UIBox*, STACK_SIZE) ui_box_stack = { 0 };

///

// -----------------------------------------------------------------------------
// Helper
// -----------------------------------------------------------------------------

static b32 axis_has_fit_attribute(const SizingMode mode)
{
    return mode == SIZING_MODE_FIT || mode == SIZING_MODE_FIT_GROW;
}

// -----------------------------------------------------------------------------
// Basic
// -----------------------------------------------------------------------------

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

UIBox* ui_box_start( const BoxConfig* config)
{
    Assert(ui_box_stack.depth <= STACK_SIZE);
    Assert(ui_box_queue.count <= QUEUE_SIZE);

    UIBox* box = ui_box_new();
    UIBox* parent = ui_box_get_parent();

    // Set the parent of the current box and add the current box as a child of its parent
    if (parent)
    {
        box->parent = parent;
        if (!parent->child_first)
        {
            parent->child_first = parent->child_last = box;
        }
        else
        {
            box->prev = parent->child_last;
            parent->child_last->next = box;
            parent->child_last = box;
        }
        parent->data.container.child_count++;
    }

    ui_box_stack.items[ui_box_stack.depth++] = box;
    memcpy(&box->config, config, sizeof(*config));
    box->size.width = box->config.sizing.width.mode == SIZING_MODE_FIXED ? box->config.sizing.width.min_max.min : 0;
    box->size.height = box->config.sizing.height.mode == SIZING_MODE_FIXED ? box->config.sizing.height.min_max.min : 0;

    return box;
}

void ui_box_end(UIBox* box)
{
    ui_box_stack.items[ui_box_stack.depth--] = NULL;
}

void ui_reset(UIContext* ui_context)
{
    Assert(ui_box_stack.depth == 0);
    memset(&ui_box_queue, 0, sizeof(ui_box_queue));
    memset(&ui_context->command_queue, 0, sizeof(ui_context->command_queue));
}

///

UIBox* ui_text(const UIContext* ui_context, const GlyphCache* glyph_cache, const String text, const TextConfig* text_config)
{
    f32 base_line_height = ui_context->get_text_height(glyph_cache, text, ui_context->dpi);
    f32 line_height = text_config->line_height > 0 ? text_config->line_height : base_line_height;
    f32 fixed_width = ui_context->get_text_width(glyph_cache, text, ui_context->dpi);
    BoxConfig box_config = { .sizing = { .width = { { fixed_width, fixed_width }, SIZING_MODE_FIXED },
                                         .height = { { line_height, line_height }, SIZING_MODE_FIXED } } };

    UIBox* text_box = ui_box_start(&box_config);
    {
        text_box->type = BOX_TYPE_TEXT;
        text_box->data.text.content = text;
        text_box->data.text.color = text_config->color;
        memset(&text_box->data.text.wrapped_lines, 0, sizeof(text_box->data.text.wrapped_lines));
        text_box->data.text.line_count = 1;
        text_box->data.text.line_height = line_height;
        text_box->data.text.half_leading = (line_height - base_line_height) / 2.0f;
    }

    // Calculate box->config.sizing.width.value.min by finding the width of the longest word in the text.
    f32 min_width = 0;
    {
        String s = text_box->data.text.content;
        isize start = 0;
        for (isize i = 0; i <= s.len; i++)
            if (s.data[i] == ' ' || i == s.len)
            {
                if (s.data[start] != ' ')
                    min_width = max(min_width, ui_context->get_text_width(glyph_cache, str_slice(s, start, i), ui_context->dpi));
                start = i + 1;
            }
        f32 whole_text_width = ui_context->get_text_width(glyph_cache, text_box->data.text.content, ui_context->dpi);
        min_width = (min_width != 0) ? min_width : whole_text_width;
    }
    text_box->config.sizing.width.min_max.min = min_width;
    text_box->config.sizing.height.min_max.min = (f32)text_box->data.text.line_height;

    ui_box_end(text_box);
    return text_box;
}

///

void ui_generate_render_commands(UIContext* ui_context, const UIBox* box)
{
    f32 dpi_scale = (f32)ui_context->dpi / USER_DEFAULT_SCREEN_DPI;

    switch (box->type)
    {
        case BOX_TYPE_CONTAINER:
        {
            UICommand* cmd = ui_context->command_queue.items + ui_context->command_queue.count++;
            cmd->rect.base.type = UI_COMMAND_RECT;
            cmd->rect.base.size = sizeof(UICommandRect);
            cmd->rect.rect = (Rect){
                box->position.x * dpi_scale,
                box->position.y * dpi_scale,
                (box->position.x + box->size.width) * dpi_scale,
                (box->position.y + box->size.height) * dpi_scale
            };
            cmd->rect.color = box->config.color;
            cmd->rect.style = box->config.rect_style;
        }
        break;
        case BOX_TYPE_TEXT:
        {
            isize i = 0;
            do {
                UICommand* cmd = ui_context->command_queue.items + ui_context->command_queue.count++;
                cmd->text.content = box->data.text.wrapped_lines.len ? *(box->data.text.wrapped_lines.data + i) : box->data.text.content;
                cmd->text.base.type = UI_COMMAND_TEXT;
                cmd->text.base.size = sizeof(UICommandText);
                cmd->text.color = box->data.text.color;
                cmd->text.position.x = box->position.x * dpi_scale;
                cmd->text.position.y = (box->position.y + box->data.text.half_leading + i * box->data.text.line_height) * dpi_scale;
            } while (++i < box->data.text.wrapped_lines.len);
        }
        break;
    }

    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_generate_render_commands(ui_context, child);
            child = child->next;
        }
    }
}

// -----------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------

// Axis-specific helper structure to eliminate code duplication
typedef struct {
    f32* size;
    f32* min_size;
    f32* remaining;
    f32 padding_start;
    f32 padding_end;
    SizingMode sizing_mode;
    LayoutDirection main_direction;
} AxisContext;

static AxisContext get_axis_context(UIBox* box, const Axis axis)
{
    AxisContext ctx = { 0 };
    if (axis == WIDTH)
    {
        ctx.size = &box->size.width;
        ctx.min_size = &box->config.sizing.width.min_max.min;
        ctx.remaining = &box->data.container.remaining_space.width;
        ctx.padding_start = box->config.padding.left;
        ctx.padding_end = box->config.padding.right;
        ctx.sizing_mode = box->config.sizing.width.mode;
        ctx.main_direction = LAYOUT_LEFT_TO_RIGHT;
    }
    else
    {
        ctx.size = &box->size.height;
        ctx.min_size = &box->config.sizing.height.min_max.min;
        ctx.remaining = &box->data.container.remaining_space.height;
        ctx.padding_start = box->config.padding.top;
        ctx.padding_end = box->config.padding.bottom;
        ctx.sizing_mode = box->config.sizing.height.mode;
        ctx.main_direction = LAYOUT_TOP_TO_BOTTOM;
    }
    return ctx;
}

///

// Recursively calculate sizes for boxes configured with 'fit' attribute
static void ui_box_calculate_fit_axis(UIBox* box, const Axis axis)
{
    AxisContext box_ctx = get_axis_context(box, axis);

    f32 box_axis_min_size_backup = *box_ctx.min_size;
    if (axis_has_fit_attribute(box_ctx.sizing_mode))
    {
        isize child_gap_count = box->data.container.child_count - 1;
        *box_ctx.size += box_ctx.padding_start + box_ctx.padding_end;
        *box_ctx.min_size = 0;
        *box_ctx.min_size += box_ctx.padding_start + box_ctx.padding_end;
        if (box->config.direction == box_ctx.main_direction)
        {
            *box_ctx.size += box->config.child_gap * child_gap_count;
            *box_ctx.min_size += box->config.child_gap * child_gap_count;
        }
    }

    // Recursively resolve child box sizes (depth-first, reverse order)
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_calculate_fit_axis(child, axis);
            child = child->next;
        }
    }

    // Select a bigger min size if box axis has both fit attribute and min size config
    if (axis_has_fit_attribute(box_ctx.sizing_mode))
        if (box_axis_min_size_backup > *box_ctx.min_size)
            *box_ctx.min_size = box_axis_min_size_backup;

    // If the current box is a child, adjust its parent's size based on box direction.
    UIBox* parent = box->parent;
    if (parent)
    {
        AxisContext parent_ctx = get_axis_context(parent, axis);
        if (axis_has_fit_attribute(parent_ctx.sizing_mode))
        {
            if (parent->config.direction == parent_ctx.main_direction)
            {
                *parent_ctx.size += *box_ctx.size;
                *parent_ctx.min_size += *box_ctx.min_size;
            }
            else
            {
                f32 padding = parent_ctx.padding_start + parent_ctx.padding_end;
                *parent_ctx.size = max(*box_ctx.size + padding, *parent_ctx.size);
                *parent_ctx.min_size = max(*box_ctx.min_size + padding, *parent_ctx.min_size);
            }
        }
    }
}

// Distribute remaining space proportionally among children that are configured to grow.
static void grow_axis(f32* remaining, F32PtrSlice* growables)
{
    while (*remaining > EPSILON && growables->len > 0)
    {
        f32 smallest = **growables->data;
        f32 second_smallest = INFINITY;
        f32 to_add = 0;

        // Find the smallest size and the next smallest size (to determine the increment).
        for (isize i = 0; i < growables->len; i++)
            if (*growables->data[i] < smallest)
            {
                second_smallest = smallest;
                smallest = *growables->data[i];
                to_add = second_smallest - smallest;
            }
            else if (*growables->data[i] > smallest)
            {
                second_smallest = min(*growables->data[i], second_smallest);
                to_add = second_smallest - smallest;
            }

        // If all growable children have the same size (second_smallest remains INFINITY),
        // distribute the remaining space equally among all growable children.
        if (second_smallest == INFINITY)
            to_add = *remaining / growables->len;

        // If distributing 'to_add' to all 'smallest_count' children would exceed the remaining space,
        // calculate the exact amount that can be distributed equally among them.
        isize smallest_count = 0;
        for (isize i = 0; i < growables->len; i++)
            if (fabsf(*growables->data[i] - smallest) < EPSILON)
                smallest_count++;
        if (to_add * smallest_count > *remaining)
            to_add = *remaining / smallest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'smallest',
        // and update the remaining space.
        for (isize i = 0; i < growables->len; i++)
            if (fabsf(*growables->data[i] - smallest) < EPSILON)
            {
                *growables->data[i] += to_add;
                *remaining -= to_add;
            }
    }
}

static void shrink_axis(f32* remaining, F32PtrSlice* shrinkables, F32PtrSlice* shrinkable_mins)
{
    while (*remaining < -EPSILON && shrinkables->len > 0)
    {
        f32 largest = *shrinkables->data[0];
        f32 second_largest = 0;
        f32 to_add = 0;

        // Find the largest size and the next largest size (to determine the decrement).
        for (isize i = 0; i < shrinkables->len; i++)
            if (*shrinkables->data[i] > largest)
            {
                second_largest = largest;
                largest = *shrinkables->data[i];
                to_add = second_largest - largest;
            }
            else if (*shrinkables->data[i] < largest)
            {
                second_largest = max(*shrinkables->data[i], second_largest);
                to_add = second_largest - largest;
            }

        // If all shrinkable children have the same size (second_largest remains 0),
        // distribute the remaining space equally among all shrinkable children.
        if (second_largest == 0)
            to_add = *remaining / shrinkables->len;

        // If 'to_add' is less than the remaining space, calculate the exact amount
        // that can be distributed equally among them.
        isize largest_count = 0;
        for (isize i = 0; i < shrinkables->len; i++)
            if (fabsf(*shrinkables->data[i] - largest) < EPSILON)
                largest_count++;
        if (to_add * largest_count < *remaining)
            to_add = *remaining / largest_count;

        // Apply the calculated 'to_add' amount to all children currently equal to 'largest',
        // and update the remaining space.
        for (isize i = 0; i < shrinkables->len; i++)
        {
            f32 width_backup = *shrinkables->data[i];
            f32* child = shrinkables->data[i];
            if (fabsf(*child - largest) < EPSILON)
            {
                *child += to_add;
                if (*child <= *shrinkable_mins->data[i])
                {
                    *child = *shrinkable_mins->data[i];
                    // Remove the shrinkable at the given index and update the count.
                    shrinkables->data[i] = shrinkables->data[shrinkables->len-- - 1];
                    shrinkable_mins->data[i] = shrinkable_mins->data[shrinkable_mins->len-- - 1];
                }
                *remaining -= (*child - width_backup);
            }
        }
    }
}

// Recursively grow/shrink axis size of box
static void ui_box_grow_shrink_children_axis(UIContext* ui_context, UIBox* box, const Axis axis)
{
    if (box->type != BOX_TYPE_CONTAINER)
        return;

    isize arena_pos_backup = ui_context->arena.pos;
    {
        isize children_count = box->data.container.child_count;
        AxisContext ctx = get_axis_context(box, axis);
        *ctx.remaining = *ctx.size;

        // Subtract padding and child gap
        // TODO: When the window is resized to very small, the root box's remaining will be less 
        // than padding, then `Assert` broken.
        *ctx.remaining -= ctx.padding_start + ctx.padding_end;
        Assert(*ctx.remaining > 0);
        isize child_gap_count = children_count - 1;
        if (box->config.direction == ctx.main_direction)
            *ctx.remaining -= box->config.child_gap * child_gap_count;

        // Subtract the childrens' determined size from the parent's remaining space.
        f32 cross_axis_remainings_min = 0;
        UIBox* child = box->child_first;
        while (child)
        {
            AxisContext child_ctx = get_axis_context(child, axis);

            // Subtract the child's determined size from the parent's remaining space.
            if (box->config.direction == ctx.main_direction)
                *ctx.remaining -= *child_ctx.size;
            else
                // Handle the situation when text child width exceeds current box
                if (cross_axis_remainings_min > *ctx.remaining - *child_ctx.size)
                    cross_axis_remainings_min = *ctx.remaining - *child_ctx.size;

            child = child->next;
        }

        // Distribute remaining space to children (growables and shrinkables are mutually exclusive)
        F32PtrSlice growables = { 0 };
        F32PtrSlice shrinkables = { 0 };
        F32PtrSlice shrinkable_mins = { 0 };
        child = box->child_first;
        while (child)
        {
            AxisContext child_ctx = get_axis_context(child, axis);
            if (child_ctx.sizing_mode == SIZING_MODE_FIT_GROW)
                *slice_push(&growables, &ui_context->arena) = child_ctx.size;

            if (*child_ctx.size > *child_ctx.min_size)
            {
                *slice_push(&shrinkables, &ui_context->arena) = child_ctx.size;
                *slice_push(&shrinkable_mins, &ui_context->arena) = child_ctx.min_size;
            }
            child = child->next;
        }
        if (box->config.direction == ctx.main_direction)
        {
            if (*ctx.remaining > 0)
                grow_axis(ctx.remaining, &growables);
            else if (*ctx.remaining < 0)
                shrink_axis(ctx.remaining, &shrinkables, &shrinkable_mins);
        }
        else
        {
            if (*ctx.remaining > 0)
                for (isize i = 0; i < growables.len; i++)
                    *growables.data[i] = max(*growables.data[i], *ctx.remaining);
            if (cross_axis_remainings_min < 0)
                for (isize i = 0; i < shrinkables.len; i++)
                    if (*ctx.remaining < *shrinkables.data[i])
                        *shrinkables.data[i] = max(*shrinkable_mins.data[i], *shrinkables.data[i] + cross_axis_remainings_min);
        }
    }
    ui_context->arena.pos = arena_pos_backup;

    // Recursively resolve size (breadth first)
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_grow_shrink_children_axis(ui_context, child, axis);
            child = child->next;
        }
    }
}

static void ui_box_resolve_position(UIBox* box)
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
            parent_data->next_child_offset_x += box->size.width + parent->config.child_gap;
        }
        else
        {
            box->position.y += parent_data->next_child_offset_y;
            parent_data->next_child_offset_y += box->size.height + parent->config.child_gap;
        }
    }

    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_resolve_position(child);
            child = child->next;
        }
    }
}

static void perform_text_wrapping(UIContext* ui_context, const GlyphCache* glyph_cache, UIBox* text_box)
{
    String text = text_box->data.text.content;
    f32 text_width = ui_context->get_text_width(glyph_cache, text, ui_context->dpi);
    f32 max_width = text_box->size.width;
    if (text_width <= max_width)
        return;

    isize new_line_idx = 0;
    isize distance_between_line_start_and_newest_space = 0;
    // TODO: don't calculate whole text width in every loop [performance]
    for (isize i = 0; i <= text.len; i++)
    {
        f32 current_line_width = ui_context->get_text_width(glyph_cache, str_slice(text, new_line_idx, i), ui_context->dpi);
        if (current_line_width > max_width)
        {
            Assert(distance_between_line_start_and_newest_space);
            isize end = new_line_idx + distance_between_line_start_and_newest_space;
            *slice_push(&text_box->data.text.wrapped_lines, &ui_context->arena) = str_slice(text, new_line_idx, end);
            new_line_idx = end + 1;
            distance_between_line_start_and_newest_space = 0;
        }
        if (text.data[i] == ' ')
            distance_between_line_start_and_newest_space = i - new_line_idx;
    }
    // Handle last line
    *slice_push(&text_box->data.text.wrapped_lines, &ui_context->arena) = str_slice(text, new_line_idx, text.len);

    // Update box dimensions
    text_box->size.height = text_box->data.text.line_height * text_box->data.text.wrapped_lines.len;
    text_box->config.sizing.height.min_max.min = text_box->size.height;
    text_box->config.sizing.width.min_max.min = max_width;
}

static void ui_box_apply_text_wrapping(UIContext* ui_context, const GlyphCache* glyph_cache, UIBox* box)
{
    if (box->type == BOX_TYPE_TEXT)
    {
        perform_text_wrapping(ui_context, glyph_cache, box);
    }
    else if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_apply_text_wrapping(ui_context, glyph_cache, child);
            child = child->next;
        }
    }
}

void ui_calculate_layout(UIContext* ui_context, const GlyphCache* glyph_cache, UIBox* box)
{
    ui_box_calculate_fit_axis(box, WIDTH);
    ui_box_grow_shrink_children_axis(ui_context, box, WIDTH);
    ui_box_apply_text_wrapping(ui_context, glyph_cache, box);
    ui_box_calculate_fit_axis(box, HEIGHT);
    ui_box_grow_shrink_children_axis(ui_context, box, HEIGHT);
    ui_box_resolve_position(box);
}

