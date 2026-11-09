#pragma once
// Tells whether a recording contains speech or only background noise.
// Pure C++ (no Arduino types) so it can be tested on a PC.
//
// Works on 20 ms frames of 16 kHz mono PCM. The noise floor follows the
// quietest frames (pauses between words) and drifts up slowly, so steady
// noise such as a fan or traffic never counts. A frame is "loud" when it is
// clearly above that floor; speech is a run of loud frames (single clicks
// and bumps are ignored). A recording counts as speech once it has enough
// loud runs in total.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

class SpeechDetector {
 public:
  static constexpr int kFrameSamples = 320;       // 20 ms at 16 kHz
  static constexpr int kMinRunFrames = 3;         // 60 ms of sound
  static constexpr int kSpeechFramesNeeded = 10;  // 200 ms of speech in total

  void feed(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
      sum_ += static_cast<uint32_t>(std::abs(static_cast<int>(samples[i])));
      if (++inFrame_ == kFrameSamples) {
        frame(static_cast<int>(sum_ / kFrameSamples));
        sum_ = 0;
        inFrame_ = 0;
      }
    }
  }

  bool heardSpeech() const { return speechFrames_ >= kSpeechFramesNeeded; }
  int speechMs() const { return speechFrames_ * 20; }
  int frames() const { return frames_; }
  int noiseLevel() const { return static_cast<int>(noise_); }

 private:
  void frame(int level) {
    frames_++;
    if (frames_ == 1 || level < noise_) noise_ = static_cast<float>(level);
    else noise_ = noise_ * 1.0015f + 0.05f;   // slow drift up (~2x in 9 s)
    // Ignore the first 100 ms (button click, codec settling).
    if (frames_ <= 5) { run_ = 0; return; }
    const float threshold = noise_ * 2.5f > noise_ + 220.0f ? noise_ * 2.5f : noise_ + 220.0f;
    if (level > threshold && level > 300) {
      run_++;
      if (run_ == kMinRunFrames) speechFrames_ += kMinRunFrames;
      else if (run_ > kMinRunFrames) speechFrames_++;
    } else {
      run_ = 0;
    }
  }

  uint32_t sum_ = 0;
  int inFrame_ = 0;
  int frames_ = 0;
  int run_ = 0;
  int speechFrames_ = 0;
  float noise_ = 0;
};
