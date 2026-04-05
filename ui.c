#include "ui.h"
#include "glyph_cache.h"
#include "utils.h"
#include <math.h>
#include <string.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"

#define EPSILON                   1e-4f
#define UI_CONTEXT_ARENA_CAPACITY MB(16)
#define BOX_CACHE_ARENA_CAPACITY  MB(8)
#define BOX_CACHE_CAPACITY        1024 // must be a power of two
#define BOX_STACK_CAPACITY        16
#define BOX_QUEUE_CAPACITY        2048

typedef Slice(f32*) F32PtrSlice;

static Queue(UIBox, BOX_QUEUE_CAPACITY) ui_box_queue = { 0 };
static Stack(UIBox*, BOX_STACK_CAPACITY) ui_box_stack = { 0 };

UIContext* g_ui_context = NULL;

//
// Context
//

static b32 is_same_box_key(const void* a, const void* b, isize size)
{
    Assert(size == (sizeof(u8) * HASH_STR_MAX_LENGTH + sizeof(isize)));
    String str_a = { .data = (u8*)a, .len = *(isize*)((u8*)a + HASH_STR_MAX_LENGTH) };
    String str_b = { .data = (u8*)b, .len = *(isize*)((u8*)b + HASH_STR_MAX_LENGTH) };
    return str_compare(str_a, str_b);
}

static void box_cache_init(UIBoxCache* box_cache)
{
    box_cache->arena = arena_new(BOX_CACHE_ARENA_CAPACITY);
    box_cache->lru_cache = lru_cache_create(&box_cache->arena, (BOX_CACHE_CAPACITY >> 1), BOX_CACHE_CAPACITY,
                                            sizeof(BoxKey), sizeof(UIBox), fnv1a_hash, is_same_box_key);
}

static void box_cache_deinit(UIBoxCache* box_cache)
{
    arena_release(&box_cache->arena);
    memset(box_cache, 0, sizeof(*box_cache));
}

void ui_init(UIContext* ui_context, u32 width, u32 height, u32 dpi, IDWriteFactory3* dwrite_factory,
             UIRenderFunc render_fn)
{
    ui_context->arena = arena_new(UI_CONTEXT_ARENA_CAPACITY);
    glyph_cache_init(&ui_context->glyph_cache, GLYPHS_LENGTH, dwrite_factory);
    box_cache_init(&ui_context->box_cache);
    ui_context->client_width = width;
    ui_context->client_height = height;
    ui_context->dpi = dpi;
    ui_context->render_fn = render_fn;
    ui_context->frame_index = 0;
}

void ui_deinit(UIContext* ui_context)
{
    box_cache_deinit(&ui_context->box_cache);
    glyph_cache_deinit(&ui_context->glyph_cache);
    arena_release(&ui_context->arena);
    memset(ui_context, 0, sizeof(*ui_context));
}

//
// Box cache
//

static UIBox* find_or_insert_last_box_with_same_hash_str(const String hash_str, b32* found)
{
    UIBoxCache* box_cache = &g_ui_context->box_cache;
    BoxKey key = { .len = hash_str.len };
    memcpy(key.str, hash_str.data, hash_str.len);

    LRUSignal signal;
    u32 entry_index = lru_cache_find_or_evict(&box_cache->lru_cache, &key, &signal);
    UIBox* last_box = (UIBox*)((byte*)box_cache->lru_cache.values_buf + entry_index * box_cache->lru_cache.value_size);

    // NOTE:
    //   Use LRUCache for its fixed-size hash table with linked-list chaining, and its
    //   insertion order (new entries appended to tail). And LRU eviction is disabled;
    //   unused boxes are explicitly dropped per frame.
    switch (signal)
    {
        case LRU_SIGNAL_FOUND:
            // Detect duplicate box key
            if (g_ui_context->frame_index != 0)
                if (last_box->last_frame_index == g_ui_context->frame_index)
                    Assert(0);
            *found = True;
            break;
        case LRU_SIGNAL_TOINSERT:
            memcpy(&last_box->key, &key, sizeof(key));
            *found = False;
            break;
        case LRU_SIGNAL_TOEVICT:
        default:
            Assert(0);
    }

    return last_box;
}

static void box_cache_remove_unused()
{
    LRUCache* lru_cache = &g_ui_context->box_cache.lru_cache;
    u32 lru_entry_index = lru_cache->entries[0].lru_prev;
    UIBox* lru_box = (UIBox*)((byte*)lru_cache->values_buf + lru_entry_index * lru_cache->value_size);
    while (lru_box->last_frame_index < g_ui_context->frame_index && lru_entry_index > 0)
    {
        lru_entry_index = lru_cache->entries[lru_entry_index].lru_prev;
        lru_cache_pop_lru_entry(lru_cache);
        memset(lru_box, 0, sizeof(*lru_box));
        lru_box = (UIBox*)((byte*)lru_cache->values_buf + lru_entry_index * lru_cache->value_size);
    }
}

//
// Basic
//

void ui_reset()
{
    Assert(ui_box_stack.depth == 0);
    memset(&ui_box_queue, 0, sizeof(ui_box_queue));
    memset(&g_ui_context->command_queue, 0, sizeof(g_ui_context->command_queue));
    g_ui_context->mouse_lclick = False;
    g_ui_context->mouse_rclick = False;
}

static UIBox* ui_box_new()
{
    Assert(ui_box_queue.count < BOX_QUEUE_CAPACITY);
    return &ui_box_queue.items[ui_box_queue.count++];
}

static UIBox* ui_box_get_parent()
{
    return (ui_box_stack.depth > 0) ? ui_box_stack.items[ui_box_stack.depth - 1] : NULL;
}

static UIBox* ui_box_get_root()
{
    Assert(ui_box_queue.count > 0);
    return &ui_box_queue.items[0];
}

UIBox* ui_box_start(const BoxConfig* config)
{
    Assert(ui_box_stack.depth <= BOX_STACK_CAPACITY);
    Assert(ui_box_queue.count <= BOX_QUEUE_CAPACITY);

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

UIBox* ui_text(const String text, const TextConfig* text_config)
{
    GlyphCache* glyph_cache = &g_ui_context->glyph_cache;
    u32 dpi = g_ui_context->dpi;
    get_text_width_fn get_text_width = g_ui_context->render_fn.get_text_width;
    get_text_height_fn get_text_height = g_ui_context->render_fn.get_text_height;

    f32 base_line_height = get_text_height(glyph_cache, text, text_config->font, text_config->font_size, dpi);
    f32 line_height = text_config->line_height > 0 ? text_config->line_height : base_line_height;
    f32 fixed_width = get_text_width(glyph_cache, text, text_config->font, text_config->font_size, dpi);
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
                    f32 word_width = get_text_width(glyph_cache, str_slice(text, start, end), text_box->data.text.font,
                                                    text_box->data.text.font_size, dpi);
                    min_width = max(min_width, word_width);
                    word_count++;
                }
                start = next - text.data;
            }
            ptr = next;
        }

        // Handle last word
        f32 word_width = get_text_width(glyph_cache, str_slice(text, start, text.len), text_box->data.text.font,
                                        text_box->data.text.font_size, dpi);
        min_width = max(min_width, word_width);
        word_count++;

        whole_text_width = get_text_width(glyph_cache, text_box->data.text.content, text_box->data.text.font,
                                          text_box->data.text.font_size, dpi);
        min_width = (min_width != 0) ? min_width : whole_text_width;
    }
    text_box->config.sizing.width.min_max.min = min_width;
    text_box->config.sizing.width.min_max.max = whole_text_width;
    text_box->config.sizing.height.min_max.min = (f32)text_box->data.text.line_height;
    text_box->config.sizing.height.min_max.max = (f32)text_box->data.text.line_height * word_count;

    ui_box_end(text_box);
    return text_box;
}

//
// Layout
//

// Axis-specific helper structure to eliminate code duplication
typedef enum
{
    WIDTH,
    HEIGHT
} Axis;

typedef struct
{
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

static b32 axis_has_fit_attribute(const SizingMode mode)
{
    return mode == SIZING_MODE_FIT || mode == SIZING_MODE_FIT_GROW;
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
static void ui_box_grow_shrink_children_axis(UIBox* box, const Axis axis)
{
    if (box->type != BOX_TYPE_CONTAINER)
        return;

    isize arena_pos_backup = g_ui_context->arena.pos;
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
                *slice_push(&growables, &g_ui_context->arena) = child_ctx.size;
                *slice_push(&growable_limits, &g_ui_context->arena) = child_ctx.max_size;
            }

            if (*child_ctx.size > *child_ctx.min_size)
            {
                *slice_push(&shrinkables, &g_ui_context->arena) = child_ctx.size;
                *slice_push(&shrinkable_limits, &g_ui_context->arena) = child_ctx.min_size;
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
    g_ui_context->arena.pos = arena_pos_backup;

    // Recursively resolve size (breadth first)
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_grow_shrink_children_axis(child, axis);
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

        // Handle scroll offset
        box->position.x += parent->config.child_offset.x;
        box->position.y += parent->config.child_offset.y;
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

    // Update the box size and position in box cache if the box has a hash string
    if (box->key.len)
    {
        b32 found;
        String box_hash_str = { box->key.str, box->key.len };
        UIBox* last_box = find_or_insert_last_box_with_same_hash_str(box_hash_str, &found);
        last_box->size = box->size;
        last_box->position = box->position;

        // NOTE:
        //   Update `last_frame_index` here because this is the final box that will be used for the current frame.
        //   This enables duplicate detection: during lookup, if a box is already marked with the current frame index,
        //   it indicates a duplicate key (panic condition).
        last_box->last_frame_index = g_ui_context->frame_index;
    }
}

static void perform_text_wrapping(UIBox* text_box)
{
    u32 dpi = g_ui_context->dpi;
    GlyphCache* glyph_cache = &g_ui_context->glyph_cache;
    get_text_width_fn get_text_width = g_ui_context->render_fn.get_text_width;

    Font font = text_box->data.text.font;
    f32 font_size = text_box->data.text.font_size;
    String text = text_box->data.text.content;
    f32 max_width = text_box->size.width;

    if (get_text_width(glyph_cache, text, font, font_size, dpi) <= max_width)
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
        f32 width = get_text_width(glyph_cache, str_slice(text, line_start, distance), font, font_size, dpi);
        if (width > max_width && last_break > line_start)
        {
            *slice_push(&text_box->data.text.wrapped_lines, &g_ui_context->arena) =
                str_slice(text, line_start, last_break);

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
    *slice_push(&text_box->data.text.wrapped_lines, &g_ui_context->arena) = str_slice(text, line_start, text.len);

    text_box->size.height = text_box->data.text.line_height * text_box->data.text.wrapped_lines.len;
    text_box->config.sizing.height.min_max.min = text_box->size.height;
}

static void ui_box_apply_text_wrapping(UIBox* box)
{
    if (box->type == BOX_TYPE_TEXT)
    {
        perform_text_wrapping(box);
    }
    else if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_apply_text_wrapping(child);
            child = child->next;
        }
    }
}

static void ui_calculate_layout(UIBox* box)
{
    TracyCZone(ctx, 1);
    ui_box_calculate_fit_axis(box, WIDTH);
    ui_box_grow_shrink_children_axis(box, WIDTH);
    ui_box_apply_text_wrapping(box);
    ui_box_calculate_fit_axis(box, HEIGHT);
    ui_box_grow_shrink_children_axis(box, HEIGHT);
    ui_box_resolve_position(box);
    TracyCZoneEnd(ctx);
}

//
// Frame
//

isize ui_begin_frame(UIContext* ui_context)
{
    g_ui_context = ui_context;
    if (g_ui_context->frame_index > 0)
        g_ui_context->render_fn.wait_for_last_submitted_frame();

    return g_ui_context->arena.pos;
}

static Rect intersect_rects(Rect r1, Rect r2)
{
    f32 xmin = max(r1.xmin, r2.xmin);
    f32 ymin = max(r1.ymin, r2.ymin);
    f32 xmax = min(r1.xmax, r2.xmax);
    f32 ymax = min(r1.ymax, r2.ymax);

    // If there is no intersection, return a rectangle whose width or height is zero
    if (xmax < xmin)
        xmax = xmin;
    if (ymax < ymin)
        ymax = ymin;

    return (Rect){ xmin, ymin, xmax, ymax };
}

static void ui_generate_render_commands(const UIBox* box, const Rect clip)
{
    f32 dpi_scale = (f32)g_ui_context->dpi / USER_DEFAULT_SCREEN_DPI;
    Rect box_rect = (Rect){
        box->position.x * dpi_scale,
        box->position.y * dpi_scale,
        (box->position.x + box->size.width) * dpi_scale,
        (box->position.y + box->size.height) * dpi_scale,
    };

    // If clip is enabled, push a clip
    Rect new_clip = clip;
    if (box->config.enable_clip)
        new_clip = intersect_rects(clip, box_rect);

    // clang-format off
    // Draw box rect/text
    switch (box->type)
    {
        case BOX_TYPE_CONTAINER:
        {
            UICommand* cmd = g_ui_context->command_queue.items + g_ui_context->command_queue.count++;

            cmd->rect.base.type = UI_COMMAND_RECT;
            cmd->rect.base.size = sizeof(UICommandRect);
            cmd->rect.rect      = box_rect;
            cmd->rect.color     = box->config.color;
            cmd->rect.style     = box->config.rect_style;
            cmd->rect.clip      = clip;
        }
        break;
        case BOX_TYPE_TEXT:
        {
            isize i = 0;
            do
            {
                UICommand* cmd = g_ui_context->command_queue.items + g_ui_context->command_queue.count++;
                const TextData* text = &box->data.text;

                cmd->text.base.type  = UI_COMMAND_TEXT;
                cmd->text.base.size  = sizeof(UICommandText);
                cmd->text.font.face  = text->font.face;
                cmd->text.font.face3 = text->font.face3;
                cmd->text.font_size  = text->font_size;
                cmd->text.content    = text->wrapped_lines.len ? *(text->wrapped_lines.data + i) : text->content;
                cmd->text.color      = text->color;
                cmd->text.position.x = box->position.x * dpi_scale;
                cmd->text.position.y = (box->position.y + text->half_leading + i * text->line_height) * dpi_scale;
                cmd->text.clip       = clip;
            } while (++i < box->data.text.wrapped_lines.len);
        }
        break;
    }
    // clang-format on

    // Recursive child boxes
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_generate_render_commands(child, new_clip);
            child = child->next;
        }
    }
}

void ui_end_frame(isize arena_pos_backup)
{
    GlyphCache* glyph_cache = &g_ui_context->glyph_cache;
    u32 dpi = g_ui_context->dpi;
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    Rect no_clip = { 0, 0, g_ui_context->client_width * dpi_scale, g_ui_context->client_height * dpi_scale };
    UIRenderFunc* render_fn = &g_ui_context->render_fn;

    g_ui_context->root = ui_box_get_root();
    ui_calculate_layout(g_ui_context->root);
    ui_generate_render_commands(g_ui_context->root, no_clip);

    // Draw
    for (isize i = 0; i < g_ui_context->command_queue.count; i++)
    {
        Rect* clip;
        UICommand* cmd = &g_ui_context->command_queue.items[i];
        switch (cmd->type)
        {
            case UI_COMMAND_RECT:
                clip = memcmp(&cmd->rect.clip, &no_clip, sizeof(no_clip)) == 0 ? NULL : &cmd->rect.clip;
                render_fn->draw_rect(glyph_cache, cmd->rect.rect, cmd->rect.color, cmd->rect.style, clip);
                break;
            case UI_COMMAND_TEXT:
                UICommandText* text = &cmd->text;
                clip = memcmp(&cmd->text.clip, &no_clip, sizeof(no_clip)) == 0 ? NULL : &cmd->text.clip;
                render_fn->draw_text(glyph_cache, text->content, text->position, text->color, text->font,
                                     text->font_size, dpi, clip);
                break;
            default:
                Assert(0);
        }
    }

    // Present
    u32 physical_client_width = (u32)(g_ui_context->client_width * dpi_scale);
    u32 physical_client_height = (u32)(g_ui_context->client_height * dpi_scale);
    g_ui_context->render_fn.flush_and_present(physical_client_width, physical_client_height);

    ui_reset();
    box_cache_remove_unused();
    g_ui_context->frame_index++;
    arena_pop_to(&g_ui_context->arena, arena_pos_backup);
    g_ui_context = NULL;
}

//
// Widgets
//

static b32 rect_contains_point(Rect r, Position p)
{
    return p.x >= r.xmin && p.x < r.xmax && p.y >= r.ymin && p.y < r.ymax;
}

static String init_and_return_hash_str(UIBox* box, const String text)
{
    String hash_str;
    for (isize i = 0; i < text.len; i++)
        if (text.data[i] == '#')
            if (text.len > i + 1)
                if (text.data[i + 1] == '#')
                {
                    if (text.len > i + 2)
                        if (text.data[i + 2] == '#')
                        {
                            String temp = str_slice(text, i + 3, text.len);
                            hash_str = str_clone(&g_ui_context->arena, temp);
                            break;
                        }
                    String str_left = { text.data, i };
                    String str_right = { text.data + i + 2, text.len - (i + 2) };
                    hash_str = str_concat(&g_ui_context->arena, str_left, str_right);
                    break;
                }

    if (!hash_str.len || hash_str.len > HASH_STR_MAX_LENGTH)
        Assert(0);

    // Update box's key. This will be used for find the same box in the box cache.
    memcpy(box->key.str, hash_str.data, hash_str.len);
    box->key.len = hash_str.len;

    return hash_str;
}

UISignalFlags ui_button(const String text, const Font font)
{
    TextConfig text_config = { .font = font, .font_size = 12, .color = { 0, 0, 0, 255 }, .line_height = 12 };

    String hash_str;
    ui_box({ .sizing = { fit({}), fit({}) },
             .color = { 94, 203, 228, 255 },
             .padding = { 6, 6, 6, 6 },
             .alignment = { ALIGN_CENTER, ALIGN_CENTER } })
    {
        ui_text(text, &text_config);

        UIBox* box = ui_box_get_parent();
        hash_str = init_and_return_hash_str(box, text);
    }

    UISignalFlags flags = UI_Signal_Flag_None;
    b32 found;
    UIBox* last_box = find_or_insert_last_box_with_same_hash_str(hash_str, &found);
    if (found)
        if (last_box->key.len)
        {
            Rect last_box_rect = {
                .xmin = last_box->position.x,
                .ymin = last_box->position.y,
                .xmax = last_box->position.x + last_box->size.width,
                .ymax = last_box->position.y + last_box->size.height,
            };
            if (rect_contains_point(last_box_rect, g_ui_context->mouse_pos))
            {
                flags |= UI_Signal_Flag_Hovered;
                if (g_ui_context->mouse_lclick)
                    flags |= UI_Signal_Flag_LClicked;
                if (g_ui_context->mouse_rclick)
                    flags |= UI_Signal_Flag_RClicked;
            }
        }

    return flags;
}

UISignalFlags ui_checkbox(const String text, b32* check)
{
    String hash_str;
    AlignPosition inner_box_x_align = *check ? ALIGN_END : ALIGN_START;
    ui_box({ .sizing = { fixed(60), fixed(30) },
             .color = { 94, 203, 228, 255 },
             .padding = { 4, 4, 4, 4 },
             .alignment = { inner_box_x_align, ALIGN_CENTER },
             .enable_clip = True })
    {
        ui_box({ .sizing = { fixed(22), fit_grow({}) }, .color = { 251, 147, 143, 255 } })
        {
        }
        UIBox* box = ui_box_get_parent();
        hash_str = init_and_return_hash_str(box, text);
    }

    UISignalFlags flags = UI_Signal_Flag_None;
    b32 found;
    UIBox* last_box = find_or_insert_last_box_with_same_hash_str(hash_str, &found);
    if (found)
        if (last_box->key.len)
        {
            Rect last_box_rect = {
                .xmin = last_box->position.x,
                .ymin = last_box->position.y,
                .xmax = last_box->position.x + last_box->size.width,
                .ymax = last_box->position.y + last_box->size.height,
            };
            if (rect_contains_point(last_box_rect, g_ui_context->mouse_pos))
            {
                flags |= UI_Signal_Flag_Hovered;
                if (g_ui_context->mouse_lclick)
                {
                    *check = !(*check);
                    flags |= UI_Signal_Flag_LClicked;
                }
                if (g_ui_context->mouse_rclick)
                    flags |= UI_Signal_Flag_RClicked;
            }
        }

    return flags;
}
