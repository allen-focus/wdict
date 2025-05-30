#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lib.h"
#include <stdint.h>

///

void swapchain_resize(const uint16_t client_width, const uint16_t client_height);

void renderer_create(const HWND window);
void renderer_flush_and_present(const uint16_t client_width, const uint16_t client_height);
void renderer_destroy();
void renderer_rect_push(const Rect destination, const Color color);
