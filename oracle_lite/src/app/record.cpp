#include "Arduino.h"
#include "sd_read.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "record.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "notes.h"
#include "settings.h"
#include "ui.h"
#include "pcm_resampler.h"
#include "wav_payload.h"
#include "../../sounds.h"
#include "minutes.h"
#include "battery.h"
#include "speech_detect.h"
#include <atomic>
#include <algorithm>
#include <cmath>

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

namespace {
std::atomic<bool> recordingUiBusy{false};
bool recordingUiProgress = false;
bool recordingUiQuestion = false;
bool recordingUiMeeting = false;
uint32_t recordingUiMarkerMs = 0;
int recordingUiMarkerCount = 0;

void recordingUiTask(void*) {
  // E-paper is intentionally refreshed on the other ESP32 core. The recording
  // loop must keep draining I2S while the comparatively slow panel updates.
  if (recordingUiMeeting) showMeetingRecording(recordingUiMarkerMs, recordingUiProgress);
  else if (recordingUiProgress) showRecordingProgress(recordingUiMarkerMs, recordingUiMarkerCount);
  else if (recordingUiQuestion) showAskListening();
  else showRecording();
  recordingUiBusy = false;
  vTaskDelete(nullptr);
}

bool startRecordingUi(bool progress = false, uint32_t markerMs = 0, int markerCount = 0, bool question = false) {
  // Never wait for e-paper while draining the microphone. A marker remains
  // saved even when its visual update is skipped during an existing refresh.
  if (recordingUiBusy.load()) return true;
  recordingUiMeeting = false;
  recordingUiProgress = progress;
  recordingUiQuestion = question;
  recordingUiMarkerMs = markerMs;
  recordingUiMarkerCount = markerCount;
  recordingUiBusy = true;
  const BaseType_t started = xTaskCreatePinnedToCore(
      recordingUiTask, "recording-ui", 4096, nullptr, 1, nullptr, 0);
  if (started != pdPASS) {
    recordingUiBusy = false;
    Serial.println("[Rec] warning: recording UI task could not be started");
  }
  return started == pdPASS;
}

void waitForRecordingUi() {
  while (recordingUiBusy) delay(1);
}

bool startMeetingUi(uint32_t elapsedMs, bool fast) {
  if (recordingUiBusy.load()) return true;
  recordingUiMeeting = true;
  recordingUiProgress = fast;
  recordingUiMarkerMs = elapsedMs;
  recordingUiBusy = true;
  const BaseType_t started = xTaskCreatePinnedToCore(
      recordingUiTask, "meeting-ui", 4096, nullptr, 1, nullptr, 0);
  if (started != pdPASS) recordingUiBusy = false;
  return started == pdPASS;
}
}  // namespace

static bool writeWavHeader(File& file, uint32_t dataBytes) {
  const uint32_t resumeAt = file.position();
  if (!file.seek(0)) return false;
  const uint32_t fileSize = dataBytes + 36;
  const uint32_t byteRate = SAMPLE_RATE * 2;
  const uint32_t fmtLength = 16;
  const uint32_t sampleRate = SAMPLE_RATE;
  const uint16_t blockAlign = 2, audioFormat = 1, channels = 1, bits = 16;
  bool ok = file.write((uint8_t*)"RIFF", 4) == 4;
  ok &= file.write((uint8_t*)&fileSize, 4) == 4;
  ok &= file.write((uint8_t*)"WAVE", 4) == 4;
  ok &= file.write((uint8_t*)"fmt ", 4) == 4;
  ok &= file.write((uint8_t*)&fmtLength, 4) == 4;
  ok &= file.write((uint8_t*)&audioFormat, 2) == 2;
  ok &= file.write((uint8_t*)&channels, 2) == 2;
  ok &= file.write((uint8_t*)&sampleRate, 4) == 4;
  ok &= file.write((uint8_t*)&byteRate, 4) == 4;
  ok &= file.write((uint8_t*)&blockAlign, 2) == 2;
  ok &= file.write((uint8_t*)&bits, 2) == 2;
  ok &= file.write((uint8_t*)"data", 4) == 4;
  ok &= file.write((uint8_t*)&dataBytes, 4) == 4;
  file.flush();
  const bool restored = file.seek(max<uint32_t>(resumeAt, 44));
  return ok && restored;
}

static bool lastHadSpeech = true;
static int lastSpeechMs = 0;
bool lastRecordingHadSpeech() { return lastHadSpeech; }
int lastRecordingSpeechMs() { return lastSpeechMs; }

static bool recordHeldToPath(const char* path, int holdPin, bool captureMoments, int noteNum) {
  Serial.printf("[Rec] %s\n", path);

  // A recording absent from the catalog must never be silently overwritten.
  // Questions remove their disposable file explicitly before reaching here.
  if (SD_MMC.exists(path)) {
    if (!sdExists(path)) sdCacheMismatch();   // the list missed this file
    return false;
  }

  File f = sdCreate(path);
  if (!f) return false;

  uint8_t header[44]={};
  if (f.write(header, 44) != 44 || !writeWavHeader(f, 0)) {
    f.close();
    return false;
  }

  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF,   MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF/2, MALLOC_CAP_8BIT);
  if (!sbuf||!mbuf) {
    if(sbuf)heap_caps_free(sbuf); if(mbuf)heap_caps_free(mbuf);
    f.close(); return false;
  }

  if (!startRecordingUi(false, 0, 0, !captureMoments)) {
    heap_caps_free(sbuf); heap_caps_free(mbuf);
    f.close();
    sdRemoveIfExists(path); // Only the empty file created by this attempt.
    return false;
  }

  uint32_t totalMono=0, t0=millis(), lastCheckpoint=t0;
  std::vector<uint32_t> momentsMs;
  bool markReady = digitalRead(BTN_PWR) == HIGH;
  bool audioOk = true;
  SpeechDetector speech;

  while (digitalRead(holdPin)==LOW || millis()-t0 < 500) {
    if (audio_playback_read((void*)sbuf, REC_BUF) != 0) {
      audioOk = false;
      Serial.println("[Rec] microphone read failed");
      break;
    }
    int mono = REC_BUF/4;
    for (int i=0;i<mono;i++) mbuf[i] = sbuf[i*2];
    speech.feed(mbuf, mono);
    size_t written = f.write((uint8_t*)mbuf, mono*2);
    if (written != (size_t)(mono * 2)) {
      audioOk = false;
      Serial.println("[Rec] storage write failed; partial recording retained");
      break;
    }
    totalMono += written;

    const uint32_t now = millis();
    if (now - lastCheckpoint >= 5000UL) {
      if (!writeWavHeader(f, totalMono)) {
        audioOk = false;
        Serial.println("[Rec] WAV checkpoint failed; partial recording retained");
        break;
      }
      lastCheckpoint = now;
      const uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
      if (used >= total || total - used < 2ULL * 1024ULL * 1024ULL) {
        Serial.println("[Rec] stopping: SD card is nearly full");
        break;
      }
    }

    bool markDown = captureMoments && digitalRead(BTN_PWR) == LOW;
    if (!markDown) {
      markReady = true;
    } else if (markReady) {
      markReady = false;
      uint32_t recordedSamples = totalMono / sizeof(int16_t);
      uint32_t markerMs = (recordedSamples * 1000ULL) / SAMPLE_RATE;
      if (momentsMs.empty() || markerMs - momentsMs.back() >= 250) {
        momentsMs.push_back(markerMs);
        startRecordingUi(true, markerMs, (int)momentsMs.size());
      }
    }
  }

  waitForRecordingUi();
  audio_idle_suspend();

  heap_caps_free(sbuf); heap_caps_free(mbuf);

  const bool headerOk = writeWavHeader(f, totalMono);
  f.close();

  if (captureMoments && !saveNoteMoments(noteNum, momentsMs)) {
    Serial.println("[Rec] warning: moments were not saved");
  }

  lastHadSpeech = speech.heardSpeech();
  lastSpeechMs = speech.speechMs();
  Serial.printf("[Rec] done: %lu bytes, %u moments, speech %d ms (noise %d)\n",
                (unsigned long)totalMono, (unsigned)momentsMs.size(), lastSpeechMs, speech.noiseLevel());
  return audioOk && headerOk && totalMono > 1000;
}

bool record() {
  int num = nextNoteNumber();
  if (num <= 0) return false;
  char path[64]; snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  bool ok = recordHeldToPath(path, BTN_REC, true, num);
  if (ok) lastRecNum = num;
  return ok;
}

bool recordQuestion(const char* path, int holdPin) {
  sdRemoveIfExists(path);
  return recordHeldToPath(path, holdPin, false, -1);
}

bool playWavFile(const char* path) {
  File f = sdOpenRead(path);
  if (!f) return false;
  if (f.size() <= 44) { f.close(); return false; }

  // Do not assume that PCM starts at byte 44. Speech services may add LIST,
  // JUNK or an extended fmt chunk, especially for longer generations.
  uint8_t riff[12];
  if (f.read(riff, sizeof(riff)) != sizeof(riff) ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
    f.close(); return false;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sampleRate = 0, dataOffset = 0, dataBytes = 0;
  while (f.available() >= 8) {
    uint8_t chunk[8];
    if (f.read(chunk, sizeof(chunk)) != sizeof(chunk)) break;
    const uint32_t chunkSize = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) |
                               ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
    const uint32_t chunkData = f.position();
    if (!wavChunkFits(chunkData, chunkSize, f.size())) { f.close(); return false; }
    if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
      uint8_t fmt[16];
      if (f.read(fmt, sizeof(fmt)) != sizeof(fmt)) break;
      format = fmt[0] | ((uint16_t)fmt[1] << 8);
      channels = fmt[2] | ((uint16_t)fmt[3] << 8);
      sampleRate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                   ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      bits = fmt[14] | ((uint16_t)fmt[15] << 8);
    } else if (memcmp(chunk, "data", 4) == 0) {
      dataOffset = chunkData;
      dataBytes = chunkSize;
      break;
    }
    const uint64_t next = uint64_t(chunkData) + chunkSize + (chunkSize & 1U);
    if (next > f.size() || !f.seek(static_cast<uint32_t>(next))) break;
  }
  if (format != 1 || (channels != 1 && channels != 2) || bits != 16 ||
      (sampleRate != 16000 && sampleRate != 24000) || !dataOffset || !wavFramesValid(dataBytes, channels) ||
      !f.seek(dataOffset)) {
    Serial.printf("[Audio] invalid WAV fmt=%u ch=%u rate=%lu bits=%u data=%lu\n",
                  format, channels, static_cast<unsigned long>(sampleRate), bits,
                  static_cast<unsigned long>(dataBytes));
    f.close(); return false;
  }
  Serial.printf("[Audio] playing %s: %lu bytes, %lu Hz, %u ch\n", path,
                static_cast<unsigned long>(dataBytes),
                static_cast<unsigned long>(sampleRate), channels);

  const int monoBytes = 1024;
  uint8_t* monoBuf   = (uint8_t*)heap_caps_malloc(monoBytes,     MALLOC_CAP_8BIT);
  int16_t* stereoBuf = (int16_t*)heap_caps_malloc(monoBytes * 2, MALLOC_CAP_8BIT);

  if (!monoBuf || !stereoBuf) {
    if (monoBuf)   heap_caps_free(monoBuf);
    if (stereoBuf) heap_caps_free(stereoBuf);
    f.close();
    return false;
  }

  audioPlaying  = true;
  stopPlayback  = false;

  oracleSoundSetEnabled(false);
  audio_playback_set_vol(oraclePlaybackVolume());
  delay(25); // Give the codec amplifier time to leave mute before first PCM.

  uint32_t remaining = dataBytes;
  uint32_t playedBytes = 0;
  bool playbackOk = true;
  bool cancelArmed = digitalRead(BTN_REC) == HIGH;
  OraclePcmResampler resampler;
  while (remaining > 0 && !stopPlayback) {
    int readBytes = f.read(monoBuf, min<uint32_t>(monoBytes, remaining));
    if (readBytes <= 0) { playbackOk = false; break; }
    remaining -= readBytes;
    if (!wavFramesValid(readBytes, channels)) { playbackOk = false; break; }

    int samples = readBytes / 2;
    int16_t* input = (int16_t*)monoBuf;
    const int frames = samples / channels;
    const int outputSamples = resampler.convert(input, frames, channels,
                                                sampleRate == 24000, stereoBuf);
    const uint32_t outputBytes = outputSamples * 2 * sizeof(int16_t);
    if (outputBytes) {
      const int result = audio_playback_write((void*)stereoBuf, outputBytes);
      if (result != 0) {
        Serial.printf("[Audio] codec write failed: %d\n", result);
        playbackOk = false;
        break;
      }
      playedBytes += outputBytes;
    }

    if (digitalRead(BTN_REC) == HIGH) cancelArmed = true;
    else if (cancelArmed) {
      delay(20);
      if (digitalRead(BTN_REC) == LOW) {
        stopPlayback = true;
      }
    }
  }

  // I2S writes queue DMA buffers; muting immediately clips the final audio.
  // Flush with 160 ms of silence (more than the configured 6 x 240-frame DMA).
  if (playbackOk && !stopPlayback && playedBytes) {
    memset(stereoBuf, 0, monoBytes * 2);
    for (int i = 0; i < 5; ++i) {
      if (audio_playback_write(stereoBuf, monoBytes * 2) != 0) {
        playbackOk = false;
        break;
      }
    }
  }
  audio_playback_set_vol(0);
  audio_idle_suspend();
  oracleSoundSetEnabled(true);

  heap_caps_free(monoBuf);
  heap_caps_free(stereoBuf);
  f.close();

  audioPlaying = false;
  stopPlayback = false;
  Serial.printf("[Audio] playback complete: %lu output bytes\n",
                static_cast<unsigned long>(playedBytes));
  return playbackOk && playedBytes > 0;
}

// ─── Meeting minutes ───────────────────────────────────────────────────────
// Records until the top button is pressed again (or the length/SD limit is
// reached). Unlike notes, the button does not need to be held.
bool recordMeeting(const char* path, uint32_t& durationMs, void (*onFlushed)(uint32_t)) {
  constexpr uint32_t kMaxMeetingMs = 4UL * 60UL * 60UL * 1000UL;  // ~230 MB
  constexpr uint32_t kScreenEveryMs = 60000UL;
  durationMs = 0;
  Serial.printf("[Meeting] %s\n", path);
  if (SD_MMC.exists(path)) {
    if (!sdExists(path)) sdCacheMismatch();   // the list missed this file
    return false;
  }
  File f = sdCreate(path);
  if (!f) return false;
  uint8_t header[44] = {};
  if (f.write(header, 44) != 44 || !writeWavHeader(f, 0)) { f.close(); return false; }

  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF, MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF / 2, MALLOC_CAP_8BIT);
  if (!sbuf || !mbuf) {
    if (sbuf) heap_caps_free(sbuf);
    if (mbuf) heap_caps_free(mbuf);
    f.close(); sdRemoveIfExists(path); return false;
  }
  startMeetingUi(0, false);

  uint32_t totalMono = 0;
  const uint32_t t0 = millis();
  uint32_t lastCheckpoint = t0, lastScreen = t0, lastBatteryCheck = t0;
  bool stopArmed = digitalRead(BTN_REC) == HIGH;   // wait for the tap to end
  uint32_t stopDownSince = 0;
  bool audioOk = true;
  bool liveShown = false;

  while (true) {
    if (audio_playback_read((void*)sbuf, REC_BUF) != 0) {
      audioOk = false;
      Serial.println("[Meeting] microphone read failed");
      break;
    }
    const int mono = REC_BUF / 4;
    for (int i = 0; i < mono; i++) mbuf[i] = sbuf[i * 2];
    const size_t written = f.write((uint8_t*)mbuf, mono * 2);
    if (written != (size_t)(mono * 2)) {
      audioOk = false;
      Serial.println("[Meeting] storage write failed; partial recording kept");
      break;
    }
    totalMono += written;

    const uint32_t now = millis();
    if (now - lastCheckpoint >= 3000UL) {   // also the live-streaming cadence
      lastCheckpoint = now;
      if (!writeWavHeader(f, totalMono)) { audioOk = false; break; }
      if (onFlushed) onFlushed(44 + totalMono);
      const uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
      if (used >= total || total - used < 8ULL * 1024ULL * 1024ULL) {
        Serial.println("[Meeting] stopping: SD card is nearly full");
        break;
      }
      // Save the recording properly before the battery gives out.
      if (now - lastBatteryCheck >= 60000UL) {
        lastBatteryCheck = now;
        const BatteryReading battery = readBattery();
        if (battery.valid && battery.percent >= 0 && battery.percent <= 3) {
          Serial.printf("[Meeting] stopping: battery %d%%\n", battery.percent);
          break;
        }
      }
    }
    if (now - t0 >= kMaxMeetingMs) {
      Serial.println("[Meeting] stopping: 4 hour limit");
      break;
    }
    const bool liveNow = minutesLiveConnected();
    if (now - lastScreen >= kScreenEveryMs ||
        (liveNow != liveShown && now - lastScreen >= 5000UL)) {
      lastScreen = now;
      liveShown = liveNow;
      startMeetingUi(now - t0, true);
    }

    // Stop on a fresh top-button press held for 60 ms (ignores contact bounce).
    if (digitalRead(BTN_REC) == HIGH) { stopArmed = true; stopDownSince = 0; }
    else if (stopArmed) {
      if (!stopDownSince) stopDownSince = now;
      else if (now - stopDownSince >= 60) break;
    }
  }

  waitForRecordingUi();
  audio_idle_suspend();
  heap_caps_free(sbuf); heap_caps_free(mbuf);
  const bool headerOk = writeWavHeader(f, totalMono);
  f.close();
  if (onFlushed) onFlushed(44 + totalMono);
  durationMs = (static_cast<uint64_t>(totalMono / 2) * 1000ULL) / SAMPLE_RATE;
  // Let go of the stop press before returning so it is not seen as a new tap.
  const uint32_t releaseBy = millis() + 3000;
  while (digitalRead(BTN_REC) == LOW && static_cast<int32_t>(releaseBy - millis()) > 0) delay(5);
  Serial.printf("[Meeting] done: %lu bytes, %lu s\n", (unsigned long)totalMono,
                (unsigned long)(durationMs / 1000));
  // Under 10 s is an accidental start: don't keep it.
  if (totalMono < SAMPLE_RATE * 2 * 10) {
    sdRemoveIfExists(path);
    Serial.println("[Meeting] under 10 s: not saved");
    return false;
  }
  return headerOk || audioOk;   // a partial recording is still worth sending
}

// ─── Spoken question without holding a button (web ask) ────────────────────
bool recordQuestionUntilPress(const char* path, uint32_t maxMs) {
  sdRemoveIfExists(path);
  File f = sdCreate(path);
  if (!f) return false;
  uint8_t header[44] = {};
  if (f.write(header, 44) != 44 || !writeWavHeader(f, 0)) { f.close(); return false; }
  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF, MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF / 2, MALLOC_CAP_8BIT);
  if (!sbuf || !mbuf) {
    if (sbuf) heap_caps_free(sbuf);
    if (mbuf) heap_caps_free(mbuf);
    f.close(); return false;
  }
  startRecordingUi(false, 0, 0, true);
  uint32_t totalMono = 0;
  const uint32_t t0 = millis();
  bool upperArmed = digitalRead(BTN_REC) == HIGH, lowerArmed = digitalRead(BTN_PWR) == HIGH;
  uint32_t downSince = 0;
  bool audioOk = true;
  while (millis() - t0 < maxMs) {
    if (audio_playback_read((void*)sbuf, REC_BUF) != 0) { audioOk = false; break; }
    const int mono = REC_BUF / 4;
    for (int i = 0; i < mono; i++) mbuf[i] = sbuf[i * 2];
    if (f.write((uint8_t*)mbuf, mono * 2) != (size_t)(mono * 2)) { audioOk = false; break; }
    totalMono += mono * 2;
    // Either button (pressed fresh, held 40 ms) sends the question.
    const bool upper = digitalRead(BTN_REC) == LOW, lower = digitalRead(BTN_PWR) == LOW;
    if (!upper) upperArmed = true;
    if (!lower) lowerArmed = true;
    if ((upper && upperArmed) || (lower && lowerArmed)) {
      if (!downSince) downSince = millis();
      else if (millis() - downSince >= 40 && millis() - t0 > 500) break;
    } else {
      downSince = 0;
    }
  }
  waitForRecordingUi();
  audio_idle_suspend();
  heap_caps_free(sbuf); heap_caps_free(mbuf);
  const bool headerOk = writeWavHeader(f, totalMono);
  f.close();
  const uint32_t releaseBy = millis() + 3000;
  while ((digitalRead(BTN_REC) == LOW || digitalRead(BTN_PWR) == LOW) &&
         static_cast<int32_t>(releaseBy - millis()) > 0) delay(5);
  return audioOk && headerOk && totalMono >= SAMPLE_RATE;   // at least 0.5 s
}

// ─── Spoken question that ends when the speaker stops (web ask) ────────────
// Voice activity is judged per 128 ms block against the room's own noise
// floor (measured at the start and while nobody speaks), so it adapts to quiet
// rooms and noisy cafes alike.
bool recordQuestionUntilSilence(const char* path, uint32_t maxMs,
                                void (*onAudio)(const int16_t*, size_t), bool& heardSpeech) {
  constexpr uint32_t kEndSilenceMs = 1000;   // pause that ends the question
  constexpr uint32_t kNoSpeechMs = 7000;     // nobody spoke: give up
  heardSpeech = false;
  sdRemoveIfExists(path);
  File f = sdCreate(path);
  if (!f) return false;
  uint8_t header[44] = {};
  if (f.write(header, 44) != 44 || !writeWavHeader(f, 0)) { f.close(); return false; }
  int16_t* sbuf = (int16_t*)heap_caps_malloc(REC_BUF, MALLOC_CAP_8BIT);
  int16_t* mbuf = (int16_t*)heap_caps_malloc(REC_BUF / 2, MALLOC_CAP_8BIT);
  if (!sbuf || !mbuf) {
    if (sbuf) heap_caps_free(sbuf);
    if (mbuf) heap_caps_free(mbuf);
    f.close(); return false;
  }
  startRecordingUi(false, 0, 0, true);
  const int mono = REC_BUF / 4;
  const uint32_t blockMs = static_cast<uint32_t>(mono) * 1000UL / SAMPLE_RATE;
  uint32_t totalMono = 0, elapsedMs = 0, silentMs = 0;
  float noise = 0, maxLevel = 0, peakSpeech = 0;
  int calibrationBlocks = 0, loudBlocks = 0;
  bool upperArmed = digitalRead(BTN_REC) == HIGH, lowerArmed = digitalRead(BTN_PWR) == HIGH;
  uint32_t downSince = 0;
  bool audioOk = true;
  const uint32_t t0 = millis();
  while (millis() - t0 < maxMs) {
    if (audio_playback_read((void*)sbuf, REC_BUF) != 0) { audioOk = false; break; }
    int64_t energy = 0;
    for (int i = 0; i < mono; i++) {
      mbuf[i] = sbuf[i * 2];
      energy += static_cast<int32_t>(mbuf[i]) * mbuf[i];
    }
    if (f.write((uint8_t*)mbuf, mono * 2) != (size_t)(mono * 2)) { audioOk = false; break; }
    if (onAudio) onAudio(mbuf, mono);
    totalMono += mono * 2;
    elapsedMs += blockMs;
    const float level = sqrtf(static_cast<float>(energy) / mono);

    maxLevel = std::max(maxLevel, level);
    if (calibrationBlocks < 3) {   // first ~0.4 s: learn the room
      noise = calibrationBlocks ? std::min(noise, level) : level;
      calibrationBlocks++;
      if (calibrationBlocks == 3) {
        // Already talking during the calibration (loud and steady): treat it
        // as speech and assume a normal quiet room instead.
        if (noise > 900.0f) { heardSpeech = true; peakSpeech = noise; noise = 250.0f; }
        if (noise < 30.0f) noise = 30.0f;
      }
    } else {
      const float speakingLevel = std::max(noise * 2.5f, noise + 250.0f);
      if (!heardSpeech) {
        loudBlocks = level > speakingLevel ? loudBlocks + 1 : 0;
        if (loudBlocks >= 2) { heardSpeech = true; peakSpeech = level; }   // ~0.25 s of voice
        else if (level < noise * 1.5f) noise = noise * 0.9f + level * 0.1f;   // track the room
        if (!heardSpeech && elapsedMs >= kNoSpeechMs) break;
      } else {
        peakSpeech = std::max(peakSpeech, level);
        // Quiet = back near the room level, or far below how loud the voice was.
        const float quietLevel = std::max(std::max(noise * 1.6f, noise + 120.0f), peakSpeech * 0.15f);
        silentMs = level < quietLevel ? silentMs + blockMs : 0;
        if (silentMs >= kEndSilenceMs) break;                            // finished speaking
      }
    }
    // A button press still sends the question at once.
    const bool upper = digitalRead(BTN_REC) == LOW, lower = digitalRead(BTN_PWR) == LOW;
    if (!upper) upperArmed = true;
    if (!lower) lowerArmed = true;
    if ((upper && upperArmed) || (lower && lowerArmed)) {
      if (!downSince) downSince = millis();
      else if (millis() - downSince >= 40 && millis() - t0 > 500) { heardSpeech = true; break; }
    } else {
      downSince = 0;
    }
  }
  waitForRecordingUi();
  audio_idle_suspend();
  heap_caps_free(sbuf); heap_caps_free(mbuf);
  const bool headerOk = writeWavHeader(f, totalMono);
  f.close();
  const uint32_t releaseBy = millis() + 3000;
  while ((digitalRead(BTN_REC) == LOW || digitalRead(BTN_PWR) == LOW) &&
         static_cast<int32_t>(releaseBy - millis()) > 0) delay(5);
  // Never drop a question because the detector was unsure: a clear rise above
  // the room level means someone spoke.
  if (!heardSpeech && maxLevel > std::max(noise * 2.0f, noise + 150.0f)) heardSpeech = true;
  Serial.printf("[Rec] question %lu ms, speech=%d, room=%.0f loudest=%.0f\n",
                static_cast<unsigned long>(elapsedMs), heardSpeech, noise, maxLevel);
  return audioOk && headerOk && totalMono >= SAMPLE_RATE;
}
