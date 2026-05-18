#include "panel.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_STACK_CAPACITY 16

static u32 s_panel_next_id = 1; /* 0 reserved for none */
static u32 s_tab_next_id = 1; /* 0 reserved for none */

Panel* panel_alloc(void)
{
    Panel* p = (Panel*)calloc(1, sizeof(Panel));
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
    if (!panel->parent)
        return root_rect;
    Rect parent_rect = panel_calc_rect(panel->parent, root_rect);
    return panel_calc_rect_from_parent(panel, parent_rect);
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
    if (!tab)
        return NULL;
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
    else
    {
        /* PanelDockSide_After: tab goes to child_b (right/bottom) */
        Panel* child_b = anchor->child_b;

        child_b->tab_first = tab;
        child_b->tab_last = tab;
        child_b->active_tab = tab;

        return child_b;
    }
}

void panel_tabs_cleanup(Panel* panel)
{
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

    return root;
}
