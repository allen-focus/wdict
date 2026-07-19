#include "shortcut.h"

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

String shortcut_lookup(const ShortcutRegistry* reg, Shortcut sc)
{
    for (isize i = 0; i < reg->count; i++)
        if (reg->bindings[i].shortcut.mods == sc.mods && reg->bindings[i].shortcut.vk == sc.vk)
            return reg->bindings[i].cmd;
    return (String){ 0 };
}
