#pragma once

#include "lib.h"

#if !defined(MEM_COMMIT_BLOCK_SIZE)
    #define MEM_COMMIT_BLOCK_SIZE MB(8) // NOTE: Must be a power of two
#endif

typedef struct {
    byte* base;
    isize pos;
    isize reserve_end;
    isize commit_block_size;
    isize commit_end;
} Arena;

Arena arena_new(isize capacity);
void arena_release(Arena* arena);
void* arena_push(Arena* arena, isize size, isize align, isize count);
void arena_pop_to(Arena* arena, isize pos);
void arena_pop(Arena* arena, isize size, isize count);
