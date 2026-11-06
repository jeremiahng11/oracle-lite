#pragma once
#include <SD_MMC.h>

// Opening a missing file for reading makes the ESP32 file layer log
// "[E] ... does not exist, no permits for creation". Many files here are
// optional or not created yet (tasks.state, a note's brain.json before the AI
// has organised it), so check first: exists() is silent.
inline File sdOpenRead(const char* path) {
  return SD_MMC.exists(path) ? SD_MMC.open(path) : File();
}
inline File sdOpenRead(const String& path) { return sdOpenRead(path.c_str()); }

// Same for deleting: removing a file that isn't there (a temp file that was
// never left behind) logs "[E] ... does not exists". Check first.
inline bool sdRemoveIfExists(const char* path) {
  return !SD_MMC.exists(path) || SD_MMC.remove(path);
}
inline bool sdRemoveIfExists(const String& path) { return sdRemoveIfExists(path.c_str()); }
