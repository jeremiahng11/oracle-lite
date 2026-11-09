#include <Arduino.h>
#include "sd_read.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "../../config.h"

OracleSdFs sdFs;

namespace {
constexpr size_t kNameLen = 40;            // longest name kept: 39 characters
constexpr const char* kDir = NOTES_DIR "/";
constexpr size_t kDirLen = sizeof(NOTES_DIR "/") - 1;

char (*names)[kNameLen] = nullptr;        // sorted, in PSRAM
size_t count = 0, capacity = 0;
bool valid = false;
SemaphoreHandle_t lock = nullptr;

struct Guard {
  Guard() { if (!lock) lock = xSemaphoreCreateRecursiveMutex(); xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
  ~Guard() { xSemaphoreGiveRecursive(lock); }
};

// "/notes/note_012.txt" -> "note_012.txt"; nullptr if not directly in /notes
// or too long to cache.
const char* cachedName(const char* path) {
  if (!path || strncmp(path, kDir, kDirLen) != 0) return nullptr;
  const char* name = path + kDirLen;
  if (!*name || strchr(name, '/') || strlen(name) >= kNameLen) return nullptr;
  return name;
}

int compareNames(const void* a, const void* b) { return strcmp(static_cast<const char*>(a), static_cast<const char*>(b)); }

bool reserve(size_t want) {
  if (want <= capacity) return true;
  const size_t next = want + 256;
  void* grown = heap_caps_realloc(names, next * kNameLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!grown) return false;
  names = static_cast<char (*)[kNameLen]>(grown);
  capacity = next;
  return true;
}

// One pass over the folder (names only, no file opens).
bool build() {
  count = 0;
  valid = false;
  DIR* dir = opendir("/sdcard" NOTES_DIR);
  if (!dir) return false;
  bool tooLong = false;
  while (struct dirent* entry = readdir(dir)) {
    if (strlen(entry->d_name) >= kNameLen) { tooLong = true; continue; }
    if (!reserve(count + 1)) { closedir(dir); return false; }
    strlcpy(names[count++], entry->d_name, kNameLen);
  }
  closedir(dir);
  qsort(names, count, kNameLen, compareNames);
  valid = true;
  if (tooLong) Serial.println("[SD] note folder has names longer than the cache keeps; those use the card");
  return true;
}

ssize_t find(const char* name) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(names[mid], name);
    if (c == 0) return mid;
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return -static_cast<ssize_t>(lo) - 1;
}

void insertName(const char* name) {
  if (!valid) return;
  const ssize_t at = find(name);
  if (at >= 0) return;
  const size_t pos = static_cast<size_t>(-at - 1);
  if (!reserve(count + 1)) { valid = false; return; }
  memmove(names[pos + 1], names[pos], (count - pos) * kNameLen);
  strlcpy(names[pos], name, kNameLen);
  count++;
}

void eraseName(const char* name) {
  if (!valid) return;
  const ssize_t at = find(name);
  if (at < 0) return;
  memmove(names[at], names[at + 1], (count - at - 1) * kNameLen);
  count--;
}
}  // namespace

void sdCacheReset() {
  Guard guard;
  valid = false;
  count = 0;
}

uint32_t buildFailedAt = 0;

bool sdExists(const char* path) {
  const char* name = cachedName(path);
  if (!name) return SD_MMC.exists(path);
  Guard guard;
  if (!valid) {
    // If the list can't be built (e.g. PSRAM short), don't rescan the folder
    // on every call: use the card directly and retry in 30 s.
    if (buildFailedAt && millis() - buildFailedAt < 30000UL) return SD_MMC.exists(path);
    if (!build()) { buildFailedAt = millis() | 1; return SD_MMC.exists(path); }
    buildFailedAt = 0;
  }
  return find(name) >= 0;
}

void sdCacheMismatch() {
  Guard guard;
  if (valid) Serial.println("[SD] folder list out of step with the card; rebuilding");
  valid = false;
}

File sdCreate(const char* path) {
  File file = SD_MMC.open(path, FILE_WRITE);
  const char* name = cachedName(path);
  if (file && name) { Guard guard; insertName(name); }
  else if (!name && strncmp(path, kDir, kDirLen) == 0) sdCacheReset();
  return file;
}

bool sdRemove(const char* path) {
  const bool ok = SD_MMC.remove(path);
  const char* name = cachedName(path);
  Guard guard;
  if (name) { if (ok) eraseName(name); else valid = false; }
  return ok;
}

bool sdRename(const char* from, const char* to) {
  const bool ok = SD_MMC.rename(from, to);
  const char* a = cachedName(from);
  const char* b = cachedName(to);
  Guard guard;
  if (!ok) { if (a || b) valid = false; return false; }
  if (a) eraseName(a);
  if (b) insertName(b);
  else if (strncmp(to, kDir, kDirLen) == 0) valid = false;
  return true;
}

bool sdMkdir(const char* path) {
  return SD_MMC.mkdir(path);
}
