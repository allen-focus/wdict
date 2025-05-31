#pragma once

#include <stdint.h>

#define FONT_SIZE 64
#define TEXT_LINE_HEIGHT (FONT_SIZE * 1.35) // TODO: Don't hard-coded it

#define Assert(cond) do { if (!(cond)) __debugbreak(); } while (0)

typedef struct { float x, y; } Pos;
typedef struct { float x0, y0, x1, y1; } Rect;
typedef struct { uint8_t r, g, b, a; } Color;

