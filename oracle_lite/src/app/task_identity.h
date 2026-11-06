#pragma once
#include <cstring>
#include <string>
#include <cctype>

inline std::string normalizedTaskUtf8(const char* value) {
  std::string out;
  if (!value) return out;
  bool gap = false;
  for (; *value; ++value) {
    const unsigned char c = static_cast<unsigned char>(*value);
    // ASCII punctuation is spacing; UTF-8 bytes must not be discarded.
    if (c >= 128 || std::isalnum(c)) {
      if (gap && !out.empty()) out += ' ';
      out += c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
      gap = false;
    } else gap = true;
  }
  return out;
}

inline bool sameTaskIdentity(const char* title, const char* oldTitle,
                             const char* type, const char* oldType,
                             const char* due, const char* oldDue) {
  return title && *title && oldTitle && type && oldType && due && oldDue &&
    std::strcmp(title, oldTitle) == 0 && std::strcmp(type, oldType) == 0 &&
    std::strcmp(due, oldDue) == 0;
}

inline bool itemRelativeDateMismatch(bool explicitlyTomorrow,
                                      const char* dueDay, const char* expectedDay) {
  return explicitlyTomorrow && expectedDay && *expectedDay &&
    (!dueDay || std::strcmp(dueDay, expectedDay) != 0);
}
