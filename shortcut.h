#pragma once

#include "utils.h"

typedef u8 Modifiers;
#define SHORTCUT_MOD_NONE  0
#define SHORTCUT_MOD_CTRL  (1 << 0)
#define SHORTCUT_MOD_ALT   (1 << 1)
#define SHORTCUT_MOD_SHIFT (1 << 2)
#define SHORTCUT_MOD_WIN   (1 << 3)

typedef struct
{
    Modifiers mods;
    u32 vk; // VK_* virtual key (Win32 convention)
} Shortcut;

typedef struct
{
    Shortcut* data;
    isize len;
    isize capacity;
} ShortcutSlice;

typedef struct
{
    Shortcut shortcut;
    String cmd_id;
} ShortcutBinding;

typedef struct
{
    Arena* arena;
    ShortcutBinding* bindings;
    isize count;
    isize capacity;
} ShortcutRegistry;

void shortcut_registry_init(ShortcutRegistry* reg, Arena* arena, isize capacity);
b32 shortcut_bind(ShortcutRegistry* reg, Shortcut sc, String cmd_id);
void shortcut_unbind(ShortcutRegistry* reg, Shortcut sc);
void shortcut_unbind_all_for_cmd(ShortcutRegistry* reg, String cmd_id);
String shortcut_lookup(const ShortcutRegistry* reg, Shortcut sc);
ShortcutSlice shortcut_list_for_cmd(const ShortcutRegistry* reg, String cmd_id, Arena* scratch);
b32 shortcut_detect_conflicts(const ShortcutRegistry* reg);
