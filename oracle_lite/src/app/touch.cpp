#include "touch.h"
#include "Arduino.h"
#include "../../config.h"
#include "settings.h"
#include "../i2c_bsp/i2c_bsp.h"

void injectButtonTap(bool upper);   // oracle_lite.ino

namespace {
i2c_master_dev_handle_t touchDev = nullptr;
bool present = false;

bool readPoint(uint16_t& x, uint16_t& y, uint8_t& points) {
  uint8_t reg = 0x02, buf[5] = {};
  if (i2c_master_transmit_receive(touchDev, &reg, 1, buf, sizeof(buf), 30) != ESP_OK) return false;
  points = buf[0] & 0x0F;
  x = (static_cast<uint16_t>(buf[1] & 0x0F) << 8) | buf[2];
  y = (static_cast<uint16_t>(buf[3] & 0x0F) << 8) | buf[4];
  return true;
}

void touchTask(void*) {
  bool down = false;
  uint16_t startX = 0, startY = 0;
  uint32_t downAt = 0;
  uint8_t releasedPolls = 0;
  for (;;) {
    uint16_t x, y; uint8_t points = 0;
    const bool touching = readPoint(x, y, points) &&
                          points > 0 && points < 3;
    if (touching) {
      releasedPolls = 0;
      if (!down) { down = true; startX = x; startY = y; downAt = millis(); }
    } else if (down && ++releasedPolls >= 2) {
      down = false;
      const uint32_t held = millis() - downAt;
      // A tap: short touch. Long touches and drags are ignored.
      if (held >= 20 && held <= 700) {
        bool upper = startY < EPD_HEIGHT / 2;
        if (oracleTouchFlipped()) upper = !upper;
        Serial.printf("[Touch] tap (%u,%u) -> %s button\n", startX, startY, upper ? "top" : "bottom");
        injectButtonTap(upper);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
}  // namespace

bool touchPresent() { return present; }

bool touchBegin() {
  i2c_master_bus_handle_t bus = i2c_bsp_bus();
  if (!bus) return false;
  pinMode(TOUCH_RST_PIN, OUTPUT);
  digitalWrite(TOUCH_RST_PIN, LOW);
  delay(10);
  digitalWrite(TOUCH_RST_PIN, HIGH);
  delay(150);   // the FT6336 needs ~100+ ms after reset
  if (i2c_master_probe(bus, TOUCH_I2C_ADDRESS, 50) != ESP_OK) {
    Serial.println("[Touch] no touch chip (button-only board)");
    return false;
  }
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = TOUCH_I2C_ADDRESS;
  cfg.scl_speed_hz = 300000;
  if (i2c_master_bus_add_device(bus, &cfg, &touchDev) != ESP_OK) return false;
  pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
  present = true;
  // Same core as the main loop, so injected taps are ordered with the
  // button interrupts.
  xTaskCreatePinnedToCore(touchTask, "touch", 3072, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);
  Serial.println("[Touch] FT6336 ready: upper half = top button, lower half = bottom button");
  return true;
}
