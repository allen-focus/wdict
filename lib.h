#pragma once

#include <stdbool.h>
#include <stdint.h>

///

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

///

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
 * @param b Alignment boundary (must be power of two)
 * @return Smallest multiple of b that is >= x
 */
#define AlignUpPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))

// clang-format off
#define Queue(type, size) struct { type items[size]; isize count; }
#define Stack(type, size) struct { type items[size]; isize depth; }
#define Slice(type)       struct { type* data; isize len; isize capacity; }
// clang-format on

///

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
    u8 border_color[4];
    f32 corner_radius, border_thickness;
    b32 enable_shadow;
} RectStyle;
