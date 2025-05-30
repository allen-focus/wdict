#pragma once

#include <stdint.h>

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)

typedef struct { float x0, y0, x1, y1; } Rect;
typedef struct { uint8_t r, g, b, a; } Color;

