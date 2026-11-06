#pragma once
#include <cstdint>

inline bool speechTransferWithinDeadline(uint32_t started, uint32_t lastByte, uint32_t now) {
  return uint32_t(now - started) < 90000U && uint32_t(now - lastByte) < 45000U;
}

inline bool speechStreamWithinDeadline(uint32_t started, uint32_t lastByte,
                                       uint32_t now, bool playing) {
  // Give generation a reasonable startup window. Once speech has started,
  // a long empty stream must not keep playing silence for 45 seconds.
  return uint32_t(now - started) < 90000U &&
         uint32_t(now - lastByte) < (playing ? 15000U : 30000U);
}

inline bool speechRetryAllowed(int attempt, int status, bool complete,
                               bool playbackStarted, bool cancelled) {
  if (attempt != 0 || complete || playbackStarted || cancelled) return false;
  return status <= 0 || status == 200 || status == 408 || status == 425 ||
         status == 429 || (status >= 500 && status <= 599);
}
