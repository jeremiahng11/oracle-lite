#pragma once
#include <stdint.h>

// Touch version only (FT6336, detected at boot; nothing changes on the
// button-only board).
//
// Two modes, chosen by the screen being shown:
//  - Buttons: a tap above splitY acts as a top-button tap, below it as a
//    bottom-button tap (double/triple taps too). Used on Home, where the split
//    sits between the TASKS and EVENTS rows.
//  - Pages: lists and answers read touch events themselves: tap the right
//    side (or swipe left) = next, tap the left side (or swipe right) =
//    previous, swipe down = back to Home.
// Holds stay on the buttons; touch never wakes the device from sleep.
enum class TouchMode : uint8_t { Buttons, Pages };
enum class TouchEvent : uint8_t { None, Next, Previous, Home };

bool touchBegin();
bool touchPresent();
void touchSetMode(TouchMode mode, int splitY = -1);
TouchEvent touchTakeEvent();    // Pages mode; None when nothing is waiting
void touchDiscardEvents();
// Main thread: plays the short click for a touch registered since the last
// call (not while speech plays), so every tap is acknowledged at once even
// though the e-paper takes ~0.5 s to redraw.
void touchServiceClick(bool audioBusy);
