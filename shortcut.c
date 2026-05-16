#include "shortcut.h"

void shortcut_registry_init(ShortcutRegistry* reg, Arena* arena, isize capacity)
{
    reg->arena = arena;
    reg->bindings = (ShortcutBinding*)arena_push(arena, sizeof(ShortcutBinding), _Alignof(ShortcutBinding), capacity);
    reg->count = 0;
    reg->capacity = capacity;
}

b32 shortcut_bind(ShortcutRegistry* reg, Shortcut sc, String cmd_id)
{
    Assert(reg->count < reg->capacity);

    /* Replace existing binding for the same shortcut */
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
        {
            reg->bindings[i].cmd_id = str_clone(reg->arena, cmd_id);
            return True;
        }

    reg->bindings[reg->count].shortcut = sc;
    reg->bindings[reg->count].cmd_id = str_clone(reg->arena, cmd_id);
    reg->count++;
    return True;
}

void shortcut_unbind(ShortcutRegistry* reg, Shortcut sc)
{
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
        {
            /* Swap-remove */
            reg->bindings[i] = reg->bindings[reg->count - 1];
            reg->count--;
            return;
        }
}

void shortcut_unbind_all_for_cmd(ShortcutRegistry* reg, String cmd_id)
{
    for (isize i = 0; i < reg->count;)
    {
        if (str_compare(reg->bindings[i].cmd_id, cmd_id))
        {
            reg->bindings[i] = reg->bindings[reg->count - 1];
            reg->count--;
        }
        else
            i++;
    }
}

String shortcut_lookup(const ShortcutRegistry* reg, Shortcut sc)
{
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
            return reg->bindings[i].cmd_id;
    return (String){ 0 };
}

ShortcutSlice shortcut_list_for_cmd(const ShortcutRegistry* reg, String cmd_id, Arena* scratch)
{
    ShortcutSlice result = { 0 };
    for (isize i = 0; i < reg->count; i++)
        if (str_compare(reg->bindings[i].cmd_id, cmd_id))
            *slice_push(scratch, &result) = reg->bindings[i].shortcut;
    return result;
}

// TODO: Currently `shortcut_bind` would replace existing binding without check whether conflicts.
b32 shortcut_detect_conflicts(const ShortcutRegistry* reg)
{
    for (isize i = 0; i < reg->count; i++)
        for (isize j = i + 1; j < reg->count; j++)
            if (reg->bindings[i].shortcut.mods == reg->bindings[j].shortcut.mods &&
                reg->bindings[i].shortcut.vk == reg->bindings[j].shortcut.vk)
                return True;
    return False;
}
