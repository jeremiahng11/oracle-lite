#pragma once
#include <cstring>

// An undated commitment is still an open task, not a missing appointment.
// Include it in today's/open overview, never assign it to a future/past day.
inline bool undatedTaskVisible(bool event, bool done, bool tomorrow = false,
                               bool past = false) {
  return !event && !done && !tomorrow && !past;
}

inline bool eventStillRelevant(const char* dueDay, const char* today,
                               const char* dueUtc, const char* nowUtc,
                               bool dayOnly) {
  if (!dueDay || !today || !*dueDay || !*today) return false;
  // A date without a time remains relevant for the complete local day.
  return dayOnly ? std::strcmp(dueDay, today) >= 0
                 : dueUtc && nowUtc && *nowUtc && std::strcmp(dueUtc, nowUtc) > 0;
}
