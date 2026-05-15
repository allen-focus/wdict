#include "panel.h"
#include "utils.h"

#include <stdlib.h>

#define PANEL_ANIM_DURATION 0.1f

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

    /* child_a was the original content — start at 100%, child_b starts invisible at 0% */
    *child_a = (Panel){ .parent = panel, .next = child_b, .pct_of_parent = 1.0f };
    *child_b = (Panel){ .parent = panel, .prev = child_a, .pct_of_parent = 0.0f };
    panel->split_axis = axis;
    panel->child_a = child_a;
    panel->child_b = child_b;

    /* Animate child_b growing from 0% to 50%; child_a shrinks via sibling delta */
    child_b->anim_state = PANEL_ANIM_OPENING;
    child_b->anim_to_pct = 0.5f;

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

Panel* panel_update_animations(Panel* root, f64 now)
{
    Stack(Panel*, 16) pending_remove;
    pending_remove.depth = 0;

    for (Panel* p = root; p; p = panel_iter_next(p))
    {
        if (p->anim_state == PANEL_ANIM_NONE)
            continue;
        if (p->child_a)
            continue;

        Panel* parent = p->parent;
        if (!parent)
            continue;

        /* First animation frame: snapshot initial state */
        if (p->anim_started_at == 0.0)
        {
            p->anim_from_pct = p->pct_of_parent;
            p->anim_started_at = now;
        }

        /* Compute progress, clamped to [0, 1] */
        f64 elapsed = now - p->anim_started_at;
        f32 t = (f32)(elapsed / (f64)PANEL_ANIM_DURATION);
        if (t > 1.0f)
            t = 1.0f;

        /* Ease-out quadratic: decelerate toward target */
        f32 e = 1.0f - (1.0f - t) * (1.0f - t);

        /* Lerp toward anim_to_pct and push the delta to the sibling */
        f32 old_pct = p->pct_of_parent;
        p->pct_of_parent = p->anim_from_pct + (p->anim_to_pct - p->anim_from_pct) * e;

        Panel* sibling = (parent->child_a == p) ? parent->child_b : parent->child_a;
        sibling->pct_of_parent += (old_pct - p->pct_of_parent);

        if (t >= 1.0f)
        {
            if (p->anim_state == PANEL_ANIM_CLOSING)
                pending_remove.items[pending_remove.depth++] = p;
            p->anim_state = PANEL_ANIM_NONE;
            p->anim_started_at = 0.0;
        }
    }

    /* Remove panels that completed their closing animation */
    for (isize i = 0; i < pending_remove.depth; i++)
    {
        Panel* target = pending_remove.items[i];
        Panel* old_parent = target->parent;
        Panel* survivor = panel_remove(target);
        if (old_parent == root)
            root = survivor;
    }

    return root;
}
