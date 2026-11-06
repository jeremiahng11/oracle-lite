#pragma once
#include <stdint.h>

// Keep the 3:2 conversion phase across SD read boundaries. Resetting the
// grouping every 512 frames used to discard two samples in every block.
class OraclePcmResampler {
 public:
  int convert(const int16_t* input, int frames, int channels, bool from24k,
              int16_t* stereo) {
    int output = 0;
    for (int frame = 0; frame < frames; ++frame) {
      const int i = frame * channels;
      const int16_t value = channels == 1 ? input[i]
        : static_cast<int16_t>((static_cast<int32_t>(input[i]) + input[i + 1]) / 2);
      if (!from24k) { emit(value, stereo, output); continue; }
      if (phase_ == 0) emit(value, stereo, output);
      else if (phase_ == 1) middle_ = value;
      else emit(static_cast<int16_t>((static_cast<int32_t>(middle_) + value) / 2), stereo, output);
      phase_ = (phase_ + 1) % 3;
    }
    return output;
  }
 private:
  int phase_ = 0;
  int16_t middle_ = 0;
  static void emit(int16_t value, int16_t* stereo, int& output) {
    stereo[output * 2] = stereo[output * 2 + 1] = value;
    ++output;
  }
};
