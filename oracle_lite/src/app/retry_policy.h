#pragma once
#include <stdint.h>

inline uint32_t queueRetryDelaySeconds(int failures) {
  if (failures <= 1) return 120;
  if (failures >= 5) return 1800;
  return 120U << (failures - 1);
}

inline int queueNextFailures(int failures, bool failed, bool progressed) {
  if (progressed) return 0;
  if (failures < 0) failures = 0;
  return failed && failures < 1000000 ? failures + 1 : failures;
}

inline bool queueRetryElapsed(int failures, int64_t failedUtc, int64_t nowUtc,
                              uint32_t startedMs, uint32_t nowMs) {
  const uint32_t seconds = queueRetryDelaySeconds(failures);
  if (failedUtc >= 1700000000 && nowUtc >= failedUtc)
    return nowUtc - failedUtc >= seconds;
  // No usable clock (or a clock correction): still retry without requiring
  // another recording to establish network time. millis subtraction wraps safely.
  return uint32_t(nowMs - startedMs) >= seconds * 1000U;
}
