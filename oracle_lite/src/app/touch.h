#pragma once
#include <stdint.h>

// Touch version only. Detects the FT6336 touch chip; if present, a tap on
// the upper half of the screen acts as a top-button tap and a tap on the
// lower half as a bottom-button tap (double and triple taps too), so every
// screen works by touch exactly as by buttons. Holds stay on the buttons.
bool touchBegin();
bool touchPresent();
