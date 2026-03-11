#include "pch.h" // IWYU pragma: keep
#include "arena.h"
#include "lib.h"

#include <string.h>

void slice_grow(Arena* arena, void* slice, isize size)
{
    struct {
        void* data;
        isize len;
        isize capacity;
    } replica;
    memcpy(&replica, slice, sizeof(replica));

    // Normally we would derive alignment from the element type, but `_Alignof`
    // only works on types, not on expressions. To simplify, we use a fixed alignment.
    isize align = 16;

    replica.capacity = replica.capacity ? replica.capacity : 1;
    void* data = arena_push(arena, size * 2, align, replica.capacity);
    replica.capacity *= 2;
    if (replica.len)
        memcpy(data, replica.data, size * replica.len);
    replica.data = data;

    memcpy(slice, &replica, sizeof(replica));
}
