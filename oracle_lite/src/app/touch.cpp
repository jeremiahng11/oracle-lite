#include "touch.h"
#include <atomic>
#include "Arduino.h"
#include "esp_attr.h"
#include "../../config.h"
#include "settings.h"
#include "../../sounds.h"
#include "../i2c_bsp/i2c_bsp.h"

void injectButtonTap(bool upper);   // oracle_lite.ino

namespace {
i2c_master_dev_handle_t touchDev = nullptr;
bool present = false;
std::atomic<uint8_t> mode{static_cast<uint8_t>(TouchMode::Buttons)};
std::atomic<int> splitY{EPD_HEIGHT / 2};
std::atomic<uint32_t> clickAt{0};   // millis() of an unacknowledged touch, 0 = none
// Survives deep sleep: after the first boot, waking skips the 150 ms chip reset
// (touch board) or the probe altogether (button-only board).
RTC_DATA_ATTR int8_t knownPresent = -1;

// Single producer (touch task), single consumer (main loop).
constexpr uint8_t kQueue = 8;
TouchEvent events[kQueue];
std::atomic<uint8_t> head{0}, tail{0};

void push(TouchEvent e) {
  const uint8_t h = head.load();
  const uint8_t next = (h + 1) % kQueue;
  if (next == tail.load()) return;   // full: drop
  events[h] = e;
  head.store(next);
}

bool readPoint(uint16_t& x, uint16_t& y, uint8_t& points) {
  uint8_t reg = 0x02, buf[5] = {};
  if (i2c_master_transmit_receive(touchDev, &reg, 1, buf, sizeof(buf), 30) != ESP_OK) return false;
  points = buf[0] & 0x0F;
  x = (static_cast<uint16_t>(buf[1] & 0x0F) << 8) | buf[2];
  y = (static_cast<uint16_t>(buf[3] & 0x0F) << 8) | buf[4];
  return true;
}

void touchTask(void*) {
  bool down = false, ignore = false;
  int startX = 0, startY = 0, lastX = 0, lastY = 0;
  uint32_t downAt = 0;
  uint8_t releasedPolls = 0;
  for (;;) {
    uint16_t rx, ry; uint8_t points = 0;
    const bool read = readPoint(rx, ry, points);
    const bool touching = read && points > 0;
    if (touching) {
      releasedPolls = 0;
      int x = rx, y = ry;
      if (oracleTouchFlipped()) { x = EPD_WIDTH - 1 - x; y = EPD_HEIGHT - 1 - y; }
      if (!down) { down = true; ignore = false; startX = x; startY = y; downAt = millis(); }
      if (points > 1) ignore = true;   // two fingers, a palm: not a gesture
      lastX = x; lastY = y;
    } else if (down && ++releasedPolls >= 2) {
      down = false;
      const uint32_t held = millis() - downAt;
      const int dx = lastX - startX, dy = lastY - startY;
      if (ignore || held < 20 || held > 1200) { vTaskDelay(pdMS_TO_TICKS(15)); continue; }
      const bool swipe = abs(dx) >= 50 || abs(dy) >= 50;
      if (static_cast<TouchMode>(mode.load()) == TouchMode::Pages) {
        TouchEvent e = TouchEvent::None;
        if (swipe) {
          if (abs(dy) > abs(dx)) e = dy > 0 ? TouchEvent::Home : TouchEvent::None;
          else e = dx < 0 ? TouchEvent::Next : TouchEvent::Previous;
        } else if (held <= 700) {
          e = startX < EPD_WIDTH / 2 ? TouchEvent::Previous : TouchEvent::Next;
        }
        if (e != TouchEvent::None) {
          Serial.printf("[Touch] %s (%d,%d)\n", e == TouchEvent::Next ? "next" : e == TouchEvent::Previous ? "previous" : "home",
                        startX, startY);
          clickAt = millis() | 1;
          push(e);
        }
      } else if (!swipe && held <= 700) {
        const bool upper = startY < splitY.load();
        Serial.printf("[Touch] tap (%d,%d) -> %s button\n", startX, startY, upper ? "top" : "bottom");
        clickAt = millis() | 1;
        injectButtonTap(upper);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
}  // namespace

bool touchPresent() { return present; }

void touchSetMode(TouchMode m, int split) {
  if (split < 0) split = EPD_HEIGHT / 2;
  if (static_cast<uint8_t>(m) != mode.load()) touchDiscardEvents();
  mode = static_cast<uint8_t>(m);
  splitY = split;
}

TouchEvent touchTakeEvent() {
  const uint8_t t = tail.load();
  if (t == head.load()) return TouchEvent::None;
  const TouchEvent e = events[t];
  tail.store((t + 1) % kQueue);
  return e;
}

void touchDiscardEvents() { tail.store(head.load()); }

void touchServiceClick(bool audioBusy) {
  const uint32_t at = clickAt.exchange(0);
  // A click only makes sense right after the touch; drop late ones.
  if (!at || audioBusy || millis() - at > 300UL) return;
  soundNext();
}

bool touchBegin() {
  i2c_master_bus_handle_t bus = i2c_bsp_bus();
  if (!bus) return false;
  if (knownPresent == 0) return false;   // button-only board, found at first boot
  bool found = knownPresent == 1 && i2c_master_probe(bus, TOUCH_I2C_ADDRESS, 50) == ESP_OK;
  if (!found) {
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(150);   // the FT6336 needs ~100+ ms after reset
    found = i2c_master_probe(bus, TOUCH_I2C_ADDRESS, 50) == ESP_OK;
  }
  knownPresent = found ? 1 : 0;
  if (!found) {
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
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);
  Serial.println("[Touch] FT6336 ready");
  return true;
}
