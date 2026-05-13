#pragma once

#include <stdint.h>

///

// clang-format off
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;

typedef float     f32;
typedef double    f64;

typedef unsigned char byte;
typedef ptrdiff_t     isize;
typedef size_t        usize;
typedef int32_t       b32; // bool
// clang-format off

///

#define True 1
#define False 0

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)
#define GB(x) ((x) << 30)
#define TB(x) ((u64)(x) << 40llu)

#define Assert(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            __debugbreak();                                                                                            \
    } while (0)

/**
 * Aligns value x up to the next multiple of b (which must be a power of two)
 *
 * How it works:
 * - For a power of two b, its binary form is 1000... (1 followed by zeros)
 * - (b - 1) creates a low-bit mask (e.g., b=8 => b-1=0b0111)
 * - ~(b - 1) creates a high-bit mask that clears lower bits
 * - Adding (b - 1) before masking rounds up instead of down
 *
 * Example with x=13, b=8:
 *   x + b - 1 = 20 (0b10100)
 *   ~(b - 1)  = ~7 = 0b...11111000
 *   20 & (~7) = 16 (0b10000)
 *
 * @param x Value to align (should be unsigned integer)
 * @param b Alignment boundary (must be a power of two)
 * @return Smallest multiple of b that is >= x
 */
#define AlignUpPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))

#define clamp(x, a, b) min(max(a, x), b)
#define countof(a) (sizeof(a) / sizeof(*(a)))

// clang-format off
#define Queue(type, size) struct { type items[size]; isize count; }
#define Stack(type, size) struct { type items[size]; isize depth; }
#define Slice(type)       struct { type* data; isize len; isize capacity; }
// clang-format on

///

typedef struct
{
    byte* base;
    isize pos;
    isize size;
} BufferCursor;

typedef struct
{
    f32 x, y;
} Vec2F32;

typedef struct
{
    f32 x, y;
} Position;

typedef struct
{
    f32 xmin, ymin, xmax, ymax;
} Rect;

typedef struct
{
    u8 r, g, b, a;
} Color;

typedef struct
{
    Color border_color;
    f32 border_thickness;
    Color shadow_color;
    Position shadow_offset;
    f32 shadow_sigma;
    f32 corner_radius;
    Color corner_colors[4];
    f32 shear;
} RectStyle;

//
// hash
//

u32 fnv1a_hash(const void* data, isize size);

//
// unicode
//

typedef struct
{
    u32 codepoint;
    const byte* next_p;
} UnicodeDecode;

b32 is_high_surrogate(const u16 c);
b32 is_low_surrogate(const u16 c);

UnicodeDecode utf8_decode(const byte* str);
isize utf8_encode(byte* str, const u32 codepoint);

UnicodeDecode utf16_decode(const u16* str);
isize utf16_encode(u16* str, const u32 codepoint);

//
// arena
//

#if !defined(MEM_COMMIT_BLOCK_SIZE)
#    define MEM_COMMIT_BLOCK_SIZE MB(8) // NOTE: must be a power of two
#endif

typedef struct
{
    byte* base;
    isize pos;
    isize reserve_end;
    isize commit_block_size;
    isize commit_end;
} Arena;

Arena arena_new(isize capacity);
void arena_release(Arena* arena);
void* arena_push(Arena* arena, const isize size, const isize align, const isize count);
void arena_pop_to(Arena* arena, const isize pos);
void arena_pop(Arena* arena, const isize size, const isize count);

//
// slice
//

#define slice_push(arena, s)                                                                                           \
    ((s)->len >= (s)->capacity ? slice_grow(arena, s, sizeof(*(s)->data)),                                             \
     (s)->data + (s)->len++    : (s)->data + (s)->len++)

void slice_grow(Arena* arena, void* slice, const isize size);

//
// string
//

#define str(s) (String){ (u8*)s, sizeof(s) - 1 }

typedef struct
{
    u8* data;
    isize len;
} String;

String str_clone(Arena* arena, String s);
String str_slice(String s, const isize start, const isize end);
b32 str_compare(const String a, const String b);
String str_concat(Arena* arena, const String a, const String b);
