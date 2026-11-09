#include <Arduino.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_partition.h>
#include <vector>
#include <algorithm>

extern "C" {
#include "config.h"
#include "src/i2c_bsp/i2c_bsp.h"
#include "src/audio/audio_bsp.h"
}

#include "types.h"
#include "globals.h"
#include "sounds.h"
#include "src/power/board_power_bsp.h"
#include "src/display/epaper_driver_bsp.h"
#include "src/app/brain.h"
#include "src/app/battery.h"
#include "src/app/network.h"
#include "src/app/notes.h"
#include "src/app/record.h"
#include "src/app/minutes.h"
bool restoreFrameAfterWake();   // draw.cpp
#include "esp_heap_caps.h"
#include "esp_wifi.h"
void waitForDisplay();   // draw.cpp

// TLS handshakes and server replies run on the Arduino loop task. The default
// 8 KB stack leaves little margin for mbedTLS; 16 KB is comfortable.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#include "src/app/rtc.h"
#include "src/app/settings.h"
#include "src/app/sleep.h"
#include "src/app/tasks.h"
#include "src/app/reminders.h"
#include <ESPmDNS.h>
#include "src/app/sd_read.h"
#include "src/app/ui.h"

board_power_bsp_t board(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
epaper_driver_display* display = nullptr;
std::vector<NoteEntry> noteIndex;

AppState state = STATE_IDLE;
int lastRecNum = -1;
uint32_t lastActivityMs = 0;

bool transferServerActive = false;
String transferUrl;

bool timeReady = false;
bool audioPlaying = false;
bool stopPlayback = false;

void resetActivity();

enum class PressResult { Released, LongSingle, Chord };

namespace {

bool companionExitArmed = false;
bool idleInputArmed = true;
uint32_t companionGestureStartedAt = 0;
uint32_t nextBackgroundAttemptAt = 0;
bool backlogSyncActive = false;
int backlogSyncTotal = 0;
uint32_t nextHomeClockCheckAt = 0;
uint32_t lastUpperTapAt = 0;
uint32_t lastLowerTapAt = 0;
volatile bool upperPressLatched = false;
volatile bool lowerPressLatched = false;
// Press edges counted by the interrupt, so taps made while the loop is busy
// (e-paper refresh, a background network check) still count as multi-taps.
volatile uint8_t upperTapEdges = 0, lowerTapEdges = 0;
volatile uint32_t upperTapEdgeAt = 0, lowerTapEdgeAt = 0;
bool companionUpperWasDown = false;
bool companionLowerWasDown = false;
uint32_t companionVolumeShownAt = 0;
uint32_t companionSingleStartedAt = 0;
int companionSinglePin = -1;
String companionApPassword;
String displayedHomeDay;
bool agendaShowsEvents = false;
uint32_t agendaShownAt = 0;
int agendaPage = 0;
bool setupRequired = false;
uint32_t setupCompanionHoldStartedAt = 0;
int processPendingMemories(int preferredNote, int maxNotes);

String usbSetupLine;

struct FlashSetupHeader {
  char magic[8];
  uint32_t jsonLength;
  uint32_t checksum;
};

uint32_t setupChecksum(const uint8_t* data, size_t length) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < length; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool importFlashSetup() {
  const esp_partition_t* partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "oraclecfg");
  if (!partition) return false;

  FlashSetupHeader header{};
  if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK ||
      memcmp(header.magic, "ORACFG01", 8) != 0 ||
      header.jsonLength == 0 || header.jsonLength > 700) return false;

  std::vector<uint8_t> bytes(header.jsonLength);
  if (esp_partition_read(partition, sizeof(header), bytes.data(), bytes.size()) != ESP_OK ||
      setupChecksum(bytes.data(), bytes.size()) != header.checksum) {
    std::fill(bytes.begin(), bytes.end(), 0);
    esp_partition_erase_range(partition, 0, partition->size);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, bytes.data(), bytes.size())) {
    std::fill(bytes.begin(), bytes.end(), 0);
    esp_partition_erase_range(partition, 0, partition->size);
    return false;
  }
  const String ssid = doc["ssid"] | "";
  const String password = doc["password"] | "";
  const String apiKey = doc["apiKey"] | "";
  const String voice = doc["voice"] | "cedar";
  const String language = doc["language"] | "en";
  const bool valid = ssid.length() && ssid.length() <= 32 && password.length() <= 63 &&
      apiKey.length() >= 11 && apiKey.length() <= 200 &&
      (voice == "cedar" || voice == "coral") &&
      (language == "de" || language == "en" || language == "fr" ||
       language == "es" || language == "it" || language == "pl" || language == "ru" ||
       language == "cs" || language == "uk" || language == "nl" || language == "pt");
       
  std::fill(bytes.begin(), bytes.end(), 0);
  if (!valid) {
    esp_partition_erase_range(partition, 0, partition->size);
    return false;
  }

  const bool saved = oracleSaveSettings(ssid, password, apiKey, voice) &&
                     oracleSetUiLanguage(language);
  // The temporary transfer area contains credentials. Remove it after the
  // single import attempt even if Preferences rejects the write, so a raw API
  // key is never left indefinitely in the staging partition.
  esp_partition_erase_range(partition, 0, partition->size);
  if (saved) {
    Serial.println("[Setup] One-step configuration imported");
  }
  return saved;
}

void replyUsbSetup(const char* status, const char* detail = nullptr) {
  Serial.print("ORACLE_SETUP_");
  Serial.print(status);
  if (detail) { Serial.print(':'); Serial.print(detail); }
  Serial.println();
}

void applyUsbSetupLine(const String& line) {
  if (!line.startsWith("ORACLE_SETUP_V1 ")) return;
  if (line.length() > 700) { replyUsbSetup("ERROR", "TOO_LONG"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, line.substring(strlen("ORACLE_SETUP_V1 ")))) {
    replyUsbSetup("ERROR", "INVALID_JSON");
    return;
  }
  const String ssid = doc["ssid"] | "";
  const String password = doc["password"] | "";
  const String apiKey = doc["apiKey"] | "";
  const String voice = doc["voice"] | "cedar";
  const String language = doc["language"] | "en";
  if (!ssid.length() || ssid.length() > 32 || password.length() > 63 ||
      apiKey.length() < 11 || apiKey.length() > 200 ||
      (voice != "cedar" && voice != "coral") ||
      (language != "de" && language != "en" && language != "fr" &&
       language != "es" && language != "it" && language != "pl" && language != "ru" &&
       language != "cs" && language != "uk" && language != "nl" && language != "pt")) {
    replyUsbSetup("ERROR", "INVALID_VALUES");
    return;
  }
  if (!oracleSaveSettings(ssid, password, apiKey, voice) || !oracleSetUiLanguage(language)) {
    replyUsbSetup("ERROR", "SAVE_FAILED");
    return;
  }
  replyUsbSetup("OK");
  Serial.flush();
  // Give WebSerial enough time to receive the acknowledgement before USB is
  // torn down by the restart. A short delay was unreliable on macOS/Chrome.
  delay(1500);
  ESP.restart();
}

void handleUsbSetup() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      usbSetupLine.trim();
      applyUsbSetupLine(usbSetupLine);
      usbSetupLine = "";
    } else if (c != '\r') {
      if (usbSetupLine.length() < 720) usbSetupLine += c;
      else usbSetupLine = "";
    }
  }
}

bool bothButtonsDown() {
  return digitalRead(BTN_REC) == LOW && digitalRead(BTN_PWR) == LOW;
}

bool bothButtonsUp() {
  return digitalRead(BTN_REC) == HIGH && digitalRead(BTN_PWR) == HIGH;
}

// Button interrupts (both edges). A press only counts as a new tap when the
// button had been released for a stable 30 ms, so contact bounce on press or
// release never turns one tap into two.
volatile uint32_t upperReleasedAt = 0, lowerReleasedAt = 0;

static inline void IRAM_ATTR buttonEdge(int pin, volatile bool& latched, volatile uint8_t& edges,
                                        volatile uint32_t& edgeAt, volatile uint32_t& releasedAt) {
  const bool low = ((REG_READ(GPIO_IN_REG) >> pin) & 1U) == 0;   // both buttons are GPIO < 32
  const uint32_t now = millis();
  if (!low) { releasedAt = now; return; }
  latched = true;
  if (now - releasedAt >= 30 && now - edgeAt >= 60) {
    if (edges < 9) edges++;
    edgeAt = now;
  }
}

void IRAM_ATTR latchUpperPress() {
  buttonEdge(BTN_REC, upperPressLatched, upperTapEdges, upperTapEdgeAt, upperReleasedAt);
}
void IRAM_ATTR latchLowerPress() {
  buttonEdge(BTN_PWR, lowerPressLatched, lowerTapEdges, lowerTapEdgeAt, lowerReleasedAt);
}

void clearLatchedPresses() {
  noInterrupts();
  upperPressLatched = false;
  lowerPressLatched = false;
  upperTapEdges = 0;
  lowerTapEdges = 0;
  interrupts();
}

bool takeLatchedPress(bool upper) {
  noInterrupts();
  const bool latched = upper ? upperPressLatched : lowerPressLatched;
  if (upper) upperPressLatched = false;
  else lowerPressLatched = false;
  interrupts();
  return latched;
}

void powerOnPeripherals() {
  pinMode(PWR_HOLD_PIN, OUTPUT);
  digitalWrite(PWR_HOLD_PIN, HIGH);
  board.VBAT_POWER_ON();
  board.POWEER_EPD_ON();
  board.POWEER_Audio_ON();
  delay(200);
}

// ─── Memory and power ──────────────────────────────────────────────────────
void logMemory(const char* where) {
  Serial.printf("[Mem] %s: internal free=%u KB (largest %u KB, lowest ever %u KB), "
                "PSRAM free=%u KB, loop stack left=%u B, CPU=%u MHz\n",
                where,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned>(getCpuFrequencyMhz()));
}

// Full speed (240 MHz) for any work; 80 MHz while resting on Home with the
// radio and audio off. The panel, SD card and timers keep their clocks.
static bool cpuResting = false;
void cpuFullSpeed() {
  if (!cpuResting) return;
  setCpuFrequencyMhz(240);
  cpuResting = false;
}
void cpuRest() {
  if (cpuResting) return;
  setCpuFrequencyMhz(80);
  cpuResting = true;
}

// ─── Fast Wi-Fi ────────────────────────────────────────────────────────────
// The access point of the last good connection (survives deep sleep). Joining
// with its BSSID and channel skips the all-channel scan WiFi.begin() would do.
RTC_DATA_ATTR static char fastSsid[33] = "";
RTC_DATA_ATTR static uint8_t fastBssid[6] = {0};
RTC_DATA_ATTR static int32_t fastChannel = 0;
static uint32_t earlyWifiStartedAt = 0;

void rememberFastWifi() {
  const uint8_t* bssid = WiFi.BSSID();
  if (!bssid) return;
  strlcpy(fastSsid, WiFi.SSID().c_str(), sizeof(fastSsid));
  memcpy(fastBssid, bssid, sizeof(fastBssid));
  fastChannel = WiFi.channel();
}

void beginWifiTo(const OracleWifiProfile& profile) {
  applyWifiRegion();
  if (fastChannel > 0 && profile.ssid == fastSsid)
    WiFi.begin(profile.ssid.c_str(), profile.password.c_str(), fastChannel, fastBssid);
  else
    WiFi.begin(profile.ssid.c_str(), profile.password.c_str());
}

OracleWifiProfile quickWifiProfile() {
  const String lastSsid = oracleLastWifiSsid();
  for (int profile = 0; profile < oracleWifiProfileCount(); profile++) {
    if (oracleWifiProfile(profile).ssid == lastSsid) return oracleWifiProfile(profile);
  }
  return oracleWifiProfileCount() > 0 ? oracleWifiProfile(0) : OracleWifiProfile{};
}

// Starts joining the last network without waiting (e.g. while the user is
// still speaking), so the connection is ready when the recording ends.
void beginWifiEarly() {
  cpuFullSpeed();
  if (!oracleHasWifi() || WiFi.status() == WL_CONNECTED) return;
  const OracleWifiProfile quick = quickWifiProfile();
  if (!quick.ssid.length()) return;
  WiFi.mode(WIFI_STA);
  beginWifiTo(quick);
  earlyWifiStartedAt = millis();
}

// ─── Wi-Fi on wake (non-blocking) ──────────────────────────────────────────
// 1) join the last access point directly, 2) otherwise an asynchronous scan
// picks the strongest saved network, 3) join it. The loop keeps running, so
// buttons respond immediately; the Home Wi-Fi icon updates when the result is
// known (only the icon's state change redraws Home).
enum WakeWifiStage : uint8_t { WAKE_WIFI_OFF, WAKE_WIFI_FAST, WAKE_WIFI_SCAN, WAKE_WIFI_JOIN, WAKE_WIFI_TRY_ALL };
static WakeWifiStage wakeWifiStage = WAKE_WIFI_OFF;
static uint32_t wakeWifiStageAt = 0;
static uint8_t wakeScanAttempts = 0;
static int wakeTryProfile = 0;

void finishWakeWifi(bool connected);

void startWakeWifi() {
  if (!oracleHasWifi()) { setWifiIndicator(false); return; }
  WiFi.setAutoReconnect(false);   // the driver must not keep chasing an absent network
  beginWifiEarly();
  wakeWifiStage = WAKE_WIFI_FAST;
  wakeWifiStageAt = millis();
  wakeScanAttempts = 0;
  wakeTryProfile = 0;
}

void cancelWakeWifi() {
  if (wakeWifiStage == WAKE_WIFI_SCAN) WiFi.scanDelete();
  wakeWifiStage = WAKE_WIFI_OFF;
}

// A scan fails while the station is still trying to join: stop that attempt
// and restart the station first (also what the blocking connect path does).
void startWakeScan() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(30);
  WiFi.mode(WIFI_STA);
  delay(60);
  applyWifiRegion();
  WiFi.scanNetworks(true, true);   // asynchronous, includes hidden networks
  wakeScanAttempts++;
  wakeWifiStage = WAKE_WIFI_SCAN;
  wakeWifiStageAt = millis();
}

void joinWakeProfile(const OracleWifiProfile& profile, uint8_t nextStage) {
  const WakeWifiStage stage = static_cast<WakeWifiStage>(nextStage);
  WiFi.disconnect(false);
  delay(20);
  WiFi.begin(profile.ssid.c_str(), profile.password.c_str());
  earlyWifiStartedAt = millis();
  wakeWifiStage = stage;
  wakeWifiStageAt = millis();
}

void serviceWakeWifi() {
  if (wakeWifiStage == WAKE_WIFI_OFF) return;
  if (WiFi.status() == WL_CONNECTED) { finishWakeWifi(true); return; }
  const uint32_t elapsed = millis() - wakeWifiStageAt;
  switch (wakeWifiStage) {
    case WAKE_WIFI_FAST:
      // Same place as before sleep: usually joined within ~1 s.
      if (elapsed < 3000UL) return;
      fastChannel = 0;   // the remembered access point is not here
      earlyWifiStartedAt = 0;
      startWakeScan();
      return;
    case WAKE_WIFI_SCAN: {
      const int found = WiFi.scanComplete();
      if (found == WIFI_SCAN_RUNNING) {
        if (elapsed > 8000UL) { WiFi.scanDelete(); wakeTryProfile = 0; wakeWifiStage = WAKE_WIFI_TRY_ALL; wakeWifiStageAt = 0; }
        return;
      }
      if (found < 0) {   // scan failed: try once more, then every saved network directly
        WiFi.scanDelete();
        if (wakeScanAttempts < 2) { startWakeScan(); return; }
        wakeTryProfile = 0;
        wakeWifiStage = WAKE_WIFI_TRY_ALL;
        wakeWifiStageAt = 0;
        return;
      }
      int best = -1, bestRssi = -1000;
      String bestAirName;
      for (int network = 0; network < found; network++) {
        Serial.printf("[WiFi]   seen \"%s\" ch%d %d dBm\n", WiFi.SSID(network).c_str(),
                      static_cast<int>(WiFi.channel(network)), static_cast<int>(WiFi.RSSI(network)));
        for (int profile = 0; profile < oracleWifiProfileCount(); profile++) {
          const String saved = oracleWifiProfile(profile).ssid;
          if ((saved == WiFi.SSID(network) || oracleSsidSimilar(saved, WiFi.SSID(network))) &&
              WiFi.RSSI(network) > bestRssi) {
            bestRssi = WiFi.RSSI(network);
            best = profile;
            bestAirName = WiFi.SSID(network);
          }
        }
      }
      for (int profile = 0; profile < oracleWifiProfileCount(); profile++)
        Serial.printf("[WiFi]   saved \"%s\"\n", oracleWifiProfile(profile).ssid.c_str());
      Serial.printf("[WiFi] wake scan: %d networks, best saved=%d (%d dBm)\n", found, best, bestRssi);
      WiFi.scanDelete();
      // Not in the scan (hidden network, or missed on a busy channel): still
      // try every saved network directly instead of giving up.
      if (best < 0) { wakeTryProfile = 0; wakeWifiStage = WAKE_WIFI_TRY_ALL; wakeWifiStageAt = 0; return; }
      OracleWifiProfile chosen = oracleWifiProfile(best);
      if (bestAirName.length() && bestAirName != chosen.ssid) {   // hand-typed name: use the real one
        oracleUpdateWifiProfile(chosen.ssid, bestAirName, "");
        Serial.printf("[WiFi] saved name corrected: \"%s\" -> \"%s\"\n", chosen.ssid.c_str(), bestAirName.c_str());
        chosen.ssid = bestAirName;
      }
      joinWakeProfile(chosen, WAKE_WIFI_JOIN);
      return;
    }
    case WAKE_WIFI_JOIN:
      if (elapsed < 7000UL) return;
      // Seen but not joined (e.g. weak or busy): try each saved network in turn.
      wakeTryProfile = 0;
      wakeWifiStage = WAKE_WIFI_TRY_ALL;
      wakeWifiStageAt = 0;
      return;
    case WAKE_WIFI_TRY_ALL:
      if (wakeWifiStageAt && elapsed < 6000UL) return;
      if (wakeTryProfile >= oracleWifiProfileCount()) { finishWakeWifi(false); return; }
      Serial.printf("[WiFi] wake: trying saved network %d/%d\n", wakeTryProfile + 1, oracleWifiProfileCount());
      joinWakeProfile(oracleWifiProfile(wakeTryProfile++), WAKE_WIFI_TRY_ALL);
      return;
    default:
      return;
  }
}

// Ends network work. While the device is awake Wi-Fi is a key feature, so a
// working connection is kept (the next request starts instantly); only a
// failed or half-made link is switched off. Deep sleep turns the radio off.
void stopWifi() {
  cancelWakeWifi();
  openAiDropConnection();
  earlyWifiStartedAt = 0;
  if (WiFi.status() == WL_CONNECTED && WiFi.getMode() == WIFI_STA) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Live Wi-Fi state for the Home icon, and full radio speed whenever connected.
// The ESP32 starts in the "world safe" region (channels 1-11 only). Routers in
// Malaysia/Singapore often use channels 12-13, which were then never found.
// Allow 1-13 (as MY/SG do); the region an AP advertises is adopted after joining.
void applyWifiRegion() { oracleApplyWifiRegion(); }   // network.cpp

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_START || event == ARDUINO_EVENT_WIFI_AP_START) {
    applyWifiRegion();
    return;
  }
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    WiFi.setSleep(false);
    setWifiIndicator(true);
    Serial.printf("[WiFi] connected to \"%s\" (%d dBm)\n", WiFi.SSID().c_str(), static_cast<int>(WiFi.RSSI()));
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    setWifiIndicator(false);
    // Why it failed: e.g. AUTH_FAIL / 4WAY_HANDSHAKE_TIMEOUT = wrong password,
    // NO_AP_FOUND = not in range (or a 5 GHz-only network).
    const uint8_t reason = info.wifi_sta_disconnected.reason;
    Serial.printf("[WiFi] not connected: reason %u (%s)\n", reason,
                  WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)));
  } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
    setWifiIndicator(false);
  }
}

String localDayKey() {
  String local = utcToLocalDeviceLabel(currentUtcIso());
  return local.length() >= 10 && local != "time not set" ? local.substring(0, 10) : "";
}

void renderReadyHome() {
  const BatteryReading battery = readBattery();
  // The home screen only exposes recordings that are not transcribed yet.
  // Rebuilding structure/vector indexes is silent maintenance: the transcript
  // is already searchable and must not leave a permanent "waiting" badge.
  int untranscribed = 0;
  for (const NoteEntry& note : noteIndex) {
    if (!note.hasText && noteQueueState(note.num) != "blocked") untranscribed++;
  }
  showIdle(untranscribed, battery.valid ? battery.percent : -1);
  displayedHomeDay = localDayKey();
  nextHomeClockCheckAt = millis() + 60000UL;
}

void returnFromBackgroundSync() {
  renderReadyHome();
  // Presses during network/display work must not become agenda shortcuts.
  clearLatchedPresses();
  idleInputArmed = false;
  lastUpperTapAt = 0;
  lastLowerTapAt = 0;
}

// Appointment reminder: chime, show it, and speak it once Wi-Fi is up.
// After a timer wake with nobody around, Oracle goes back to sleep soon.
bool wokeForReminder = false;

void runReminder() {
  const String message = reminderTakeMessage();
  if (!message.length()) return;
  Serial.printf("[Reminder] %s\n", message.c_str());
  state = STATE_ASKING;
  showAskSpeaking(message, 1);
  soundListening();
  // A timer wake starts Wi-Fi in the background; give it a moment.
  const uint32_t waitFrom = millis();
  while (oracleHasWifi() && WiFi.status() != WL_CONNECTED && millis() - waitFrom < 10000UL &&
         bothButtonsUp()) {
    serviceWakeWifi();
    delay(20);
  }
  if (WiFi.status() == WL_CONNECTED && bothButtonsUp())
    playBrainSpeech(message, NOTES_DIR "/.oracle-reminder.wav", 1, true);
  // Leave it on screen for a while (any button returns to Home).
  clearLatchedPresses();
  const uint32_t shownAt = millis();
  while (millis() - shownAt < 20000UL && !upperPressLatched && !lowerPressLatched && bothButtonsUp()) delay(20);
  returnToReady();
  if (wokeForReminder) {
    // Nobody pressed anything: sleep again ~20 s after the reminder.
    lastActivityMs = millis() - (AUTO_SLEEP_IDLE_MS - 20000UL);
    wokeForReminder = false;
  }
}

void returnToReady() {
  stopWifi();
  state = STATE_IDLE;
  idleInputArmed = false;
  resetActivity();
  // Discard the edge that selected Back, then retain any new edge occurring
  // while the e-paper is busy redrawing Home.
  clearLatchedPresses();
  renderReadyHome();
  audio_idle_suspend();
}

// Answer pages: upper = close, lower = next page. With allowFollowUp, holding
// the lower button returns true: the caller asks a follow-up question.
bool browseAnswerPages(const String& answer, int sourceCount, bool allowFollowUp = false,
                       const String& question = String()) {
  // The REC press that stopped speech belongs to playback, not navigation.
  // The caller has already displayed page one. Avoid another panel refresh.
  bool upperArmed = digitalRead(BTN_REC) == HIGH;
  // With a question, it is one more page after the answer's last page.
  const int answerPages = askAnswerPageCount(answer);
  const int pages = answerPages + (question.length() ? 1 : 0);
  auto showPage = [&](int p) {
    if (p < answerPages) showAskSpeakingPage(answer, sourceCount, p);
    else showAskQuestion(question);
  };
  int page = 0;
  bool lowerWasDown = digitalRead(BTN_PWR) == LOW;
  bool lowerArmed = !lowerWasDown;   // a press still held from before is not a new one
  uint32_t lowerDownAt = 0;
  uint32_t closesAt = millis() + 15000UL;
  while (static_cast<int32_t>(millis() - closesAt) < 0) {
    if (digitalRead(BTN_REC) == HIGH) upperArmed = true;
    else if (upperArmed) {
      break;
    }
    const bool lowerDown = digitalRead(BTN_PWR) == LOW;
    if (!allowFollowUp) {
      if (lowerDown && !lowerWasDown) {
        page = (page + 1) % pages;
        showPage(page);
        closesAt = millis() + 15000UL;
      }
    } else {
      // Short press (on release) = next page; hold = ask the next question.
      if (!lowerDown) {
        if (lowerWasDown && lowerArmed && lowerDownAt && millis() - lowerDownAt < BTN_LONG_MS) {
          page = (page + 1) % pages;
          showPage(page);
          closesAt = millis() + 15000UL;
        }
        lowerArmed = true;
        lowerDownAt = 0;
      } else if (lowerArmed) {
        if (!lowerWasDown) lowerDownAt = millis();
        else if (lowerDownAt && millis() - lowerDownAt >= BTN_LONG_MS) return true;
      }
    }
    lowerWasDown = lowerDown;
    delay(10);
  }
  return false;
}

bool connectConfiguredWifiOnce(uint8_t attempts);

// Every connection attempt updates the Home screen Wi-Fi icon.
bool connectConfiguredWifi(uint8_t attempts = 20) {
  cpuFullSpeed();   // TLS handshakes are ~3x slower at 80 MHz
  const bool connected = connectConfiguredWifiOnce(attempts);
  setWifiIndicator(connected);
  if (connected) {
    // Modem power-save delays every packet by up to a beacon interval; while
    // the radio is on for real work, full speed matters more. It is switched
    // off completely afterwards anyway.
    WiFi.setSleep(false);
    rememberFastWifi();
  }
  return connected;
}

void finishWakeWifi(bool connected) {
  wakeWifiStage = WAKE_WIFI_OFF;
  earlyWifiStartedAt = 0;
  WiFi.setAutoReconnect(true);
  if (connected) {
    oracleSetLastWifiSsid(WiFi.SSID());
    rememberFastWifi();
    Serial.printf("[WiFi] ready after wake: %s\n", WiFi.SSID().c_str());
  } else {
    Serial.println("[WiFi] no saved network after wake");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  setWifiIndicator(connected);   // Home redraws if its icon is now out of date
}

bool connectConfiguredWifiOnce(uint8_t attempts) {
  if (!oracleHasWifi()) return false;
  // An action needs Wi-Fi now: take over from the wake-up join (an early join
  // in progress is still awaited below rather than restarted).
  if (wakeWifiStage == WAKE_WIFI_SCAN) { cancelWakeWifi(); }
  else wakeWifiStage = WAKE_WIFI_OFF;
  WiFi.setAutoReconnect(true);
  // Background processing may already have a healthy association. Calling
  // WiFi.begin again tears it down and adds an avoidable 1-3 second reconnect.
  if (WiFi.status() == WL_CONNECTED) {
    if (time(nullptr) < 1700000000) syncTimeFromNTP(4000);
    return true;
  }
  // An early join (started while the user was speaking) may still be in
  // progress: give it its remaining window instead of restarting it.
  if (earlyWifiStartedAt) {
    const uint32_t earlyDeadline = earlyWifiStartedAt + 5000UL;
    while (WiFi.status() != WL_CONNECTED && static_cast<int32_t>(earlyDeadline - millis()) > 0) delay(20);
    earlyWifiStartedAt = 0;
    if (WiFi.status() == WL_CONNECTED) {
      oracleSetLastWifiSsid(WiFi.SSID());
      if (time(nullptr) < 1700000000) syncTimeFromNTP(4000);
      Serial.println("[WiFi] early join ready");
      return true;
    }
    fastChannel = 0;   // the remembered access point did not answer
  }
  WiFi.mode(WIFI_STA);

  // Reconnect to the last working network first. A full radio scan is only a
  // fallback for roaming between saved locations and hotspots.
  const OracleWifiProfile quick = quickWifiProfile();
  if (quick.ssid.length()) {
    // Association commonly needs a little over one second. Giving the known
    // network a short direct window is much faster than falling into a full
    // channel scan on every wake. Poll often so a ready link is used at once.
    const uint32_t quickWindowMs = (attempts < 10 ? attempts : 10) * 200UL;
    WiFi.mode(WIFI_STA);
    delay(20);
    beginWifiTo(quick);
    const uint32_t quickStarted = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - quickStarted < quickWindowMs) delay(20);
    if (WiFi.status() != WL_CONNECTED && fastChannel > 0 && quick.ssid == fastSsid) {
      // The remembered channel/BSSID may be stale (router changed channel).
      fastChannel = 0;
      WiFi.disconnect(false);
      delay(20);
      WiFi.begin(quick.ssid.c_str(), quick.password.c_str());
      const uint32_t retryStarted = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - retryStarted < quickWindowMs) delay(20);
    }
    if (WiFi.status() == WL_CONNECTED) {
      oracleSetLastWifiSsid(quick.ssid);
      if (time(nullptr) < 1700000000) syncTimeFromNTP(4000);
      Serial.printf("[WiFi] quick reconnect: %s\n", quick.ssid.c_str());
      return true;
    }
    WiFi.disconnect(false);
    WiFi.mode(WIFI_STA);
    delay(80);
  }

  // Rank every saved network seen by the scan, then try all profiles. A scan
  // can occasionally miss an AP while its channel is busy; unseen profiles
  // remain valid fallbacks instead of silently falling back to profile zero.
  int profileRssi[ORACLE_MAX_WIFI_PROFILES];
  bool tried[ORACLE_MAX_WIFI_PROFILES];
  for (int profile = 0; profile < ORACLE_MAX_WIFI_PROFILES; profile++) {
    profileRssi[profile] = -2000;
    // A short direct attempt is only an optimization, not a final verdict.
    // Retry that profile after the scan as well; association can legitimately
    // take longer than the quick window.
    tried[profile] = false;
  }
  // Restart the station first: a scan fails while it is still trying to join
  // the previous location's network. Retry once if the radio was busy.
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(30);
  WiFi.mode(WIFI_STA);
  delay(60);
  applyWifiRegion();
  int visible = WiFi.scanNetworks(false, true);
  if (visible < 0) {
    WiFi.scanDelete();
    delay(200);
    visible = WiFi.scanNetworks(false, true);
  }
  Serial.printf("[WiFi] scan found %d networks; %d profiles saved\n",
                visible, oracleWifiProfileCount());
  String profileAirName[ORACLE_MAX_WIFI_PROFILES];   // the name as broadcast
  for (int network = 0; network < visible; network++) {
    const String visibleSsid = WiFi.SSID(network);
    Serial.printf("[WiFi]   seen \"%s\" ch%d %d dBm\n", visibleSsid.c_str(),
                  static_cast<int>(WiFi.channel(network)), static_cast<int>(WiFi.RSSI(network)));
    for (int profile = 0; profile < oracleWifiProfileCount(); profile++) {
      const OracleWifiProfile saved = oracleWifiProfile(profile);
      if ((saved.ssid == visibleSsid || oracleSsidSimilar(saved.ssid, visibleSsid)) &&
          WiFi.RSSI(network) > profileRssi[profile]) {
        profileRssi[profile] = WiFi.RSSI(network);
        profileAirName[profile] = visibleSsid;
      }
    }
  }
  WiFi.scanDelete();

  // Networks the scan missed still get ~3 s: joining a new place needs DHCP time.
  const uint8_t perProfileAttempts = attempts < 10 ? 10 : (attempts < 14 ? attempts : 14);
  for (int pass = 0; pass < oracleWifiProfileCount(); pass++) {
    int selectedProfile = -1;
    int strongestRssi = -3000;
    for (int profile = 0; profile < oracleWifiProfileCount(); profile++) {
      if (!tried[profile] && profileRssi[profile] > strongestRssi) {
        strongestRssi = profileRssi[profile];
        selectedProfile = profile;
      }
    }
    if (selectedProfile < 0) break;
    tried[selectedProfile] = true;
    OracleWifiProfile selected = oracleWifiProfile(selectedProfile);
    if (!selected.ssid.length()) continue;
    const String savedName = selected.ssid;
    if (profileAirName[selectedProfile].length()) selected.ssid = profileAirName[selectedProfile];
    Serial.printf("[WiFi] trying profile %d/%d: %s (%s)\n",
                  selectedProfile + 1, oracleWifiProfileCount(), selected.ssid.c_str(),
                  strongestRssi > -2000 ? (String(strongestRssi) + " dBm").c_str() : "not seen");
    WiFi.disconnect(false);
    // scanNetworks()/disconnect() can leave ESP32 Arduino's STA interface
    // stopped even though the high-level mode still reports WIFI_STA.
    // Fully restart it for every profile before begin().
    WiFi.mode(WIFI_OFF);
    delay(40);
    WiFi.mode(WIFI_STA);
    delay(80);
    WiFi.begin(selected.ssid.c_str(), selected.password.c_str());
    // A newly encountered router may need several seconds for DHCP. Keep the
    // short fallback for unseen profiles, but allow visible saved APs six seconds.
    const uint8_t associationAttempts = strongestRssi > -2000 ? 20 : perProfileAttempts;
    for (uint8_t attempt = 0;
         attempt < associationAttempts && WiFi.status() != WL_CONNECTED;
         ++attempt) delay(300);
    if (WiFi.status() == WL_CONNECTED) {
      if (savedName != selected.ssid) {   // fix a hand-typed name to the real one
        oracleUpdateWifiProfile(savedName, selected.ssid, "");
        Serial.printf("[WiFi] saved name corrected: \"%s\" -> \"%s\"\n", savedName.c_str(), selected.ssid.c_str());
      }
      oracleSetLastWifiSsid(selected.ssid);
      if (time(nullptr) < 1700000000) syncTimeFromNTP(4000);
      Serial.printf("[WiFi] connected: %s\n", selected.ssid.c_str());
      return true;
    }
    Serial.printf("[WiFi] profile failed: %s status=%d\n",
                  selected.ssid.c_str(), static_cast<int>(WiFi.status()));
  }
  stopWifi();
  Serial.println("[WiFi] no saved profile connected");
  return false;
}

int processPendingMemories(int preferredNote = -1, int maxNotes = 1) {
  const uint8_t attempts = oracleBackgroundNetworkMode() ? 8 : 20;
  if (!oracleHasOpenAiKey() || !connectConfiguredWifi(attempts)) {
    stopWifi();
    return 0;
  }
  loadIndex();
  const int processed = processPendingNotes(preferredNote, maxNotes);
  loadIndex();
  // Keep one stable Wi-Fi/TLS environment while the background queue still
  // has work. Rapid WIFI_OFF -> reconnect cycles produced invalid sockets on
  // the ESP32-S3. Foreground actions still shut Wi-Fi down immediately.
  if (!oracleBackgroundNetworkMode()) stopWifi();
  return processed;
}

void enterCompanion() {
  resetActivity();
  state = STATE_TRANSFER;
  companionExitArmed = false;
  companionGestureStartedAt = 0;
  companionUpperWasDown = digitalRead(BTN_REC) == LOW;
  companionLowerWasDown = digitalRead(BTN_PWR) == LOW;
  companionVolumeShownAt = 0;
  companionSingleStartedAt = 0;
  companionSinglePin = -1;
  clearLatchedPresses();
  showTransferConnecting();

  bool joinedSavedNetwork = connectConfiguredWifi(12);
  String accessPointPassword;
  if (!joinedSavedNetwork) {
    stopWifi();
    WiFi.mode(WIFI_AP);
    if (oracleApPasswordFixed()) {
      accessPointPassword = oracleApPassword();   // the fixed one from Setup
    } else {
      char password[16];
      // A fresh random password each time (Setup → Hotspot can fix one).
      snprintf(password, sizeof(password), "Ora-%08lX", static_cast<unsigned long>(esp_random()));
      accessPointPassword = password;
    }
    if (!WiFi.softAP("Oracle Lite", accessPointPassword.c_str())) {
      showError("WIFI ERROR");
      delay(1400);
      returnToReady();
      return;
    }
    WiFi.setSleep(false);   // no radio power saving while the Companion is open
  } else {
    syncTimeFromNTP(5000);
  }

  transferServerActive = companionServerStart();
  // http://oracle.local works on the same network (and on the hotspot).
  if (MDNS.begin("oracle")) {
    MDNS.setInstanceName("Oracle Lite");
    MDNS.addService("http", "tcp", 80);
    Serial.println("[Companion] http://oracle.local");
  }
  const IPAddress ip = joinedSavedNetwork ? WiFi.localIP() : WiFi.softAPIP();
  transferUrl = ip.toString();
  companionApPassword = joinedSavedNetwork ? "" : accessPointPassword;
  showTransferMode(transferUrl.c_str(),
                   joinedSavedNetwork ? nullptr : accessPointPassword.c_str());
}

void leaveCompanion() {
  soundBack();
  MDNS.end();
  stopTransferMode();
  // Join saved networks again right away (including one just added in the
  // companion), in the background; the Home icon follows the result.
  if (WiFi.status() != WL_CONNECTED) startWakeWifi();
  companionExitArmed = false;
  companionGestureStartedAt = 0;
  companionSingleStartedAt = 0;
  companionSinglePin = -1;
  state = STATE_IDLE;
  idleInputArmed = false;
  clearLatchedPresses();
  resetActivity();
  renderReadyHome();
  clearLatchedPresses();
}

bool processCapturedMemoryNow(int noteNum) {
  if (WiFi.status() != WL_CONNECTED) showMemoryProcessing(noteNum, "CONNECTING");
  if (!oracleHasWifi() || !oracleHasOpenAiKey() || !connectConfiguredWifi(14)) {
    stopWifi();
    writeNoteQueueState(noteNum, "queued", "saved locally; waiting for connection");
    showMemoryProcessing(noteNum, "QUEUED");
    delay(900);
    return false;
  }

  loadIndex();
  bool ready = false;
  // Exactly one durable network stage per iteration. The screen therefore
  // always describes the operation that can currently take time.
  // Finish the two user-visible essentials now: transcription and structured
  // understanding. The optional semantic embedding is derived maintenance and
  // must never keep a freshly captured thought on a processing screen.
  for (int stage = 0; stage < 2; stage++) {
    auto entry = std::find_if(noteIndex.begin(), noteIndex.end(), [noteNum](const NoteEntry& note) {
      return note.num == noteNum;
    });
    if (entry == noteIndex.end()) break;
    if (!entry->hasText) showMemoryProcessing(noteNum, "DECODING");
    else if (!brainMemoryIsCurrent(noteNum)) showMemoryProcessing(noteNum, "UNDERSTANDING");
    else if (!brainEmbeddingExists(noteNum)) showMemoryProcessing(noteNum, "REMEMBERING");
    else { ready = true; break; }

    if (processPendingNotes(noteNum, 1) <= 0) break;
    loadIndex();
  }
  ready = ready || brainMemoryIsCurrent(noteNum);
  stopWifi();
  if (noteQueueError(noteNum) == "no speech recognized") {
    showError(oracleUiLanguage() == "de" ? "KEINE SPRACHE" : "NO SPEECH");
    delay(900);
    return false;
  }
  if (ready) {
    writeNoteQueueState(noteNum, "ready");
    showSaved(noteNum);
    delay(650);
  } else {
    if (noteQueueState(noteNum) == "blocked") showError("PROCESSING FAILED");
    else showMemoryProcessing(noteNum, "QUEUED");
    delay(900);
  }
  return ready;
}

void redrawCompanionHome() {
  showTransferMode(transferUrl.c_str(),
                   companionApPassword.length() ? companionApPassword.c_str() : nullptr);
}

void captureMemory() {
  constexpr uint64_t kMinimumFreeBytes = 10ULL * 1024ULL * 1024ULL;
  if (SD_MMC.totalBytes() <= SD_MMC.usedBytes() ||
      SD_MMC.totalBytes() - SD_MMC.usedBytes() < kMinimumFreeBytes) {
    showError("STORAGE FULL");
    delay(1600);
    returnToReady();
    return;
  }
  state = STATE_RECORDING;
  // Join Wi-Fi and open the OpenAI connection while the note is spoken.
  if (oracleHasOpenAiKey()) { beginWifiEarly(); openAiPrewarm(); }
  // E-paper refreshes are blocking. Starting one here used to delay I2S reads
  // and cut the first spoken words from the recording.
  oracleSoundSetEnabled(false);
  const bool recorded = record();
  oracleSoundSetEnabled(true);

  if (!recorded) {
    showError("RECORD AGAIN");
    delay(1300);
    returnToReady();
    return;
  }

  // Only background noise: don't keep it.
  if (!lastRecordingHadSpeech()) {
    char path[64];
    for (const char* ext : {"wav", "moments"}) {
      snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, lastRecNum, ext);
      sdRemoveIfExists(path);
    }
    Serial.printf("[Rec] no speech in note %d; not saved\n", lastRecNum);
    showAskProgress("NO VOICE", "NOT SAVED");
    delay(1500);
    returnToReady();
    return;
  }

  if (!saveTag(lastRecNum, "Inbox")) {
    // The WAV is retained for orphan recovery; never claim a failed index
    // commit succeeded or launch cloud work against an uncommitted capture.
    showError("STORAGE FULL"); delay(1300); returnToReady(); return;
  }
  soundSaved();
  showSaved(lastRecNum);
  delay(350);

  // The WAV is durable before any network work starts. Process this specific
  // memory visibly now; if the network fails it remains safely queued.
  processCapturedMemoryNow(lastRecNum);
  // Do not make the freshly returned home screen mysteriously unresponsive by
  // immediately starting unrelated backlog work.
  nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
  returnToReady();
}

void askBrain() {
  if (!oracleHasWifi() || !oracleHasOpenAiKey()) {
    enterCompanion();
    return;
  }

  state = STATE_ASKING;
  // Join Wi-Fi and open the OpenAI connection while the question is spoken.
  beginWifiEarly();
  openAiPrewarm();
  constexpr const char* kQuestionPath = NOTES_DIR "/.ask.wav";
  constexpr const char* kAnswerPath = NOTES_DIR "/.oracle-answer.wav";

  oracleSoundSetEnabled(false);
  const bool recorded = recordQuestion(kQuestionPath, BTN_PWR);
  oracleSoundSetEnabled(true);
  if (!recorded) {
    showError("ASK AGAIN");
    delay(1200);
    returnToReady();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) showAskProgress("CONNECTING", nullptr);
  if (!connectConfiguredWifi()) {
    stopWifi();
    showError("NO WIFI");
    delay(1400);
    returnToReady();
    return;
  }

  loadIndex();

  String question;
  String answer;
  std::vector<int> sources;
  showAskProgress("DECODING", "YOUR QUESTION");
  const bool questionReady = transcribeQuestion(kQuestionPath, question);
  if (!questionReady) {
    stopWifi();
    const String reason = oracleLastCloudError();
    showError(reason.length() ? reason.c_str() : "ASK AGAIN");
    delay(reason.length() ? 2600 : 0);
    delay(1400);
    returnToReady();
    return;
  }
  bool answered = false;
  int taskSource = -1;
  if (applyBrainTaskCommand(question, answer, taskSource)) {
    answered = true;
    if (taskSource >= 0) sources.push_back(taskSource);
  } else if (answerAgendaQuestion(question, answer, sources)) {
    answered = true;
    // Date-sensitive agenda questions are answered from the same local source
    // as the Today screen, avoiding broad semantic matches from other days.
  } else {

  // If the newest capture has not completed its background transcription yet,
  // transcribe only that one. Raw transcripts are immediately searchable; Ask
  // never waits for the complete organization or embedding backlog.
  String recentQuestion = question;
  recentQuestion.toLowerCase();
  const bool asksForNewest = recentQuestion.indexOf("gerade") >= 0 ||
                             recentQuestion.indexOf("eben") >= 0 ||
                             recentQuestion.indexOf("zuletzt") >= 0 ||
                             recentQuestion.indexOf("latest") >= 0 ||
                             recentQuestion.indexOf("most recent") >= 0;
  if (asksForNewest && !noteIndex.empty() && !noteIndex.back().hasText) {
    const int newest = noteIndex.back().num;
    char newestPath[64];
    snprintf(newestPath, sizeof(newestPath), "%s/note_%03d.wav", NOTES_DIR, newest);
    showAskProgress("PREPARING", "LATEST THOUGHT");
    transcribe(String(newestPath), newest);
    loadIndex();
  }
    showAskProgress("SEARCHING", "YOUR MEMORY");
    answered = answerFromBrain(question, answer, sources);
  }

  if (!answered) {
    stopWifi();
    const String reason = oracleLastCloudError();
    showError(reason.length() ? reason.c_str() : "NO ANSWER");
    delay(reason.length() ? 2600 : 0);
    delay(1400);
    returnToReady();
    return;
  }

#if ORACLE_PRIVATE_DIAGNOSTICS
  Serial.printf("[Ask] %s\n[Answer] %s\n", question.c_str(), answer.c_str());
#endif
  showAskProgress("BUILDING", "VOICE");
  const bool speechReady = playBrainSpeech(answer, kAnswerPath, static_cast<int>(sources.size()));
  if (!speechReady) {
    stopWifi();
    showError("VOICE FAILED");
    delay(1000);
    showAskSpeaking(answer, static_cast<int>(sources.size()));
  }
  stopWifi();
  browseAnswerPages(answer, static_cast<int>(sources.size()));
  logMemory("askBrain");
  returnToReady();
}

void recordMeetingMinutes() {
  lastUpperTapAt = 0;
  lastLowerTapAt = 0;
  constexpr uint64_t kMinimumFreeBytes = 64ULL * 1024ULL * 1024ULL;  // ~30 min of audio
  if (SD_MMC.totalBytes() <= SD_MMC.usedBytes() ||
      SD_MMC.totalBytes() - SD_MMC.usedBytes() < kMinimumFreeBytes) {
    showError("STORAGE FULL");
    delay(1600);
    returnToReady();
    return;
  }
  // Low-battery guard: a meeting can run for hours.
  {
    const BatteryReading battery = readBattery();
    if (battery.valid && battery.percent >= 0 && battery.percent < 5) {
      showError("CHARGE FIRST");
      delay(1800);
      returnToReady();
      return;
    }
    if (battery.valid && battery.percent >= 0 && battery.percent < 25) {
      // Warn, then wait: top (or no answer) records anyway, bottom cancels.
      showMeetingStatus("BATTERY LOW", battery.percent, "TOP GO / BOTTOM STOP");
      soundBack();
      clearLatchedPresses();
      const uint32_t askedAt = millis();
      bool cancel = false;
      while (millis() - askedAt < 8000UL) {
        if (upperPressLatched) break;
        if (lowerPressLatched) { cancel = true; break; }
        delay(20);
      }
      while (!bothButtonsUp()) delay(10);
      clearLatchedPresses();
      if (cancel) { returnToReady(); return; }
    }
  }
  const int num = nextNoteNumber();
  if (num <= 0) { showError("STORAGE ERROR"); delay(1400); returnToReady(); return; }
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  const String now = utcToLocalDeviceLabel(currentUtcIso());
  const String title = now.length() >= 16 && now != "time not set"
    ? "Meeting " + now.substring(0, 16) : String("Meeting");

  // Join Wi-Fi first so the meeting streams live; without it, record locally
  // and send the whole recording afterwards.
  bool online = false;
  if (oracleHasWifi() && oracleHasMinutesKey()) {
    showMeetingStatus("CONNECTING", -1, "ORACLE MINUTES");
    online = connectConfiguredWifi(10);
  }
  if (!minutesLiveStart(num, title, online)) {
    stopWifi();
    showError("STORAGE ERROR");
    delay(1400);
    returnToReady();
    return;
  }

  state = STATE_RECORDING;
  soundSelect();
  oracleSoundSetEnabled(false);
  uint32_t durationMs = 0;
  const bool recorded = recordMeeting(path, durationMs, minutesLivePublish);
  oracleSoundSetEnabled(true);
  minutesLiveFinish(recorded);
  if (!recorded) {
    stopWifi();
    showError("MEETING TOO SHORT");
    delay(1400);
    returnToReady();
    return;
  }
  // Keep the audio as a memory; its text arrives as the server's minutes.
  if (!saveTag(num, "Meeting")) {
    stopWifi();
    showError("STORAGE FULL");
    delay(1400);
    returnToReady();
    return;
  }
  writeNoteQueueState(num, "minutes");
  soundSaved();

  bool sent = false;
  String error;
  if (oracleHasMinutesKey() && oracleHasWifi()) {
    showMeetingStatus("SENDING", 0, "TO ORACLE MINUTES");
    if (WiFi.status() == WL_CONNECTED || connectConfiguredWifi(10)) {
      WiFi.setSleep(false);   // finish the upload at full speed
      int shownPercent = 0;
      sent = minutesFinishUpload(num, [&](int percent) {
        // E-paper refreshes are slow: redraw only in 25% steps.
        if (percent >= shownPercent + 25 && percent < 100) {
          shownPercent = percent - percent % 25;
          showMeetingStatus("SENDING", percent, "TO ORACLE MINUTES");
        }
      }, error);
    } else {
      error = "NO WIFI";
    }
  } else {
    error = oracleHasMinutesKey() ? "NO WIFI SAVED" : "ADD MINUTES KEY";
  }
  stopWifi();
  if (sent) {
    showMeetingStatus("SENT", -1, "MINUTES ON THE WAY");
  } else {
    showMeetingStatus("SAVED", -1, error.length() ? error.c_str() : "WILL SEND LATER");
  }
  delay(1800);
  nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
  logMemory("recordMeetingMinutes");
  returnToReady();
}

// Finishes meeting uploads and fetches written minutes while the device idles.
void handleMeetingBackground() {
  static uint32_t nextAttemptAt = 0;
  if (wakeWifiStage != WAKE_WIFI_OFF) return;
  if (!oracleHasWifi() || !oracleHasMinutesKey()) return;
  if (nextAttemptAt && static_cast<int32_t>(millis() - nextAttemptAt) < 0) return;
  const uint32_t idleFor = millis() - lastActivityMs;
  // Never postpone sleep for this: it runs again at the next wake.
  if (idleFor < BACKGROUND_IDLE_MS || idleFor >= AUTO_SLEEP_IDLE_MS - 15000UL) return;
  if (!bothButtonsUp()) return;
  nextAttemptAt = millis() + 120000UL;
  if (minutesPendingCount() == 0) return;
  // Checking whether minutes are ready is quiet; only sending a meeting's
  // audio shows a screen.
  if (!connectConfiguredWifi(8)) { stopWifi(); return; }
  String error;
  bool shown = false;
  const bool progressed = minutesServiceOne([&shown](const char* phase) {
    if (strcmp(phase, "SENDING") == 0) { showMeetingStatus("SENDING", -1, "MEETING MINUTES"); shown = true; }
  }, error);
  stopWifi();
  if (progressed) loadIndex();
  else Serial.printf("[Minutes] background: %s\n", error.c_str());
  if (shown) returnFromBackgroundSync();
  // Freshly arrived minutes are organised by the normal memory pass next.
  nextBackgroundAttemptAt = millis() + BACKGROUND_IDLE_MS;
}

// Triple press lower: speak a question; it is sent when you stop talking
// (or press a button) and the answer is read out.
// Uses the Oracle server's one-trip voice API (web search + streamed voice).
void askWebByVoice() {
  lastUpperTapAt = 0;
  lastLowerTapAt = 0;
  if (!oracleHasWifi() || !oracleHasMinutesKey()) {
    showError(oracleHasWifi() ? "ADD MINUTES KEY" : "NO WIFI SAVED");
    delay(1800);
    returnToReady();
    return;
  }
  oracleVoiceHistoryClear();   // a new conversation
  // Each pass asks one question; holding the lower button on the answer asks
  // the next one (the server sees the earlier questions and answers).
  for (;;) {
    if (!askOneWebQuestion()) break;
  }
  logMemory("askWebByVoice");
  returnToReady();
}

// One spoken web question. Returns true if the user held the lower button on
// the answer to ask a follow-up.
bool askOneWebQuestion() {
  state = STATE_ASKING;
  // Join Wi-Fi and open the server connection while the question is spoken.
  beginWifiEarly();
  const bool streaming = oracleVoiceBegin();
  constexpr const char* kQuestionPath = NOTES_DIR "/.webask.wav";
  soundListening();   // speak after the chime
  oracleSoundSetEnabled(false);
  bool heardSpeech = false;
  const bool recorded = recordQuestionUntilSilence(kQuestionPath, 30000UL,
                                                   streaming ? oracleVoicePush : nullptr, heardSpeech);
  oracleSoundSetEnabled(true);
  if (!recorded || !heardSpeech) {
    if (streaming) oracleVoiceCancel();
    stopWifi();
    showError(recorded ? "NO SPEECH" : "ASK AGAIN");
    delay(1200);
    return false;
  }
  showAskProgress("SEARCHING", nullptr);
  if (WiFi.status() != WL_CONNECTED && !connectConfiguredWifi()) {
    if (streaming) oracleVoiceCancel();
    stopWifi();
    showError("NO WIFI");
    delay(1400);
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(false);   // upload at full speed
  String question, answer, error;
  bool answered = streaming ? oracleVoiceFinish(kQuestionPath, question, answer, error)
                            : oracleVoiceAsk(kQuestionPath, question, answer, error);
  // The server understood the question but its voice reply had no answer:
  // ask it as text and speak the answer with the device's voice.
  if (!answered && question.length()) {
    Serial.printf("[Voice] falling back to a text question: %s\n", question.c_str());
    showAskProgress("SEARCHING", nullptr);
    if ((WiFi.status() == WL_CONNECTED || connectConfiguredWifi()) &&
        oracleTextAsk(question, answer, error)) {
      answered = true;
      showAskProgress("BUILDING", "VOICE");
      playBrainSpeech(answer, NOTES_DIR "/.webanswer.wav", 0);
      showAskSpeaking(answer, 0);
    }
  }
  stopWifi();
  if (!answered) {
    showError(error.length() ? error.c_str() : "NO ANSWER");
    delay(2000);
    return false;
  }
  oracleVoiceHistoryAdd(question, answer);
  // The answer is on screen; pressing cycles its pages and then the question.
  return browseAnswerPages(answer, 0, true, question);
}

// Briefings are composed on the device (instant). Wi-Fi is only needed for
// the voice; without it the briefing is shown as text.
void playBriefing(bool weekly) {
  lastUpperTapAt = 0;
  lastLowerTapAt = 0;
  state = STATE_ASKING;
  loadIndex();
  String answer;
  int tasks = 0, events = 0;
  const bool built = weekly ? buildWeeklyBriefing(answer, tasks, events)
                            : buildDailyBriefing(answer, tasks, events);
  if (!built) {
    showError("SET THE TIME");
    delay(1400);
    returnToReady();
    return;
  }
  Serial.printf("[Briefing] %s: %d tasks, %d events, %u characters\n", weekly ? "weekly" : "daily",
                tasks, events, answer.length());
  bool spoken = false;
  if (oracleHasWifi()) {
    if (WiFi.status() != WL_CONNECTED) showAskProgress("CONNECTING", weekly ? "THIS WEEK" : "TODAY");
    if (WiFi.status() == WL_CONNECTED || connectConfiguredWifi()) {
      showAskProgress("BUILDING", "VOICE");
      spoken = playBrainSpeech(answer, weekly ? NOTES_DIR "/.oracle-weekly.wav" : NOTES_DIR "/.oracle-briefing.wav",
                               tasks + events, true);
    }
  }
  if (!spoken) showAskSpeaking(answer, tasks + events);
  browseAnswerPages(answer, tasks + events);
  returnToReady();
}

void playDailyBriefing() { playBriefing(false); }

void playWeeklyBriefing() { playBriefing(true); }

PressResult classifyPress(int pin) {
  if (digitalRead(pin) != LOW) return PressResult::Released;
  delay(14);
  if (digitalRead(pin) != LOW) return PressResult::Released;
  const uint32_t pressedAt = millis();
  while (digitalRead(pin) == LOW && millis() - pressedAt < REC_HOLD_MS) {
    // A two-button gesture always wins, even when the fingers land a little
    // apart. The old blocking hold detector missed this transition and began
    // Capture/Ask before Companion could see both buttons.
    if (bothButtonsDown()) {
      companionGestureStartedAt = millis();
      lastUpperTapAt = 0;
      lastLowerTapAt = 0;
      return PressResult::Chord;
    }
    delay(4);
  }
  return digitalRead(pin) == LOW ? PressResult::LongSingle : PressResult::Released;
}

// Reads one button gesture: -1 = both buttons (Companion), 0 = hold,
// 1..3 = number of taps. Taps are counted by the interrupt, so presses made
// while the loop was busy are not lost. The gesture ends after a quiet gap.
// Max pause between taps of one gesture. After two taps a little more time
// is allowed for the third, so a slightly slow triple press is not taken as
// a double press.
constexpr uint32_t kTapGapMs = 350;
constexpr uint32_t kThirdTapGapMs = 650;
int readButtonGesture(bool upper) {
  const int pin = upper ? BTN_REC : BTN_PWR;
  volatile uint8_t& edges = upper ? upperTapEdges : lowerTapEdges;
  volatile uint32_t& edgeAt = upper ? upperTapEdgeAt : lowerTapEdgeAt;
  noInterrupts();
  if (edges == 0) { edges = 1; edgeAt = millis(); }   // pressed before the ISR saw it
  interrupts();
  uint32_t lastLowAt = digitalRead(pin) == LOW ? millis() : 0;
  bool warmed = false;
  for (;;) {
    if (bothButtonsDown()) {
      companionGestureStartedAt = millis();
      noInterrupts(); edges = 0; interrupts();
      return -1;
    }
    const uint32_t now = millis();
    noInterrupts();
    const uint8_t count = edges;
    const uint32_t lastEdge = edgeAt;
    interrupts();
    if (digitalRead(pin) == LOW) {
      lastLowAt = now;
      // A press held past the hold time: hold (first press) or the end of
      // a tap sequence (count the taps, wait for release).
      if (now - lastEdge >= REC_HOLD_MS) {
        if (count <= 1) { noInterrupts(); edges = 0; interrupts(); return 0; }
        while (digitalRead(pin) == LOW) delay(4);
        noInterrupts(); edges = 0; interrupts();
        return min<int>(count, 3);
      }
    } else {
      // While waiting to see if more taps follow, load the agenda so a
      // single press can show it straight away (instant when cached).
      if (!warmed && count == 1) { warmed = true; loadBrainTasks(); }
      const uint32_t since = now - max(lastEdge, lastLowAt);
      if (count >= 3 || since >= (count >= 2 ? kThirdTapGapMs : kTapGapMs)) {
        noInterrupts(); edges = 0; interrupts();
        return min<int>(count, 3);
      }
    }
    delay(4);
  }
}

void handleIdle() {
  if (!idleInputArmed) {
    // A fresh edge captured during the blocking e-paper refresh is safe to
    // consume. The old Back/wake edge was cleared before that refresh.
    if (upperPressLatched || lowerPressLatched) idleInputArmed = true;
    else {
      if (bothButtonsUp()) idleInputArmed = true;
      return;
    }
  }

  if (bothButtonsDown()) {
    clearLatchedPresses();
    lastUpperTapAt = 0;
    lastLowerTapAt = 0;
    if (companionGestureStartedAt == 0) companionGestureStartedAt = millis();
    if (millis() - companionGestureStartedAt >= COMPANION_HOLD_MS) {
      soundSelect();
      enterCompanion();
    }
    return;
  }
  companionGestureStartedAt = 0;

  const bool upperLatched = upperPressLatched;
  const bool lowerLatched = lowerPressLatched;
  if (digitalRead(BTN_REC) == LOW || (upperLatched && !lowerLatched)) {
    takeLatchedPress(true);
    resetActivity();
    lastUpperTapAt = 0;
    lastLowerTapAt = 0;
    const uint32_t pressAt = upperTapEdges ? upperTapEdgeAt : millis();
    const int gesture = readButtonGesture(true);
    if (gesture < 0) return;
    if (gesture == 0) { captureMemory(); return; }
    // Top button: 1 = tasks, 2 = daily briefing, 3 = meeting minutes.
    if (gesture == 2) { playDailyBriefing(); return; }
    if (gesture >= 3) { recordMeetingMinutes(); return; }
    state = STATE_AGENDA;
    agendaShowsEvents = false;
    agendaPage = 0;
    agendaShownAt = millis();
    showTodayAgenda(false, agendaPage);
    Serial.printf("[UI] tasks drawn %lu ms after the press\n", static_cast<unsigned long>(millis() - pressAt));
    return;
  }
  if (digitalRead(BTN_PWR) == LOW || lowerLatched) {
    takeLatchedPress(false);
    resetActivity();
    lastUpperTapAt = 0;
    lastLowerTapAt = 0;
    const uint32_t pressAt = lowerTapEdges ? lowerTapEdgeAt : millis();
    const int gesture = readButtonGesture(false);
    if (gesture < 0) return;
    if (gesture == 0) { askBrain(); return; }
    // Bottom button: 1 = appointments, 2 = weekly briefing, 3 = ask the web.
    if (gesture == 2) { playWeeklyBriefing(); return; }
    if (gesture >= 3) { askWebByVoice(); return; }
    state = STATE_AGENDA;
    agendaShowsEvents = true;
    agendaPage = 0;
    agendaShownAt = millis();
    showTodayAgenda(true, agendaPage);
    Serial.printf("[UI] appointments drawn %lu ms after the press\n", static_cast<unsigned long>(millis() - pressAt));
  }
}

void handleAgenda() {
  if (millis() - agendaShownAt >= 30000UL) {
    returnToReady();
    return;
  }
  if (bothButtonsDown()) {
    if (companionGestureStartedAt == 0) companionGestureStartedAt = millis();
    if (millis() - companionGestureStartedAt >= COMPANION_HOLD_MS) enterCompanion();
    return;
  }
  companionGestureStartedAt = 0;

  const bool upperLatched = upperPressLatched;
  const bool lowerLatched = lowerPressLatched;
  if (digitalRead(BTN_REC) == LOW || (upperLatched && !lowerLatched)) {
    takeLatchedPress(true);
    const PressResult press = digitalRead(BTN_REC) == LOW
      ? classifyPress(BTN_REC) : PressResult::Released;
    if (press == PressResult::Chord) return;
    resetActivity();
    if (!agendaShowsEvents && agendaPage + 1 < todayAgendaPageCount(false)) {
      agendaPage++;
      agendaShownAt = millis();
      showTodayAgenda(false, agendaPage);
    } else returnToReady();
    return;
  }
  if (digitalRead(BTN_PWR) == LOW || lowerLatched) {
    takeLatchedPress(false);
    const PressResult press = digitalRead(BTN_PWR) == LOW
      ? classifyPress(BTN_PWR) : PressResult::Released;
    if (press == PressResult::Chord) return;
    resetActivity();
    if (agendaShowsEvents && agendaPage + 1 < todayAgendaPageCount(true)) {
      agendaPage++;
      agendaShownAt = millis();
      showTodayAgenda(true, agendaPage);
    } else returnToReady();
  }
}

// Builds missing semantic search vectors quietly (no screen changes). Stops
// for this wake after repeated failures so a bad key or no credit cannot make
// it retry on every pass.
void runSilentIndexing(int firstNote) {
  static uint8_t failuresThisWake = 0;
  if (failuresThisWake >= 2) { nextBackgroundAttemptAt = millis() + 10UL * BACKGROUND_RETRY_MS; return; }
  if (!connectConfiguredWifi(8)) {
    stopWifi();
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    return;
  }
  oracleSetBackgroundNetworkMode(true);
  const uint32_t started = millis();
  int note = firstNote;
  int indexed = 0;
  bool more = false;
  while (note >= 0) {
    if (!bothButtonsUp() || upperPressLatched || lowerPressLatched) { more = true; break; }   // user first
    const int processed = processPendingNotes(note, 1);
    if (processed <= 0) {
      failuresThisWake++;
      Serial.printf("[Index] note #%d failed: %s\n", note, oracleLastCloudError().c_str());
      break;
    }
    indexed++;
    note = -1;
    if (millis() - started > 6000UL) { more = true; break; }   // keep the device responsive
    for (const NoteEntry& entry : noteIndex) {
      if (entry.hasText && brainMemoryIsCurrent(entry.num) &&
          !brainEmbeddingExists(entry.num) && noteQueueRetryDue(entry.num)) { note = entry.num; break; }
    }
  }
  oracleSetBackgroundNetworkMode(false);
  stopWifi();
  if (indexed) Serial.printf("[Index] %d search vectors added in %lu ms\n", indexed,
                             static_cast<unsigned long>(millis() - started));
  nextBackgroundAttemptAt = millis() + (more ? BACKGROUND_IDLE_MS : BACKGROUND_RETRY_MS);
}

void handleBackgroundBrain() {
  if (wakeWifiStage != WAKE_WIFI_OFF) return;   // let the wake-up join finish first
  // Do not launch another network stage after the sleep budget has expired.
  // A stage already running is allowed to finish and save its result safely.
  if (millis() - lastActivityMs >= AUTO_SLEEP_GRACE_MS) return;
  if (!oracleHasWifi() || !oracleHasOpenAiKey()) return;
  if (nextBackgroundAttemptAt && static_cast<int32_t>(millis() - nextBackgroundAttemptAt) < 0) return;
  if (millis() - lastActivityMs < BACKGROUND_IDLE_MS) return;
  if (!bothButtonsUp()) return;
  // This scan touches per-note metadata, so never perform it in every 8 ms
  // idle-loop iteration. At scale it must only run when a pass is actually due.
  int pendingBefore = pendingBrainMemoryCount();
  int indexingNote = -1;
  // Optional search maintenance must never extend the normal sleep window.
  if (pendingBefore == 0 && millis() - lastActivityMs < AUTO_SLEEP_IDLE_MS) {
    for (const NoteEntry& note : noteIndex) {
      if (note.hasText && brainMemoryIsCurrent(note.num) &&
          !brainEmbeddingExists(note.num) && noteQueueRetryDue(note.num)) {
        indexingNote = note.num;
        pendingBefore = 1;
        break;
      }
    }
  }
  // Search-index (embedding) maintenance is optional derived data: do it
  // silently, several notes per pass, and never flash the sync screen for it.
  if (indexingNote >= 0) {
    runSilentIndexing(indexingNote);
    return;
  }
  if (pendingBefore == 0) {
    stopWifi();
    if (backlogSyncActive) {
      showBacklogProcessing(backlogSyncTotal, backlogSyncTotal, "DONE");
      delay(900);
      backlogSyncActive = false;
      backlogSyncTotal = 0;
      returnFromBackgroundSync();
    }
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    return;
  }

  // Never perform an invisible network operation behind an apparently idle
  // home screen. Pick the exact durable item this pass will work on and show
  // its real stage before a TLS request can temporarily occupy the device.
  int visibleNote = indexingNote;
  const char* visiblePhase = indexingNote >= 0 ? "REMEMBERING" : "CONNECTING";
  // Rotate the selected item too: passing a preferred note bypasses the
  // network worker's own rotation. A bad newest note must not starve old ones.
  // Only one network stage runs per pass so buttons and the e-paper UI remain
  // responsive between transcription and organization steps.
  static size_t queueCursor = 0;
  for (size_t offset = 0; indexingNote < 0 && offset < noteIndex.size(); ++offset) {
    const size_t index = (queueCursor + offset) % noteIndex.size();
    const NoteEntry& note = noteIndex[index];
    if (noteQueueRetryDue(note.num)) {
      if (!note.hasText) { visiblePhase = "DECODING"; visibleNote = note.num; }
      else if (!brainMemoryIsCurrent(note.num)) { visiblePhase = "UNDERSTANDING"; visibleNote = note.num; }
    }
    if (visibleNote >= 0) { queueCursor = (index + 1) % noteIndex.size(); break; }
  }
  if (visibleNote < 0) {
    stopWifi();
    if (backlogSyncActive) {
      backlogSyncActive = false;
      backlogSyncTotal = 0;
      returnFromBackgroundSync();
    }
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    return;
  }
  if (!backlogSyncActive) {
    backlogSyncActive = true;
    backlogSyncTotal = pendingBefore;
  } else if (pendingBefore > backlogSyncTotal) {
    backlogSyncTotal = pendingBefore;
  }
  const int completedBefore = max(0, backlogSyncTotal - pendingBefore);
  if (WiFi.status() != WL_CONNECTED) showBacklogProcessing(completedBefore, backlogSyncTotal, "CONNECTING");

  // Separate network association from the cloud stage so the screen always
  // describes what the device is actually doing. A failed association keeps
  // the durable recording queued and returns to Home without appearing stuck.
  if (!connectConfiguredWifi(8)) {
    showBacklogProcessing(completedBefore, backlogSyncTotal, "QUEUED");
    delay(900);
    backlogSyncActive = false;
    backlogSyncTotal = 0;
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    returnFromBackgroundSync();
    return;
  }
  showBacklogProcessing(completedBefore, backlogSyncTotal, visiblePhase);

  // Wi-Fi and Arduino's TCP stack must stay on the normal loop context.
  // Running this on a separate core starved IDLE0 and repeatedly crashed the
  // socket layer. Process one durable recording per bounded idle pass instead.
  oracleSetBackgroundNetworkMode(true);
  const int processed = processPendingMemories(visibleNote, 1);
  oracleSetBackgroundNetworkMode(false);
  // Leaving the runnable queue is not proof of success: permanent errors are
  // blocked too. Do not count such a transition as a completed memory.
  if (indexingNote < 0 && noteQueueState(visibleNote) == "blocked" &&
      !brainMemoryIsCurrent(visibleNote)) {
    stopWifi();
    showError(noteQueueError(visibleNote) == "no speech recognized" ? "NO SPEECH" : "PROCESSING FAILED");
    delay(900);
    backlogSyncActive = false;
    backlogSyncTotal = 0;
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    returnFromBackgroundSync();
    return;
  }
  const int pendingAfter = indexingNote >= 0 ? (processed > 0 ? 0 : 1) : pendingBrainMemoryCount();
  if (pendingAfter == 0) {
    stopWifi();
    showBacklogProcessing(backlogSyncTotal, backlogSyncTotal, "DONE");
    delay(900);
    backlogSyncActive = false;
    backlogSyncTotal = 0;
    returnFromBackgroundSync();
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
  } else if (processed > 0) {
    showBacklogProcessing(max(0, backlogSyncTotal - pendingAfter), backlogSyncTotal, visiblePhase);
    nextBackgroundAttemptAt = millis() + BACKGROUND_IDLE_MS;
  } else {
    stopWifi();
    showBacklogProcessing(max(0, backlogSyncTotal - pendingAfter), backlogSyncTotal, "QUEUED");
    delay(900);
    backlogSyncActive = false;
    backlogSyncTotal = 0;
    nextBackgroundAttemptAt = millis() + BACKGROUND_RETRY_MS;
    returnFromBackgroundSync();
  }
}

void handleCompanion() {

  const bool upperDown = digitalRead(BTN_REC) == LOW;
  const bool lowerDown = digitalRead(BTN_PWR) == LOW;

  if (upperDown || lowerDown) resetActivity();
  if (millis() - lastActivityMs >= COMPANION_IDLE_SLEEP_MS) {
    leaveCompanion();
    enterUltraSleep();
    return;
  }

  if (!companionExitArmed) {
    companionUpperWasDown = upperDown;
    companionLowerWasDown = lowerDown;
    if (!upperDown && !lowerDown) companionExitArmed = true;
    return;
  }

  if (upperDown && lowerDown) {
    companionSingleStartedAt = 0;
    companionSinglePin = -1;
    if (companionGestureStartedAt == 0) companionGestureStartedAt = millis();
    if (millis() - companionGestureStartedAt >= COMPANION_HOLD_MS) {
      leaveCompanion();
      return;
    }
    companionUpperWasDown = true;
    companionLowerWasDown = true;
    return;
  }

  companionGestureStartedAt = 0;
  if (upperDown && !companionUpperWasDown) {
    companionSingleStartedAt = millis();
    companionSinglePin = BTN_REC;
  } else if (lowerDown && !companionLowerWasDown) {
    companionSingleStartedAt = millis();
    companionSinglePin = BTN_PWR;
  }

  if (companionSinglePin >= 0 && millis() - companionSingleStartedAt >= 140UL) {
    if (!bothButtonsDown()) {
      const int volume = companionSinglePin == BTN_REC
        ? min(100, oraclePlaybackVolume() + 20)
        : max(20, oraclePlaybackVolume() - 20);
      oracleSetPlaybackVolume(volume);
      showVolumeSetting(volume);
      companionVolumeShownAt = millis();
    }
    companionSingleStartedAt = 0;
    companionSinglePin = -1;
  } else if (companionVolumeShownAt && millis() - companionVolumeShownAt >= 1800UL) {
    companionVolumeShownAt = 0;
    redrawCompanionHome();
  }

  companionUpperWasDown = upperDown;
  companionLowerWasDown = lowerDown;
}

void initializeDisplay() {
  custom_lcd_spi_t config = {};
  config.cs = EPD_CS_PIN;
  config.dc = EPD_DC_PIN;
  config.rst = EPD_RST_PIN;
  config.busy = EPD_BUSY_PIN;
  config.mosi = EPD_MOSI_PIN;
  config.scl = EPD_SCK_PIN;
  config.spi_host = EPD_SPI_NUM;
  config.buffer_len = (200 * 200) / 8;

  display = new epaper_driver_display(200, 200, config);
  // Do not visibly clear the panel here. The first actual UI frame establishes
  // the controller base image, avoiding two slow full refreshes during wake.
}

void initializeStorage(bool recoverOrphans) {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    showError("SD ERROR");
    while (true) delay(1000);
  }
  if (!SD_MMC.exists(NOTES_DIR)) SD_MMC.mkdir(NOTES_DIR);
  // Only boot performs the comparatively expensive orphan-WAV recovery scan.
  // Normal reloads read the compact catalog, while search/vector indexes are
  // rebuilt lazily by the first operation that actually needs them.
  loadIndex(recoverOrphans);
  Serial.printf("[SD] %d memories\n", static_cast<int>(noteIndex.size()));
  migrateDueTimesToSingapore();
}

}  // namespace

void setup() {
  // Start-up timing in the serial log ("[Boot] audio at 812 ms").
  auto bootStep = [](const char* what) {
    Serial.printf("[Boot] %s at %lu ms\n", what, static_cast<unsigned long>(millis()));
  };
  Serial.begin(115200);
  delay(40);
  Serial.println("\n=== Oracle Lite " FIRMWARE_VERSION " ===");

  releasePeripheralPowerHolds();
  oracleSettingsBegin();
  importFlashSetup();
  batteryBegin();
  WiFi.persistent(false);   // no flash write on every WiFi.begin()
  WiFi.onEvent(onWifiEvent);
  pinMode(BTN_REC, INPUT_PULLUP);
  pinMode(BTN_PWR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_REC), latchUpperPress, CHANGE);
  attachInterrupt(digitalPinToInterrupt(BTN_PWR), latchLowerPress, CHANGE);
  board.VBAT_POWER_ON();

  const bool wokeFromUltraSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
  wokeForReminder = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  Serial.printf("[Wake] cause=%d\n", static_cast<int>(esp_sleep_get_wakeup_cause()));
  if (wokeFromUltraSleep) {
    // EXT1 keeps the pin that caused wake-up even when a quick tap has already
    // been released by the time Arduino setup runs. Reading only digitalRead()
    // here lost exactly those quick lower-button taps.
    const uint64_t wakePins = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("[Wake] ext1 pins=0x%llx source=%s\n",
                  static_cast<unsigned long long>(wakePins),
                  (wakePins & (1ULL << BTN_PWR)) ? "pwr" : "unknown");
  }

  resetActivity();
  powerOnPeripherals();
  initializeDisplay();
  // The panel still shows the pre-sleep screen: load it back into the
  // controller so Home appears with a quick partial update, not a flash.
  // (restoreFrameAfterWake() can skip the wake-up flash; the clean full
  // refresh is preferred, so it is not used.)

  const size_t psramBytes = ESP.getPsramSize();
  Serial.printf("[HW] Flash: %u MB, PSRAM: %u MB\n",
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(psramBytes / (1024 * 1024)));
  if (!psramFound() || psramBytes < 8UL * 1024UL * 1024UL) {
    showError("8/8 ONLY");
    while (true) delay(1000);
  }

  i2c_master_Init();
  rtcSyncSystemFromChip();
  bootStep("display + clock");

  // Waking from sleep: draw Home straight away from the counts kept through
  // sleep (the panel refresh runs on core 0 while the rest starts up).
  setupRequired = !oracleHasWifi() || !oracleHasOpenAiKey();
  const bool fromSleep = wokeFromUltraSleep || wokeForReminder;
  bool homeShown = false;
  if (fromSleep && !setupRequired) {
    const BatteryReading battery = readBattery();
    homeShown = showIdleFromCache(battery.valid ? battery.percent : -1);
    if (homeShown) bootStep("home drawn (cached)");
  }
  // Join Wi-Fi right away, in the background; Home's icon updates when known.
  startWakeWifi();
  delay(50);
  audio_bsp_init();
  audio_play_init();
  bootStep("audio");
  // The orphaned-recording scan is only needed after a power loss, not after
  // a normal sleep.
  initializeStorage(!fromSleep);
  bootStep("sd card");

  state = STATE_IDLE;
  if (setupRequired) {
    showSetupRequired();
  } else if (!homeShown || homeCacheStale()) {
    renderReadyHome();
    bootStep("home drawn");
  } else {
    displayedHomeDay = localDayKey();
    nextHomeClockCheckAt = millis() + 60000UL;
  }

  bootStep("ready");
  logMemory("boot");
  // Waking is deliberately a separate interaction: PWR only wakes to Home.
  // Ignore the held wake press until both buttons are released so it cannot
  // accidentally open Appointments or start recording during boot.
  if (wokeFromUltraSleep && state == STATE_IDLE) idleInputArmed = false;
  // The wake press itself only wakes to Home. New presses from this point on
  // are retained even if they happen during a display update.
  clearLatchedPresses();
}

void loop() {
  handleUsbSetup();

  // Companion must also be serviced on a device that still needs setup.
  if (state == STATE_TRANSFER) {
    handleCompanion();
    delay(5);
    return;
  }

  // A freshly flashed device waits transparently for the local USB setup.
  // Do not start Wi-Fi Companion automatically: opening WebSerial resets the
  // ESP32 and the browser must still be able to deliver the setup command.
  if (setupRequired) {
    if (bothButtonsDown()) {
      if (!setupCompanionHoldStartedAt) setupCompanionHoldStartedAt = millis();
      else if (millis() - setupCompanionHoldStartedAt >= COMPANION_HOLD_MS) {
        setupCompanionHoldStartedAt = 0;
        enterCompanion();
      }
    } else {
      setupCompanionHoldStartedAt = 0;
    }
    delay(5);
    return;
  }

  if (state == STATE_AGENDA) {
    handleAgenda();
    delay(8);
    return;
  }

  // Rest the CPU only when nothing is happening; any press or work = full speed.
  {
    const bool working = state != STATE_IDLE || audioPlaying || upperPressLatched || lowerPressLatched ||
                         !bothButtonsUp() || WiFi.getMode() != WIFI_OFF || wakeWifiStage != WAKE_WIFI_OFF ||
                         transferServerActive || millis() - lastActivityMs < 3000UL;
    if (working) cpuFullSpeed();
    // Stays at full speed while awake: responsiveness first, sleep saves power.
  }
  serviceWakeWifi();
  // Keep Home's Wi-Fi icon true to the last connection result, whichever
  // path (wake-up join, background sync, an action) changed it.
  if (state == STATE_IDLE && !setupRequired && homeWifiIconStale() && bothButtonsUp() &&
      !upperPressLatched && !lowerPressLatched) {
    renderReadyHome();
  }

  if (state == STATE_IDLE) {
    handleIdle();
    if (state == STATE_IDLE) handleBackgroundBrain();
    if (state == STATE_IDLE) handleMeetingBackground();
    if (state == STATE_IDLE && !setupRequired) {
      // Rescan the agenda every 5 minutes; check the cached reminder often.
      static uint32_t nextReminderScan = 0, nextReminderCheck = 0;
      if (!nextReminderScan || static_cast<int32_t>(millis() - nextReminderScan) >= 0) {
        reminderRefresh();
        nextReminderScan = millis() + 300000UL;
      }
      if (static_cast<int32_t>(millis() - nextReminderCheck) >= 0) {
        nextReminderCheck = millis() + 5000UL;
        if (reminderDue() && bothButtonsUp() && !upperPressLatched && !lowerPressLatched) runReminder();
      }
    }
    if (state == STATE_IDLE &&
        static_cast<int32_t>(millis() - nextHomeClockCheckAt) >= 0) {
      nextHomeClockCheckAt = millis() + 60000UL;
      const String day = localDayKey();
      if (day.length() && day != displayedHomeDay) renderReadyHome();
    }
    if (state == STATE_IDLE &&
        idleInputArmed && bothButtonsUp() &&
        (!lastUpperTapAt || millis() - lastUpperTapAt > ASK_DOUBLE_TAP_MS) &&
        (!lastLowerTapAt || millis() - lastLowerTapAt > ASK_DOUBLE_TAP_MS)) {
      const uint32_t idleFor = millis() - lastActivityMs;
      if (idleFor >= AUTO_SLEEP_IDLE_MS) {
        // Give queued memories time to finish, but never let a bad network or
        // failed cloud request keep the device awake indefinitely.
        // This scans SD metadata. Never repeat it on every 8 ms idle tick.
        static uint32_t nextQueueCheck = 0;
        static bool workPending = false;
        if (!nextQueueCheck || static_cast<int32_t>(millis() - nextQueueCheck) >= 0) {
          workPending = oracleHasWifi() && oracleHasOpenAiKey() && pendingBrainMemoryCount() > 0;
          nextQueueCheck = millis() + 1000UL;
        }
        if (!workPending || idleFor >= AUTO_SLEEP_GRACE_MS) enterUltraSleep();
      }
    }
  }
  delay(8);
}
