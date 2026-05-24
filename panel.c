#include "panel.h"
#include "utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thirdparty/tracy/public/tracy/TracyC.h"
#include "tracy_config.h" // IWYU pragma: keep

#define PANEL_STACK_CAPACITY 16
#define AFFECTED_MAX         256

static u32 s_panel_next_id = 1; /* 0 reserved for none */
static u32 s_tab_next_id = 1; /* 0 reserved for none */

Panel* panel_alloc(void)
{
    Panel* p = (Panel*)calloc(1, sizeof(Panel));
    Assert(p);
    p->id = s_panel_next_id++;
    return p;
}

Panel* panel_find_by_id(const Panel* root, u32 id)
{
    for (const Panel* p = root; p; p = panel_iter_next(p))
        if (p->id == id)
            return (Panel*)p;
    return NULL;
}

Panel* panel_iter_next(const Panel* panel)
{
    if (panel->child_a)
        return panel->child_a;

    for (const Panel* p = panel; p; p = p->parent)
        if (p->next)
            return p->next;

    return NULL;
}

Rect panel_calc_rect_from_parent(const Panel* child, const Rect parent_rect)
{
    const Panel* parent = child->parent;
    if (!parent)
        return parent_rect;

    Rect result = parent_rect;
    f32 parent_dim =
        (parent->split_axis == Axis2_X) ? parent_rect.xmax - parent_rect.xmin : parent_rect.ymax - parent_rect.ymin;

    f32 offset = 0.f;
    for (const Panel* p = parent->child_a; p != child; p = p->next)
        offset += p->pct_of_parent * parent_dim;

    f32 child_dim = child->pct_of_parent * parent_dim;
    if (parent->split_axis == Axis2_X)
        result.xmin += offset, result.xmax = result.xmin + child_dim;
    else
        result.ymin += offset, result.ymax = result.ymin + child_dim;

    return result;
}

Rect panel_calc_rect(const Panel* panel, const Rect root_rect)
{
    TracyCZoneNC(ctx_cr, "CalcRect", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);
    Rect result = root_rect;
    if (panel->parent)
    {
        Rect parent_rect = panel_calc_rect(panel->parent, root_rect);
        result = panel_calc_rect_from_parent(panel, parent_rect);
    }
    TracyCZoneEnd(ctx_cr);
    return result;
}

//
// Tab management
//

void panel_tab_generate_default_name(const Panel* panel, u8* buf, const isize buf_size, isize* out_len)
{
    isize next_num = 1;
    for (const PanelTab* tab = panel->tab_first; tab; tab = tab->next)
    {
        isize num = 0;
        if (sscanf((const char*)tab->name, "Tab %zd", &num) == 1)
        {
            if (num >= next_num)
                next_num = num + 1;
        }
    }
    *out_len = (isize)snprintf((char*)buf, buf_size, "Tab %zd", next_num);
}

PanelTab* panel_tab_declare(Panel* panel, const String name)
{
    Assert(name.len > 0 && name.len < PANEL_TAB_NAME_MAX);

    /* Search for existing tab by name */
    for (PanelTab* tab = panel->tab_first; tab; tab = tab->next)
        if (tab->name_len == name.len && memcmp(tab->name, name.data, (size_t)name.len) == 0)
        {
            tab->frame_declared = True;
            return tab;
        }

    /* Create new tab */
    PanelTab* tab = (PanelTab*)calloc(1, sizeof(PanelTab));
    Assert(tab);
    tab->id = s_tab_next_id++;
    memcpy(tab->name, name.data, (size_t)name.len);
    tab->name_len = name.len;
    tab->frame_declared = True;

    /* Append to linked list */
    if (!panel->tab_last)
    {
        panel->tab_first = tab;
        panel->tab_last = tab;
    }
    else
    {
        panel->tab_last->next = tab;
        panel->tab_last = tab;
    }

    /* Auto-activate first tab */
    if (!panel->active_tab)
        panel->active_tab = tab;

    return tab;
}

PanelTab* panel_find_tab_by_id(const Panel* root, u32 tab_id)
{
    for (const Panel* p = root; p; p = panel_iter_next(p))
    {
        if (p->child_a)
            continue;
        for (PanelTab* t = p->tab_first; t; t = t->next)
            if (t->id == tab_id)
                return t;
    }
    return NULL;
}

PanelTab* panel_tab_get_active(Panel* panel)
{
    if (panel->active_tab)
        return panel->active_tab;
    /* Auto-select first tab if none is active */
    if (panel->tab_first)
        panel->active_tab = panel->tab_first;
    return panel->active_tab;
}

void panel_tab_activate(Panel* panel, PanelTab* tab)
{
    if (!tab)
        return;
    /* Verify the tab belongs to this panel */
    for (PanelTab* t = panel->tab_first; t; t = t->next)
    {
        if (t == tab)
        {
            panel->active_tab = tab;
            return;
        }
    }
}

static void panel_tab_list_refresh_last(Panel* panel)
{
    if (!panel->tab_first)
    {
        panel->tab_last = NULL;
        return;
    }
    PanelTab* t = panel->tab_first;
    while (t->next)
        t = t->next;
    panel->tab_last = t;
}

void panel_tab_close(Panel* panel, PanelTab* tab)
{
    if (!tab)
        return;

    /* Unlink from singly-linked list */
    PanelTab** prev = &panel->tab_first;
    while (*prev && *prev != tab)
        prev = &(*prev)->next;

    if (*prev != tab)
        return; /* tab not found in this panel */

    *prev = tab->next;
    panel_tab_list_refresh_last(panel);

    /* Reassign active tab if needed */
    if (panel->active_tab == tab)
        panel->active_tab = tab->next ? tab->next : panel->tab_first;

    free(tab);
}

void panel_tab_move(Panel* panel, PanelTab* tab, const i32 delta)
{
    if (!tab || delta == 0)
        return;

    /* Find current index and previous-pointer */
    isize cur_index = 0;
    PanelTab** prev_ptr = &panel->tab_first;
    b32 found = False;
    for (PanelTab* t = panel->tab_first; t; t = t->next, cur_index++)
    {
        if (t == tab)
        {
            found = True;
            break;
        }
        prev_ptr = &t->next;
    }
    if (!found)
        return;

    /* Compute target index, clamped */
    isize new_index = cur_index + delta;
    if (new_index < 0)
        new_index = 0;

    /* Unlink from current position */
    *prev_ptr = tab->next;
    panel_tab_list_refresh_last(panel);
    tab->next = NULL;

    /* Count remaining tabs */
    isize remaining = 0;
    for (PanelTab* t = panel->tab_first; t; t = t->next)
        remaining++;
    if (new_index > remaining)
        new_index = remaining;

    /* Insert at target index */
    if (new_index == 0)
    {
        tab->next = panel->tab_first;
        panel->tab_first = tab;
        if (!panel->tab_last)
            panel->tab_last = tab;
    }
    else
    {
        PanelTab* prev = panel->tab_first;
        for (isize i = 1; i < new_index; i++)
            prev = prev->next;
        tab->next = prev->next;
        prev->next = tab;
        if (!tab->next)
            panel->tab_last = tab;
    }
}

isize panel_tab_count(const Panel* panel)
{
    isize n = 0;
    for (PanelTab* t = panel->tab_first; t; t = t->next)
        n++;
    return n;
}

void panel_tab_move_to_panel(Panel* from, PanelTab* tab, Panel* to, i32 to_idx)
{
    if (!from || !tab || !to || from == to)
        return;

    /* Verify tab belongs to 'from' */
    PanelTab** prev = &from->tab_first;
    b32 found = False;
    while (*prev)
    {
        if (*prev == tab)
        {
            found = True;
            break;
        }
        prev = &(*prev)->next;
    }
    if (!found)
        return;

    /* Unlink from source panel */
    *prev = tab->next;
    panel_tab_list_refresh_last(from);
    tab->next = NULL;
    if (from->active_tab == tab)
        from->active_tab = tab->next ? tab->next : from->tab_first;

    /* Close source panel if now empty */
    if (!from->tab_first && from->parent)
        from->pending_remove = True;

    /* Insert into destination panel at specified position */
    if (to_idx < 0 || to_idx >= panel_tab_count(to))
    {
        /* Append to end */
        if (!to->tab_last)
        {
            to->tab_first = tab;
            to->tab_last = tab;
        }
        else
        {
            to->tab_last->next = tab;
            to->tab_last = tab;
        }
    }
    else if (to_idx == 0)
    {
        /* Prepend */
        tab->next = to->tab_first;
        to->tab_first = tab;
        if (!to->tab_last)
            to->tab_last = tab;
    }
    else
    {
        /* Insert before position to_idx */
        PanelTab* pos = to->tab_first;
        for (i32 i = 1; i < to_idx; i++)
            pos = pos->next;
        tab->next = pos->next;
        pos->next = tab;
        if (!tab->next)
            to->tab_last = tab;
    }

    /* Activate the moved tab in the destination */
    to->active_tab = tab;
}

Panel* panel_tab_to_new_panel(Panel* from, PanelTab* tab, Panel* anchor, const Axis2 axis, PanelDockSide side)
{
    if (!from || !tab || !anchor || anchor->child_a)
        return NULL;

    /* Find and remove tab from source panel */
    PanelTab** prev = &from->tab_first;
    b32 found = False;
    while (*prev)
    {
        if (*prev == tab)
        {
            found = True;
            break;
        }
        prev = &(*prev)->next;
    }
    if (!found)
        return NULL;

    *prev = tab->next;
    panel_tab_list_refresh_last(from);
    tab->next = NULL;
    if (from->active_tab == tab)
        from->active_tab = tab->next ? tab->next : from->tab_first;

    /* Close source panel if now empty (cross-panel dock only) */
    if (from != anchor && !from->tab_first && from->parent)
        from->pending_remove = True;

    /* Split the anchor panel; child_a inherits anchor's tabs */
    if (!panel_split(anchor, axis, False))
        return NULL;

    if (side == PanelDockSide_Before)
    {
        Panel* child_a = anchor->child_a;
        Panel* child_b = anchor->child_b;

        /* Move anchor's original tabs from child_a to child_b */
        child_b->tab_first = child_a->tab_first;
        child_b->tab_last = child_a->tab_last;
        child_b->active_tab = child_a->active_tab;

        /* Place the moved tab in child_a (the new panel on the left/top) */
        child_a->tab_first = tab;
        child_a->tab_last = tab;
        child_a->active_tab = tab;

        child_a->pct_of_parent = 0.5f;
        child_b->pct_of_parent = 0.5f;

        return child_a;
    }

    /* PanelDockSide_After: tab goes to child_b (right/bottom) */
    Panel* child_b = anchor->child_b;

    child_b->tab_first = tab;
    child_b->tab_last = tab;
    child_b->active_tab = tab;

    return child_b;
}

void panel_tabs_cleanup(Panel* panel)
{
    TracyCZoneNC(ctx_tc, "TabsCleanup", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);
    /* Remove undeclared tabs */
    PanelTab** prev = &panel->tab_first;
    while (*prev)
    {
        PanelTab* tab = *prev;
        if (!tab->frame_declared)
        {
            /* Unlink */
            *prev = tab->next;
            if (panel->active_tab == tab)
                panel->active_tab = tab->next ? tab->next : panel->tab_first;
            free(tab);
        }
        else
        {
            /* Reset flag for next frame */
            tab->frame_declared = False;
            prev = &tab->next;
        }
    }
    panel_tab_list_refresh_last(panel);
    TracyCZoneEnd(ctx_tc);
}

static void panel_free_tabs(Panel* panel)
{
    PanelTab* tab = panel->tab_first;
    while (tab)
    {
        PanelTab* next = tab->next;
        free(tab);
        tab = next;
    }
    panel->tab_first = NULL;
    panel->tab_last = NULL;
    panel->active_tab = NULL;
}

//
// Panel lifecycle
//

Panel* panel_split(Panel* panel, const Axis2 axis, const b32 create_default_tab)
{
    if (!panel || panel->child_a)
        return NULL;

    Panel* child_a = panel_alloc();
    Panel* child_b = panel_alloc();
    if (!child_a || !child_b)
    {
        free(child_a);
        free(child_b);
        return NULL;
    }

    /* child_a inherits the parent's tabs */
    child_a->tab_first = panel->tab_first;
    child_a->tab_last = panel->tab_last;
    child_a->active_tab = panel->active_tab;
    panel->tab_first = NULL;
    panel->tab_last = NULL;
    panel->active_tab = NULL;

    /* child_b gets a default tab only when requested */
    if (create_default_tab)
    {
        u8 name_buf[PANEL_TAB_NAME_MAX];
        isize name_len;
        panel_tab_generate_default_name(child_b, name_buf, sizeof(name_buf), &name_len);
        panel_tab_declare(child_b, (String){ name_buf, name_len });
    }

    /* Set tree relationship fields without overwriting tab data */
    child_a->parent = panel;
    child_a->next = child_b;
    child_a->pct_of_parent = 1.0f;

    child_b->parent = panel;
    child_b->prev = child_a;
    child_b->pct_of_parent = 0.0f;

    panel->split_axis = axis;
    panel->child_a = child_a;
    panel->child_b = child_b;

    child_a->pct_of_parent = 0.5f;
    child_b->pct_of_parent = 0.5f;

    return panel;
}

Panel* panel_remove(Panel* panel)
{
    /* Only allow removal of leaf nodes. Non-leaf nodes should be freed recursively first. */
    if (!panel || !panel->parent || panel->child_a)
        Assert(0);

    Panel* parent = panel->parent;
    Panel* grandparent = parent->parent;

    /* Strict binary tree: after removing one child, the other child is necessarily the only one. */
    Panel* survivor = (parent->child_a == panel) ? parent->child_b : parent->child_a;

    /* Promote survivor to the parent's level, inheriting parent's position and percentage in the grandparent. */
    /* NOTE: survivor->split_axis retains its own value, not inherited from parent. */
    survivor->parent = grandparent;
    survivor->pct_of_parent = parent->pct_of_parent;

    survivor->prev = parent->prev;
    survivor->next = parent->next;

    if (grandparent)
    {
        /* Replace parent with survivor in the grandparent's child linked list. */
        if (parent->prev)
            parent->prev->next = survivor;
        else
            grandparent->child_a = survivor;
        if (parent->next)
            parent->next->prev = survivor;
        else
            grandparent->child_b = survivor;
    }

    panel_free_tabs(panel);
    free(panel);
    free(parent);

    return survivor;
}

static void panel_free_node(Panel* panel)
{
    for (Panel* child = panel->child_a; child;)
    {
        Panel* next = child->next;
        panel_free_node(child);
        child = next;
    }
    panel_free_tabs(panel);
    free(panel);
}

void panel_free_tree(Panel* root)
{
    Assert(root);
    panel_free_node(root);
}

Panel* panel_process_pending_removes(Panel* root)
{
    TracyCZoneNC(ctx_ppr, "PendingRemoves", TracyColor_Panel, TRACY_SUBSYSTEMS & TracySys_Panel);
    Stack(u32, PANEL_STACK_CAPACITY) pending_ids;
    pending_ids.depth = 0;

    /* Collect IDs of leaf panels marked for removal */
    for (Panel* p = root; p; p = panel_iter_next(p))
    {
        if (p->child_a)
            continue;
        if (p->pending_remove)
        {
            Assert(pending_ids.depth < PANEL_STACK_CAPACITY);
            pending_ids.items[pending_ids.depth++] = p->id;
        }
    }

    /* Process removals by ID (stable across tree mutations) */
    for (isize i = 0; i < pending_ids.depth; i++)
    {
        Panel* target = panel_find_by_id(root, pending_ids.items[i]);
        if (!target || !target->parent)
            continue;
        Panel* old_parent = target->parent;
        Panel* survivor = panel_remove(target);
        if (old_parent == root)
            root = survivor;
    }

    TracyCZoneEnd(ctx_ppr);
    return root;
}

//
// Panel navigation
//

Panel* panel_find_first_leaf(Panel* root)
{
    if (!root)
        return NULL;
    for (Panel* p = root; p; p = panel_iter_next(p))
        if (!p->child_a)
            return p;
    return NULL;
}

Panel* panel_find_next_leaf(Panel* root, Panel* current)
{
    if (!root || !current)
        return NULL;
    Panel* p = current;
    for (;;)
    {
        p = panel_iter_next(p);
        if (!p)
            return panel_find_first_leaf(root);
        if (!p->child_a)
            return p;
    }
}

Panel* panel_find_prev_leaf(Panel* root, Panel* current)
{
    if (!root || !current)
        return NULL;

    /* Walk full depth-first to find the leaf preceding 'current' */
    Panel* prev = NULL;
    for (Panel* p = root; p; p = panel_iter_next(p))
    {
        if (p == current)
            break;
        if (!p->child_a)
            prev = p;
    }

    if (prev)
        return prev;

    /* Wrap to last leaf */
    Panel* last = NULL;
    for (Panel* p = root; p; p = panel_iter_next(p))
        if (!p->child_a)
            last = p;
    return last;
}

Panel* panel_find_spatial(Panel* root, Panel* current, Rect root_rect, PanelSpatial direction)
{
    if (!root || !current)
        return NULL;

    Rect cur = panel_calc_rect(current, root_rect);

    Panel* best = NULL;
    f32 best_primary = 1e12f;
    f32 best_overlap = -1.f;

    for (Panel* p = root; p; p = panel_iter_next(p))
    {
        if (p == current || p->child_a)
            continue;

        Rect r = panel_calc_rect(p, root_rect);
        f32 primary;
        f32 overlap;

        switch (direction)
        {
            case PanelSpatial_Left:
            {
                if (r.xmax > cur.xmin)
                    continue;
                f32 ol_top = cur.ymin > r.ymin ? cur.ymin : r.ymin;
                f32 ol_bot = cur.ymax < r.ymax ? cur.ymax : r.ymax;
                overlap = ol_bot - ol_top;
                if (overlap <= 0.f)
                    continue;
                primary = cur.xmin - r.xmax;
            }
            break;

            case PanelSpatial_Right:
            {
                if (r.xmin < cur.xmax)
                    continue;
                f32 ol_top = cur.ymin > r.ymin ? cur.ymin : r.ymin;
                f32 ol_bot = cur.ymax < r.ymax ? cur.ymax : r.ymax;
                overlap = ol_bot - ol_top;
                if (overlap <= 0.f)
                    continue;
                primary = r.xmin - cur.xmax;
            }
            break;

            case PanelSpatial_Up:
            {
                if (r.ymax > cur.ymin)
                    continue;
                f32 ol_left = cur.xmin > r.xmin ? cur.xmin : r.xmin;
                f32 ol_right = cur.xmax < r.xmax ? cur.xmax : r.xmax;
                overlap = ol_right - ol_left;
                if (overlap <= 0.f)
                    continue;
                primary = cur.ymin - r.ymax;
            }
            break;

            case PanelSpatial_Down:
            {
                if (r.ymin < cur.ymax)
                    continue;
                f32 ol_left = cur.xmin > r.xmin ? cur.xmin : r.xmin;
                f32 ol_right = cur.xmax < r.xmax ? cur.xmax : r.xmax;
                overlap = ol_right - ol_left;
                if (overlap <= 0.f)
                    continue;
                primary = r.ymin - cur.ymax;
            }
            break;
        }

        if (primary < best_primary || (primary == best_primary && overlap > best_overlap))
        {
            best = p;
            best_primary = primary;
            best_overlap = overlap;
        }
    }

    return best;
}

//
// Pixel-level panel resize
//

//
// Top-down layout pass: converts pct_of_parent (source of truth) into
// temporary absolute-pixel computed_rect for every node in the tree.
// Called before any pixel-level resize operation.
//
void panel_compute_rects(Panel* root, Rect root_rect)
{
    root->computed_rect = root_rect;

    if (!root->child_a)
        return;

    f32 total_dim =
        (root->split_axis == Axis2_X) ? (root_rect.xmax - root_rect.xmin) : (root_rect.ymax - root_rect.ymin);

    f32 child_a_dim = root->child_a->pct_of_parent * total_dim;

    Rect a_rect = root_rect;
    Rect b_rect = root_rect;

    if (root->split_axis == Axis2_X)
    {
        a_rect.xmax = a_rect.xmin + child_a_dim;
        b_rect.xmin = a_rect.xmax;
    }
    else
    {
        a_rect.ymax = a_rect.ymin + child_a_dim;
        b_rect.ymin = a_rect.ymax;
    }

    panel_compute_rects(root->child_a, a_rect);
    panel_compute_rects(root->child_b, b_rect);
}

//
// Bottom-up (post-order) pass: after leaf pixel rects have been mutated
// by a resize, rebuilds pct_of_parent for every internal node from its
// children's current computed_rect dimensions.
//
// This guarantees the percentage tree stays a faithful source of truth
// for future layout passes — unaffected panels keep their exact pixel
// sizes because pct * new_parent_dim = old_child_dim.
//
static void panel_sync_percentages_from_rects(Panel* node)
{
    if (!node->child_a)
        return;

    panel_sync_percentages_from_rects(node->child_a);
    panel_sync_percentages_from_rects(node->child_b);

    Rect* a = &node->child_a->computed_rect;
    Rect* b = &node->child_b->computed_rect;

    if (node->split_axis == Axis2_X)
    {
        f32 a_w = a->xmax - a->xmin;
        f32 b_w = b->xmax - b->xmin;
        f32 total = a_w + b_w;
        if (total > 0.f)
        {
            node->child_a->pct_of_parent = a_w / total;
            node->child_b->pct_of_parent = b_w / total;
        }

        node->computed_rect.xmin = a->xmin;
        node->computed_rect.xmax = b->xmax;
        node->computed_rect.ymin = a->ymin;
        node->computed_rect.ymax = a->ymax;
    }
    else
    {
        f32 a_h = a->ymax - a->ymin;
        f32 b_h = b->ymax - b->ymin;
        f32 total = a_h + b_h;
        if (total > 0.f)
        {
            node->child_a->pct_of_parent = a_h / total;
            node->child_b->pct_of_parent = b_h / total;
        }

        node->computed_rect.xmin = a->xmin;
        node->computed_rect.xmax = a->xmax;
        node->computed_rect.ymin = a->ymin;
        node->computed_rect.ymax = b->ymax;
    }
}

typedef struct
{
    Panel* leaf;
    Rect new_rect;
} AffectedEntry;

//
// Recursively walks a subtree and collects every leaf whose pixel edge
// aligns with current_line (within 0.1f epsilon).  Matching edges snap
// to target_line directly — no branching on direction.
//
// Scoped to the qualifying ancestor's subtree so panels in unrelated
// branches that happen to share the same pixel coordinate are never
// falsely affected.
//
static void collect_affected_in_subtree(Panel* node, f32 current_line, f32 target_line, b32 want_x,
                                        AffectedEntry affected[], i32* count)
{
    if (!node->child_a)
    {
        Rect* r = &node->computed_rect;
        b32 touched = False;
        Rect new_r = *r;

        if (want_x)
        {
            if (fabsf(r->xmax - current_line) < 0.1f)
            {
                touched = True;
                new_r.xmax = target_line;
            }
            if (fabsf(r->xmin - current_line) < 0.1f)
            {
                touched = True;
                new_r.xmin = target_line;
            }
        }
        else
        {
            if (fabsf(r->ymax - current_line) < 0.1f)
            {
                touched = True;
                new_r.ymax = target_line;
            }
            if (fabsf(r->ymin - current_line) < 0.1f)
            {
                touched = True;
                new_r.ymin = target_line;
            }
        }

        if (touched)
        {
            Assert(*count < AFFECTED_MAX);
            affected[*count].leaf = node;
            affected[*count].new_rect = new_r;
            (*count)++;
        }
        return;
    }

    collect_affected_in_subtree(node->child_a, current_line, target_line, want_x, affected, count);
    collect_affected_in_subtree(node->child_b, current_line, target_line, want_x, affected, count);
}

// Pipeline (transactional — aborts entirely if any leaf drops below
// MIN_PANEL_PIXELS):
//
//   1. panel_compute_rects         → absolute pixel layout
//   2. Two-pass ancestor search    → topology-based boundary selection
//      · Pass 1: right/bottom border  (focused leaf in child_a subtree)
//      · Pass 2: left/top border      (fallback — panel is at screen edge)
//   3. delta = ±pixel_step          → direction = boundary movement vector
//   4. collect_affected_in_subtree  → only leaves touching this boundary
//   5. MIN_PANEL_PIXELS validation  → commit or abort
//   6. panel_sync_percentages       → back-propagate pixel sizes into pct_of_parent
//
// Total window dimensions never change — the boundary moves between two
// adjacent subtrees: one side gains exactly what the other loses.
//
void panel_resize_pixel(Panel* root, Rect root_rect, Panel* focused_leaf, PanelSpatial direction, f32 pixel_step)
{
    if (!root || !focused_leaf || pixel_step <= 0.f)
        return;

    /* Phase 1: transient pixel layout */
    panel_compute_rects(root, root_rect);

    b32 want_x = (direction == PanelSpatial_Left || direction == PanelSpatial_Right);

    /* Phase 2: topology-based ancestor selection */
    Panel* ancestor = NULL;

    /* Pass 1 — right/bottom border (focused leaf is inside child_a) */
    for (Panel* node = focused_leaf; node && node->parent; node = node->parent)
    {
        if (want_x != (node->parent->split_axis == Axis2_X))
            continue;
        if (node == node->parent->child_a)
        {
            ancestor = node->parent;
            break;
        }
    }

    /* Pass 2 — left/top border (fallback: no right/bottom border exists) */
    if (!ancestor)
    {
        for (Panel* node = focused_leaf; node && node->parent; node = node->parent)
        {
            if (want_x != (node->parent->split_axis == Axis2_X))
                continue;
            if (node == node->parent->child_b)
            {
                ancestor = node->parent;
                break;
            }
        }
    }

    if (!ancestor)
        return;

    /* Phase 3: compute boundary movement */
    f32 current_line = want_x ? ancestor->child_a->computed_rect.xmax : ancestor->child_a->computed_rect.ymax;

    f32 delta = (direction == PanelSpatial_Right || direction == PanelSpatial_Down) ? pixel_step : -pixel_step;
    f32 target_line = current_line + delta;

    /* Phase 4: collect leaves touching the boundary, scoped to ancestor */
    AffectedEntry affected[AFFECTED_MAX];
    i32 affected_count = 0;

    collect_affected_in_subtree(ancestor, current_line, target_line, want_x, affected, &affected_count);

    if (affected_count == 0)
        return;

    /* Phase 5: validate minimum pixel size */
    for (i32 i = 0; i < affected_count; i++)
    {
        f32 new_w = affected[i].new_rect.xmax - affected[i].new_rect.xmin;
        f32 new_h = affected[i].new_rect.ymax - affected[i].new_rect.ymin;
        if (new_w < MIN_PANEL_PIXELS || new_h < MIN_PANEL_PIXELS)
            return;
    }

    /* Phase 6: commit rect mutations */
    for (i32 i = 0; i < affected_count; i++)
        affected[i].leaf->computed_rect = affected[i].new_rect;

    /* Phase 7: back-propagate pixel sizes into pct_of_parent */
    panel_sync_percentages_from_rects(root);
}
