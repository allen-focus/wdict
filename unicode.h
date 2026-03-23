#pragma once

#include "lib.h"

byte* utf8_decode(byte* str, u32* codepoint);
isize utf8_encode(byte* str, u32 codepoint);
