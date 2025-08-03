#pragma once

#include <stdint.h>
#include <stdbool.h>

#define Assert(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            __debugbreak();                                                                                            \
    } while (0)

typedef struct
{
    float x, y;
} Pos;

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
