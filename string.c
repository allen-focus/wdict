#include "pch.h" // IWYU pragma: keep
#include "arena.h"
#include "lib.h"
#include "string.h"

#include <string.h>

String str_clone(Arena* arena, String s)
{
    String s_clone = {
        .data = (u8*)arena_push(arena, sizeof(u8), _Alignof(u8), s.len + 1),
        .len = s.len
    };
    Assert(s_clone.data);
    memcpy(s_clone.data, s.data, s.len + 1);
    return s_clone;
}

String str_slice(String s, isize start, isize end)
{
    String slice = s;
    slice.data = s.data + start;
    slice.len = end - start;
    return slice;
}
