#include "panel.h"
#include "utils.h"

#include <stdlib.h>

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

Panel* panel_split(Panel* panel, const Axis2 axis)
{
    if (!panel || panel->child_a)
        return NULL;

    Panel* child_a = (Panel*)calloc(1, sizeof(Panel));
    Panel* child_b = (Panel*)calloc(1, sizeof(Panel));
    if (!child_a || !child_b)
    {
        free(child_a);
        free(child_b);
        return NULL;
    }

    *child_a = (Panel){ .parent = panel, .next = child_b, .pct_of_parent = 0.5f };
    *child_b = (Panel){ .parent = panel, .prev = child_a, .pct_of_parent = 0.5f };
    panel->split_axis = axis;
    panel->child_a = child_a;
    panel->child_b = child_b;

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
    free(panel);
}

void panel_free_tree(Panel* root)
{
    Assert(root);
    panel_free_node(root);
}
