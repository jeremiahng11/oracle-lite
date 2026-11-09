#pragma once
#include <SD_MMC.h>

// ─── Fast SD access for the notes folder ────────────────────────────────────
// /notes holds several files per memory, so it grows to thousands of entries.
// FAT has no directory index: every exists()/open() of a missing name scans
// the whole folder (hundreds of KB read from the card). The names in /notes
// are therefore kept in a sorted list in PSRAM, updated by the helpers below
// (create / remove / rename), so "is it there?" is answered from RAM.
// Everything that creates, removes or renames files must use these helpers.

// True if the file exists (RAM lookup for /notes/<name>, the card otherwise).
bool sdExists(const char* path);
inline bool sdExists(const String& path) { return sdExists(path.c_str()); }

// Opens a new/overwritten file for writing (FILE_WRITE) and records it.
File sdCreate(const char* path);
inline File sdCreate(const String& path) { return sdCreate(path.c_str()); }

bool sdRemove(const char* path);
inline bool sdRemove(const String& path) { return sdRemove(path.c_str()); }
bool sdRename(const char* from, const char* to);
inline bool sdRename(const String& from, const String& to) { return sdRename(from.c_str(), to.c_str()); }
bool sdMkdir(const char* path);

// Forget the cached listing (after mounting, or if the card was changed).
void sdCacheReset();
// Called when the card disagrees with the list (a listed file can't be
// opened, or an unlisted one exists): rebuild it on next use.
void sdCacheMismatch();

// Opening a missing file for reading makes the ESP32 file layer log
// "[E] ... does not exist"; many files here are optional, so check first.
inline File sdOpenRead(const char* path) {
  if (!sdExists(path)) return File();
  File file = SD_MMC.open(path);
  if (!file) sdCacheMismatch();
  return file;
}
inline File sdOpenRead(const String& path) { return sdOpenRead(path.c_str()); }

// Same for deleting: no error log for a file that isn't there.
inline bool sdRemoveIfExists(const char* path) {
  return !sdExists(path) || sdRemove(path);
}
inline bool sdRemoveIfExists(const String& path) { return sdRemoveIfExists(path.c_str()); }

// File-system object for commitStoredFile()/recoverStoredFile().
struct OracleSdFs {
  bool exists(const char* path) { return sdExists(path); }
  bool remove(const char* path) { return sdRemove(path); }
  bool rename(const char* from, const char* to) { return sdRename(from, to); }
};
extern OracleSdFs sdFs;
