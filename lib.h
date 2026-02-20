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

#define Assert(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            __debugbreak();                                                                                            \
    } while (0)

// clang-format off
#define Queue(type, size) struct { type items[size]; size_t count; }
#define Stack(type, size) struct { type items[size]; size_t depth; }
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

typedef struct
{
    f32 top, right, bottom, left;
} Padding;
