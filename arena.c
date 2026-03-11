#include "pch.h" // IWYU pragma: keep
#include "arena.h"
#include "lib.h"

#include <math.h>
#include <stdlib.h>
#include <windows.h>

Arena arena_new(isize size)
{
    Arena arena = { 0 };
    arena.base = VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
    arena.pos = 0;
    arena.reserve_end = size;
    arena.commit_block_size = MEM_COMMIT_BLOCK_SIZE;
    arena.commit_end = 0;
    return arena;
}

void arena_release(Arena* arena)
{
    if (arena->base)
        VirtualFree(arena->base, 0, MEM_RELEASE);
    memset(arena, 0, sizeof(*arena));
}

void* arena_push(Arena* arena, isize size, isize align, isize count)
{
    void* p = arena->base + arena->pos;
    isize padding = (0 - (uintptr_t)p) & (align - 1);
    void* p_aligned = arena->base + arena->pos + padding;

    isize available = arena->reserve_end - (arena->pos + padding);
    if (available < 0 || count > (available / size))
        Assert(0);

    arena->pos += padding + (size * count);
    if (arena->pos > arena->commit_end)
    {
        isize pos_aligned = AlignUpPow2(arena->pos, arena->commit_block_size);
        isize pos_next_commit = min(pos_aligned, arena->reserve_end);
        isize commit_size = pos_next_commit - arena->commit_end;
        VirtualAlloc(arena->base + arena->commit_end, commit_size, MEM_COMMIT, PAGE_READWRITE);
        arena->commit_end = pos_next_commit;
    }

    return memset(p_aligned, 0, count * size);
}

void arena_pop_to(Arena* arena, isize pos)
{
    Assert(pos >= 0);

    isize new_commit_end = AlignUpPow2(pos, arena->commit_block_size); 
    if (new_commit_end < arena->commit_end)
    {
        isize decommit_size = arena->commit_end - new_commit_end;
        VirtualFree(arena->base + new_commit_end, decommit_size, MEM_DECOMMIT);
        arena->commit_end = new_commit_end;
    }

    arena->pos = pos;
}

void arena_pop(Arena* arena, isize size, isize count)
{
    isize need_pop = size * count;
    if (need_pop > arena->pos)
        Assert(0);
    arena_pop_to(arena, arena->pos - need_pop);
}
