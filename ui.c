#include "ui.h"
#include "glyph_cache.h"
#include "utils.h"
#include "win32_helper.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

#define EPSILON                   1e-4f
#define UI_CONTEXT_ARENA_CAPACITY MB(16)
#define BOX_CACHE_ARENA_CAPACITY  MB(8)
#define BOX_CACHE_CAPACITY        1024 // must be a power of two

///

#define UI_IDLE_WAKE_FRAMES 4

UIContext* g_ui_ctx = NULL;

void ui_request_frames(void)
{
    if (g_ui_ctx)
        g_ui_ctx->requested_frames = UI_IDLE_WAKE_FRAMES;
}

//
// Box cache
//

static b32 is_same_u64(const void* a, const void* b, isize size)
{
    (void)size;
    return *(const u64*)a == *(const u64*)b;
}

static void box_cache_init(UIBoxCache* box_cache)
{
    box_cache->arena = arena_new(BOX_CACHE_ARENA_CAPACITY);
    box_cache->lru_cache = lru_cache_create(&box_cache->arena, (BOX_CACHE_CAPACITY >> 1), BOX_CACHE_CAPACITY,
                                            sizeof(u64), sizeof(UIBox), fnv1a_hash, is_same_u64);
}

static void box_cache_deinit(UIBoxCache* box_cache)
{
    arena_release(&box_cache->arena);
    memset(box_cache, 0, sizeof(*box_cache));
}

typedef struct
{
    String display_str;
    String hash_str;
} TextHash;

// NOTE:
//   "hello##world"  -> { text: "hello", hash_str: "hello world" };
//   "hello###world" -> { text: "hello", hash_str: "world" };
static TextHash extract_hash_str(const String* text)
{
    TextHash text_hash = { 0 };
    for (isize i = 0; i < text->len; i++)
        if (text->data[i] == '#')
            if (text->len > i + 1)
                if (text->data[i + 1] == '#')
                {
                    // Init display string
                    text_hash.display_str = str_clone(&g_ui_ctx->arena, (String){ text->data, i });

                    if (text->len > i + 2)
                        if (text->data[i + 2] == '#')
                        {
                            String temp = str_slice(*text, i + 3, text->len);
                            text_hash.hash_str = str_clone(&g_ui_ctx->arena, temp);
                            break;
                        }
                    String str_left = { text->data, i };
                    String str_right = { text->data + i + 2, text->len - (i + 2) };
                    text_hash.hash_str = str_concat(&g_ui_ctx->arena, str_left, str_right);
                    break;
                }

    if (!text_hash.hash_str.len || text_hash.hash_str.len > HASH_STR_MAX_LENGTH)
        Assert(0);

    return text_hash;
}

static void update_box_key(UIBox* box, u64 hash)
{
    box->key_hash = hash;
}

static u64 generate_box_key(String hash_str)
{
    return fnv1a_64(hash_str.data, hash_str.len);
}

static UIBox* find_or_insert_box_with_hash(u64 hash)
{
    TracyCZoneNC(ctx_boxfind, "BoxFind", TracyColor_Cache, TRACY_SUBSYSTEMS & TracySys_Cache);
    UIBoxCache* box_cache = &g_ui_ctx->box_cache;

    LRUCacheFindOrEvictResult lru_result = lru_cache_find_or_evict(&box_cache->lru_cache, &hash);
    UIBox* last_box =
        (UIBox*)((byte*)box_cache->lru_cache.values_buf + lru_result.index * box_cache->lru_cache.value_size);

    // NOTE:
    //   Use LRUCache for its fixed-size hash table with linked-list chaining, and its
    //   insertion order (new entries appended to tail). And LRU eviction is disabled;
    //   unused boxes are explicitly dropped per frame.
    switch (lru_result.signal)
    {
        case LRU_SIGNAL_FOUND:
            // Detect duplicate box key
            if (g_ui_ctx->frame_index != 0)
                if (last_box->last_frame_index == g_ui_ctx->frame_index)
                    Assert(0);
            break;
        case LRU_SIGNAL_TOINSERT:
            last_box->key_hash = hash;
            break;
        case LRU_SIGNAL_TOEVICT:
        default:
            Assert(0);
    }

    TracyCZoneEnd(ctx_boxfind);
    return last_box;
}

static void box_cache_remove_unused()
{
    TracyCZoneNC(ctx_rmunused, "BoxCacheCleanup", TracyColor_Cache, TRACY_SUBSYSTEMS & TracySys_Cache);
    LRUCache* lru_cache = &g_ui_ctx->box_cache.lru_cache;
    u32 lru_entry_index = lru_cache->entries[0].lru_prev;
    UIBox* lru_box = (UIBox*)((byte*)lru_cache->values_buf + lru_entry_index * lru_cache->value_size);
    while (lru_box->last_frame_index < g_ui_ctx->frame_index && lru_entry_index > 0)
    {
        lru_entry_index = lru_cache->entries[lru_entry_index].lru_prev;
        lru_cache_pop_lru_entry(lru_cache);
        memset(lru_box, 0, sizeof(*lru_box));
        lru_box = (UIBox*)((byte*)lru_cache->values_buf + lru_entry_index * lru_cache->value_size);
    }
    TracyCZoneEnd(ctx_rmunused);
}

//
// Context
//

void ui_init(const HWND window, UIContext* ui_ctx, struct Renderer* renderer, GlyphRasterCache* raster_cache, u32 width,
             u32 height, u32 dpi, UIRenderFunc render_fn)
{
    ui_ctx->arena = arena_new(UI_CONTEXT_ARENA_CAPACITY);
    box_cache_init(&ui_ctx->box_cache);
    ui_ctx->window = window;
    ui_ctx->renderer = renderer;
    ui_ctx->raster_cache = raster_cache;
    ui_ctx->client_width = width;
    ui_ctx->client_height = height;
    ui_ctx->dpi = dpi;
    ui_ctx->render_fn = render_fn;
    ui_ctx->frame_index = 0;
}

void ui_deinit(UIContext* ui_ctx)
{
    box_cache_deinit(&ui_ctx->box_cache);
    arena_release(&ui_ctx->arena);
    memset(ui_ctx, 0, sizeof(*ui_ctx));
}

//
// Basic
//

static UIBox* ui_box_new()
{
    Assert(g_ui_ctx->box_queue.count < BOX_QUEUE_CAPACITY);
    return &g_ui_ctx->box_queue.items[g_ui_ctx->box_queue.count++];
}

static UIBox* ui_box_get_parent()
{
    return (g_ui_ctx->box_stack.depth > 0) ? g_ui_ctx->box_stack.items[g_ui_ctx->box_stack.depth - 1] : NULL;
}

static UIBox* ui_box_get_root()
{
    Assert(g_ui_ctx->box_queue.count > 0);
    return &g_ui_ctx->box_queue.items[0];
}

static b32 axis_has_fit_attribute(const SizingMode mode)
{
    return mode == SIZING_MODE_FIT || mode == SIZING_MODE_FIT_GROW;
}

static b32 axis_has_grow_attribute(const SizingMode mode)
{
    return mode == SIZING_MODE_GROW || mode == SIZING_MODE_FIT_GROW;
}

UIBox* ui_box_begin(const BoxConfig* cfg)
{
    Assert(g_ui_ctx->box_stack.depth <= BOX_STACK_CAPACITY);
    Assert(g_ui_ctx->box_queue.count <= BOX_QUEUE_CAPACITY);

    UIBox* box = ui_box_new();
    UIBox* parent = ui_box_get_parent();

    /* Set the parent of the current box and add the current box as a child of its parent */
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

    g_ui_ctx->box_stack.items[g_ui_ctx->box_stack.depth++] = box;
    memcpy(&box->cfg, cfg, sizeof(*cfg));
    box->flags = cfg->flags;
    box->size.width = cfg->sizing.width.mode == SIZING_MODE_FIXED ? cfg->sizing.width.min_max.min : 0;
    box->size.height = cfg->sizing.height.mode == SIZING_MODE_FIXED ? cfg->sizing.height.min_max.min : 0;
    Assert(box->size.width >= 0.f && box->size.height >= 0.f);
    if (axis_has_grow_attribute(cfg->sizing.width.mode) && cfg->sizing.width.min_max.max == 0)
        box->cfg.sizing.width.min_max.max = INFINITY;
    if (axis_has_grow_attribute(cfg->sizing.height.mode) && cfg->sizing.height.min_max.max == 0)
        box->cfg.sizing.height.min_max.max = INFINITY;

    return box;
}

// TODO: Do not need `box` argument actually ...
void ui_box_end(UIBox* box)
{
    g_ui_ctx->box_stack.items[g_ui_ctx->box_stack.depth--] = NULL;
}

static u64 text_measure_key(String text, const Font* font, f32 font_size, u32 dpi)
{
    u64 h = fnv1a_64(text.data, text.len);
    u64 font_ptr = (u64)(uintptr_t)font;
    h = fnv1a_64_continue(h, &font_ptr, sizeof(font_ptr));
    f32 fs = font_size;
    h = fnv1a_64_continue(h, &fs, sizeof(fs));
    u32 d = dpi;
    h = fnv1a_64_continue(h, &d, sizeof(d));
    return h;
}

static TextMeasureSlot* text_measure_cache_find(u64 key)
{
    u32 idx = (u32)(key % TEXT_MEASURE_CACHE_SIZE);
    for (u32 i = 0; i < TEXT_MEASURE_CACHE_SIZE; i++)
    {
        u32 probe = (idx + i) % TEXT_MEASURE_CACHE_SIZE;
        if (g_ui_ctx->text_measure_cache[probe].key == 0)
            return NULL;
        if (g_ui_ctx->text_measure_cache[probe].key == key)
            return &g_ui_ctx->text_measure_cache[probe];
    }
    return NULL;
}

static void text_measure_cache_insert(u64 key, WordBreakSlice words, f32 full_text_width, f32 min_word_width,
                                      f32 space_width, isize word_count)
{
    u32 idx = (u32)(key % TEXT_MEASURE_CACHE_SIZE);
    for (u32 i = 0; i < TEXT_MEASURE_CACHE_SIZE; i++)
    {
        u32 probe = (idx + i) % TEXT_MEASURE_CACHE_SIZE;
        if (g_ui_ctx->text_measure_cache[probe].key == 0 || g_ui_ctx->text_measure_cache[probe].key == key)
        {
            g_ui_ctx->text_measure_cache[probe].key = key;
            g_ui_ctx->text_measure_cache[probe].words = words;
            g_ui_ctx->text_measure_cache[probe].full_text_width = full_text_width;
            g_ui_ctx->text_measure_cache[probe].min_word_width = min_word_width;
            g_ui_ctx->text_measure_cache[probe].space_width = space_width;
            g_ui_ctx->text_measure_cache[probe].word_count = word_count;
            return;
        }
    }
}

UIBox* ui_text(const String text, const TextConfig* text_cfg)
{
    TracyCZoneNC(ctx_ut, "UIText", TracyColor_Text, TRACY_SUBSYSTEMS & TracySys_Text);
    GlyphRasterCache* raster_cache = g_ui_ctx->raster_cache;
    u32 dpi = g_ui_ctx->dpi;
    get_text_width_fn get_text_width = g_ui_ctx->render_fn.get_text_width;
    get_text_height_fn get_text_height = g_ui_ctx->render_fn.get_text_height;
    struct Renderer* renderer = g_ui_ctx->renderer;

    f32 base_line_height = get_text_height(renderer, raster_cache, text, text_cfg->font, text_cfg->font_size, dpi);
    f32 line_height = text_cfg->line_height > 0 ? text_cfg->line_height : base_line_height;

    f32 fixed_width;
    f32 min_width = 0;
    f32 whole_text_width;
    isize word_count = 0;
    WordBreakSlice local_words = { 0 };
    f32 space_w = 0;
    b32 cache_hit = False;
    u64 cache_key = 0;

    if (text_cfg->wrap)
    {
        cache_key = text_measure_key(text, text_cfg->font, text_cfg->font_size, dpi);
        TextMeasureSlot* slot = text_measure_cache_find(cache_key);

        if (slot)
        {
            fixed_width = slot->full_text_width;
            min_width = slot->min_word_width;
            word_count = slot->word_count;
            local_words = slot->words;
            space_w = slot->space_width;
            cache_hit = True;
        }
        else
        {
            fixed_width = get_text_width(renderer, raster_cache, text, text_cfg->font, text_cfg->font_size, dpi);
            space_w = get_text_width(renderer, raster_cache, str(" "), text_cfg->font, text_cfg->font_size, dpi);

            isize start = 0;
            const byte* ptr = text.data;
            while (ptr - text.data < text.len)
            {
                UnicodeDecode res_current = utf8_decode(ptr);
                const byte* next = res_current.next_p;
                if (res_current.codepoint == ' ' || res_current.codepoint > 127)
                {
                    UnicodeDecode res_start = utf8_decode(&text.data[start]);
                    if (res_start.codepoint != ' ')
                    {
                        isize end = res_start.codepoint < 127 ? ptr - text.data : next - text.data;
                        f32 word_width = get_text_width(renderer, raster_cache, str_slice(text, start, end),
                                                        text_cfg->font, text_cfg->font_size, dpi);

                        WordBreak* wb = slice_push(&g_ui_ctx->arena, &local_words);
                        wb->byte_start = start;
                        wb->byte_end = end;
                        wb->width = word_width;

                        min_width = max(min_width, word_width);
                        word_count++;
                    }
                    start = next - text.data;
                }
                ptr = next;
            }

            /* Handle last word */
            if (start < text.len && text.data[start] != ' ')
            {
                f32 word_width = get_text_width(renderer, raster_cache, str_slice(text, start, text.len),
                                                text_cfg->font, text_cfg->font_size, dpi);

                WordBreak* wb = slice_push(&g_ui_ctx->arena, &local_words);
                wb->byte_start = start;
                wb->byte_end = text.len;
                wb->width = word_width;

                min_width = max(min_width, word_width);
                word_count++;
            }
        }
        whole_text_width = fixed_width;
        min_width = (min_width != 0) ? min_width : whole_text_width;
    }
    else
    {
        fixed_width = get_text_width(renderer, raster_cache, text, text_cfg->font, text_cfg->font_size, dpi);
        whole_text_width = fixed_width;
        min_width = fixed_width;
        word_count = 1;
    }

    BoxConfig box_cfg = { .sizing = { .width = { { fixed_width, fixed_width }, SIZING_MODE_FIXED },
                                      .height = { { line_height, line_height }, SIZING_MODE_FIXED } } };

    UIBox* text_box = ui_box_begin(&box_cfg);
    {
        text_box->type = BOX_TYPE_TEXT;

        Assert(text_cfg->font->face3);
        Assert(text_cfg->font_size);
        text_box->data.text.font = text_cfg->font;
        text_box->data.text.font_size = text_cfg->font_size;

        text_box->data.text.content = text;
        text_box->data.text.color = text_cfg->color;
        memset(&text_box->data.text.wrapped_lines, 0, sizeof(text_box->data.text.wrapped_lines));
        text_box->data.text.line_count = 1;
        text_box->data.text.line_height = line_height;
        text_box->data.text.half_leading = (line_height - base_line_height) / 2.0f;
        text_box->data.text.words = local_words;
        text_box->data.text.space_width = space_w;
        text_box->data.text.full_text_width = fixed_width;

        if (text_cfg->wrap && !cache_hit && local_words.len > 0)
            text_measure_cache_insert(cache_key, local_words, fixed_width, min_width, space_w, word_count);
    }

    text_box->cfg.sizing.width.min_max.min = min_width;
    text_box->cfg.sizing.width.min_max.max = whole_text_width;
    text_box->cfg.sizing.height.min_max.min = (f32)text_box->data.text.line_height;
    text_box->cfg.sizing.height.min_max.max = (f32)text_box->data.text.line_height * word_count;

    ui_box_end(text_box);
    TracyCZoneEnd(ctx_ut);
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

static AxisContext get_axis_ctx(UIBox* box, const Axis axis)
{
    AxisContext ctx = { 0 };
    if (axis == WIDTH)
    {
        ctx.size = &box->size.width;
        ctx.min_size = &box->cfg.sizing.width.min_max.min;
        ctx.max_size = &box->cfg.sizing.width.min_max.max;
        ctx.remaining = &box->data.container.remaining_space.width;
        ctx.padding_start = box->cfg.padding.left;
        ctx.padding_end = box->cfg.padding.right;
        ctx.sizing_mode = box->cfg.sizing.width.mode;
        ctx.main_direction = LAYOUT_LEFT_TO_RIGHT;
    }
    else
    {
        ctx.size = &box->size.height;
        ctx.min_size = &box->cfg.sizing.height.min_max.min;
        ctx.max_size = &box->cfg.sizing.height.min_max.max;
        ctx.remaining = &box->data.container.remaining_space.height;
        ctx.padding_start = box->cfg.padding.top;
        ctx.padding_end = box->cfg.padding.bottom;
        ctx.sizing_mode = box->cfg.sizing.height.mode;
        ctx.main_direction = LAYOUT_TOP_TO_BOTTOM;
    }
    return ctx;
}

// Recursively calculate sizes for boxes cfg with 'fit' attribute
static void ui_box_calculate_fit_axis(UIBox* box, const Axis axis)
{
    TracyCZoneNC(ctx_fit, "FitAxis", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
    AxisContext box_ctx = get_axis_ctx(box, axis);

    f32 box_ctx_min_size_backup = *box_ctx.min_size;
    if (axis_has_fit_attribute(box_ctx.sizing_mode))
    {
        isize child_gap_count = box->data.container.child_count - 1;
        *box_ctx.size += box_ctx.padding_start + box_ctx.padding_end;
        *box_ctx.min_size = 0;
        *box_ctx.min_size += box_ctx.padding_start + box_ctx.padding_end;
        if (box->cfg.direction == box_ctx.main_direction)
        {
            *box_ctx.size += box->cfg.child_gap * child_gap_count;
            *box_ctx.min_size += box->cfg.child_gap * child_gap_count;
        }
    }

    /* Recursively resolve child box sizes (depth-first, reverse order) */
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_calculate_fit_axis(child, axis);
            child = child->next;
        }
    }

    /* Select a bigger min size if box axis has both fit attribute and min size cfg
    And clamp box axis size to less than box axis max size */
    if (axis_has_fit_attribute(box_ctx.sizing_mode))
    {
        if (box_ctx_min_size_backup > *box_ctx.min_size)
            *box_ctx.min_size = box_ctx_min_size_backup;
        if (*box_ctx.size < *box_ctx.min_size)
            *box_ctx.size = *box_ctx.min_size;
        if (*box_ctx.max_size != 0 && *box_ctx.max_size != INFINITY && *box_ctx.size > *box_ctx.max_size)
            *box_ctx.size = *box_ctx.max_size;
    }
    TracyCZoneEnd(ctx_fit);

    /* If the current box is a child, adjust its parent's size based on box direction. */
    UIBox* parent = box->parent;
    if (parent && !(box->flags & BoxFlag_Float))
    {
        AxisContext parent_ctx = get_axis_ctx(parent, axis);
        if (axis_has_fit_attribute(parent_ctx.sizing_mode))
        {
            if (parent->cfg.direction == parent_ctx.main_direction)
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

typedef struct
{
    f32* value;
    f32* limit;
} DistributeAble;

typedef Slice(DistributeAble) DistributeAbles;

static void distribute_axis(f32* remaining, DistributeAbles ables, const DistributeMode mode)
{
    while ((mode == GROW && *remaining > EPSILON) || (mode == SHRINK && *remaining < -EPSILON))
    {
        if (ables.len == 0)
            break;

        f32 pivot = *ables.data[0].value;
        f32 second_pivot = mode == GROW ? INFINITY : 0;
        f32 to_add = 0;

        /* Find the pivot size (smallest for grow, largest for shrink) and the next pivot. */
        for (isize i = 0; i < ables.len; i++)
        {
            if (mode == GROW ? (*ables.data[i].value < pivot) : (*ables.data[i].value > pivot))
            {
                second_pivot = pivot;
                pivot = *ables.data[i].value;
                to_add = second_pivot - pivot;
            }
            else if (mode == GROW ? (*ables.data[i].value > pivot) : (*ables.data[i].value < pivot))
            {
                second_pivot =
                    mode == GROW ? min(*ables.data[i].value, second_pivot) : max(*ables.data[i].value, second_pivot);
                to_add = second_pivot - pivot;
            }
        }

        /* If all items have the same size, distribute remaining space equally. */
        if ((mode == GROW && second_pivot == INFINITY) || (mode == SHRINK && second_pivot == 0))
            to_add = *remaining / ables.len;

        /* Count items at the pivot and adjust to_add if it would "exceed" remaining space. */
        isize pivot_count = 0;
        for (isize i = 0; i < ables.len; i++)
            if (fabsf(*ables.data[i].value - pivot) < EPSILON)
                pivot_count++;
        if (mode == GROW ? (to_add * pivot_count > *remaining) : (to_add * pivot_count < *remaining))
            to_add = *remaining / pivot_count;

        /* Distribute space among items at the pivot size.
        When an item hits its limit, clamp it and remove from further distribution. */
        for (isize i = 0; i < ables.len; i++)
        {
            if (fabsf(*ables.data[i].value - pivot) >= EPSILON)
                continue;

            f32 size_backup = *ables.data[i].value;
            f32* child = ables.data[i].value;
            f32 limit = *ables.data[i].limit;

            *child += to_add;
            if (mode == GROW ? (*child >= limit) : (*child <= limit))
            {
                *child = limit;
                ables.data[i].value = ables.data[ables.len - 1].value;
                ables.data[i].limit = ables.data[ables.len - 1].limit;
                ables.len--;
                i--;
                continue;
            }
            *remaining -= (*child - size_backup);
        }
    }
}

// Recursively grow/shrink axis size of box
static void ui_box_grow_shrink_children_axis(UIBox* box, const Axis axis)
{
    TracyCZoneNC(ctx_gs, "GrowShrinkAxis", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
    if (box->type != BOX_TYPE_CONTAINER)
    {
        TracyCZoneEnd(ctx_gs);
        return;
    }

    isize arena_pos_backup = g_ui_ctx->arena.pos;
    {
        isize children_count = box->data.container.child_count;
        AxisContext ctx = get_axis_ctx(box, axis);
        *ctx.remaining = *ctx.size;

        /* Subtract padding and child gap */
        // TODO: When the window is resized to very small, the root box's remaining will be less
        // than padding, then `Assert` broken.
        *ctx.remaining -= ctx.padding_start + ctx.padding_end;
        // Assert(*ctx.remaining >= 0); // TODO: important?
        isize child_gap_count = children_count - 1;
        if (box->cfg.direction == ctx.main_direction)
            *ctx.remaining -= box->cfg.child_gap * child_gap_count;

        /* Subtract the childrens' determined size from the parent's remaining space. */
        UIBox* child = box->child_first;
        while (child)
        {
            if (child->flags & BoxFlag_Float)
                *ctx.remaining += box->cfg.child_gap;

            AxisContext child_ctx = get_axis_ctx(child, axis);

            /* Subtract the child's determined size from the parent's remaining space. */
            if (box->cfg.direction == ctx.main_direction)
                if (!(child->flags & BoxFlag_Float))
                    *ctx.remaining -= *child_ctx.size;

            child = child->next;
        }

        /* Distribute remaining space to children (growables and shrinkables are mutually exclusive) */
        DistributeAbles growables = { 0 };
        DistributeAbles shrinkables = { 0 };
        child = box->child_first;
        while (child)
        {
            AxisContext child_ctx = get_axis_ctx(child, axis);
            if (axis_has_grow_attribute(child_ctx.sizing_mode))
            {
                DistributeAble growable = { child_ctx.size, child_ctx.max_size };
                *slice_push(&g_ui_ctx->arena, &growables) = growable;
            }

            if (*child_ctx.size > *child_ctx.min_size)
            {
                DistributeAble shrinkable = { child_ctx.size, child_ctx.min_size };
                *slice_push(&g_ui_ctx->arena, &shrinkables) = shrinkable;
            }
            child = child->next;
        }
        if (box->cfg.direction == ctx.main_direction)
        {
            if (*ctx.remaining > 1) // NOTE: Use 1 instead of 0 to avoid distributing meaningless slivers
                distribute_axis(ctx.remaining, growables, GROW);
            else if (*ctx.remaining < 0)
                distribute_axis(ctx.remaining, shrinkables, SHRINK);
        }
        else
        {
            if (*ctx.remaining > 0)
                for (isize i = 0; i < growables.len; i++)
                    *growables.data[i].value =
                        max(*growables.data[i].value, min(*ctx.remaining, *growables.data[i].limit));
            for (isize i = 0; i < shrinkables.len; i++)
                if (*ctx.remaining < *shrinkables.data[i].value)
                    *shrinkables.data[i].value = max(*shrinkables.data[i].limit, *ctx.remaining);
        }
    }
    g_ui_ctx->arena.pos = arena_pos_backup;

    /* Recursively resolve size (breadth first) */
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_box_grow_shrink_children_axis(child, axis);
            child = child->next;
        }
    }
    TracyCZoneEnd(ctx_gs);
}

static void ui_box_resolve_position(UIBox* box)
{
    TracyCZoneNC(ctx_pos, "ResolvePos", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
    if (box->parent)
    {
        UIBox* parent = box->parent;
        ContainerData* parent_data = &parent->data.container;
        box->position.x += parent->position.x + parent->cfg.padding.left;
        box->position.y += parent->position.y + parent->cfg.padding.top;
        if (parent->cfg.direction == LAYOUT_LEFT_TO_RIGHT)
        {
            box->position.x += parent_data->next_child_offset_x;
            if (!(box->flags & BoxFlag_Float))
                parent_data->next_child_offset_x += box->size.width + parent->cfg.child_gap;

            if (parent->cfg.alignment.x == ALIGN_CENTER)
                box->position.x += parent->data.container.remaining_space.width / 2;
            else if (parent->cfg.alignment.x == ALIGN_END)
                box->position.x += parent->data.container.remaining_space.width;

            if (parent->cfg.alignment.y == ALIGN_CENTER)
                box->position.y += (parent->data.container.remaining_space.height - box->size.height) / 2;
            else if (parent->cfg.alignment.y == ALIGN_END)
                box->position.y += parent->data.container.remaining_space.height - box->size.height;
        }
        else
        {
            box->position.y += parent_data->next_child_offset_y;
            if (!(box->flags & BoxFlag_Float))
                parent_data->next_child_offset_y += box->size.height + parent->cfg.child_gap;

            if (parent->cfg.alignment.y == ALIGN_CENTER)
                box->position.y += parent->data.container.remaining_space.height / 2;
            else if (parent->cfg.alignment.y == ALIGN_END)
                box->position.y += parent->data.container.remaining_space.height;

            if (parent->cfg.alignment.x == ALIGN_CENTER)
                box->position.x += (parent->data.container.remaining_space.width - box->size.width) / 2;
            else if (parent->cfg.alignment.x == ALIGN_END)
                box->position.x += parent->data.container.remaining_space.width - box->size.width;
        }

        /* Handle scroll & float offset */
        box->position.x += parent->cfg.child_offset.x + ((box->flags & BoxFlag_Float) ? box->cfg.float_offset.x : 0.f);
        box->position.y += parent->cfg.child_offset.y + ((box->flags & BoxFlag_Float) ? box->cfg.float_offset.y : 0.f);
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

    /* Update the box size and position in box cache if the box has a hash */
    if (box->key_hash)
    {
        UIBox* last_box = find_or_insert_box_with_hash(box->key_hash);
        Assert(last_box);
        last_box->size = box->size;
        last_box->position = box->position;

        // NOTE:
        //   Update `last_frame_index` here because this is the final box that will be used for the current frame.
        //   This enables duplicate detection: during lookup, if a box is already marked with the current frame index,
        //   it indicates a duplicate key (panic condition).
        last_box->last_frame_index = g_ui_ctx->frame_index;
    }
    TracyCZoneEnd(ctx_pos);
}

static void perform_text_wrapping(UIBox* text_box)
{
    TracyCZoneNC(ctx_wp, "TextWrap", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);

    TextData* td = &text_box->data.text;
    String text = td->content;
    f32 max_width = text_box->size.width;

    if (td->full_text_width <= max_width || td->words.len == 0)
    {
        TracyCZoneEnd(ctx_wp);
        return;
    }

    WordBreak* words = td->words.data;
    isize word_count = td->words.len;
    f32 space_width = td->space_width;

    /* Reset wrapped lines (arena-allocated, cleared each frame) */
    td->wrapped_lines.len = 0;

    f32 line_width = 0;
    isize line_word_start = 0;

    for (isize i = 0; i < word_count; i++)
    {
        f32 w_width = words[i].width;
        if (i > line_word_start)
        {
            isize gap = words[i].byte_start - words[i - 1].byte_end;
            w_width += (f32)gap * space_width;
        }

        if (line_width + w_width > max_width && i > line_word_start)
        {
            isize emit_start = words[line_word_start].byte_start;
            isize emit_end = words[i - 1].byte_end;
            *slice_push(&g_ui_ctx->arena, &td->wrapped_lines) = str_slice(text, emit_start, emit_end);
            line_word_start = i;
            line_width = words[i].width;
        }
        else
        {
            line_width += w_width;
        }
    }

    if (line_word_start < word_count)
    {
        *slice_push(&g_ui_ctx->arena, &td->wrapped_lines) =
            str_slice(text, words[line_word_start].byte_start, text.len);
    }

    td->line_count = td->wrapped_lines.len;
    text_box->size.height = td->line_height * (f32)td->wrapped_lines.len;
    text_box->cfg.sizing.height.min_max.min = text_box->size.height;
    TracyCZoneEnd(ctx_wp);
}

static void ui_box_apply_text_wrapping(UIBox* box)
{
    TracyCZoneNC(ctx_wrap, "WrapTree", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
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
    TracyCZoneEnd(ctx_wrap);
}

static void ui_calculate_layout(UIBox* box)
{
    TracyCZoneNC(ctx_layout, "CalcLayout", TracyColor_Layout, TRACY_SUBSYSTEMS & TracySys_Layout);
    ui_box_calculate_fit_axis(box, WIDTH);
    ui_box_grow_shrink_children_axis(box, WIDTH);
    ui_box_apply_text_wrapping(box);
    ui_box_calculate_fit_axis(box, HEIGHT);
    ui_box_grow_shrink_children_axis(box, HEIGHT);
    ui_box_resolve_position(box);
    TracyCZoneEnd(ctx_layout);
}

//
// Frame
//

static f64 get_current_time(u64 frame_index)
{
    static LARGE_INTEGER freq;
    static LARGE_INTEGER current_time;

    if (frame_index == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&current_time);
    return (f64)current_time.QuadPart / freq.QuadPart;
}

isize ui_frame_begin(UIContext* ui_ctx)
{
    TracyCZoneNC(ctx_fb, "FrameBegin", TracyColor_Frame, TRACY_SUBSYSTEMS & TracySys_Frame);
    ui_ctx->prev_ctx = g_ui_ctx;
    g_ui_ctx = ui_ctx;
    if (g_ui_ctx->frame_index > 0)
        g_ui_ctx->render_fn.wait_for_last_submitted_frame(g_ui_ctx->renderer);
    f64 last_time = g_ui_ctx->current_time;
    g_ui_ctx->current_time = get_current_time(g_ui_ctx->frame_index);
    g_ui_ctx->frame_delta_time = (f32)(g_ui_ctx->current_time - last_time);
    g_ui_ctx->desired_cursor = UI_CURSOR_ARROW;
    g_ui_ctx->drag_payload_consumed = False;
    memset(g_ui_ctx->text_measure_cache, 0, sizeof(g_ui_ctx->text_measure_cache));

    TracyCZoneEnd(ctx_fb);
    return g_ui_ctx->arena.pos;
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
    TracyCZoneNC(ctx_rc, "GenRenderCmds", TracyColor_Render, TRACY_SUBSYSTEMS & TracySys_Render);
    f32 dpi_scale = (f32)g_ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;
    Rect rect = {
        box->position.x * dpi_scale,
        box->position.y * dpi_scale,
        (box->position.x + box->size.width) * dpi_scale,
        (box->position.y + box->size.height) * dpi_scale,
    };
    RectStyle rect_style = { .border_color = box->cfg.rect_style.border_color,
                             .border_thickness = box->cfg.rect_style.border_thickness * dpi_scale,
                             .shadow_color = box->cfg.rect_style.shadow_color,
                             .shadow_offset = {
                                 box->cfg.rect_style.shadow_offset.x * dpi_scale,
                                 box->cfg.rect_style.shadow_offset.y * dpi_scale,
                             },
                             .shadow_sigma = box->cfg.rect_style.shadow_sigma * dpi_scale,
                             .corner_radius = box->cfg.rect_style.corner_radius * dpi_scale,
                             .corner_colors = {
                                 box->cfg.rect_style.corner_colors[0],
                                 box->cfg.rect_style.corner_colors[1],
                                 box->cfg.rect_style.corner_colors[2],
                                 box->cfg.rect_style.corner_colors[3],
                             },
                             .shear = box->cfg.rect_style.shear * dpi_scale };

    /* Store clip rect (ancestor restrictions) for interaction culling */
    if (box->key_hash)
    {
        u64 key = box->key_hash;
        LRUCacheFindResult find_result = lru_cache_find(&g_ui_ctx->box_cache.lru_cache, &key);
        if (find_result.found)
        {
            UIBox* last_box = (UIBox*)((byte*)g_ui_ctx->box_cache.lru_cache.values_buf +
                                       find_result.index * g_ui_ctx->box_cache.lru_cache.value_size);
            last_box->clip = (Rect){
                .xmin = clip.xmin / dpi_scale,
                .ymin = clip.ymin / dpi_scale,
                .xmax = clip.xmax / dpi_scale,
                .ymax = clip.ymax / dpi_scale,
            };
        }
    }

    // clang-format off
    /* Draw box rect/text */
    switch (box->type)
    {
        case BOX_TYPE_CONTAINER:
        {
            UICommand* cmd = g_ui_ctx->command_queue.items + g_ui_ctx->command_queue.count++;

            cmd->rect.base.type = UI_COMMAND_RECT;
            cmd->rect.base.size = sizeof(UICommandRect);
            cmd->rect.rect      = rect;
            cmd->rect.color     = box->cfg.color;
            cmd->rect.style     = rect_style;
            cmd->rect.clip      = clip;
        }
        break;
        case BOX_TYPE_TEXT:
        {
            isize i = 0;
            do
            {
                UICommand* cmd = g_ui_ctx->command_queue.items + g_ui_ctx->command_queue.count++;
                const TextData* text = &box->data.text;

                cmd->text.base.type  = UI_COMMAND_TEXT;
                cmd->text.base.size  = sizeof(UICommandText);
                cmd->text.font       = text->font;
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

    /* If clip is enabled, push a clip */
    Rect new_clip = clip;
    if (box->cfg.flags & BoxFlag_Clip)
        new_clip = intersect_rects(clip, rect);

    /* Recursive child boxes */
    if (box->type == BOX_TYPE_CONTAINER)
    {
        UIBox* child = box->child_first;
        while (child)
        {
            ui_generate_render_commands(child, new_clip);
            child = child->next;
        }
    }
    TracyCZoneEnd(ctx_rc);
}

void ui_frame_end(isize arena_pos_backup)
{
    TracyCZoneNC(ctx_fe, "FrameEnd", TracyColor_Frame, TRACY_SUBSYSTEMS & TracySys_Frame);
    GlyphRasterCache* raster_cache = g_ui_ctx->raster_cache;
    u32 dpi = g_ui_ctx->dpi;
    f32 dpi_scale = (f32)dpi / USER_DEFAULT_SCREEN_DPI;
    Rect no_clip = { 0, 0, g_ui_ctx->client_width * dpi_scale, g_ui_ctx->client_height * dpi_scale };
    UIRenderFunc* render_fn = &g_ui_ctx->render_fn;

    g_ui_ctx->root = ui_box_get_root();
    ui_calculate_layout(g_ui_ctx->root);
    ui_generate_render_commands(g_ui_ctx->root, no_clip);

    /* Draw */
    for (isize i = 0; i < g_ui_ctx->command_queue.count; i++)
    {
        Rect* clip;
        UICommand* cmd = &g_ui_ctx->command_queue.items[i];
        switch (cmd->type)
        {
            case UI_COMMAND_RECT:
                clip = memcmp(&cmd->rect.clip, &no_clip, sizeof(no_clip)) == 0 ? NULL : &cmd->rect.clip;
                render_fn->draw_rect(g_ui_ctx->renderer, cmd->rect.rect, cmd->rect.color, cmd->rect.style, clip);
                break;
            case UI_COMMAND_TEXT:
                UICommandText* text = &cmd->text;
                clip = memcmp(&cmd->text.clip, &no_clip, sizeof(no_clip)) == 0 ? NULL : &cmd->text.clip;
                render_fn->draw_text(g_ui_ctx->renderer, raster_cache, text->content, text->position, text->color,
                                     text->font, text->font_size, dpi, clip);
                break;
            default:
                Assert(0);
        }
    }

    /* Present */
    u32 physical_client_width = (u32)(g_ui_ctx->client_width * dpi_scale);
    u32 physical_client_height = (u32)(g_ui_ctx->client_height * dpi_scale);
    g_ui_ctx->render_fn.flush_and_present(g_ui_ctx->renderer, physical_client_width, physical_client_height);

    /* Reset state */
    Assert(g_ui_ctx->box_stack.depth == 0);
    memset(&g_ui_ctx->box_queue, 0, sizeof(g_ui_ctx->box_queue));
    memset(&g_ui_ctx->command_queue, 0, sizeof(g_ui_ctx->command_queue));
    box_cache_remove_unused();

    g_ui_ctx->mouse_lclick = False;
    g_ui_ctx->mouse_rclick = False;
    g_ui_ctx->mouse_mclick = False;
    g_ui_ctx->mouse_double_click = False;
    g_ui_ctx->mouse_delta = (Position){ 0.f, 0.f };
    g_ui_ctx->mouse_scroll_delta = (Position){ 0.f, 0.f };
    g_ui_ctx->char_input_queue_count = 0;
    g_ui_ctx->text_action_queue_count = 0;

    /* drag-drop */
    if (!g_ui_ctx->mouse_press)
    {
        if (g_ui_ctx->drag_active)
        {
            g_ui_ctx->drag_active = False;
            g_ui_ctx->drag_payload_size = 0;
            g_ui_ctx->drag_source_box = NULL;
        }
        g_ui_ctx->mouse_captured_by_hash = 0;
    }

    g_ui_ctx->frame_index++;
    arena_pop_to(&g_ui_ctx->arena, arena_pos_backup);
    TracyCZoneEnd(ctx_fe);
    g_ui_ctx = g_ui_ctx->prev_ctx;
}

//
// Cursor
//

void ui_set_desired_cursor(Cursor shape)
{
    g_ui_ctx->desired_cursor = shape;
}

//
// Interaction
//

static b32 rect_contains_point(Rect r, Position p)
{
    return p.x >= r.xmin && p.x < r.xmax && p.y >= r.ymin && p.y < r.ymax;
}

static void update_interaction_flags(UIBox* box, UISignalFlags* flags)
{
    TracyCZoneNC(ctx_interact, "Interaction", TracyColor_Interaction, TRACY_SUBSYSTEMS & TracySys_Interaction);
    *flags = UI_Signal_Flag_None;
    Rect box_rect = {
        .xmin = box->position.x,
        .ymin = box->position.y,
        .xmax = box->position.x + box->size.width,
        .ymax = box->position.y + box->size.height,
    };

    /* Was the mouse pressed on this box in a prior frame? */
    b32 has_mouse_anchor = box->drag_mouse_anchor.x != 0.f || box->drag_mouse_anchor.y != 0.f;

    /* Release must be detected even when not hovering */
    if (!g_ui_ctx->mouse_press && has_mouse_anchor)
    {
        *flags |= UI_Signal_Flag_Released;
        box->drag_mouse_anchor = (Position){ 0 };
    }

    b32 in_box = rect_contains_point(box_rect, g_ui_ctx->mouse_pos);
    b32 in_clip = rect_contains_point(box->clip, g_ui_ctx->mouse_pos);
    if (in_box)
    {
        /* Reject interactions outside the ancestor clip region (e.g. scrolled out of view) */
        if (in_clip)
        {
            *flags |= UI_Signal_Flag_Hovered;
            if (g_ui_ctx->mouse_lclick)
                *flags |= UI_Signal_Flag_LClicked;
            if (g_ui_ctx->mouse_rclick)
                *flags |= UI_Signal_Flag_RClicked;
            if (g_ui_ctx->mouse_mclick)
                *flags |= UI_Signal_Flag_MClicked;
            if (g_ui_ctx->mouse_press)
                *flags |= UI_Signal_Flag_Pressed;
            if (g_ui_ctx->mouse_double_click)
                *flags |= UI_Signal_Flag_DoubleClicked;

            /* Drag: record mouse anchor on press (must be over the box to start drag) */
            if (g_ui_ctx->mouse_lclick)
                box->drag_mouse_anchor = g_ui_ctx->mouse_pos;

            /* DragOver: a drag payload is active and the mouse is over this (non-source) box */
            if (g_ui_ctx->drag_active && box != g_ui_ctx->drag_source_box)
                *flags |= UI_Signal_Flag_DragOver;
        }
    }

    /* Drag: signal Dragging when mouse moves while held, even outside the box */
    if (g_ui_ctx->mouse_press && has_mouse_anchor)
    {
        Position delta = {
            g_ui_ctx->mouse_pos.x - box->drag_mouse_anchor.x,
            g_ui_ctx->mouse_pos.y - box->drag_mouse_anchor.y,
        };
        if (delta.x != 0.f || delta.y != 0.f)
        {
            *flags |= UI_Signal_Flag_Dragging;
            /* First box to report dragging becomes the drag source */
            if (!g_ui_ctx->drag_active)
            {
                g_ui_ctx->drag_active = True;
                g_ui_ctx->drag_source_box = box;
            }
        }
    }

    /* Dropped: release while a drag was active, over this box */
    if (!g_ui_ctx->mouse_press && g_ui_ctx->drag_active && in_box && in_clip)
        *flags |= UI_Signal_Flag_Dropped;
    TracyCZoneEnd(ctx_interact);
}

UIBoxInteractResult ui_box_interact(UIBox* box, const String hash_str)
{
    u64 hash = fnv1a_64(hash_str.data, hash_str.len);
    UIBoxInteractResult result = { .flags = UI_Signal_Flag_None, .last_box = NULL };
    result.last_box = find_or_insert_box_with_hash(hash);
    update_interaction_flags(result.last_box, &result.flags);
    update_box_key(box, hash);
    return result;
}

Position ui_box_drag_delta(const UIBox* box)
{
    Position delta = { 0 };
    if (g_ui_ctx->mouse_press && (box->drag_mouse_anchor.x != 0.f || box->drag_mouse_anchor.y != 0.f))
    {
        delta.x = g_ui_ctx->mouse_pos.x - box->drag_mouse_anchor.x;
        delta.y = g_ui_ctx->mouse_pos.y - box->drag_mouse_anchor.y;
    }
    return delta;
}

void ui_set_drag_payload(void* payload, isize size)
{
    Assert(size <= DRAG_PAYLOAD_MAX);
    memcpy(g_ui_ctx->drag_payload_buf, payload, (size_t)size);
    g_ui_ctx->drag_payload_size = size;
}

//
// Animation
//

// Linear interpolation
static u8 lerp_u8(u8 a, u8 b, f32 t)
{
    return (u8)(a + (b - a) * t);
}

static f32 lerp_f32(f32 a, f32 b, f32 t)
{
    return a + (b - a) * t;
}

Color lerp_color(const Color a, const Color b, const f32 t)
{
    Color c = {
        .r = lerp_u8(a.r, b.r, t), .g = lerp_u8(a.g, b.g, t), .b = lerp_u8(a.b, b.b, t), .a = lerp_u8(a.a, b.a, t)
    };
    return c;
}

b32 update_transition(f32* value, const f32 target, const f32 speed)
{
    // Clamp animation timestep to avoid huge dt spikes after idle/sleep.
    f32 anim_dt = min(g_ui_ctx->frame_delta_time, 1.f / 60.f);

    *value += (target - *value) * min(speed * anim_dt, 1.f);
    b32 is_done = fabs(*value - target) < 0.001f;
    if (is_done)
        *value = target;
    else
        ui_request_frames();
    return is_done;
}

static void start_timed_lerp(TimedLerpAnimation* anim, f32 current_pos, f32 new_target, f64 now, f32 duration)
{
    anim->start = current_pos;
    anim->target = new_target;
    anim->started_at = now;
    anim->duration = duration;
}

static f32 evaluate_timed_lerp(const TimedLerpAnimation* anim, f64 now)
{
    Assert(anim->duration >= 0.f);
    if (anim->duration > 0.f)
    {
        f32 progress = (f32)((now - anim->started_at) / anim->duration); // range: [0, 1]
        if (progress >= 1.0)
            return anim->target;
        return anim->start + (anim->target - anim->start) * progress;
    }
    return anim->target;
}

//
// Scroll Area
//

// Diagram:
//   Scroll (LAYOUT_LEFT_TO_RIGHT): {
//   |   Container (LAYOUT_TOP_TO_BOTTOM): {
//   |   |   Inner Container (used for setting back to LAYOUT_LEFT_TO_RIGHT): {
//   |   |   | Content
//   |   |   },
//   |   |   Scrollbar (horizontal)
//   |   },
//   |   Scrollbar (vertical)
//   }
//
//  <--------- Scroll ---------->
//  ┏━━━━━━━━━━━━━━━━━━━━━━━━━┯━┓<-- Scrollar (vertical) [float]
//  ┃ ╔═════════════════════╗ | ┃
//  ┃ ║ +-----------------+ ║ | ┃
//  ┃ ║ |                 | ║ |█┃
//  ┃ ║ |     Content     | ║ |█┃<--- Thumb (vertical)
//  ┃ ║ |                 | ║ | ┃
//  ┃ ║ +-----------------+ ║ | ┃
//  ┃ ║   Inner Container   ║ | ┃
//  ┃ ╚═════════════════════╝ | ┃
//  ┠-------------------------+-┨  And...
//  ┃            ▓▓▓▓▓▓▓      | ┃    Scrollbar (horizontal) [float]
//  ┗━━━━━━━━━━━━━━━━━━━━━━━━━┷━┛    Thumb (horizontal)
//  <-------- Container -------->

#define SCROLLBAR_THICKNESS_MIN          5
#define SCROLLBAR_THICKNESS_MAX          8.5
#define SCROLLBAR_PADDING_END_MIN        SCROLLBAR_THICKNESS_MIN * 0.5
#define SCROLLBAR_PADDING_END_MAX        SCROLLBAR_THICKNESS_MIN
#define SCROLLBAR_SPACER                 (SCROLLBAR_THICKNESS_MIN * 2.25)
#define SCROLLBAR_THUMB_OPACITY_TRACK    0.4f
#define SCROLLBAR_THUMB_OPACITY_INACTIVE 0.5f // range: [0, 1]
#define SCROLLBAR_THUMB_OPACITY_HOVER    0.75f
#define SCROLL_SENSITIVITY               4.f
#define SCROLL_ANIM_DURATION             0.09f

typedef enum
{
    // clang-format off
    SCROLLBAR_NONE       = 0b00,
    SCROLLBAR_HORIZONTAL = 0b01,
    SCROLLBAR_VERTICAL   = 0b10,
    SCROLLBAR_BOTH       = 0b11,
    // clang-format on
} ScrollBarFlags;

typedef struct
{
    Sizing container_sizing;
    LayoutDirection padding_direction;
    Sizing padding_start_sizing;
    Sizing padding_end_sizing;
    Sizing inner_container_sizing;
    LayoutDirection spacer_direction;
    Sizing spacer_sizing;
    Sizing track_sizing;
    Sizing thumb_container_sizing;
    Position thumb_container_child_offset;
    Sizing thumb_sizing;
    Position float_offset;
} ScrollBarLayout;

static ScrollBarLayout make_scrollbar_layout(b32 is_horizontal, f32 thumb_thickness, f32 padding_end,
                                             f32 bar_thickness_max, f32 thumb_size, Position delta)
{
    if (is_horizontal)
        return (ScrollBarLayout){
            .container_sizing = { grow({}), fixed(bar_thickness_max) },
            .float_offset = { 0.f, -bar_thickness_max },
            .padding_direction = LAYOUT_TOP_TO_BOTTOM,
            .padding_start_sizing = { grow({}), fixed(bar_thickness_max - thumb_thickness - padding_end) },
            .padding_end_sizing = { grow({}), fixed(padding_end) },
            .inner_container_sizing = { grow({}), fixed(thumb_thickness) },
            .spacer_direction = LAYOUT_LEFT_TO_RIGHT,
            .spacer_sizing = { fixed(SCROLLBAR_SPACER), fixed(thumb_thickness) },
            .track_sizing = { grow({}), fixed(thumb_thickness) },
            .thumb_container_sizing = { grow({}), fixed(thumb_thickness) },
            .thumb_container_child_offset = { delta.x, 0 },
            .thumb_sizing = { fixed(thumb_size), fixed(thumb_thickness) },
        };
    return (ScrollBarLayout){
        .container_sizing = { fixed(bar_thickness_max), grow({}) },
        .float_offset = { -bar_thickness_max, 0.f },
        .padding_direction = LAYOUT_LEFT_TO_RIGHT,
        .padding_start_sizing = { fixed(bar_thickness_max - thumb_thickness - padding_end), grow({}) },
        .padding_end_sizing = { fixed(padding_end), grow({}) },
        .inner_container_sizing = { fixed(thumb_thickness), grow({}) },
        .spacer_direction = LAYOUT_TOP_TO_BOTTOM,
        .spacer_sizing = { fixed(thumb_thickness), fixed(SCROLLBAR_SPACER) },
        .track_sizing = { fixed(thumb_thickness), grow({}) },
        .thumb_container_sizing = { fixed(thumb_thickness), grow({}) },
        .thumb_container_child_offset = { 0, delta.y },
        .thumb_sizing = { fixed(thumb_thickness), fixed(thumb_size) },
    };
}

static void scrollbar(ScrollContext scroll_ctx, const b32 is_horizontal, const f32 thumb_size)
{
    TracyCZoneNC(ctx_sb, "Scrollbar", TracyColor_Scroll, TRACY_SUBSYSTEMS & TracySys_Scroll);

    UIBox* last_area = scroll_ctx.last_area;

    // clang-format off
    f32 mouse_pos                   = is_horizontal ? g_ui_ctx->mouse_pos.x        : g_ui_ctx->mouse_pos.y;
    f32 thumb_delta_scale           = is_horizontal ? scroll_ctx.thumb_delta_scale.x   : scroll_ctx.thumb_delta_scale.y;
    f32 scroll_max_delta            = is_horizontal ? scroll_ctx.max_delta.x           : scroll_ctx.max_delta.y;
    f32* scroll_delta               = is_horizontal ? &last_area->scroll_delta.x       : &last_area->scroll_delta.y;
    TimedLerpAnimation* scroll_anim = is_horizontal ? &last_area->scroll_anim_x        : &last_area->scroll_anim_y;
    f32* area_drag_scroll_anchor    = is_horizontal ? &last_area->drag_scroll_anchor.x : &last_area->drag_scroll_anchor.y;
    // clang-format on

    /* Transition-related variables */
    f32 thickness = 0.f;
    f32 padding_end = 0.f;
    f32 bar_thickness_max = SCROLLBAR_THICKNESS_MAX + SCROLLBAR_PADDING_END_MAX;
    Color track_color = scroll_ctx.thumb_color;
    track_color.a = (u8)(scroll_ctx.thumb_color.a * SCROLLBAR_THUMB_OPACITY_TRACK);
    Color thumb_color = scroll_ctx.thumb_color;

    /* Get last bar and thumb box from cache */
    UISignalFlags bar_flags = UI_Signal_Flag_None;
    u64 bar_hash = fnv1a_64_continue(scroll_ctx.hash, is_horizontal ? " (hbar)" : " (vbar)", 7);
    UIBox* last_bar = find_or_insert_box_with_hash(bar_hash);

    UISignalFlags thumb_flags = UI_Signal_Flag_None;
    u64 thumb_hash = fnv1a_64_continue(scroll_ctx.hash, is_horizontal ? " (hthumb)" : " (vthumb)", 9);
    UIBox* last_thumb = find_or_insert_box_with_hash(thumb_hash);

    /* [THUMB] Handle interaction and transition */
    if (last_thumb)
    {
        update_interaction_flags(last_thumb, &thumb_flags);
        f32* drag_mouse_anchor = is_horizontal ? &last_thumb->drag_mouse_anchor.x : &last_thumb->drag_mouse_anchor.y;

        /* Handle transition */
        if (ui_pressed(thumb_flags) || ui_dragging(thumb_flags) ||
            (ui_hovered(thumb_flags) && last_thumb->active_t == 1.f))
        {
            if (ui_lclicked(thumb_flags))
            {
                last_thumb->anim_state = TRANSITION_FORWARD;
                last_thumb->hot_t = SCROLLBAR_THUMB_OPACITY_HOVER;
                *area_drag_scroll_anchor = *scroll_delta;
                g_ui_ctx->mouse_captured_by_hash = scroll_ctx.hash;
            }
            if (ui_dragging(thumb_flags) || last_thumb->anim_state == TRANSITION_FORWARD)
            {
                if (ui_dragging(thumb_flags))
                {
                    f32 mouse_drag_delta = mouse_pos - *drag_mouse_anchor;
                    f32 target = *area_drag_scroll_anchor + mouse_drag_delta * thumb_delta_scale;
                    *scroll_delta = clamp(target, 0.f, scroll_max_delta);
                    start_timed_lerp(scroll_anim, *scroll_delta, *scroll_delta, g_ui_ctx->current_time, 0.f);
                }
                if (update_transition(&last_thumb->hot_t, 1.f, 20.f))
                    last_thumb->anim_state = TRANSITION_REVERSE;
            }
            else
            {
                if (update_transition(&last_thumb->hot_t, SCROLLBAR_THUMB_OPACITY_HOVER, 18.f))
                    last_thumb->anim_state = TRANSITION_IDLE;
            }

            /* use `active_t == 1.f` to indicate the this transition is still active */
            last_thumb->active_t = 1.f;
        }
        else if (ui_hovered(thumb_flags))
        {
            last_thumb->anim_state = TRANSITION_FORWARD;
            update_transition(&last_thumb->hot_t, SCROLLBAR_THUMB_OPACITY_HOVER, 14.f);

            last_thumb->active_t = 0.f;
        }
        else
        {
            // Hide/show the thumb with a fade-out transition when moving/idle mouse or scrolling in the scroll area
            if (ui_hovered(scroll_ctx.area_flags))
                if (g_ui_ctx->mouse_delta.x || g_ui_ctx->mouse_delta.y ||
                    last_area->scroll_anim_x.target - last_area->scroll_delta.x ||
                    last_area->scroll_anim_y.target - last_area->scroll_delta.y)
                {
                    last_area->idle_timer = 0.f;
                    last_thumb->anim_state = TRANSITION_FORWARD;
                }

            last_area->idle_timer += g_ui_ctx->frame_delta_time;
            if (last_area->idle_timer > 1.5f || !ui_hovered(scroll_ctx.area_flags))
                last_thumb->anim_state = TRANSITION_REVERSE;

            if (last_thumb->anim_state == TRANSITION_FORWARD)
                update_transition(&last_thumb->hot_t, SCROLLBAR_THUMB_OPACITY_INACTIVE, 8.f);
            else if (last_thumb->anim_state == TRANSITION_REVERSE)
                if (update_transition(&last_thumb->hot_t, 0.f, 4.f))
                    last_thumb->anim_state = TRANSITION_IDLE;

            last_thumb->active_t = 0.f;
        }
        thumb_color.a = lerp_u8(0, scroll_ctx.thumb_color.a, last_thumb->hot_t);
    }

    /* [WHOLE BAR] Handle interaction and transition */
    if (last_bar)
    {
        update_interaction_flags(last_bar, &bar_flags);

        /* Expand scrollbar (track & thumb) with a fade-out transition when mouse hovers over it */
        if (ui_dragging(thumb_flags) || ui_hovered(bar_flags))
        {
            scroll_ctx.last_area->idle_timer = 0.f; // to suppress thumb auto-hide transition
            last_bar->anim_state = TRANSITION_FORWARD;
            if (last_bar->anim_state == TRANSITION_FORWARD)
                update_transition(&last_bar->hot_t, 1.f, 14.f);

            /* Click to jump to clicked position */
            if (ui_lclicked(bar_flags) && !ui_pressed(thumb_flags))
            {
                Assert(last_thumb);
                f32 last_bar_position = is_horizontal ? last_bar->position.x : last_bar->position.y;
                f32 last_thumb_size = is_horizontal ? last_thumb->size.width : last_thumb->size.height;
                f32 target = (mouse_pos - last_bar_position - last_thumb_size / 2) * thumb_delta_scale;
                start_timed_lerp(scroll_anim, *scroll_delta, target, g_ui_ctx->current_time, SCROLL_ANIM_DURATION);
                g_ui_ctx->mouse_captured_by_hash = scroll_ctx.hash;
            }
        }
        else
        {
            if (last_bar->hot_t)
            {
                last_bar->anim_state = TRANSITION_REVERSE;
                if (update_transition(&last_bar->hot_t, 0.f, 12.f))
                    last_bar->anim_state = TRANSITION_IDLE;
            }
        }

        if (scroll_ctx.fixed_track)
        {
            thickness = SCROLLBAR_THICKNESS_MIN;
            padding_end = SCROLLBAR_PADDING_END_MIN;
        }
        else
        {
            thickness = lerp_f32(SCROLLBAR_THICKNESS_MIN, SCROLLBAR_THICKNESS_MAX, last_bar->hot_t);
            padding_end = lerp_f32(SCROLLBAR_PADDING_END_MIN, SCROLLBAR_PADDING_END_MAX, last_bar->hot_t);
        }
        track_color.a = lerp_u8(0, track_color.a, last_bar->hot_t);
    }

    /* Create scrollbar */
    // clang-format off
    ScrollBarLayout L = make_scrollbar_layout(is_horizontal, thickness, padding_end, bar_thickness_max, thumb_size, scroll_ctx.thumb_delta);
    UIBox* bar_container = ui_box_begin(&(BoxConfig){ .sizing = L.container_sizing, .flags = BoxFlag_Float, .float_offset = L.float_offset, .direction = L.padding_direction });
    {
        // `padding-start` and `padding-end` is used to do the transition of scrollbar expanding
        ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = L.padding_start_sizing, .alignment = { ALIGN_START, ALIGN_CENTER } }));
        UIBox* inner_container = ui_box_begin(&(BoxConfig){ .sizing = L.inner_container_sizing, .direction = L.spacer_direction });
        {
            ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = L.spacer_sizing }));
            UIBox* track = ui_box_begin(&(BoxConfig){
                .sizing = L.track_sizing,
                .color = track_color,
                .rect_style = { .corner_radius = thickness / 2 }
            });
            {
                UIBox* thumb_container = ui_box_begin(&(BoxConfig){
                    .sizing = L.thumb_container_sizing,
                    .child_offset = L.thumb_container_child_offset
                });
                UIBox* thumb = ui_box_begin(&(BoxConfig){
                    .sizing = L.thumb_sizing,
                    .color = thumb_color,
                    .rect_style = { .corner_radius = thickness / 2 }
                });
                ui_box_end(thumb);
                update_box_key(thumb, thumb_hash);
                ui_box_end(thumb_container);
            }
            ui_box_end(track);
            ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = L.spacer_sizing }));
        }
        ui_box_end(inner_container);
        ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = L.padding_end_sizing, .alignment = { ALIGN_END, ALIGN_CENTER } }));
    }
    ui_box_end(bar_container);
    update_box_key(bar_container, bar_hash);
    // clang-format on
    TracyCZoneEnd(ctx_sb);
}

ScrollContext ui_scrollable_area_begin(const ScrollableAreaConfig* cfg)
{
    TracyCZoneNC(ctx_sab, "SABegin", TracyColor_Scroll, TRACY_SUBSYSTEMS & TracySys_Scroll);
    f64 now = g_ui_ctx->current_time;
    ScrollContext scroll_ctx = { 0 };
    scroll_ctx.thumb_color = cfg->thumb_color;
    scroll_ctx.fixed_track = cfg->fixed_track;
    scroll_ctx.cursor_content_x = -1.f;

    /* Create area box */
    scroll_ctx.area = ui_box_begin(&(BoxConfig){ .sizing = cfg->sizing, .color = cfg->bg, .flags = BoxFlag_Clip });

    /* Handle interaction and animation */
    UIBoxInteractResult result = ui_box_interact(scroll_ctx.area, cfg->hash_str);
    scroll_ctx.area_flags = result.flags;
    scroll_ctx.last_area = result.last_box;
    scroll_ctx.hash = fnv1a_64(cfg->hash_str.data, cfg->hash_str.len);
    u64 content_hash = fnv1a_64_continue(scroll_ctx.hash, " (content box)", 14);

    /* Update scroll delta */
    scroll_ctx.last_area->scroll_delta.x = evaluate_timed_lerp(&scroll_ctx.last_area->scroll_anim_x, now);
    scroll_ctx.last_area->scroll_delta.y = evaluate_timed_lerp(&scroll_ctx.last_area->scroll_anim_y, now);
    scroll_ctx.delta = scroll_ctx.last_area->scroll_delta;

    if (fabs(scroll_ctx.last_area->scroll_anim_x.target - scroll_ctx.last_area->scroll_delta.x) > 0.01f ||
        fabs(scroll_ctx.last_area->scroll_anim_y.target - scroll_ctx.last_area->scroll_delta.y) > 0.01f)
        ui_request_frames();

    /* Prepare content result for _end() */
    scroll_ctx.last_content = find_or_insert_box_with_hash(content_hash);
    Assert(scroll_ctx.last_content);

    /* Create container & inner container & content box */
    ui_box_begin(
        &(BoxConfig){ .sizing = { fit_grow({}), fit_grow({}) }, .direction = LAYOUT_TOP_TO_BOTTOM }); // Container
    ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit_grow({}) } }); // Inner Container
    UIBox* content = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fit_grow({}) },
                                                .padding = cfg->padding,
                                                .child_gap = cfg->child_gap,
                                                .direction = cfg->direction,
                                                .alignment = cfg->alignment,
                                                .child_offset = { -scroll_ctx.delta.x, -scroll_ctx.delta.y } });
    update_box_key(content, content_hash);

    TracyCZoneEnd(ctx_sab);
    return scroll_ctx;
}

void ui_scrollable_area_end(ScrollContext scroll_ctx)
{
    TracyCZoneNC(ctx_sae, "SAEnd", TracyColor_Scroll, TRACY_SUBSYSTEMS & TracySys_Scroll);
    ScrollBarFlags bar_flags = SCROLLBAR_NONE;
    Size thumb_size = { 0 };

    /* Calculate thumb delta and size */
    if (scroll_ctx.last_content)
    {
        Assert(scroll_ctx.last_area);

        /* Reserve padding on both ends of the scrollbar */
        Size virtual_area_size = scroll_ctx.last_area->size;
        Size content_size = scroll_ctx.last_content->size;

        /* Compute max scroll delta and clamp targets */
        scroll_ctx.max_delta.x = content_size.width - virtual_area_size.width;
        scroll_ctx.max_delta.y = content_size.height - virtual_area_size.height;

        UIBox* last_area = scroll_ctx.last_area;
        last_area->scroll_anim_x.target = clamp(last_area->scroll_anim_x.target, 0, scroll_ctx.max_delta.x);
        last_area->scroll_anim_y.target = clamp(last_area->scroll_anim_y.target, 0, scroll_ctx.max_delta.y);

        // NOTE:
        //   Scroll wheel is handled in _end() rather than _start() so that
        //   deeply nested scroll areas (whose _end() runs first) consume the
        //   scroll delta before their ancestors.  This avoids the case where
        //   an outer and inner area both respond to the same wheel event.
        /* Handle scroll wheel (deepest hovered area consumes scroll first) */
        if (ui_hovered(scroll_ctx.area_flags))
        {
            if (g_ui_ctx->mouse_scroll_delta.x && scroll_ctx.max_delta.x > 0)
            {
                f32 new_target = last_area->scroll_anim_x.target + g_ui_ctx->mouse_scroll_delta.x * SCROLL_SENSITIVITY;
                new_target = clamp(new_target, 0, scroll_ctx.max_delta.x);
                start_timed_lerp(&last_area->scroll_anim_x, last_area->scroll_delta.x, new_target,
                                 g_ui_ctx->current_time, SCROLL_ANIM_DURATION);
                g_ui_ctx->mouse_scroll_delta.x = 0;
            }
            if (g_ui_ctx->mouse_scroll_delta.y && scroll_ctx.max_delta.y > 0)
            {
                f32 new_target = last_area->scroll_anim_y.target + g_ui_ctx->mouse_scroll_delta.y * SCROLL_SENSITIVITY;
                new_target = clamp(new_target, 0, scroll_ctx.max_delta.y);
                start_timed_lerp(&last_area->scroll_anim_y, last_area->scroll_delta.y, new_target,
                                 g_ui_ctx->current_time, SCROLL_ANIM_DURATION);
                g_ui_ctx->mouse_scroll_delta.y = 0;
            }
        }

        /* Auto-scroll to keep cursor visible with scrolloff margin */
        if (scroll_ctx.cursor_content_x >= 0.f && scroll_ctx.max_delta.x > 0)
        {
            f32 viewport_width = virtual_area_size.width;
            f32 current_scroll = last_area->scroll_delta.x;
            f32 margin = scroll_ctx.scroll_margin;
            f32 target = current_scroll;

            if (scroll_ctx.cursor_content_x < current_scroll + margin)
                target = max(0.f, scroll_ctx.cursor_content_x - margin);
            else if (scroll_ctx.cursor_content_x > current_scroll + viewport_width - margin)
                target = min(scroll_ctx.max_delta.x, scroll_ctx.cursor_content_x - viewport_width + margin);

            if (target != current_scroll)
                start_timed_lerp(&last_area->scroll_anim_x, current_scroll, target, g_ui_ctx->current_time,
                                 SCROLL_ANIM_DURATION);
        }

        /* Reserve padding on both ends of the scrollbar */
        Size bar_track_size = { virtual_area_size.width - (f32)SCROLLBAR_SPACER * 2,
                                virtual_area_size.height - (f32)SCROLLBAR_SPACER * 2 };

        /* Determine whether the area need to create horizontal/vertical scrollbar */
        bar_flags |= content_size.height > virtual_area_size.height ? SCROLLBAR_VERTICAL : 0;
        bar_flags |= content_size.width > virtual_area_size.width ? SCROLLBAR_HORIZONTAL : 0;

        /* Get the scrollbar thumb delta value */
        if (bar_flags & SCROLLBAR_HORIZONTAL)
        {
            thumb_size.width = bar_track_size.width * (bar_track_size.width / content_size.width);
            f32 scroll_range = content_size.width - virtual_area_size.width;
            f32 thumb_range = bar_track_size.width - thumb_size.width;
            scroll_ctx.thumb_delta_scale.x = scroll_range / thumb_range;
            scroll_ctx.thumb_delta.x = (scroll_ctx.delta.x / scroll_range) * thumb_range;
        }
        if (bar_flags & SCROLLBAR_VERTICAL)
        {
            thumb_size.height = bar_track_size.height * (bar_track_size.height / content_size.height);
            f32 scroll_range = content_size.height - virtual_area_size.height;
            f32 thumb_range = bar_track_size.height - thumb_size.height;
            scroll_ctx.thumb_delta_scale.y = scroll_range / thumb_range;
            scroll_ctx.thumb_delta.y = (scroll_ctx.delta.y / scroll_range) * thumb_range;
        }
    }

    /* Close nested UI boxes & Create scrollbar */
    ui_box_end(scroll_ctx.area->child_first->child_first->child_first); // Content
    ui_box_end(scroll_ctx.area->child_first->child_first); // Inner Container
    if (bar_flags & SCROLLBAR_HORIZONTAL)
        if (scroll_ctx.last_area)
            scrollbar(scroll_ctx, True, thumb_size.width);
    ui_box_end(scroll_ctx.area->child_first); // Container
    if (bar_flags & SCROLLBAR_VERTICAL)
        if (scroll_ctx.last_area)
            scrollbar(scroll_ctx, False, thumb_size.height);
    ui_box_end(scroll_ctx.area);
    TracyCZoneEnd(ctx_sae);
}

//
// Panel
//

#define PANEL_BOUNDARY 6.0f
#define PANEL_PCT_MIN  0.05f
#define PANEL_PCT_MAX  0.95f

void ui_panel_boundaries(const Panel* root, const Rect root_rect, const PanelTheme* theme)
{
    TracyCZoneNC(ctx_pbd, "PanelBounds", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);

    for (const Panel* p = root; p; p = panel_iter_next(p))
    {
        /* Skip leaf panels (no split) */
        if (!p->child_a)
            continue;

        Rect panel_rect = panel_calc_rect(p, root_rect);
        Panel* child_a = p->child_a;
        Panel* child_b = p->child_b;
        Rect child_a_rect = panel_calc_rect_from_parent(child_a, panel_rect);

        /* Compute boundary position and size along the split axis */
        Position bound_pos;
        f32 bound_w, bound_h;
        if (p->split_axis == Axis2_X)
        {
            bound_pos.x = child_a_rect.xmax - (PANEL_BOUNDARY / 2);
            bound_pos.y = child_a_rect.ymin;
            bound_w = PANEL_BOUNDARY;
            bound_h = child_a_rect.ymax - child_a_rect.ymin;
        }
        else
        {
            bound_pos.x = child_a_rect.xmin;
            bound_pos.y = child_a_rect.ymax - (PANEL_BOUNDARY / 2);
            bound_w = child_a_rect.xmax - child_a_rect.xmin;
            bound_h = PANEL_BOUNDARY;
        }

        /* Create boundary box and handle interaction */
        UIBox* boundary = ui_box_begin(&(BoxConfig){
            .sizing = { fixed(bound_w), fixed(bound_h) },
            .flags = BoxFlag_Float,
            .float_offset = bound_pos,
            .direction = (p->split_axis == Axis2_X) ? LAYOUT_LEFT_TO_RIGHT : LAYOUT_TOP_TO_BOTTOM,
        });
        {
            UIBoxInteractResult result =
                ui_box_interact(boundary, str_fmt(HASH_STR_MAX_LENGTH, "panel_bound_%u", p->id));
            UISignalFlags flags = result.flags;

            /* Hover → resize cursor */
            if (ui_hovered(flags))
            {
                Cursor cur = (p->split_axis == Axis2_X) ? UI_CURSOR_HORIZONTAL : UI_CURSOR_VERTICAL;
                ui_set_desired_cursor(cur);
            }

            /* Click → save initial split ratios for drag */
            if (ui_lclicked(flags))
            {
                child_a->drag_saved_pct = child_a->pct_of_parent;
                child_a->drag_saved_partner_pct = child_b->pct_of_parent;
            }

            /* Drag → update split ratio and boundary position */
            if (ui_dragging(flags))
            {
                Position delta = ui_box_drag_delta(result.last_box);
                f32 parent_dim = (p->split_axis == Axis2_X) ? (panel_rect.xmax - panel_rect.xmin)
                                                            : (panel_rect.ymax - panel_rect.ymin);
                f32 dp = (p->split_axis == Axis2_X ? delta.x : delta.y) / parent_dim;
                child_a->pct_of_parent = clamp(child_a->drag_saved_pct + dp, PANEL_PCT_MIN, PANEL_PCT_MAX);
                child_b->pct_of_parent = clamp(child_a->drag_saved_partner_pct - dp, PANEL_PCT_MIN, PANEL_PCT_MAX);

                /* Snap boundary to updated ratio to eliminate one-frame visual lag */
                Rect r = panel_calc_rect_from_parent(child_a, panel_rect);
                if (p->split_axis == Axis2_X)
                {
                    boundary->cfg.float_offset.x = r.xmax - (PANEL_BOUNDARY / 2);
                    boundary->cfg.float_offset.y = r.ymin;
                }
                else
                {
                    boundary->cfg.float_offset.x = r.xmin;
                    boundary->cfg.float_offset.y = r.ymax - (PANEL_BOUNDARY / 2);
                }
            }

            /* Transition and draw splitter line */
            if (result.last_box)
            {
                update_transition(&result.last_box->hot_t,
                                  ui_hovered(flags) || ui_pressed(flags) || ui_dragging(flags) ? 1.f : 0.f,
                                  20.f);
                update_transition(&result.last_box->active_t,
                                  ui_pressed(flags) || ui_dragging(flags) ? 1.f : 0.f,
                                  20.f);
            }
            f32 hot_t = result.last_box ? result.last_box->hot_t : 0.f;
            f32 active_t = result.last_box ? result.last_box->active_t : 0.f;
            Color virtual_line_color = lerp_color((Color){ 0 }, theme->tab_accent_weak, hot_t);
            Color line_color = lerp_color(theme->tab_border, theme->tab_accent_weak, hot_t);
            virtual_line_color = lerp_color(virtual_line_color, theme->tab_accent, active_t);
            line_color = lerp_color(line_color, theme->tab_accent, active_t);

            // clang-format off
            if (p->split_axis == Axis2_X)
            {
                UIBox* line_container = ui_box_begin(
                    &(BoxConfig){ .sizing = { grow({}), grow({}) }, .alignment = { ALIGN_CENTER, ALIGN_CENTER } });
                {
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(1), fixed(bound_h) }, .color = virtual_line_color }));
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(1), fixed(bound_h) }, .color = line_color }));
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(1), fixed(bound_h) }, .color = virtual_line_color }));
                }
                ui_box_end(line_container);
            }
            else
            {
                UIBox* line_container = ui_box_begin(
                    &(BoxConfig){ .sizing = { grow({}), grow({}) }, .alignment = { ALIGN_CENTER, ALIGN_CENTER }, .direction = LAYOUT_TOP_TO_BOTTOM });
                {
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(bound_w), fixed(1) }, .color = virtual_line_color }));
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(bound_w), fixed(1) }, .color = line_color }));
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(bound_w), fixed(1) }, .color = virtual_line_color }));
                }
                ui_box_end(line_container);
            }
            // clang-format on
        }
        ui_box_end(boundary);
    }
    TracyCZoneEnd(ctx_pbd);
}

PanelContext ui_panel_begin(const PanelConfig* cfg)
{
    /* Only leaf panels (no children) can be UI containers */
    if (cfg->panel->child_a)
        Assert(0);

    TracyCZoneNC(ctx_pb, "PanelBegin", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);

    /* Mark all existing tabs as declared so they survive cleanup */
    for (PanelTab* tab = cfg->panel->tab_first; tab; tab = tab->next)
        tab->frame_declared = True;

    /* Compute panel rect (use pre-computed when provided) */
    Rect rect;
    if (cfg->panel_rect.xmax > cfg->panel_rect.xmin || cfg->panel_rect.ymax > cfg->panel_rect.ymin)
        rect = cfg->panel_rect;
    else
        rect = panel_calc_rect(cfg->panel, cfg->root_rect);
    f32 rect_w = max(0.f, rect.xmax - rect.xmin);
    f32 rect_h = max(0.f, rect.ymax - rect.ymin);

    Rect tab_bar_spacer_rect = { 0 };

    /* Create panel container */
    UIBox* container = ui_box_begin(&(BoxConfig){
        .sizing = { fixed(rect_w), fixed(rect_h) },
        .direction = LAYOUT_TOP_TO_BOTTOM,
        .flags = BoxFlag_Float,
        .float_offset = { rect.xmin, rect.ymin },
    });
    {
        ui_box_interact(container, str_fmt(HASH_STR_MAX_LENGTH, "panel_%u", cfg->panel->id));

        /*
         * Tab bar
         * Height matches decoration (min/max/close) buttons.
         */
        f32 tab_bar_height = cfg->font_size * 2.5f + 2.f - 1;

        UIBox* tab_bar = ui_box_begin(&(BoxConfig){ .sizing = { fit_grow({}), fixed(tab_bar_height) },
                                                    .color = cfg->theme->tab_bg,
                                                    .alignment = { ALIGN_START, ALIGN_CENTER } });
        {
            PanelTab* active = panel_tab_get_active(cfg->panel);
            isize tab_index = 0;

            ScrollContext tab_bar_scroll_ctx = ui_scrollable_area_begin(&(ScrollableAreaConfig){
                .hash_str = str_fmt(HASH_STR_MAX_LENGTH, "panel_%u (scroll area)", cfg->panel->id),
                .sizing = { fit({ .max = rect.xmax - rect.xmin - cfg->tab_bar_right_inset - 1 }), // 1 is boundary width
                            fixed(tab_bar_height) },
                .bg = cfg->theme->tab_bg,
                .thumb_color = cfg->theme->scrollbar_thumb,
                .alignment = { ALIGN_START, ALIGN_CENTER },
                .fixed_track = True });
            {
                for (PanelTab* tab = cfg->panel->tab_first; tab; tab = tab->next, tab_index++)
                {
                    b32 is_active = (tab == active);

                    UIBox* tab_container = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), fixed(tab_bar_height) },
                                                                      .direction = LAYOUT_TOP_TO_BOTTOM });
                    {
                        /* Tab interaction */
                        UIBoxInteractResult tab_interact_res = ui_box_interact(
                            tab_container, str_fmt(HASH_STR_MAX_LENGTH, "tab_%u_%u", cfg->panel->id, tab->id));

                        /* Drop-target highlight transition */
                        if (tab_interact_res.last_box)
                            update_transition(
                                &tab_interact_res.last_box->hot_t,
                                ui_drag_over(tab_interact_res.flags) && g_ui_ctx->drag_payload_size ? 1.f : 0.f,
                                15.f);
                        f32 drop_t = tab_interact_res.last_box ? tab_interact_res.last_box->hot_t : 0.f;

                        /* Drag payload */
                        if (ui_dragging(tab_interact_res.flags) && !g_ui_ctx->mouse_captured_by_hash)
                        {
                            TabDragPayload tab_drag_payload = { DRAG_TYPE_TAB, cfg->panel->id, tab->id,
                                                                cfg->window_id };
                            isize copy_len = tab->name_len < 43 ? tab->name_len : 43;
                            memcpy(tab_drag_payload.title, tab->name, (size_t)copy_len);
                            tab_drag_payload.title[copy_len] = '\0';
                            ui_set_drag_payload(&tab_drag_payload, sizeof(tab_drag_payload));
                            ui_set_desired_cursor(UI_CURSOR_MOVE);
                        }

                        /* Tab title and close button */
                        UIBox* tab_title_cell = ui_box_begin(&(BoxConfig){
                            .sizing = { fit({}), fixed(tab_bar_height - 1) },
                            .padding = { 5, 10, 5, 10 },
                            .direction = LAYOUT_LEFT_TO_RIGHT,
                            .child_gap = 4,
                            .alignment = { ALIGN_START, ALIGN_CENTER },
                            .color = lerp_color(ui_dragging(tab_interact_res.flags)
                                                    ? cfg->theme->tab_accent
                                                    : (is_active ? cfg->theme->tab_active_bg : cfg->theme->tab_bg),
                                                cfg->theme->tab_accent_weak, drop_t) });
                        {
                            /* Activate tab on click */
                            if (ui_lclicked(tab_interact_res.flags))
                                cmd_queue_push(cfg->cmd_queue,
                                               str_fmt(CMD_STR_MAX_LENGTH, "tab.activate panel=%u tab=%u window=%u",
                                                       cfg->panel->id, tab->id, cfg->window_id));

                            /* Close tab on middle-click */
                            if (ui_mclicked(tab_interact_res.flags))
                                cmd_queue_push(cfg->cmd_queue,
                                               str_fmt(CMD_STR_MAX_LENGTH, "tab.close panel=%u tab=%u window=%u",
                                                       cfg->panel->id, tab->id, cfg->window_id));

                            /* Tab title */
                            ui_text((String){ tab->name, tab->name_len },
                                    &(TextConfig){ .font = cfg->font_ui,
                                                   .font_size = cfg->font_size,
                                                   .color = cfg->theme->tab_active_fg,
                                                   .line_height = cfg->font_size });

                            /* Close button (hidden while dragging) */
                            UISignalFlags close_button_flags = ui_button(
                                str_fmt(HASH_STR_MAX_LENGTH, "×##tc_%u_%u", cfg->panel->id, tab->id), cfg->font_ui, 11,
                                (Sizing){ fit({}), fit({}) }, (Padding){ 3, 3, 3, 3 }, (Color){ 0 }, // background color
                                (ui_hovered(tab_interact_res.flags) && !ui_drag_over(tab_interact_res.flags))
                                    ? cfg->theme->tab_active_fg
                                    : (Color){ 0 }, // text color
                                ui_drag_over(tab_interact_res.flags) ? (Color){ 0 }
                                                                     : cfg->theme->hover_bg // hover color
                            );
                            if (ui_hovered(close_button_flags) && !is_active && !ui_drag_over(tab_interact_res.flags))
                                tab_title_cell->cfg.color = cfg->theme->tab_bg;
                            if (ui_lclicked(close_button_flags))
                                cmd_queue_push(cfg->cmd_queue,
                                               str_fmt(CMD_STR_MAX_LENGTH, "tab.close panel=%u tab=%u window=%u",
                                                       cfg->panel->id, tab->id, cfg->window_id));

                            /* Drop: reorder (same panel) or move to panel (cross-panel) */
                            if (ui_dropped(tab_interact_res.flags) && !g_ui_ctx->drag_payload_consumed &&
                                g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
                            {
                                TabDragPayload* payload = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                                if (payload->drag_type == DRAG_TYPE_TAB)
                                {
                                    g_ui_ctx->drag_payload_consumed = True;
                                    if (payload->from_tab_id != tab->id)
                                    {
                                        if (payload->from_panel_id == cfg->panel->id)
                                        {
                                            isize dragged_idx = 0;
                                            for (PanelTab* t = cfg->panel->tab_first; t; t = t->next, dragged_idx++)
                                                if (t->id == payload->from_tab_id)
                                                    break;
                                            i32 delta = (i32)(tab_index - dragged_idx);
                                            cmd_queue_push(cfg->cmd_queue,
                                                           str_fmt(CMD_STR_MAX_LENGTH,
                                                                   "tab.move panel=%u tab=%u delta=%+d window=%u",
                                                                   cfg->panel->id, payload->from_tab_id, delta,
                                                                   cfg->window_id));
                                        }
                                        else
                                        {
                                            cmd_queue_push(
                                                cfg->cmd_queue,
                                                str_fmt(CMD_STR_MAX_LENGTH,
                                                        "tab.move_to_panel panel=%u tab=%u to_panel=%u to_idx=%d "
                                                        "window=%u to_window=%u",
                                                        payload->from_panel_id, payload->from_tab_id, cfg->panel->id,
                                                        (i32)tab_index, payload->from_window_id, cfg->window_id));
                                        }
                                    }
                                }
                            }
                        }
                        ui_box_end(tab_title_cell);

                        /* Insertion indicator line */
                        if (drop_t > 0.01f)
                        {
                            b32 indent_left = True;
                            if (g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
                            {
                                TabDragPayload* peek = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                                if (peek->drag_type == DRAG_TYPE_TAB && peek->from_panel_id == cfg->panel->id)
                                {
                                    isize dragged_idx = 0;
                                    for (PanelTab* t = cfg->panel->tab_first; t; t = t->next, dragged_idx++)
                                        if (t->id == peek->from_tab_id)
                                            break;
                                    indent_left = (dragged_idx > tab_index);
                                }
                            }
                            f32 line_x = indent_left ? 0 : (tab_interact_res.last_box->size.width - 3.f);

                            ui_box_end(ui_box_begin(&(BoxConfig){
                                .sizing = { fixed(3), fixed(tab_bar_height - 1) },
                                .color = lerp_color((Color){ 0 }, cfg->theme->tab_accent, drop_t),
                                .flags = BoxFlag_Float,
                                .float_offset = { line_x, -(tab_bar_height - 1) },
                            }));
                        }

                        /* Underline */
                        ui_box_end(ui_box_begin(&(BoxConfig){
                            .sizing = { grow({}), fixed(1) },
                            .color = (ui_drag_over(tab_interact_res.flags) && g_ui_ctx->drag_payload_size)
                                         ? cfg->theme->tab_border
                                         : (ui_dragging(tab_interact_res.flags)
                                                ? cfg->theme->tab_accent
                                                : (is_active ? cfg->theme->panel_bg : cfg->theme->tab_border)) }));
                    }
                    ui_box_end(tab_container);

                    /* Tab splitter */
                    ui_box_end(ui_box_begin(
                        &(BoxConfig){ .sizing = { fixed(1), grow({}) }, .color = cfg->theme->tab_border }));
                }

                /* New tab button */
                UIBox* new_button_container =
                    ui_box_begin(&(BoxConfig){ .sizing = { fit({}), grow({}) }, .direction = LAYOUT_TOP_TO_BOTTOM });
                {
                    UIBox* inner_container = ui_box_begin(&(BoxConfig){ .sizing = { fit({}), grow({}) },
                                                                        .padding = { 0, 3, 0, 3 },
                                                                        .alignment = { ALIGN_CENTER, ALIGN_CENTER } });
                    {
                        UISignalFlags new_tab_button_flags =
                            ui_button(str_fmt(HASH_STR_MAX_LENGTH, "+##tab_add_%u", cfg->panel->id), cfg->font_ui, 12,
                                      (Sizing){ fit({}), fit({}) }, (Padding){ 3, 4, 4, 4 }, (Color){ 0 },
                                      cfg->theme->tab_fg, cfg->theme->hover_bg);
                        if (ui_lclicked(new_tab_button_flags))
                            cmd_queue_push(cfg->cmd_queue, str_fmt(CMD_STR_MAX_LENGTH, "tab.new panel=%u window=%u",
                                                                   cfg->panel->id, cfg->window_id));
                    }
                    ui_box_end(inner_container);

                    /* Underline */
                    ui_box_end(ui_box_begin(
                        &(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = cfg->theme->tab_border }));
                }
                ui_box_end(new_button_container);
            }
            ui_scrollable_area_end(tab_bar_scroll_ctx);

            /* Spacer — also serves as tab bar drop target for appending tabs */
            UIBox* spacer_container =
                ui_box_begin(&(BoxConfig){ .sizing = { grow({}), grow({}) }, .direction = LAYOUT_TOP_TO_BOTTOM });
            {
                UIBox* spacer_inner = ui_box_begin(&(BoxConfig){ .sizing = { grow({}), grow({}) } });
                {
                    UIBoxInteractResult spacer_interact_res =
                        ui_box_interact(spacer_inner, str_fmt(HASH_STR_MAX_LENGTH, "panel_drop_%u", cfg->panel->id));
                    b32 spacer_is_drop_target = ui_drag_over(spacer_interact_res.flags) && g_ui_ctx->drag_payload_size;
                    if (spacer_interact_res.last_box)
                    {
                        update_transition(&spacer_interact_res.last_box->hot_t,
                                          spacer_is_drop_target ? 1.f : 0.f,
                                          15.f);

                        /* Capture spacer rect for HTCAPTION hit-test */
                        {
                            f32 sx = spacer_interact_res.last_box->position.x;
                            f32 sy = spacer_interact_res.last_box->position.y;
                            f32 sw = spacer_interact_res.last_box->size.width;
                            f32 sh = spacer_interact_res.last_box->size.height;
                            tab_bar_spacer_rect = (Rect){ sx, sy, sx + sw, sy + sh };
                        }
                    }
                    f32 spacer_hot_transition =
                        spacer_interact_res.last_box ? spacer_interact_res.last_box->hot_t : 0.f;

                    /* Insertion indicator line at spacer left edge (append position) */
                    if (spacer_hot_transition > 0.01f)
                        ui_box_end(ui_box_begin(&(BoxConfig){
                            .sizing = { fixed(3), fixed(tab_bar_height - 1) },
                            .color = lerp_color((Color){ 0 }, cfg->theme->tab_accent, spacer_hot_transition),
                            .flags = BoxFlag_Float,
                            .float_offset = { 0, 0 },
                        }));

                    spacer_inner->cfg.color =
                        lerp_color((Color){ 0 }, cfg->theme->tab_accent_weak, spacer_hot_transition);

                    /* Drop handling */
                    if (ui_dropped(spacer_interact_res.flags))
                    {
                        TabDragPayload* payload = NULL;
                        if (g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
                        {
                            TabDragPayload* p = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                            if (p->drag_type == DRAG_TYPE_TAB)
                            {
                                g_ui_ctx->drag_payload_consumed = True;
                                payload = p;
                            }
                        }
                        if (payload && payload->from_panel_id == cfg->panel->id)
                        {
                            /* Same panel: move to end */
                            isize count = 0;
                            isize dragged_idx = -1;
                            for (PanelTab* t = cfg->panel->tab_first; t; t = t->next, count++)
                                if (t->id == payload->from_tab_id)
                                    dragged_idx = count;
                            if (dragged_idx >= 0 && dragged_idx < count - 1)
                            {
                                i32 delta = (i32)(count - 1 - dragged_idx);
                                cmd_queue_push(cfg->cmd_queue,
                                               str_fmt(CMD_STR_MAX_LENGTH,
                                                       "tab.move panel=%u tab=%u delta=%+d window=%u", cfg->panel->id,
                                                       payload->from_tab_id, delta, cfg->window_id));
                            }
                        }
                        else if (payload)
                        {
                            cmd_queue_push(
                                cfg->cmd_queue,
                                str_fmt(
                                    CMD_STR_MAX_LENGTH,
                                    "tab.move_to_panel panel=%u tab=%u to_panel=%u to_idx=-1 window=%u to_window=%u",
                                    payload->from_panel_id, payload->from_tab_id, cfg->panel->id,
                                    payload->from_window_id, cfg->window_id));
                        }
                    }
                }
                ui_box_end(spacer_inner);

                /* Underline */
                ui_box_end(
                    ui_box_begin(&(BoxConfig){ .sizing = { grow({}), fixed(1) }, .color = cfg->theme->tab_border }));
            }
            ui_box_end(spacer_container);

            /* Inset filler — invisible box reserving space for decoration buttons */
            if (cfg->tab_bar_right_inset > 0)
            {
                UIBox* inset_container =
                    ui_box_begin(&(BoxConfig){ .sizing = { fit({}), grow({}) }, .direction = LAYOUT_TOP_TO_BOTTOM });
                {
                    ui_box_end(ui_box_begin(&(BoxConfig){
                        .sizing = { fixed(cfg->tab_bar_right_inset), grow({}) },
                    }));

                    /* Underline */
                    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(cfg->tab_bar_right_inset), fixed(1) },
                                                          .color = cfg->theme->tab_border }));
                }
                ui_box_end(inset_container);
            }
        }
        ui_box_end(tab_bar);

        /* Scrollable content area */
        ScrollContext panel_content_scroll_ctx = ui_scrollable_area_begin(
            &(ScrollableAreaConfig){ .hash_str = str_fmt(HASH_STR_MAX_LENGTH, "panel_scroll_%u", cfg->panel->id),
                                     .sizing = { grow({}), grow({}) },
                                     .bg = cfg->theme->tab_active_bg,
                                     .padding = cfg->padding,
                                     .child_gap = cfg->child_gap,
                                     .direction = cfg->direction,
                                     .thumb_color = cfg->theme->scrollbar_thumb });

        PanelContext panel_ctx = { .panel = cfg->panel,
                                   .scroll_ctx = panel_content_scroll_ctx,
                                   .outer_box = container,
                                   .panel_w = rect_w,
                                   .panel_h = rect_h,
                                   .tab_bar_spacer_rect = tab_bar_spacer_rect,
                                   .window_id = cfg->window_id,
                                   .cmd_queue = cfg->cmd_queue,
                                   .theme = cfg->theme,
                                   .font_size = cfg->font_size };

        TracyCZoneEnd(ctx_pb);
        return panel_ctx;
    }
}

void ui_panel_end(PanelContext* panel_ctx)
{
    TracyCZoneNC(ctx_pe, "PanelEnd", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);
    ui_scrollable_area_end(panel_ctx->scroll_ctx);

    //  Edge drop zones for docking tabs at panel edges
    //
    //    +----+-------+----+
    //    |    |  Top  |    |
    //    |    +-------+    |
    //    +---+         +---+
    //    | L |         | R |
    //    +---+         +---+
    //    |   +--------+    |
    //    |   | Bottom |    |
    //    +---+--------+----+
    {
        f32 tab_bar_height = panel_ctx->font_size * 2.5f + 2.f - 1; // TODO: don't hard-code ...
        f32 usable_w = panel_ctx->panel_w;
        f32 usable_h = panel_ctx->panel_h - tab_bar_height;
        f32 pad = 12.f;
        f32 usable_w_padded = usable_w - pad * 2;
        f32 usable_h_padded = usable_h - pad * 2;

        /* Layout definitions */
        const char* dock_zone_suffixes[4] = { "Y_before", "Y_after", "X_before", "X_after" };

        // clang-format off
        f32 qw = usable_w_padded * 0.25f;
        f32 qh = usable_h_padded * 0.25f;
        f32 hw = usable_w_padded * 0.5f;
        f32 hh = usable_h_padded * 0.5f;
        Position dock_zone_positions[4]  = {
            { qw,      0            },  // top
            { qw,      hh + qh      },  // bottom
            { 0,       qh           },  // left
            { hw + qw, qh           },  // right
        };
        Sizing dock_zone_sizings[4] = {
            { fixed(hw), fixed(qh) },  // top
            { fixed(hw), fixed(qh) },  // bottom
            { fixed(qw), fixed(hh) },  // left
            { fixed(qw), fixed(hh) },  // right
        };
        // clang-format on

        UIBox* dock_overlay_container =
            ui_box_begin(&(BoxConfig){ .sizing = { fixed(panel_ctx->panel_w), fixed(panel_ctx->panel_h) },
                                       .flags = BoxFlag_Float,
                                       .float_offset = { 0, -usable_h },
                                       .padding = { pad, pad, pad, pad } });
        {
            /* Skip zones when dragging the last tab of this panel onto itself */
            b32 skip_dock_zones = False;
            if (g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
            {
                TabDragPayload* peek = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                if (peek->drag_type == DRAG_TYPE_TAB && peek->from_panel_id == panel_ctx->panel->id &&
                    panel_tab_count(panel_ctx->panel) == 1)
                    skip_dock_zones = True;
            }

            if (!skip_dock_zones)
                for (i32 zone_idx = 0; zone_idx < 4; zone_idx++)
                {
                    UIBox* dock_zone = ui_box_begin(&(BoxConfig){
                        .sizing = dock_zone_sizings[zone_idx],
                        .rect_style = { .corner_radius = 12 },
                        .flags = BoxFlag_Float,
                        .float_offset = dock_zone_positions[zone_idx],
                    });
                    {
                        UIBoxInteractResult zone_interact_res =
                            ui_box_interact(dock_zone, str_fmt(HASH_STR_MAX_LENGTH, "dock_%s_%u",
                                                               dock_zone_suffixes[zone_idx], panel_ctx->panel->id));

                        /* Color transition with DragOver accent */
                        {
                            b32 has_active_drag = g_ui_ctx->drag_active && g_ui_ctx->drag_payload_size;
                            b32 is_zone_hovered = ui_drag_over(zone_interact_res.flags) && has_active_drag;

                            if (zone_interact_res.last_box)
                            {
                                update_transition(&zone_interact_res.last_box->hot_t,
                                                  has_active_drag ? 1.f : 0.f,
                                                  12.f);
                                update_transition(&zone_interact_res.last_box->active_t,
                                                  is_zone_hovered ? 1.f : 0.f,
                                                  12.f);
                            }

                            f32 hot_t = zone_interact_res.last_box ? zone_interact_res.last_box->hot_t : 0.f;
                            f32 active_t = zone_interact_res.last_box ? zone_interact_res.last_box->active_t : 0.f;

                            Color dock_zone_inactive = panel_ctx->theme->tab_accent;
                            dock_zone_inactive.a = (u8)(255 * 0.3f);
                            Color dock_zone_active = panel_ctx->theme->tab_accent;
                            dock_zone_active.a = (u8)(255 * 0.6f);

                            Color dock_zone_inactive_now = lerp_color((Color){ 0 }, dock_zone_inactive, hot_t);
                            dock_zone->cfg.color = lerp_color(dock_zone_inactive_now, dock_zone_active, active_t);
                        }

                        /* Handle drop: create new panel from dragged tab */
                        if (ui_dropped(zone_interact_res.flags) && !g_ui_ctx->drag_payload_consumed &&
                            g_ui_ctx->drag_payload_size >= (isize)sizeof(TabDragPayload))
                        {
                            TabDragPayload* dock_payload = (TabDragPayload*)g_ui_ctx->drag_payload_buf;
                            if (dock_payload->drag_type == DRAG_TYPE_TAB)
                            {
                                g_ui_ctx->drag_payload_consumed = True;
                                cmd_queue_push(panel_ctx->cmd_queue,
                                               str_fmt(CMD_STR_MAX_LENGTH,
                                                       "tab.to_new_panel panel=%u tab=%u to_panel=%u axis=%s side=%s "
                                                       "window=%u to_window=%u",
                                                       dock_payload->from_panel_id, dock_payload->from_tab_id,
                                                       panel_ctx->panel->id, (zone_idx < 2) ? "Y" : "X",
                                                       (zone_idx == 0 || zone_idx == 2) ? "before" : "after",
                                                       dock_payload->from_window_id, panel_ctx->window_id));
                            }
                        }
                    }
                    ui_box_end(dock_zone);
                }
        }
        ui_box_end(dock_overlay_container);
    }
    ui_box_end(panel_ctx->outer_box);
    TracyCZoneEnd(ctx_pe);
}

//
// Widgets: Simple
//

#define CHECKBOX_HEIGHT   22
#define CHECKBOX_PAD      3
#define CURSORBAR_PADDING 7
#define IME_OFFSET_TOP    (-12)

UISignalFlags ui_button(const String text_with_hash_str, const Font* font, f32 font_size, Sizing sizing,
                        Padding padding, Color bg, Color fg, Color hover_bg)
{
    TracyCZoneNC(ctx_btn, "Button", TracyColor_Widget, TRACY_SUBSYSTEMS & TracySys_Widget);

    /* Create button box */
    UIBox* box = ui_box_begin(&(BoxConfig){ .sizing = sizing,
                                            .rect_style = { .corner_radius = 4 },
                                            .padding = padding,
                                            .alignment = { ALIGN_CENTER, ALIGN_CENTER } });

    /* Handle interaction */
    TextHash text_hash = extract_hash_str(&text_with_hash_str);
    UIBoxInteractResult result = ui_box_interact(box, text_hash.hash_str);
    if (ui_hovered(result.flags))
        bg = hover_bg;
    box->cfg.color = bg;

    ui_text(
        text_hash.display_str,
        &(TextConfig){ .font = font, .font_size = font_size, .color = fg, .line_height = font_size, .wrap = False });
    ui_box_end(box);

    TracyCZoneEnd(ctx_btn);
    return result.flags;
}

UISignalFlags ui_switchbox(const String hash_str, const Font* font, b32* check, const Color bg, const Color active_bg,
                           const Color fg, const Color shadow_color)
{
    TracyCZoneNC(ctx_sw, "Switchbox", TracyColor_Widget, TRACY_SUBSYSTEMS & TracySys_Widget);

    /* Transition-related variables */
    Color bg_color_transition = bg;
    Color status_color_ok = fg;
    Color status_color_cancel = fg;
    f32 pad_width = 0.f;
    f32 shadow_offset_x = 0.f;

    /* Create checkbox */
    UIBox* container = ui_box_begin(&(BoxConfig){ .sizing = { fixed(CHECKBOX_HEIGHT * 2), fixed(CHECKBOX_HEIGHT) },
                                                  .rect_style = { .corner_radius = CHECKBOX_HEIGHT / 2 },
                                                  .padding = { CHECKBOX_PAD, CHECKBOX_PAD, CHECKBOX_PAD, CHECKBOX_PAD },
                                                  .alignment = { ALIGN_START, ALIGN_CENTER } });

    /* Handle interaction and transition */
    UIBoxInteractResult result = ui_box_interact(container, hash_str);

    /* Transition */
    if (ui_lclicked(result.flags) || (result.last_box->active_t > 0))
    {
        if (ui_lclicked(result.flags))
            result.last_box->anim_state = *check ? TRANSITION_REVERSE : TRANSITION_FORWARD;
        if (result.last_box->anim_state != TRANSITION_IDLE)
            if (update_transition(&result.last_box->active_t,
                                  result.last_box->anim_state == TRANSITION_REVERSE ? 0.f : 1.f,
                                  18.f))
                result.last_box->anim_state = TRANSITION_IDLE;
    }
    bg_color_transition = lerp_color(bg_color_transition, active_bg, result.last_box->active_t);
    status_color_ok.a = lerp_u8(0, 255, result.last_box->active_t);
    status_color_cancel.a = lerp_u8(255, 0, result.last_box->active_t);
    pad_width = lerp_f32(0.f, CHECKBOX_HEIGHT, result.last_box->active_t);
    shadow_offset_x = lerp_f32(0.5f, -0.5f, result.last_box->active_t);

    container->cfg.color = bg_color_transition;
    {
        /* left padding */
        UIBox* pad_left = ui_box_begin(
            &(BoxConfig){ .sizing = { fixed(pad_width), grow({}) }, .alignment = { ALIGN_START, ALIGN_CENTER } });
        ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(5), grow({}) } }));
        ui_text(str(ICON_FONT_UTF8_OK), &(TextConfig){ .font = font, .font_size = 9, .color = status_color_ok });
        ui_box_end(pad_left);

        /* switch button */
        f32 switch_button_width = CHECKBOX_HEIGHT - CHECKBOX_PAD * 2;
        ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(switch_button_width), fixed(switch_button_width) },
                                              .color = fg,
                                              .rect_style = { .corner_radius = switch_button_width / 2,
                                                              .shadow_color = shadow_color,
                                                              .shadow_offset = { shadow_offset_x, 0.5f },
                                                              .shadow_sigma = 1.2f } }));

        /* right padding */
        UIBox* pad_right = ui_box_begin(&(BoxConfig){ .sizing = { fixed(CHECKBOX_HEIGHT - pad_width), grow({}) },
                                                      .alignment = { ALIGN_END, ALIGN_CENTER } });
        ui_text(str(ICON_FONT_UTF8_CANCEL),
                &(TextConfig){ .font = font, .font_size = 9, .color = status_color_cancel });
        ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(6), grow({}) } }));
        ui_box_end(pad_right);
    }
    ui_box_end(container);

    TracyCZoneEnd(ctx_sw);
    return result.flags;
}

//
// Widgets: Text Field
//

static isize scan_codepoint_forward(const byte* base, const isize text_len, isize pos, isize count)
{
    while (count > 0 && pos < text_len)
    {
        pos++;
        while (pos < text_len && (base[pos] & 0xC0) == 0x80)
            pos++;
        count--;
    }
    return pos;
}

static isize scan_codepoint_backward(const byte* base, isize pos, isize count)
{
    while (count > 0 && pos > 0)
    {
        pos--;
        while (pos > 0 && (base[pos] & 0xC0) == 0x80)
            pos--;
        count--;
    }
    return pos;
}

static b32 is_word_char(const u32 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static isize scan_word_forward(const byte* base, isize text_len, isize pos)
{
    while (pos < text_len && is_word_char(base[pos]))
        pos++;
    while (pos < text_len && !is_word_char(base[pos]))
        pos++;
    return pos;
}

static isize scan_word_backward(const byte* base, isize pos)
{
    while (pos > 0 && !is_word_char(base[pos - 1]))
        pos--;
    while (pos > 0 && is_word_char(base[pos - 1]))
        pos--;
    return pos;
}

static isize find_cursor_at_x(byte* base, const isize text_len, const f32 target_x, GlyphRasterCache* raster_cache,
                              const Font* font, const f32 font_size, const u32 dpi,
                              const get_text_width_fn get_text_width, struct Renderer* renderer)
{
    if (text_len == 0 || target_x <= 0.f)
        return 0;
    isize pos = 0;
    while (pos < text_len)
    {
        isize next = scan_codepoint_forward(base, text_len, pos, 1);
        String to_next = { base, next };
        if (get_text_width(renderer, raster_cache, to_next, font, font_size, dpi) >= target_x)
        {
            String to_pos = { base, pos };
            f32 w_pos = get_text_width(renderer, raster_cache, to_pos, font, font_size, dpi);
            f32 w_next = get_text_width(renderer, raster_cache, to_next, font, font_size, dpi);
            return (target_x - w_pos >= w_next - target_x) ? next : pos;
        }
        pos = next;
    }
    return text_len;
}

static void delete_selection(TextEditState* state)
{
    if (state->cursor == state->mark)
        return;
    isize start = state->cursor < state->mark ? state->cursor : state->mark;
    isize end = state->cursor > state->mark ? state->cursor : state->mark;
    memmove(state->base + start, state->base + end, state->text_len - end);
    state->text_len -= end - start;
    state->cursor = start;
    state->mark = start;
}

static void insert_text_at_cursor(TextEditState* state, const byte* text, const isize len)
{
    if (len == 0 || state->text_len + len > state->size)
        return;
    memmove(state->base + state->cursor + len, state->base + state->cursor, state->text_len - state->cursor);
    memcpy(state->base + state->cursor, text, len);
    state->cursor += len;
    state->text_len += len;
    state->mark = state->cursor;
}

static void cursor_bar(const f32 parent_height, const Padding parent_padding, const f32 x_offset, const Color color)
{
    f32 bar_height = parent_height - CURSORBAR_PADDING * 2;
    Position float_offset = { x_offset, -parent_padding.top + CURSORBAR_PADDING };
    ui_box_end(ui_box_begin(&(BoxConfig){ .sizing = { fixed(2), fixed(bar_height) },
                                          .color = color,
                                          .flags = BoxFlag_Float,
                                          .float_offset = float_offset }));
}

UISignalFlags ui_text_field(TextEditState* state, const String text_with_hash_str, const Font* font,
                            const f32 font_size, const SizingAxis sizing_x, const Padding padding, const Color bg,
                            const Color border_color, const Color fg, const Color thumb_color,
                            const Color cursor_bar_color, const Color cursor_trail_color, const Color selection_color,
                            const Color selection_flash_color)
{
    TracyCZoneNC(ctx_tf, "TextField", TracyColor_Widget, TRACY_SUBSYSTEMS & TracySys_Widget);
    // clang-format off
    get_text_height_fn get_text_height = g_ui_ctx->render_fn.get_text_height;
    get_text_width_fn get_text_width = g_ui_ctx->render_fn.get_text_width;
    struct Renderer* renderer = g_ui_ctx->renderer;
    f32 text_height = get_text_height(renderer, g_ui_ctx->raster_cache, text_with_hash_str, font, font_size, g_ui_ctx->dpi);
    SizingAxis text_container_height = fixed(text_height + padding.top + padding.bottom);
    Color placeholder_color = { fg.r, fg.g, fg.b, fg.a / 2 };
    TextConfig text_cfg = { .font = font, .font_size = font_size, .line_height = font_size, .wrap = False };
    // clang-format on

    /* Transition-related variables */
    Color border_color_transition = border_color;

    /* Handle interaction */
    TextHash text_hash = extract_hash_str(&text_with_hash_str);
    u64 text_hash_key = fnv1a_64(text_hash.hash_str.data, text_hash.hash_str.len);
    u64 content_hash = fnv1a_64_continue(text_hash_key, " (content box)", 14);
    isize cursor_before = state->cursor;
    b32 is_focused = text_hash_key == g_ui_ctx->focused_hash;
    if (is_focused)
    {
        /* IME composition: delete previous range, then insert new composition */
        if (state->composition_len > 0)
        {
            isize comp_start = state->composition_start;
            isize comp_end = comp_start + state->composition_len;
            memmove(state->base + comp_start, state->base + comp_end, state->text_len - comp_end);
            state->text_len -= state->composition_len;
            if (state->cursor > comp_start)
                state->cursor = state->cursor > comp_end ? state->cursor - state->composition_len : comp_start;
            if (state->mark > comp_start)
                state->mark = state->mark > comp_end ? state->mark - state->composition_len : comp_start;
            state->composition_len = 0;
        }

        if (g_ui_ctx->ime_composing && g_ui_ctx->ime_composition.len > 0)
        {
            String comp = g_ui_ctx->ime_composition;
            Assert(state->text_len + comp.len <= state->size);
            memmove(state->base + state->cursor + comp.len, state->base + state->cursor,
                    state->text_len - state->cursor);
            memcpy(state->base + state->cursor, comp.data, comp.len);
            state->composition_start = state->cursor;
            state->composition_len = comp.len;
            state->text_len += comp.len;
            state->cursor += comp.len;
            state->mark = state->cursor;
        }

        /* Unified action processing (navigation + deletion) */
        for (isize i = 0; i < g_ui_ctx->text_action_queue_count; i++)
        {
            TextAction* action = &g_ui_ctx->text_action_queue[i];

            /* Copy: grab selection to clipboard (before any mutation) */
            if ((action->flags & TextActionFlag_Copy) && g_ui_ctx->clipboard_copy && state->cursor != state->mark)
            {
                isize start = state->cursor < state->mark ? state->cursor : state->mark;
                isize end = state->cursor > state->mark ? state->cursor : state->mark;
                g_ui_ctx->clipboard_copy(g_ui_ctx->window, (String){ state->base + start, end - start });
                state->copy_t = 1.f;
            }

            /* SelectAll: set cursor to end, mark to beginning */
            if (action->flags & TextActionFlag_SelectAll)
            {
                state->cursor = state->text_len;
                state->mark = 0;
                continue;
            }

            if (action->flags & TextActionFlag_Delete)
            {
                /* ------- Deletion ------- */
                isize new_cursor = state->cursor;
                isize delta = action->delta;

                /* ZeroDeltaWithSelection: if selection exists, delete it without moving */
                if (state->cursor != state->mark && (action->flags & TextActionFlag_ZeroDeltaWithSelection))
                    delta = 0;

                /* Resolve delta via word-scan or codepoint-scan */
                if (delta != 0)
                {
                    if (action->flags & TextActionFlag_WordScan)
                    {
                        if (delta > 0)
                            new_cursor = scan_word_forward(state->base, state->text_len, state->cursor);
                        else
                            new_cursor = scan_word_backward(state->base, state->cursor);
                    }
                    else
                    {
                        if (delta > 0)
                            new_cursor = scan_codepoint_forward(state->base, state->text_len, state->cursor, delta);
                        else
                            new_cursor = scan_codepoint_backward(state->base, state->cursor, -delta);
                    }
                }

                /* Compute and delete the range */
                isize del_start, del_end;
                if (delta == 0)
                {
                    del_start = state->cursor < state->mark ? state->cursor : state->mark;
                    del_end = state->cursor > state->mark ? state->cursor : state->mark;
                }
                else
                {
                    del_start = state->cursor < new_cursor ? state->cursor : new_cursor;
                    del_end = state->cursor > new_cursor ? state->cursor : new_cursor;
                }
                if (del_start < del_end)
                {
                    memmove(state->base + del_start, state->base + del_end, state->text_len - del_end);
                    state->text_len -= del_end - del_start;
                }
                state->cursor = del_start;
                state->mark = state->cursor;
            }
            else
            {
                /* Paste: insert clipboard text at cursor */
                if ((action->flags & TextActionFlag_Paste) && g_ui_ctx->clipboard_paste)
                {
                    String pasted = g_ui_ctx->clipboard_paste(g_ui_ctx->window, &g_ui_ctx->arena);
                    if (pasted.len > 0)
                    {
                        delete_selection(state);
                        insert_text_at_cursor(state, pasted.data, pasted.len);
                    }
                }

                /* ------- Navigation ------- */
                isize new_cursor;

                /* DeltaPicksSelectionSide: Right/Left on selection → jump to max/min side */
                if (state->cursor != state->mark && (action->flags & TextActionFlag_DeltaPicksSelectionSide))
                {
                    if (action->delta > 0)
                        new_cursor = state->cursor > state->mark ? state->cursor : state->mark;
                    else if (action->delta < 0)
                        new_cursor = state->cursor < state->mark ? state->cursor : state->mark;
                    else
                        new_cursor = state->cursor;
                }
                else
                {
                    if (action->flags & TextActionFlag_WordScan)
                    {
                        if (action->delta > 0)
                            new_cursor = scan_word_forward(state->base, state->text_len, state->cursor);
                        else if (action->delta < 0)
                            new_cursor = scan_word_backward(state->base, state->cursor);
                        else
                            new_cursor = state->cursor;
                    }
                    else
                    {
                        if (action->delta > 0)
                            new_cursor =
                                scan_codepoint_forward(state->base, state->text_len, state->cursor, action->delta);
                        else if (action->delta < 0)
                            new_cursor = scan_codepoint_backward(state->base, state->cursor, -action->delta);
                        else
                            new_cursor = state->cursor;
                    }
                }

                state->cursor = new_cursor;
                if (!(action->flags & TextActionFlag_KeepMark) && !(action->flags & TextActionFlag_Copy) &&
                    !(action->flags & TextActionFlag_Paste))
                    state->mark = state->cursor;
            }
        }

        /* Text input (printable characters only) */
        for (isize i = 0; i < g_ui_ctx->char_input_queue_count; i++)
        {
            u32 codepoint = g_ui_ctx->char_input_queue[i];
            if (codepoint == 0)
                continue;
            delete_selection(state);
            u8 utf8[4];
            isize len = utf8_encode(utf8, codepoint);
            Assert(state->text_len + len <= state->size);
            insert_text_at_cursor(state, utf8, len);
        }

        /* Mouse drag to extend selection (only continue existing press, not new click) */
        if (!g_ui_ctx->mouse_captured_by_hash && is_focused && g_ui_ctx->mouse_press && !g_ui_ctx->mouse_lclick &&
            state->text_len > 0 && (g_ui_ctx->mouse_delta.x != 0.f || g_ui_ctx->mouse_delta.y != 0.f))
        {
            UIBox* last_inner_box = find_or_insert_box_with_hash(content_hash);
            if (last_inner_box)
            {
                f32 click_x = g_ui_ctx->mouse_pos.x - last_inner_box->position.x - padding.left;
                b32 inside_y = g_ui_ctx->mouse_pos.y >= last_inner_box->position.y &&
                               g_ui_ctx->mouse_pos.y <=
                                   last_inner_box->position.y + last_inner_box->size.height - SCROLLBAR_THICKNESS_MAX;
                if (click_x >= 0.f && inside_y)
                {
                    state->cursor =
                        find_cursor_at_x(state->base, state->text_len, click_x, g_ui_ctx->raster_cache, font, font_size,
                                         g_ui_ctx->dpi, get_text_width, g_ui_ctx->renderer);
                }
            }
        }
    }

    /* Copy and Cursor glide/trail animation */
    update_transition(&state->copy_t, 0.f, 1.5f);
    if (is_focused)
    {
        f32 cursor_x = 0.f;
        if (state->text_len > 0)
        {
            String text_to_cursor = { state->base, state->cursor };
            cursor_x = get_text_width(renderer, g_ui_ctx->raster_cache, text_to_cursor, font, font_size, g_ui_ctx->dpi);
        }
        update_transition(&state->cursor_glide_x, cursor_x, 34.f);
        update_transition(&state->cursor_trail_x, cursor_x, 12.f);
    }

    /* Create text field box */
    UIBoxInteractResult result = { 0 };
    UIBox* box = ui_box_begin(&(BoxConfig){
        .sizing = { sizing_x, text_container_height },
        .rect_style = { .corner_radius = 4, .border_thickness = 2 },
        .color = bg,
        .flags = BoxFlag_Clip,
    });
    {
        /* Handle interaction and transition */
        result = ui_box_interact(box, text_hash.hash_str);
        if (ui_hovered(result.flags))
            ui_set_desired_cursor(UI_CURSOR_IBEAM);
        if (ui_lclicked(result.flags) || is_focused)
        {
            if (ui_lclicked(result.flags))
            {
                g_ui_ctx->focused_hash = generate_box_key(text_hash.hash_str);

                /* Position cursor at click point */
                if (state->text_len > 0)
                {
                    UIBox* last_inner_box = find_or_insert_box_with_hash(content_hash);
                    if (last_inner_box)
                    {
                        f32 click_x = g_ui_ctx->mouse_pos.x - last_inner_box->position.x - padding.left;
                        b32 inside_y = g_ui_ctx->mouse_pos.y >= last_inner_box->position.y &&
                                       g_ui_ctx->mouse_pos.y <= last_inner_box->position.y +
                                                                    last_inner_box->size.height -
                                                                    SCROLLBAR_THICKNESS_MAX;
                        if (click_x >= 0.f && inside_y)
                        {
                            state->cursor =
                                find_cursor_at_x(state->base, state->text_len, click_x, g_ui_ctx->raster_cache, font,
                                                 font_size, g_ui_ctx->dpi, get_text_width, g_ui_ctx->renderer);
                            state->mark = state->cursor;

                            if (ui_double_clicked(result.flags))
                            {
                                isize word_start = scan_word_backward(state->base, state->cursor);
                                isize word_end = scan_word_forward(state->base, state->text_len, state->cursor);
                                while (word_start < word_end && !is_word_char(state->base[word_start]))
                                    word_start++;
                                while (word_end > word_start && !is_word_char(state->base[word_end - 1]))
                                    word_end--;
                                state->mark = word_start;
                                state->cursor = word_end;
                            }
                        }
                    }
                }
            }
            update_transition(&result.last_box->active_t, 1.f, 20.f);
        }
        else
            update_transition(&result.last_box->active_t, 0.f, 18.f);
        border_color_transition.a = lerp_u8(0, border_color.a, result.last_box->active_t);
        box->cfg.rect_style.border_color = border_color_transition;
        {
            b32 cursor_moved = state->cursor != cursor_before;

            ScrollContext scroll_ctx = ui_scrollable_area_begin(&(ScrollableAreaConfig){
                .hash_str = str_concat(&g_ui_ctx->arena, text_hash.hash_str, str(" (scroll area)")),
                .sizing = { grow({}), grow({}) },
                .thumb_color = thumb_color,
                .fixed_track = True });
            scroll_ctx.scroll_margin = font_size * 2.f;
            if (cursor_moved && state->text_len > 0)
            {
                String text_to_cursor = { state->base, state->cursor };
                scroll_ctx.cursor_content_x =
                    get_text_width(renderer, g_ui_ctx->raster_cache, text_to_cursor, font, font_size, g_ui_ctx->dpi) +
                    padding.left;
            }
            {
                b32 has_text = state->text_len > 0;
                String full_text = has_text ? (String){ state->base, state->text_len } : text_hash.display_str;
                f32 text_width =
                    get_text_width(renderer, g_ui_ctx->raster_cache, full_text, font, font_size, g_ui_ctx->dpi);
                f32 inner_width = text_width + padding.left + padding.right;

                UIBox* inner =
                    ui_box_begin(&(BoxConfig){ .sizing = { fixed(inner_width), fit_grow({}) }, .padding = padding });
                {
                    b32 has_selection = is_focused && state->cursor != state->mark;
                    isize sel_start = state->cursor < state->mark ? state->cursor : state->mark;
                    isize sel_end = state->cursor > state->mark ? state->cursor : state->mark;
                    f32 cursor_x = 0.f;
                    f32 trail_x = 0.f;
                    f32 glide_x = 0.f;
                    if (is_focused && has_text)
                    {
                        String text_to_cursor = { state->base, state->cursor };
                        cursor_x = get_text_width(renderer, g_ui_ctx->raster_cache, text_to_cursor, font, font_size,
                                                  g_ui_ctx->dpi);
                    }
                    if (is_focused)
                    {
                        trail_x = state->cursor_trail_x;
                        glide_x = state->cursor_glide_x;
                    }
                    f32 glide_offset = glide_x - cursor_x;

                    /* Cursor trail (spans from slow trail position to fast glide position) */
                    if (is_focused)
                    {
                        f32 t_start = trail_x < glide_x ? trail_x : glide_x;
                        f32 t_width = trail_x > glide_x ? trail_x - glide_x : glide_x - trail_x;
                        if (t_width > 0.5f)
                        {
                            f32 trail_h = text_container_height.min_max.min - CURSORBAR_PADDING * 2;
                            Color base = cursor_trail_color;
                            Color fade = { base.r, base.g, base.b, base.a / 4 };
                            Color corners[4] = { base, base, base, base };
                            if (trail_x < glide_x)
                            {
                                corners[0] = fade;
                                corners[2] = fade;
                            }
                            else
                            {
                                corners[1] = fade;
                                corners[3] = fade;
                            }
                            ui_box_end(ui_box_begin(&(BoxConfig){
                                .sizing = { fixed(t_width), fixed(trail_h) },
                                .color = base,
                                .rect_style = { .corner_colors = { corners[0], corners[1], corners[2], corners[3] } },
                                .flags = BoxFlag_Float,
                                .float_offset = { t_start, -padding.top + CURSORBAR_PADDING },
                            }));
                        }
                    }

                    /* IME composition underline */
                    if (is_focused && state->composition_len > 0)
                    {
                        String text_before = { state->base, state->composition_start };
                        f32 comp_x = get_text_width(renderer, g_ui_ctx->raster_cache, text_before, font, font_size,
                                                    g_ui_ctx->dpi);
                        String comp_text = { state->base + state->composition_start, state->composition_len };
                        f32 comp_w =
                            get_text_width(renderer, g_ui_ctx->raster_cache, comp_text, font, font_size, g_ui_ctx->dpi);
                        if (comp_w > 0.f)
                        {
                            f32 underline_y = font_size;
                            ui_box_end(ui_box_begin(&(BoxConfig){
                                .sizing = { fixed(comp_w), fixed(1) },
                                .color = fg,
                                .flags = BoxFlag_Float,
                                .float_offset = { comp_x, underline_y },
                            }));
                        }
                    }

                    if (has_text)
                    {
                        /* Text before selection (or before cursor) */
                        if (sel_start > 0)
                        {
                            String pre_text = { state->base, sel_start };
                            text_cfg.color = fg;
                            ui_text(pre_text, &text_cfg);
                        }

                        /* Selection highlight (only when focused and selection exists) */
                        if (has_selection)
                        {
                            String sel_text = { state->base + sel_start, sel_end - sel_start };
                            f32 sel_width = get_text_width(renderer, g_ui_ctx->raster_cache, sel_text, font, font_size,
                                                           g_ui_ctx->dpi);
                            f32 sel_height = text_container_height.min_max.min - CURSORBAR_PADDING * 2;
                            Color sel_color = selection_color;
                            Color copy_flash = selection_flash_color;
                            sel_color = lerp_color(sel_color, copy_flash, state->copy_t);
                            ui_box_end(ui_box_begin(&(BoxConfig){
                                .sizing = { fixed(sel_width), fixed(sel_height) },
                                .color = sel_color,
                                .rect_style = { .corner_radius = 2 },
                                .flags = BoxFlag_Float,
                                .float_offset = { 0, -padding.top + CURSORBAR_PADDING },
                            }));

                            if (state->cursor == sel_start)
                                cursor_bar(text_container_height.min_max.min, padding, glide_offset, cursor_bar_color);
                        }

                        /* Selected text (always rendered; highlight box sits behind when focused) */
                        if (sel_start < sel_end)
                        {
                            String sel_text = { state->base + sel_start, sel_end - sel_start };
                            text_cfg.color = fg;
                            ui_text(sel_text, &text_cfg);
                        }

                        /* Cursorbar at cursor position */
                        if (is_focused && state->cursor == sel_end)
                            cursor_bar(text_container_height.min_max.min, padding, glide_offset, cursor_bar_color);

                        /* Text after selection (or after cursor) */
                        if (sel_end < state->text_len)
                        {
                            String post_text = { state->base + sel_end, state->text_len - sel_end };
                            text_cfg.color = fg;
                            ui_text(post_text, &text_cfg);
                        }
                    }
                    else
                    {
                        if (is_focused)
                            cursor_bar(text_container_height.min_max.min, padding, glide_offset, cursor_bar_color);
                        String placeholder = text_hash.display_str;
                        text_cfg.color = placeholder_color;
                        ui_text(placeholder, &text_cfg);
                    }
                }
                ui_box_end(inner);
                update_box_key(inner, content_hash);
            }
            ui_scrollable_area_end(scroll_ctx);
        }
    }
    ui_box_end(box);

    /* Update IME candidate window position */
    if (is_focused && result.last_box)
    {
        f32 ime_cursor_x = 0.f;
        if (state->composition_len > 0)
        {
            String text_before = { state->base, state->composition_start };
            ime_cursor_x =
                get_text_width(renderer, g_ui_ctx->raster_cache, text_before, font, font_size, g_ui_ctx->dpi);
        }
        else if (state->text_len > 0)
        {
            String text_to_cursor = { state->base, state->cursor };
            ime_cursor_x =
                get_text_width(renderer, g_ui_ctx->raster_cache, text_to_cursor, font, font_size, g_ui_ctx->dpi);
        }
        f32 dpi_scale = (f32)g_ui_ctx->dpi / USER_DEFAULT_SCREEN_DPI;

        LONG cx = (LONG)((result.last_box->position.x + padding.left + ime_cursor_x) * dpi_scale);
        LONG cy = (LONG)((result.last_box->position.y + result.last_box->size.height + IME_OFFSET_TOP) * dpi_scale);

        win32_ime_update_candidate(g_ui_ctx->window, cx, cy, &g_ui_ctx->ime_cursor_screen_pos);
    }

    TracyCZoneEnd(ctx_tf);
    return result.flags;
}
