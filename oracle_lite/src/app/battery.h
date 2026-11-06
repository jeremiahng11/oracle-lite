#pragma once

#include <Arduino.h>

struct BatteryReading {
  uint16_t millivolts = 0;
  int percent = -1;
  bool valid = false;
};

void batteryBegin();
BatteryReading readBattery();

