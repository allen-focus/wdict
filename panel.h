#pragma once

#include "utils.h"

typedef enum
{
    Axis2_X,
    Axis2_Y,
} Axis2;

typedef enum
{
    PANEL_ANIM_NONE = 0,
    PANEL_ANIM_CLOSING,
    PANEL_ANIM_OPENING,
} PanelAnimState;

#define PANEL_TAB_NAME_MAX 64

typedef struct PanelTab PanelTab;
struct PanelTab
{
    u8 name[PANEL_TAB_NAME_MAX];
    isize name_len;
    PanelTab* next;
    b32 frame_declared;
};

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

    PanelAnimState anim_state;
    f32 anim_from_pct;
    f32 anim_to_pct;
    f64 anim_started_at;

    PanelTab* tab_first;
    PanelTab* tab_last;
    PanelTab* active_tab;
};

Panel* panel_iter_next(const Panel* panel);
Rect panel_calc_rect(const Panel* panel, const Rect root_rect);
Rect panel_calc_rect_from_parent(const Panel* child, const Rect parent_rect);
Panel* panel_split(Panel* panel, const Axis2 axis);
Panel* panel_remove(Panel* panel);
void panel_free_tree(Panel* root);

PanelTab* panel_tab_declare(Panel* panel, const String name);
PanelTab* panel_tab_get_active(Panel* panel);
void panel_tab_activate(Panel* panel, PanelTab* tab);
void panel_tab_close(Panel* panel, PanelTab* tab);
void panel_tab_move(Panel* panel, PanelTab* tab, i32 delta);
void panel_tab_move_to_panel(Panel* from, PanelTab* tab, Panel* to);
Panel* panel_tab_to_new_panel(Panel* from, PanelTab* tab, Panel* anchor, Axis2 axis);
void panel_tabs_cleanup(Panel* panel);
void panel_tab_generate_default_name(const Panel* panel, u8* buf, isize buf_size, isize* out_len);
Panel* panel_update_animations(Panel* root, f64 now);
