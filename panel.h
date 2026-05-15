#pragma once

#include "utils.h"

typedef enum
{
    Axis2_X,
    Axis2_Y,
} Axis2;

typedef struct Panel Panel;
struct Panel
{
    Panel* child_a;
    Panel* child_b;
    Panel* next;
    Panel* prev;
    Panel* parent;
    f32 pct_of_parent;
    Axis2 split_axis;
    f32 drag_saved_pct;
    f32 drag_saved_partner_pct;
};

Panel* panel_iter_next(const Panel* panel);
Rect panel_calc_rect(const Panel* panel, const Rect root_rect);
Rect panel_calc_rect_from_parent(const Panel* child, const Rect parent_rect);
Panel* panel_split(Panel* panel, const Axis2 axis);
Panel* panel_remove(Panel* panel);
void panel_free_tree(Panel* root);
