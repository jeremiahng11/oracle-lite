#pragma once
#include <Arduino.h>

void oracleSettingsBegin();
constexpr int ORACLE_MAX_WIFI_PROFILES = 5;
struct OracleWifiProfile {
  String ssid;
  String password;
};
int oracleWifiProfileCount();
OracleWifiProfile oracleWifiProfile(int index);
bool oracleSaveWifiProfile(const String& ssid, const String& password);
bool oracleRemoveWifiProfile(const String& ssid);
// Changes a saved network in place (name and/or password). Empty password =
// keep the saved one.
bool oracleUpdateWifiProfile(const String& oldSsid, const String& newSsid, const String& password);
// Same network name ignoring spaces and capitals ("ACPGAPMY- Guest" vs
// "ACPGAPMY-Guest"): saved names typed by hand often differ only like that.
bool oracleSsidSimilar(const String& a, const String& b);
String oracleWifiSsid();
String oracleWifiPassword();
String oracleLastWifiSsid();
bool oracleSetLastWifiSsid(const String& ssid);
String oracleOpenAiKey();
String oracleSpeechVoice();   // "cedar" | "coral" (OpenAI) | "eleven" (Oracle server)
bool oracleSetSpeechVoice(const String& voice);
String oracleUiLanguage();
bool oracleSetUiLanguage(const String& language);
int oraclePlaybackVolume();
bool oracleSetPlaybackVolume(int volume);
bool oracleHasWifi();
bool oracleHasOpenAiKey();
bool oracleSaveSettings(const String& ssid, const String& password, const String& apiKey,
                      const String& speechVoice);
bool oracleClearOpenAiKey();

// Oracle Minutes server (meeting minutes uploads).
constexpr const char* ORACLE_MINUTES_DEFAULT_URL = "https://whisper.jeremiah.sg";
String oracleMinutesUrl();
String oracleMinutesKey();
bool oracleHasMinutesKey();
bool oracleSaveMinutes(const String& url, const String& apiKey);
bool oracleClearMinutesKey();
