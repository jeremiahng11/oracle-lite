#pragma once

#include <Arduino.h>

// The device UI intentionally exposes only Record, Ask, processing, answer,
// Companion, error, and sleep. Organization belongs to the Web Companion.
// Wi-Fi reachability shown on the Home header: true after the last connection
// attempt succeeded, false after it failed (or no Wi-Fi is saved). The radio
// is switched off between jobs to save power, so this is the last known state.
void setWifiIndicator(bool connected);
bool wifiIndicatorState();
bool homeWifiIconStale();   // Home shows an out-of-date Wi-Fi icon
bool showIdleFromCache(int batteryPercent);   // Home from the pre-sleep counts
bool homeCacheStale();
void showIdle(int pendingCount = 0, int batteryPercent = -1);
void showTodayAgenda(bool events, int page = 0);
int todayAgendaPageCount(bool events);
void showRecording();
void showRecordingProgress(uint32_t elapsedMs, int moments);
void showSaved(int noteNumber);
void showMemoryProcessing(int noteNumber, const char* phase);
void showBacklogProcessing(int completed, int total, const char* phase);
void showAskListening();
void showAskThinking();
void showAskProgress(const char* action, const char* object = nullptr);
void showAskSpeaking(const String& answer, int sourceCount);
int askAnswerPageCount(const String& answer);
void showAskSpeakingPage(const String& answer, int sourceCount, int page);
void showAskQuestion(const String& question);
void showTransferConnecting();
void showTransferMode(const char* ip, const char* apPassword = nullptr);
void showVolumeSetting(int percent);
void showSetupRequired();
void showError(const char* message);
void showMeetingRecording(uint32_t elapsedMs, bool fast);
void showMeetingStatus(const char* phase, int percent = -1, const char* detail = nullptr);
void showSleepLocked();
void showUltraSleepScreen();
