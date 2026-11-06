#pragma once
#include <ctime>
// Resolve target wall-clock dates with the timezone rules for that date,
// rather than the UTC offset at the time the recording was made.
inline time_t localWallClockEpoch(time_t wallAsUtc) {
  struct tm wall = {};
  gmtime_r(&wallAsUtc, &wall);
  wall.tm_isdst = -1;
  const struct tm requested = wall;
  const time_t result = mktime(&wall);
  if (wall.tm_year != requested.tm_year || wall.tm_mon != requested.tm_mon ||
      wall.tm_mday != requested.tm_mday || wall.tm_hour != requested.tm_hour ||
      wall.tm_min != requested.tm_min) return -1; // nonexistent DST wall time
  return result;
}
