#include "pch.h"  // IWYU pragma: keep
#include "lib.h"

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. No validation of continuation‑byte format (the prefix should be 10xxx).
//   3. Does not support multi‑codepoint sequences.
byte* utf8_decode(byte* str, u32* codepoint)
{
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
    *codepoint = (str[0] & mask[length]) << 18;
    switch (length) {
        case 4: *codepoint |= str[3] & 0x3F;
        case 3: *codepoint |= (str[2] & 0x3F) << 6;
        case 2: *codepoint |= (str[1] & 0x3F) << 12;
        default: *codepoint >>= shift[length];
    }
    return str + length;
}

// NOTE:
//   1. No check for the string buffer’s capacity.
//   2. Does not support multi‑codepoint sequences.
isize utf8_encode(byte* str, u32 codepoint)
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
