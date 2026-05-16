#include "shortcut.h"

#include <string.h>

/*
 * Extract the first token (up to first space or end) from text.
 * Returns True if it matches `ref`.
 */
static b32 first_token_matches(String text, String ref)
{
    isize end = 0;
    while (end < text.len && text.data[end] != ' ')
        end++;
    if (end != ref.len)
        return False;
    return memcmp(text.data, ref.data, (size_t)end) == 0;
}

void shortcut_registry_init(ShortcutRegistry* reg, Arena* arena, isize capacity)
{
    reg->arena = arena;
    reg->bindings =
        (ShortcutBinding*)arena_push(arena, sizeof(ShortcutBinding), _Alignof(ShortcutBinding), capacity);
    reg->count = 0;
    reg->capacity = capacity;
}

b32 shortcut_bind(ShortcutRegistry* reg, Shortcut sc, String cmd)
{
    Assert(reg->count < reg->capacity);

    /* Replace existing binding for the same shortcut */
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
        {
            reg->bindings[i].cmd = str_clone(reg->arena, cmd);
            return True;
        }

    reg->bindings[reg->count].shortcut = sc;
    reg->bindings[reg->count].cmd = str_clone(reg->arena, cmd);
    reg->count++;
    return True;
}

void shortcut_unbind(ShortcutRegistry* reg, Shortcut sc)
{
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
        {
            reg->bindings[i] = reg->bindings[reg->count - 1];
            reg->count--;
            return;
        }
}

void shortcut_unbind_all_for_cmd(ShortcutRegistry* reg, String cmd_id)
{
    for (isize i = 0; i < reg->count;)
    {
        if (first_token_matches(reg->bindings[i].cmd, cmd_id))
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
            return reg->bindings[i].cmd;
    return (String){ 0 };
}

ShortcutSlice shortcut_list_for_cmd(const ShortcutRegistry* reg, String cmd_id, Arena* scratch)
{
    ShortcutSlice result = { 0 };
    for (isize i = 0; i < reg->count; i++)
        if (first_token_matches(reg->bindings[i].cmd, cmd_id))
            *slice_push(scratch, &result) = reg->bindings[i].shortcut;
    return result;
}

b32 shortcut_detect_conflicts(const ShortcutRegistry* reg)
{
    for (isize i = 0; i < reg->count; i++)
        for (isize j = i + 1; j < reg->count; j++)
            if (reg->bindings[i].shortcut.mods == reg->bindings[j].shortcut.mods &&
                reg->bindings[i].shortcut.vk == reg->bindings[j].shortcut.vk)
                return True;
    return False;
}
