#pragma once
#include <cstddef>
#include <cstdint>
// SPSC ring: caller publishes the counters using acquire/release atomics.
// Absolute unsigned counters make wraparound safe; capacity must divide 2^32.
inline uint32_t pcmStreamAvailable(uint32_t written, uint32_t read) { return written - read; }
inline bool pcmStreamValid(size_t bytes) { return bytes > 0 && !(bytes & 1U); }
constexpr uint32_t kPcmStreamCapacity = 1024 * 1024;
constexpr uint32_t kPcmStreamSmallCapacity = 65536;
constexpr uint32_t kPcmStreamPrebuffer = 24576; // 512 ms of 24 kHz mono PCM
