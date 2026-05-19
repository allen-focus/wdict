#pragma once

#include "cmd.h"
#include "utils.h"

typedef enum
{
    Axis2_X,
    Axis2_Y,
} Axis2;

typedef enum
{
    PanelDockSide_Before,
    PanelDockSide_After,
} PanelDockSide;

#define PANEL_TAB_NAME_MAX 64

typedef enum
{
    DRAG_TYPE_TAB = 0,
} DragType;

typedef struct
{
    u32 drag_type;
    u32 from_panel_id;
    u32 from_tab_id;
    u32 from_window_id;
    char title[44]; // UTF-8 tab name, null-terminated
} TabDragPayload;

typedef struct PanelTab PanelTab;
struct PanelTab
{
    u8 name[PANEL_TAB_NAME_MAX];
    isize name_len;
    PanelTab* next;
    u32 id;
    b32 frame_declared;
};

typedef struct Panel Panel;
struct Panel
{
    u32 id;

    Panel* child_a;
    Panel* child_b;
    Panel* next;
    Panel* prev;
    Panel* parent;
    f32 pct_of_parent;
    Axis2 split_axis;
    f32 drag_saved_pct;
    f32 drag_saved_partner_pct;

    b32 pending_remove;

    PanelTab* tab_first;
    PanelTab* tab_last;
    PanelTab* active_tab;
};

Panel* panel_iter_next(const Panel* panel);
Panel* panel_alloc(void);
Panel* panel_find_by_id(const Panel* root, u32 id);
Rect panel_calc_rect(const Panel* panel, const Rect root_rect);
Rect panel_calc_rect_from_parent(const Panel* child, const Rect parent_rect);
Panel* panel_split(Panel* panel, const Axis2 axis, b32 create_default_tab);
Panel* panel_remove(Panel* panel);
void panel_free_tree(Panel* root);

PanelTab* panel_find_tab_by_id(const Panel* root, u32 tab_id);
PanelTab* panel_tab_declare(Panel* panel, const String name);
PanelTab* panel_tab_get_active(Panel* panel);
void panel_tab_activate(Panel* panel, PanelTab* tab);
void panel_tab_close(Panel* panel, PanelTab* tab);
void panel_tab_move(Panel* panel, PanelTab* tab, i32 delta);
isize panel_tab_count(const Panel* panel);
void panel_tab_move_to_panel(Panel* from, PanelTab* tab, Panel* to, i32 to_idx);
Panel* panel_tab_to_new_panel(Panel* from, PanelTab* tab, Panel* anchor, Axis2 axis, PanelDockSide side);
void panel_tabs_cleanup(Panel* panel);
void panel_tab_generate_default_name(const Panel* panel, u8* buf, isize buf_size, isize* out_len);
Panel* panel_process_pending_removes(Panel* root);
