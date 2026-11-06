#pragma once
#include <string>
#include <algorithm>

inline bool utf8Continuation(unsigned char c) { return (c & 0xc0) == 0x80; }
inline std::string utf8Excerpt(const std::string& text, size_t start, size_t end) {
  start = std::min(start, text.size());
  end = std::min(end, text.size());
  while (start < end && utf8Continuation(static_cast<unsigned char>(text[start]))) ++start;
  if (end < text.size()) {
    while (end > start && utf8Continuation(static_cast<unsigned char>(text[end]))) --end;
  } else if (end > start) {
    size_t lead = end - 1;
    while (lead > start && utf8Continuation(static_cast<unsigned char>(text[lead]))) --lead;
    const unsigned char c = static_cast<unsigned char>(text[lead]);
    const size_t width = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
      (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
    if (end - lead < width) end = lead;
  }
  return end > start ? text.substr(start, end - start) : std::string();
}
