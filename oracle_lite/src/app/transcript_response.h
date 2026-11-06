#pragma once
#include <ArduinoJson.h>
#include <cctype>

inline bool transcriptHasNoSpeech(JsonVariantConst value) {
  if (!value.is<const char*>()) return false;
  const char* text = value.as<const char*>();
  for (; *text; ++text) if (!std::isspace(static_cast<unsigned char>(*text))) return false;
  return true;
}

template <typename Text>
bool parseTranscriptResponse(const char* json, Text& text) {
  JsonDocument document;
  text = "";
  if (deserializeJson(document, json) || !document["text"].is<const char*>()) return false;
  const char* value = document["text"];
  if (!value || !*value) return false;
  text = value;
  return true;
}
