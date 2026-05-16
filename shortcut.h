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
    String cmd; // full command text, e.g. "window.create w=600 h=600"
} ShortcutBinding;

typedef struct
{
    Arena* arena;
    ShortcutBinding* bindings;
    isize count;
    isize capacity;
} ShortcutRegistry;

void shortcut_registry_init(ShortcutRegistry* reg, Arena* arena, isize capacity);

/* Bind a shortcut to a full command text string.  The string is cloned into the
   registry's arena.  Replaces any existing binding for the same shortcut. */
b32 shortcut_bind(ShortcutRegistry* reg, Shortcut sc, String cmd);

void shortcut_unbind(ShortcutRegistry* reg, Shortcut sc);

/* Unbind all shortcuts whose command text starts with `cmd_id`. */
void shortcut_unbind_all_for_cmd(ShortcutRegistry* reg, String cmd_id);

/* Returns the full command text bound to `sc`, or {0} if unbound. */
String shortcut_lookup(const ShortcutRegistry* reg, Shortcut sc);

/* List all Shortcut values bound to commands whose text starts with `cmd_id`.
   Allocates into `scratch`. */
ShortcutSlice shortcut_list_for_cmd(const ShortcutRegistry* reg, String cmd_id, Arena* scratch);

b32 shortcut_detect_conflicts(const ShortcutRegistry* reg);
