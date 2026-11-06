#include "settings.h"
#include <Preferences.h>
#include "notes.h"
#include "settings_pair.h"

static Preferences prefs;
static bool ready = false;
static int cachedWifiCount = -1;
static int cachedApiConfigured = -1;

static bool supportedLanguage(const String& language) {
  return language == "de" || language == "en" || language == "fr" ||
         language == "es" || language == "it" || language == "pl" ||
         language == "ru" || language == "cs" || language == "uk" ||
         language == "nl" || language == "pt";
}

static String wifiKey(int index, const char* suffix) {
  return "wifi" + String(index) + suffix;
}

static String storedString(const String& key) {
  if (!ready || !prefs.isKey(key.c_str())) return "";
  return prefs.getString(key.c_str(), "");
}

static bool storeWifiPair(int index, const String& ssid, const String& password) {
  return saveSettingsPair(ssid, password,
    [&](bool secret) { return storedString(wifiKey(index, secret ? "p" : "s")); },
    [&](bool secret, const String& text) { prefs.putString(wifiKey(index, secret ? "p" : "s").c_str(), text); });
}

static bool storeLegacyWifi(const OracleWifiProfile& profile) {
  return saveSettingsPair(profile.ssid, profile.password,
    [&](bool secret) { return storedString(secret ? "wifi_pass" : "wifi_ssid"); },
    [&](bool secret, const String& text) { prefs.putString(secret ? "wifi_pass" : "wifi_ssid", text); });
}

void oracleSettingsBegin() {
  if (ready) return;
  ready = prefs.begin("oracle", false);
  if (!ready) return;
  // Migrate the original single-network setting without losing it.
  if (!storedString("wifi0s").length()) {
    const String legacySsid = storedString("wifi_ssid");
    if (legacySsid.length()) {
      storeWifiPair(0, legacySsid, storedString("wifi_pass"));
    }
  }
}

static String value(const char* key) {
  oracleSettingsBegin();
  return ready ? storedString(key) : String();
}

int oracleWifiProfileCount() {
  oracleSettingsBegin();
  if (!ready) return 0;
  if (cachedWifiCount >= 0) return cachedWifiCount;
  int count = 0;
  for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; i++) {
    if (storedString(wifiKey(i, "s")).length()) count++;
  }
  cachedWifiCount = count;
  return cachedWifiCount;
}

OracleWifiProfile oracleWifiProfile(int index) {
  oracleSettingsBegin();
  if (!ready || index < 0 || index >= ORACLE_MAX_WIFI_PROFILES) return {};
  return {storedString(wifiKey(index, "s")), storedString(wifiKey(index, "p"))};
}

bool oracleSaveWifiProfile(const String& ssid, const String& password) {
  oracleSettingsBegin();
  if (!ready || !ssid.length() || ssid.length() > 32 || password.length() > 63) return false;
  int target = -1;
  for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; i++) {
    const String stored = storedString(wifiKey(i, "s"));
    if (stored == ssid) { target = i; break; }
    if (target < 0 && !stored.length()) target = i;
  }
  if (target < 0) return false;
  const bool ok = storeWifiPair(target, ssid, password);
  cachedWifiCount = -1;
  // Keep legacy keys synchronized for downgrade compatibility.
  if (ok && target == 0) {
    if (!storeLegacyWifi({ssid, password})) Serial.println("[Settings] legacy Wi-Fi mirror write failed");
  }
  return ok;
}

bool oracleRemoveWifiProfile(const String& ssid) {
  oracleSettingsBegin();
  if (!ready || !ssid.length()) return false;
  int found = -1;
  for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; i++) {
    if (storedString(wifiKey(i, "s")) == ssid) { found = i; break; }
  }
  if (found < 0) return false;
  OracleWifiProfile before[ORACLE_MAX_WIFI_PROFILES];
  for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; ++i) before[i] = oracleWifiProfile(i);
  bool ok = true;
  for (int i = found; i < ORACLE_MAX_WIFI_PROFILES && ok; ++i) {
    const OracleWifiProfile next = i + 1 < ORACLE_MAX_WIFI_PROFILES ? before[i + 1] : OracleWifiProfile{};
    ok = storeWifiPair(i, next.ssid, next.password);
  }
  if (ok) ok = storeLegacyWifi(oracleWifiProfile(0));
  if (!ok) {
    for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; ++i) storeWifiPair(i, before[i].ssid, before[i].password);
    storeLegacyWifi(before[0]);
    cachedWifiCount = -1;
    Serial.println("[Settings] Wi-Fi removal failed; rollback attempted");
    return false;
  }
  cachedWifiCount = -1;
  return true;
}

bool oracleUpdateWifiProfile(const String& oldSsid, const String& newSsid, const String& password) {
  oracleSettingsBegin();
  if (!ready || !oldSsid.length() || !newSsid.length() || newSsid.length() > 32 || password.length() > 63) return false;
  int found = -1;
  for (int i = 0; i < ORACLE_MAX_WIFI_PROFILES; i++) {
    const String stored = storedString(wifiKey(i, "s"));
    if (stored == oldSsid) found = i;
    else if (stored == newSsid && newSsid != oldSsid) return false;   // that name is already saved
  }
  if (found < 0) return false;
  const String keptPassword = password.length() ? password : storedString(wifiKey(found, "p"));
  const bool ok = storeWifiPair(found, newSsid, keptPassword);
  cachedWifiCount = -1;
  if (ok && found == 0 && !storeLegacyWifi({newSsid, keptPassword}))
    Serial.println("[Settings] legacy Wi-Fi mirror write failed");
  if (ok && storedString("last_wifi") == oldSsid) prefs.putString("last_wifi", newSsid);
  return ok;
}

bool oracleSsidSimilar(const String& a, const String& b) {
  auto key = [](const String& s) {
    String k;
    for (size_t i = 0; i < s.length(); ++i) {
      const char c = s[i];
      if (c == ' ' || c == '\t') continue;
      k += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return k;
  };
  return a.length() && b.length() && key(a) == key(b);
}

String oracleWifiSsid() { return oracleWifiProfile(0).ssid; }
String oracleWifiPassword() { return oracleWifiProfile(0).password; }
String oracleLastWifiSsid() { return value("last_wifi"); }
bool oracleSetLastWifiSsid(const String& ssid) {
  oracleSettingsBegin();
  if (!ready || !ssid.length() || ssid.length() > 32) return false;
  if (storedString("last_wifi") == ssid) return true;   // skip a flash write per connect
  return prefs.putString("last_wifi", ssid) > 0;
}
String oracleOpenAiKey() { return value("openai_key"); }
String oracleSpeechVoice() {
  String voice = value("tts_voice");
  if (voice == "eleven" || voice == "coral") return voice;
  return "cedar";
}
String oracleUiLanguage() {
  const String language = value("ui_lang");
  return supportedLanguage(language) ? language : "en";
}
bool oracleSetUiLanguage(const String& language) {
  oracleSettingsBegin();
  if (!ready || !supportedLanguage(language)) return false;
  return prefs.putString("ui_lang", language) > 0;
}
int oraclePlaybackVolume() {
  oracleSettingsBegin();
  if (!ready) return 80;
  const int stored = prefs.getInt("volume", 80);
  const int step = constrain((stored + 10) / 20, 1, 5);
  return step * 20;
}
bool oracleSetPlaybackVolume(int volume) {
  oracleSettingsBegin();
  if (!ready) return false;
  const int step = constrain((volume + 10) / 20, 1, 5);
  return prefs.putInt("volume", step * 20) == sizeof(int32_t);
}
bool oracleHasWifi() { return oracleWifiProfileCount() > 0; }
bool oracleHasOpenAiKey() {
  if (cachedApiConfigured < 0) cachedApiConfigured = oracleOpenAiKey().length() > 10 ? 1 : 0;
  return cachedApiConfigured == 1;
}

bool oracleSaveSettings(const String& ssid, const String& password, const String& apiKey,
                      const String& speechVoice) {
  oracleSettingsBegin();
  if (!ready || ssid.length() > 32 || password.length() > 63 || (!ssid.length() && password.length()) ||
      apiKey.length() > 200 || (apiKey.length() && apiKey.length() < 11) ||
      (speechVoice != "cedar" && speechVoice != "coral" && speechVoice != "eleven")) return false;
  // Updating only the voice/key must not require adding a new network.
  bool ok = ssid.length() ? oracleSaveWifiProfile(ssid, password) : true;
  if (!ok) return false;
  if (apiKey.length() > 0) {
    const bool stored = prefs.putString("openai_key", apiKey) == apiKey.length();
    ok = stored && ok;
    if (stored) {
      cachedApiConfigured = apiKey.length() > 10 ? 1 : 0;
      resumeBlockedCloudNotes();
    }
  }
  ok = (prefs.putString("tts_voice", speechVoice) == speechVoice.length()) && ok;
  return ok;
}

bool oracleSetSpeechVoice(const String& voice) {
  oracleSettingsBegin();
  if (!ready || (voice != "cedar" && voice != "coral" && voice != "eleven")) return false;
  return prefs.putString("tts_voice", voice) == voice.length();
}

bool oracleClearOpenAiKey() {
  oracleSettingsBegin();
  if (!ready) return false;
  if (prefs.isKey("openai_key") && !prefs.remove("openai_key")) return false;
  cachedApiConfigured = 0;
  return true;
}

// ─── Oracle Minutes ────────────────────────────────────────────────────────
// Optional build-time fallbacks (define in secrets.h). Device setup wins.
#if __has_include("../../secrets.h")
#include "../../secrets.h"
#endif
#ifndef ORACLE_MINUTES_URL
#define ORACLE_MINUTES_URL ""
#endif
#ifndef ORACLE_MINUTES_KEY
#define ORACLE_MINUTES_KEY ""
#endif

String oracleMinutesUrl() {
  String url = value("om_url");
  if (!url.length()) url = ORACLE_MINUTES_URL;
  if (!url.length()) url = ORACLE_MINUTES_DEFAULT_URL;
  while (url.endsWith("/")) url.remove(url.length() - 1);
  return url;
}

String oracleMinutesKey() {
  const String key = value("om_key");
  return key.length() ? key : String(ORACLE_MINUTES_KEY);
}

bool oracleHasMinutesKey() { return oracleMinutesKey().length() >= 8; }

bool oracleSaveMinutes(const String& url, const String& apiKey) {
  oracleSettingsBegin();
  if (!ready || url.length() > 120 || apiKey.length() > 200) return false;
  if (url.length() && !url.startsWith("https://") && !url.startsWith("http://")) return false;
  bool ok = true;
  if (url.length()) ok = prefs.putString("om_url", url) == url.length();
  if (apiKey.length()) ok = (prefs.putString("om_key", apiKey) == apiKey.length()) && ok;
  return ok;
}

bool oracleClearMinutesKey() {
  oracleSettingsBegin();
  if (!ready) return false;
  return !prefs.isKey("om_key") || prefs.remove("om_key");
}
