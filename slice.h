#pragma once

#include "lib.h"
#include "arena.h"

#define slice_push(s, arena) \
    ((s)->len >= (s)->capacity \
        ? slice_grow(arena, s, sizeof(*(s)->data)), \
        (s)->data + (s)->len++ \
        : (s)->data + (s)->len++)

void slice_grow(Arena* arena, void* slice, isize size);
