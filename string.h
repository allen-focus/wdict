#pragma once

#include "lib.h"
#include "arena.h"

#define str(s) (String){ (u8*)s, sizeof(s) - 1 }

typedef struct
{
    u8 *data;
    isize len;
} String;

String str_clone(Arena* arena, String s);
String str_slice(String s, isize start, isize end);
