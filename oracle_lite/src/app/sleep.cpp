#include "Arduino.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "sleep.h"
#include "draw.h"
#include "ui.h"
#include "network.h"
#include "brain.h"
#include "battery.h"
#include "../../sounds.h"
#include "WiFi.h"
#include "SD_MMC.h"
#include "driver/gpio.h"

extern "C" {
#include "../../src/audio/audio_bsp.h"
}

void resetActivity() {
  lastActivityMs = millis();
}

void releasePeripheralPowerHolds() {
  // GPIO holds survive deep sleep. Release them before the normal startup
  // sequence powers the display and codec again.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(EPD_PWR_PIN);
  gpio_hold_dis(Audio_PWR_PIN);
  gpio_hold_dis(VBAT_PWR_PIN);
}

void enterUltraSleep() {
  // Never shut peripherals down while the level-triggered wake key is held.
  // Defer to the main loop instead of sleeping and immediately waking again.
  if (digitalRead(BTN_PWR) == LOW) { resetActivity(); return; }
  delay(50);
  if (digitalRead(BTN_PWR) == LOW) { resetActivity(); return; }
  const esp_err_t wakeResult = esp_sleep_enable_ext1_wakeup(
      (1ULL << BTN_PWR), ESP_EXT1_WAKEUP_ANY_LOW);
  if (wakeResult != ESP_OK) {
    Serial.printf("[Sleep] Wake configuration failed: %s\n", esp_err_to_name(wakeResult));
    resetActivity();
    return;
  }
  // Keep the last e-paper home screen visible. Refreshing a dedicated sleep
  // screen would cost power and hide useful information for no benefit.
  // A small closed lock makes the persistent screen's sleeping state clear.
  showSleepLocked();
  // A press during the panel refresh must still cancel sleep safely.
  if (digitalRead(BTN_PWR) == LOW) {
    resetActivity();
    showIdle(pendingBrainMemoryCount(), readBattery().percent);
    return;
  }
  stopTransferMode();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  audio_playback_set_vol(0);
  oracleSoundSetEnabled(false);
  audio_prepare_sleep();

  // Flush/unmount storage before power domains are reduced. A wake is a full
  // boot, so the card and audio codec will be initialized again in setup().
  SD_MMC.end();

  board.POWEER_Audio_OFF();
  rememberFrameForSleep();   // waits for the panel; lets wake skip the full flash
  board.POWEER_EPD_OFF();

  // Keep the battery latch on for ESP32 EXT1 wake. Hardware power-off is a
  // different mode and requires the board's power-on gesture instead.
  board.VBAT_POWER_ON();

  gpio_hold_en(EPD_PWR_PIN);
  gpio_hold_en(Audio_PWR_PIN);
  gpio_hold_en(VBAT_PWR_PIN);
  gpio_deep_sleep_hold_en();

  // PWR is the board's dedicated hardware wake button. Keeping BOOT out of
  // the wake mask gives the device one unambiguous wake gesture and avoids
  // involving the ESP32-S3 strapping/download pin while the device sleeps.
  Serial.printf("[Sleep] Entering deep sleep; wake=PWR GPIO%d; idle=%lu ms\n",
                BTN_PWR, static_cast<unsigned long>(millis() - lastActivityMs));
  esp_deep_sleep_start();
}
