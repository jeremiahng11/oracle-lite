#pragma once
// ArduinoJson documents allocate many small blocks. Put them in the 8 MB
// PSRAM instead of the ~300 KB internal RAM, so large replies (AI answers,
// agenda, Companion data) can't fragment or exhaust the memory that Wi-Fi,
// TLS and audio need.
#include <ArduinoJson.h>
#include "esp_heap_caps.h"

struct SpiRamJsonAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  void deallocate(void* pointer) override { heap_caps_free(pointer); }
  void* reallocate(void* pointer, size_t newSize) override {
    void* p = heap_caps_realloc(pointer, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_realloc(pointer, newSize, MALLOC_CAP_8BIT);
  }
};

inline ArduinoJson::Allocator* psramJson() {
  static SpiRamJsonAllocator allocator;
  return &allocator;
}
