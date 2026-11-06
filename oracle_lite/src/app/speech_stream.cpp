#include "speech_stream.h"
#include "pcm_resampler.h"
#ifndef ORACLE_SPEECH_STREAM_HOST_TEST
#include "settings.h"
#include "../../config.h"
#include "../../sounds.h"
#include "esp_heap_caps.h"
#endif
#include <algorithm>
#include <cstring>
#ifndef ORACLE_SPEECH_STREAM_HOST_TEST
extern "C" {
#include "../audio/audio_bsp.h"
}
#endif

bool OracleSpeechStream::begin() {
  // Keep the network's internal heap available for TLS.
  buffer = static_cast<uint8_t*>(heap_caps_malloc(kPcmStreamCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer) {
    capacity = kPcmStreamSmallCapacity;
    buffer = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (!buffer) return false;
  cancelArmed = digitalRead(BTN_REC) == HIGH;
  if (xTaskCreatePinnedToCore(worker, "speech-audio", 6144, this, 1, nullptr, 1) != pdPASS) {
    heap_caps_free(buffer); buffer = nullptr; return false;
  }
  return true;
}

bool OracleSpeechStream::ready() const {
  return pcmStreamAvailable(written.load(std::memory_order_acquire), read.load(std::memory_order_acquire)) >= kPcmStreamPrebuffer;
}
void OracleSpeechStream::start() { started.store(true, std::memory_order_release); }

bool OracleSpeechStream::poll() {
  if (digitalRead(BTN_REC) == HIGH) { cancelArmed = true; lowSince = 0; }
  else if (cancelArmed) {
    if (!lowSince) lowSince = millis();
    else if (millis() - lowSince >= 20) { cancelled = true; aborting = true; return false; }
  }
  return !failed && !cancelled;
}
bool OracleSpeechStream::put(char byte) {
  if (!poll()) return false;
  const uint32_t waitStarted = millis();
  uint32_t position = written.load(std::memory_order_relaxed);
  while (pcmStreamAvailable(position, read.load(std::memory_order_acquire)) >= capacity) {
    if (!poll() || millis() - waitStarted >= 5000UL) return false;
    delay(1);
  }
  if (failed || cancelled) return false;
  buffer[position % capacity] = static_cast<uint8_t>(byte);
  written.store(position + 1, std::memory_order_release);
  return true;
}

bool OracleSpeechStream::finish(bool complete) {
  if (!buffer) return false;
  if (!complete && !cancelled) { failed = true; aborting = true; }
  if (complete && !pcmStreamValid(written.load())) { failed = true; aborting = true; }
  ended.store(true, std::memory_order_release);
  // A valid short response must not wait forever for the prebuffer threshold.
  if (complete) start();
  // Worker exit is bounded by finite I2S writes; never free its storage early.
  while (!done.load(std::memory_order_acquire)) delay(1);
  heap_caps_free(buffer); buffer = nullptr;
  return cancelled || !failed;
}

void OracleSpeechStream::worker(void* context) {
  auto& stream = *static_cast<OracleSpeechStream*>(context);
  alignas(4) int16_t mono[512];
  int16_t stereo[1024];
  OraclePcmResampler resampler;
  uint32_t outputBytes = 0, underruns = 0;
  bool cancelArmed = digitalRead(BTN_REC) == HIGH;
  uint32_t lowSince = 0;
  while (!stream.started && !stream.ended && !stream.aborting) vTaskDelay(pdMS_TO_TICKS(5));
  if (!stream.aborting) {
    audio_playback_set_vol(oraclePlaybackVolume());
    vTaskDelay(pdMS_TO_TICKS(25));
    while (!stream.aborting) {
      if (digitalRead(BTN_REC) == HIGH) { cancelArmed = true; lowSince = 0; }
      else if (cancelArmed) {
        if (!lowSince) lowSince = millis();
        else if (millis() - lowSince >= 20) { stream.cancelled = true; stream.aborting = true; break; }
      }
      const uint32_t position = stream.read.load(std::memory_order_relaxed);
      const uint32_t available = pcmStreamAvailable(stream.written.load(std::memory_order_acquire), position);
      if (available < sizeof(mono) && !stream.ended) {
        // Rebuffer before sending another block; no truncated/duplicated PCM.
        ++underruns;
        memset(stereo, 0, sizeof(stereo));
        if (audio_playback_write(stereo, sizeof(stereo)) != 0) { stream.failed = true; stream.aborting = true; break; }
        continue;
      }
      const uint32_t bytes = std::min<uint32_t>(available & ~1U, sizeof(mono));
      if (!bytes) break;
      auto* destination = reinterpret_cast<uint8_t*>(mono);
      for (uint32_t i = 0; i < bytes; ++i) destination[i] = stream.buffer[(position + i) % stream.capacity];
      stream.read.store(position + bytes, std::memory_order_release);
      const int frames = resampler.convert(mono, bytes / 2, 1, stream.from24k, stereo);
      const uint32_t output = frames * 4;
      if (output && audio_playback_write(stereo, output) != 0) { stream.failed = true; stream.aborting = true; break; }
      outputBytes += output;
    }
    if (!stream.aborting && outputBytes) {
      memset(stereo, 0, sizeof(stereo));
      for (int i = 0; i < 3; ++i)
        if (audio_playback_write(stereo, sizeof(stereo)) != 0) stream.failed = true;
    }
    audio_playback_set_vol(0);
    audio_idle_suspend();
  }
  Serial.printf("[Audio] stream output=%lu bytes rebuffer=%lu cancelled=%d failed=%d\n",
    (unsigned long)outputBytes, (unsigned long)underruns, stream.cancelled.load(), stream.failed.load());
  stream.done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
