#pragma once

#include <stdbool.h>
#include <stdint.h>

///

#define Assert(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            __debugbreak();                                                                                            \
    } while (0)

#define Pool(type, size) struct { type items[size]; size_t count; }
#define Stack(type, size) struct { type items[size]; size_t depth; }

///

typedef struct
{
    float x, y;
} Position;

typedef struct
{
    float width, height;
} Size;

typedef struct
{
    float xmin, ymin, xmax, ymax;
} Rect;

typedef struct
{
    uint8_t r, g, b, a;
} Color;

typedef struct
{
    uint8_t border_color[4];
    float corner_radius, border_thickness;
    bool enable_shadow;
} RectStyle;

typedef struct
{
    float top, right, bottom, left;
} Padding;
