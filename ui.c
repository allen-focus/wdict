#include "pch.h"  // IWYU pragma: keep
#include "ui.h"
#include "utils.h"

#include "thirdparty/tracy/public/tracy/TracyC.h"

#include <math.h>

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
    if (box->config.sizing.width.mode == SIZING_MODE_FIT_GROW && box->config.sizing.width.min_max.max == 0)
        box->config.sizing.width.min_max.max = INFINITY;
    if (box->config.sizing.height.mode == SIZING_MODE_FIT_GROW && box->config.sizing.height.min_max.max == 0)
        box->config.sizing.height.min_max.max = INFINITY;

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

UIBox* ui_text(const UIContext* ui_context, GlyphCache* glyph_cache, const String text, const TextConfig* text_config)
{
    f32 base_line_height = ui_context->get_text_height(glyph_cache, text, text_config->font, text_config->font_size, ui_context->dpi);
    f32 line_height = text_config->line_height > 0 ? text_config->line_height : base_line_height;
    f32 fixed_width = ui_context->get_text_width(glyph_cache, text, text_config->font, text_config->font_size, ui_context->dpi);
    BoxConfig box_config = { .sizing = { .width = { { fixed_width, fixed_width }, SIZING_MODE_FIXED },
                                         .height = { { line_height, line_height }, SIZING_MODE_FIXED } } };

    UIBox* text_box = ui_box_start(&box_config);
    {
        text_box->type = BOX_TYPE_TEXT;

        Assert(text_config->font.face && text_config->font.face3);
        Assert(text_config->font_size);
        text_box->data.text.font.face = text_config->font.face;
        text_box->data.text.font.face3 = text_config->font.face3;
        text_box->data.text.font_size = text_config->font_size;

        text_box->data.text.content = text;
        text_box->data.text.color = text_config->color;
        memset(&text_box->data.text.wrapped_lines, 0, sizeof(text_box->data.text.wrapped_lines));
        text_box->data.text.line_count = 1;
        text_box->data.text.line_height = line_height;
        text_box->data.text.half_leading = (line_height - base_line_height) / 2.0f;
    }

    // Calculate box->config.sizing.width.value.min by finding the width of the longest word in the text.
    f32 min_width = 0;
    f32 whole_text_width = 0;
    isize word_count = 0;
    {
        String text = text_box->data.text.content;
        Assert(text.len == 0 || (text.data[0] != ' ' && text.data[text.len - 1] != ' '));

        isize start = 0;
        u32 start_codepoint = 0;

        u32 current_codepoint = 0;
        byte* ptr = text.data;
        while (ptr - text.data < text.len)
        {
            byte* next = utf8_decode(ptr, &current_codepoint);
            if (current_codepoint == ' ' || current_codepoint > 127)
            {
                utf8_decode(&text.data[start], &start_codepoint);
                if (start_codepoint != ' ')
                {
                    // For ASCII: measure up to current position (word boundary before delimiter).
                    // For non-ASCII: include this character itself, treating it as a single-word unit.
                    isize end = start_codepoint < 127 ? ptr - text.data : next - text.data;
                    f32 word_width = ui_context->get_text_width(glyph_cache, str_slice(text, start, end), text_box->data.text.font, text_box->data.text.font_size, ui_context->dpi);
                    min_width = max(min_width, word_width);
                    word_count++;
                }
                start = next - text.data;
            }
            ptr = next;
        }

        // Handle last word
        f32 word_width = ui_context->get_text_width(glyph_cache, str_slice(text, start, text.len), text_box->data.text.font, text_box->data.text.font_size, ui_context->dpi);
        min_width = max(min_width, word_width);
        word_count++;

        whole_text_width = ui_context->get_text_width(glyph_cache, text_box->data.text.content, text_box->data.text.font, text_box->data.text.font_size, ui_context->dpi);
        min_width = (min_width != 0) ? min_width : whole_text_width;
    }
    text_box->config.sizing.width.min_max.min = min_width;
    text_box->config.sizing.width.min_max.max = whole_text_width;
    text_box->config.sizing.height.min_max.min = (f32)text_box->data.text.line_height;
    text_box->config.sizing.height.min_max.max = (f32)text_box->data.text.line_height * word_count;

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
                cmd->text.base.type = UI_COMMAND_TEXT;
                cmd->text.base.size = sizeof(UICommandText);
                cmd->text.font.face = box->data.text.font.face;
                cmd->text.font.face3 = box->data.text.font.face3;
                cmd->text.font_size = box->data.text.font_size;
                cmd->text.content = box->data.text.wrapped_lines.len ? *(box->data.text.wrapped_lines.data + i) : box->data.text.content;
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
typedef enum
{
    WIDTH,
    HEIGHT
} Axis;

typedef struct {
    f32* size;
    f32* min_size;
    f32* max_size;
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
        ctx.max_size = &box->config.sizing.width.min_max.max;
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
        ctx.max_size = &box->config.sizing.height.min_max.max;
        ctx.remaining = &box->data.container.remaining_space.height;
        ctx.padding_start = box->config.padding.top;
        ctx.padding_end = box->config.padding.bottom;
        ctx.sizing_mode = box->config.sizing.height.mode;
        ctx.main_direction = LAYOUT_TOP_TO_BOTTOM;
    }
    return ctx;
}

// Recursively calculate sizes for boxes configured with 'fit' attribute
static void ui_box_calculate_fit_axis(UIBox* box, const Axis axis)
{
    AxisContext box_ctx = get_axis_context(box, axis);

    f32 box_ctx_min_size_backup = *box_ctx.min_size;
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
    // And clamp box axis size to less than box axis max size
    if (axis_has_fit_attribute(box_ctx.sizing_mode))
    {
        if (box_ctx_min_size_backup > *box_ctx.min_size)
            *box_ctx.min_size = box_ctx_min_size_backup;
        if (*box_ctx.size < *box_ctx.min_size)
            *box_ctx.size = *box_ctx.min_size;
        if (*box_ctx.max_size != 0 && *box_ctx.max_size != INFINITY && *box_ctx.size > *box_ctx.max_size)
            *box_ctx.size = *box_ctx.max_size;
    }

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

typedef enum
{
    GROW,
    SHRINK
} DistributeMode;

static void distribute_axis(f32* remaining, F32PtrSlice* items, F32PtrSlice* limits, const DistributeMode mode)
{
    while ((mode == GROW && *remaining > EPSILON) || (mode == SHRINK && *remaining < -EPSILON))
    {
        if (items->len == 0)
            break;

        f32 pivot = *items->data[0];
        f32 second_pivot = mode == GROW ? INFINITY : 0;
        f32 to_add = 0;

        // Find the pivot size (smallest for grow, largest for shrink) and the next pivot.
        for (isize i = 0; i < items->len; i++)
        {
            if (mode == GROW ? (*items->data[i] < pivot) : (*items->data[i] > pivot))
            {
                second_pivot = pivot;
                pivot = *items->data[i];
                to_add = second_pivot - pivot;
            }
            else if (mode == GROW ? (*items->data[i] > pivot) : (*items->data[i] < pivot))
            {
                second_pivot = mode == GROW ? min(*items->data[i], second_pivot) : max(*items->data[i], second_pivot);
                to_add = second_pivot - pivot;
            }
        }

        // If all items have the same size, distribute remaining space equally.
        if ((mode == GROW && second_pivot == INFINITY) || (mode == SHRINK && second_pivot == 0))
            to_add = *remaining / items->len;

        // Count items at the pivot and adjust to_add if it would "exceed" remaining space.
        isize pivot_count = 0;
        for (isize i = 0; i < items->len; i++)
            if (fabsf(*items->data[i] - pivot) < EPSILON)
                pivot_count++;
        if (mode == GROW ? (to_add * pivot_count > *remaining) : (to_add * pivot_count < *remaining))
            to_add = *remaining / pivot_count;

        // Distribute space among items at the pivot size.
        // When an item hits its limit, clamp it and remove from further distribution.
        for (isize i = 0; i < items->len; i++)
        {
            if (fabsf(*items->data[i] - pivot) >= EPSILON)
                continue;

            f32 size_backup = *items->data[i];
            f32* child = items->data[i];
            f32 limit = *limits->data[i];

            *child += to_add;
            if (mode == GROW ? (*child >= limit) : (*child <= limit))
            {
                *child = limit;
                items->data[i] = items->data[items->len-- - 1];
                limits->data[i] = limits->data[limits->len-- - 1];
            }
            *remaining -= (*child - size_backup);
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
        Assert(*ctx.remaining >= 0);
        isize child_gap_count = children_count - 1;
        if (box->config.direction == ctx.main_direction)
            *ctx.remaining -= box->config.child_gap * child_gap_count;

        // Subtract the childrens' determined size from the parent's remaining space.
        UIBox* child = box->child_first;
        while (child)
        {
            AxisContext child_ctx = get_axis_context(child, axis);

            // Subtract the child's determined size from the parent's remaining space.
            if (box->config.direction == ctx.main_direction)
                *ctx.remaining -= *child_ctx.size;

            child = child->next;
        }

        // Distribute remaining space to children (growables and shrinkables are mutually exclusive)
        F32PtrSlice growables = { 0 };
        F32PtrSlice growable_limits = { 0 };
        F32PtrSlice shrinkables = { 0 };
        F32PtrSlice shrinkable_limits = { 0 };
        child = box->child_first;
        while (child)
        {
            AxisContext child_ctx = get_axis_context(child, axis);
            if (child_ctx.sizing_mode == SIZING_MODE_FIT_GROW)
            {
                *slice_push(&growables, &ui_context->arena) = child_ctx.size;
                *slice_push(&growable_limits, &ui_context->arena) = child_ctx.max_size;
            }

            if (*child_ctx.size > *child_ctx.min_size)
            {
                *slice_push(&shrinkables, &ui_context->arena) = child_ctx.size;
                *slice_push(&shrinkable_limits, &ui_context->arena) = child_ctx.min_size;
            }
            child = child->next;
        }
        if (box->config.direction == ctx.main_direction)
        {
            if (*ctx.remaining > 1) // NOTE: To avoid distributing meaningless slivers
                distribute_axis(ctx.remaining, &growables, &growable_limits, GROW);
            else if (*ctx.remaining < 0)
                distribute_axis(ctx.remaining, &shrinkables, &shrinkable_limits, SHRINK);
        }
        else
        {
            if (*ctx.remaining > 0)
                for (isize i = 0; i < growables.len; i++)
                    *growables.data[i] = max(*growables.data[i], min(*ctx.remaining, *growable_limits.data[i]));
            for (isize i = 0; i < shrinkables.len; i++)
                if (*ctx.remaining < *shrinkables.data[i])
                    *shrinkables.data[i] = max(*shrinkable_limits.data[i], *ctx.remaining);
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

            if (parent->config.alignment.x == ALIGN_CENTER)
                box->position.x += parent->data.container.remaining_space.width / 2;
            else if (parent->config.alignment.x == ALIGN_END)
                box->position.x += parent->data.container.remaining_space.width;

            if (parent->config.alignment.y == ALIGN_CENTER)
                box->position.y += (parent->data.container.remaining_space.height - box->size.height) / 2;
            else if (parent->config.alignment.y == ALIGN_END)
                box->position.y += parent->data.container.remaining_space.height - box->size.height;
        }
        else
        {
            box->position.y += parent_data->next_child_offset_y;
            parent_data->next_child_offset_y += box->size.height + parent->config.child_gap;

            if (parent->config.alignment.y == ALIGN_CENTER)
                box->position.y += parent->data.container.remaining_space.height / 2;
            else if (parent->config.alignment.y == ALIGN_END)
                box->position.y += parent->data.container.remaining_space.height;

            if (parent->config.alignment.x == ALIGN_CENTER)
                box->position.x += (parent->data.container.remaining_space.width - box->size.width) / 2;
            else if (parent->config.alignment.x == ALIGN_END)
                box->position.x += parent->data.container.remaining_space.width - box->size.width;
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

static void perform_text_wrapping(UIContext* ui_context, GlyphCache* glyph_cache, UIBox* text_box)
{
    String text = text_box->data.text.content;
    f32 max_width = text_box->size.width;

    if (ui_context->get_text_width(glyph_cache, text, text_box->data.text.font, text_box->data.text.font_size, ui_context->dpi) <= max_width)
        return;

    isize line_start = 0;
    isize last_break = 0;

    byte* ptr = text.data;
    while (ptr - text.data < text.len)
    {
        u32 codepoint;
        byte* next = utf8_decode(ptr, &codepoint);
        isize distance = ptr - text.data;

        // Check width
        f32 width = ui_context->get_text_width(glyph_cache, str_slice(text, line_start, distance), text_box->data.text.font, text_box->data.text.font_size, ui_context->dpi);
        if (width > max_width && last_break > line_start)
        {
            *slice_push(&text_box->data.text.wrapped_lines, &ui_context->arena) = str_slice(text, line_start, last_break);

            // Skip space if needed
            line_start = (text.data[last_break] == ' ') ? last_break + 1 : last_break;
            last_break = line_start;
            continue;
        }

        // Update break position
        if (codepoint == ' ' || codepoint > 127)
            last_break = distance;

        ptr = next;
    }

    // Handle last line
    *slice_push(&text_box->data.text.wrapped_lines, &ui_context->arena) = str_slice(text, line_start, text.len);

    text_box->size.height = text_box->data.text.line_height * text_box->data.text.wrapped_lines.len;
    text_box->config.sizing.height.min_max.min = text_box->size.height;
}

static void ui_box_apply_text_wrapping(UIContext* ui_context, GlyphCache* glyph_cache, UIBox* box)
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

void ui_calculate_layout(UIContext* ui_context, GlyphCache* glyph_cache, UIBox* box)
{
    TracyCZone(ctx, 1);
    ui_box_calculate_fit_axis(box, WIDTH);
    ui_box_grow_shrink_children_axis(ui_context, box, WIDTH);
    ui_box_apply_text_wrapping(ui_context, glyph_cache, box);
    ui_box_calculate_fit_axis(box, HEIGHT);
    ui_box_grow_shrink_children_axis(ui_context, box, HEIGHT);
    ui_box_resolve_position(box);
    TracyCZoneEnd(ctx);
}
