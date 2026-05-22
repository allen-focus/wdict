#include "utils.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

//
// hash
//

u32 fnv1a_hash(const void* data, isize size)
{
    u32 hash = 2166136261u;
    const u8* p = data;
    while (size--)
        hash = (hash ^ *p++) * 16777619u;
    return hash;
}

u64 fnv1a_64(const void* data, isize size)
{
    u64 h = 14695981039346656037ULL;
    const u8* p = (const u8*)data;
    while (size--)
        h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

u64 fnv1a_64_continue(u64 h, const void* data, isize size)
{
    const u8* p = (const u8*)data;
    while (size--)
        h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

//
// unicode
//

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. No validation of continuation‑byte format (the prefix should be 10xxx).
//   3. Does not support multi‑codepoint sequences.
UnicodeDecode utf8_decode(const byte* str)
{
    // clang-format off
    u8 length_table[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xxxx
        0, 0, 0, 0, 0, 0, 0, 0,                          // 10xxx
        2, 2, 2, 2,                                      // 110xx
        3, 3,                                            // 1110x
        4,                                               // 11110
        0,                                               // 11111
    };
    byte mask[] = { 0, 0x7F, 0x1F, 0x0F, 0x07 };
    u8 shift[] = { 0, 18, 12, 6, 0 };

    u8 length = length_table[str[0] >> 3];
    Assert(length > 0);
    u32 codepoint = (str[0] & mask[length]) << 18;
    switch (length) {
        case 4: codepoint |= str[3] & 0x3F;
        case 3: codepoint |= (str[2] & 0x3F) << 6;
        case 2: codepoint |= (str[1] & 0x3F) << 12;
        default: codepoint >>= shift[length];
    }
    return (UnicodeDecode){ codepoint, str + length };
    // clang-format on
}

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. Does not support multi‑codepoint sequences.
isize utf8_encode(byte* str, const u32 codepoint)
{
    isize length = 0;
    if (codepoint < (1 << 8))
    {
        str[0] = codepoint;
        length = 1;
    }
    else if (codepoint < (1 << 11))
    {
        str[0] = 0xC0 | (codepoint >> 6);
        str[1] = 0x80 | (codepoint & 0x3F);
        length = 2;
    }
    else if (codepoint < (1 << 16))
    {
        str[0] = 0xE0 | (codepoint >> 12);
        str[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        str[2] = 0x80 | (codepoint & 0x3F);
        length = 3;
    }
    else if (codepoint < (1 << 21))
    {
        str[0] = 0xF0 | (codepoint >> 18);
        str[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        str[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        str[3] = 0x80 | (codepoint & 0x3F);
        length = 4;
    }
    else
    {
        Assert(0);
    }
    return length;
}

b32 is_high_surrogate(const u16 c)
{
    return c >= 0xD800 && c <= 0xDBFF;
}

b32 is_low_surrogate(const u16 c)
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. Does not support multi‑codepoint sequences.
UnicodeDecode utf16_decode(const u16* str)
{
    Assert(!is_low_surrogate(str[0]));

    u32 codepoint = 0;
    const u16* next_p = str;
    if (is_high_surrogate(str[0]))
    {
        Assert(is_low_surrogate(str[1]));
        codepoint += 0x10000;
        codepoint += (str[0] - 0xD800) << 10;
        codepoint += str[1] - 0xDC00;
        next_p += 2;
    }
    else
    {
        codepoint = *str;
        next_p += 1;
    }
    return (UnicodeDecode){ codepoint, (const byte*)next_p };
}

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. Does not support multi‑codepoint sequences.
isize utf16_encode(u16* str, const u32 codepoint)
{
    isize length = 0;
    if (codepoint <= 0xFFFF)
    {
        Assert(!(codepoint >= 0xD800 && codepoint <= 0xDFFF));
        str[0] = (u16)codepoint;
        length = 1;
    }
    else if (codepoint <= 0x10FFFF)
    {
        u32 v = codepoint - 0x10000;
        str[0] = 0xD800 + (v >> 10);
        str[1] = 0xDC00 + (v & 0x3FF);
        length = 2;
    }
    else
        Assert(0); // Invalid unicode
    return length;
}

//
// arena
//

Arena arena_new(const isize size)
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

void* arena_push(Arena* arena, const isize size, const isize align, const isize count)
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

void arena_pop_to(Arena* arena, const isize pos)
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

void arena_pop(Arena* arena, const isize size, const isize count)
{
    isize need_pop = size * count;
    if (need_pop > arena->pos)
        Assert(0);
    arena_pop_to(arena, arena->pos - need_pop);
}

//
// slice
//

void slice_grow(Arena* arena, void* slice, const isize size)
{
    struct
    {
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

//
// string
//

String str_clone(Arena* arena, const String s)
{
    String s_clone = { .data = (u8*)arena_push(arena, sizeof(u8), _Alignof(u8), s.len), .len = s.len };
    Assert(s_clone.data);
    memcpy(s_clone.data, s.data, s.len);
    return s_clone;
}

String str_slice(String s, const isize start, const isize end)
{
    String slice = s;
    slice.data = s.data + start;
    slice.len = end - start;
    return slice;
}

b32 str_compare(const String a, const String b)
{
    if (a.len != b.len)
        return False;
    return memcmp(a.data, b.data, a.len) == 0;
}

String str_concat(Arena* arena, const String a, const String b)
{
    String c = { .len = a.len + b.len };
    c.data = arena_push(arena, sizeof(u8), _Alignof(u8), c.len);
    memcpy(c.data, a.data, a.len);
    memcpy(c.data + a.len, b.data, b.len);
    return c;
}

String str_fmt_impl(u8* buf, isize buf_size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    isize len = vsnprintf((char*)buf, buf_size, fmt, args);
    va_end(args);
    Assert(len >= 0 && len < buf_size); // Must be `<` there as `vsnprintf` auto insert '\0' at the end
    return (String){ buf, len };
}
