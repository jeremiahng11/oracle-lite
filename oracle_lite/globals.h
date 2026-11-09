#pragma once
#include <vector>
#include "types.h"
#include "src/power/board_power_bsp.h"
#include "src/display/epaper_driver_bsp.h"

extern std::vector<NoteEntry> noteIndex;

extern AppState  state;
extern int       lastRecNum;

extern uint32_t  lastActivityMs;

extern bool      transferServerActive;
extern String    transferUrl;

extern bool      timeReady;

extern bool      audioPlaying;
extern bool      stopPlayback;

extern board_power_bsp_t      board;
extern epaper_driver_display* display;
