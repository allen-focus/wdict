#include "pch.h"  // IWYU pragma: keep
#include "lib.h"
#include "string.h"

#include <stdlib.h>
#include <string.h>

String str_clone(String s)
{
    String s_clone = {
        .data = (u8*)malloc(s.len + 1),
        .len = s.len
    };
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
