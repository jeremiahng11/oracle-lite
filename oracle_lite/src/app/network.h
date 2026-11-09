#pragma once
#include <Arduino.h>

struct OraclePerformanceStats {
  uint32_t transcriptionMs = 0;
  uint32_t embeddingMs = 0;
  uint32_t answerMs = 0;
  uint32_t speechMs = 0;
};

OraclePerformanceStats oraclePerformanceStats();
void oracleSetBackgroundNetworkMode(bool active);
bool oracleBackgroundNetworkMode();
// Short, screen-sized reason for the most recent failed OpenAI call
// (e.g. "TLS/CONNECT FAILED", "API KEY REJECTED", "NO API CREDIT"). Empty
// when unknown. Shown on the TRY AGAIN screen so failures are diagnosable.
String oracleLastCloudError();
// Opens the TLS connection to api.openai.com in the background (call while
// the user is still speaking) and closes it (call when Wi-Fi goes off).
void openAiPrewarm();
// Wi-Fi region with channels 1-13 (call after the radio starts).
void oracleApplyWifiRegion();
void openAiDropConnection();

bool transcribe(const String& wavPath, int noteNum);
bool transcribeQuestion(const String& wavPath, String& question);
bool understandNote(int noteNum);
bool answerFromBrain(const String& question, String& answer, std::vector<int>& sourceNotes);
bool answerAgendaQuestion(const String& question, String& answer, std::vector<int>& sourceNotes);
bool buildDailyBriefing(String& answer, int& taskCount, int& eventCount);
bool buildWeeklyBriefing(String& answer, int& taskCount, int& eventCount);
bool synthesizeBrainSpeech(const String& text, const char* outputPath, bool fast = true);
bool playBrainSpeech(const String& text, const char* fallbackPath, int sourceCount, bool fast = true);
int processPendingNotes(int preferredNote = -1, int maxNotes = 1);

void stopTransferMode();

// Shared with the Web Companion server (companion_server.cpp).
bool openAiJsonPost(const char* endpoint, const String& payload, String& response,
                    uint16_t timeoutMs = 0, String* failure = nullptr);
String responseOutputText(const String& response);
String readSmallFile(const char* path, size_t maxLen);
String localizedQueueState(const String& state);
String localizedQueueError(const String& error);
void oracleApplyWifiRegion();

// Web Companion (fast single-page app + JSON API on esp_http_server).
bool companionServerStart();
void companionServerStop();
