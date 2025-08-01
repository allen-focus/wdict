#pragma once

#include <stdint.h>

#define Assert(cond)        \
    do                      \
    {                       \
        if (!(cond))        \
            __debugbreak(); \
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
