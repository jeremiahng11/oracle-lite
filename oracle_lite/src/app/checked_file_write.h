#pragma once
#include <cstddef>
#include <cstdint>

template <typename File>
bool checkedFileWrite(File& file, const uint8_t* bytes, size_t length, size_t& expected) {
  if (length > SIZE_MAX - expected) return false;
  expected += length;
  return file.write(bytes, length) == length;
}
template <typename File>
bool checkedFileComplete(File& file, size_t expected) {
  file.flush();
  return file.size() == expected;
}
