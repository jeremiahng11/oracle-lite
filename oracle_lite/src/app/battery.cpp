#include "battery.h"

#include "../../config.h"
#include <algorithm>

namespace {

constexpr size_t kSampleCount = 21;
constexpr float kDividerRatio = 2.0f;  // Waveshare: 200K / 200K.
constexpr float kBoardCalibration = 1.0f;

struct CurvePoint { uint16_t millivolts; uint8_t percent; };

// Resting-voltage approximation for a single-cell Li-ion/LiPo battery. The
// piecewise curve avoids the large errors caused by a linear 3.4-4.2 V map.
constexpr CurvePoint kCurve[] = {
  {3400, 0}, {3500, 5}, {3600, 12}, {3680, 22}, {3740, 35}, {3790, 50},
  {3850, 62}, {3920, 74}, {4000, 84}, {4080, 92}, {4160, 98}, {4200, 100}
};

float filteredMillivolts = 0.0f;

int percentFromVoltage(uint16_t millivolts) {
  if (millivolts <= kCurve[0].millivolts) return 0;
  const size_t last = sizeof(kCurve) / sizeof(kCurve[0]) - 1;
  if (millivolts >= kCurve[last].millivolts) return 100;
  for (size_t i = 1; i <= last; i++) {
    if (millivolts > kCurve[i].millivolts) continue;
    const CurvePoint& low = kCurve[i - 1];
    const CurvePoint& high = kCurve[i];
    return low.percent +
      ((millivolts - low.millivolts) * (high.percent - low.percent)) /
      (high.millivolts - low.millivolts);
  }
  return 0;
}

}  // namespace

void batteryBegin() {
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
}

BatteryReading readBattery() {
  uint16_t samples[kSampleCount];
  for (size_t i = 0; i < kSampleCount; i++) {
    samples[i] = static_cast<uint16_t>(analogReadMilliVolts(BAT_ADC_PIN));
    delay(3);
  }
  std::sort(samples, samples + kSampleCount);
  const float measured = samples[kSampleCount / 2] * kDividerRatio * kBoardCalibration;
  if (measured < 2800.0f || measured > 4500.0f) return {};

  // Strongly suppress short voltage dips from Wi-Fi/audio while still allowing
  // the estimate to follow real discharge across normal interactions.
  if (filteredMillivolts <= 0.0f) filteredMillivolts = measured;
  else filteredMillivolts = filteredMillivolts * 0.82f + measured * 0.18f;

  BatteryReading reading;
  reading.millivolts = static_cast<uint16_t>(filteredMillivolts + 0.5f);
  reading.percent = percentFromVoltage(reading.millivolts);
  reading.valid = true;
  return reading;
}

