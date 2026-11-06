#pragma once
#include <ArduinoJson.h>
#include <cstring>
#include <cctype>

constexpr unsigned kMemoryOutputTokens = 8192;
constexpr unsigned kMemoryMaxTasks = 32;
constexpr unsigned kMemoryTranscriptBytes = 32000;
constexpr unsigned kMemoryTimeoutMs = 60000;
// The router returns an answer plus task/source IDs. Its JSON envelope needs
// room for a concise project overview independently of task/source IDs.
constexpr unsigned kActionOutputTokens = 1024;

inline bool memoryNonemptyText(JsonVariantConst value) {
  if (!value.is<const char*>()) return false;
  const char* text = value.as<const char*>();
  for (; *text; ++text) if (!std::isspace(static_cast<unsigned char>(*text))) return true;
  return false;
}

inline bool validMemoryBody(JsonVariantConst memory) {
  if (!memory.is<JsonObjectConst>() || !memoryNonemptyText(memory["title"]) ||
      !memoryNonemptyText(memory["summary"]) || !memoryNonemptyText(memory["kind"]) ||
      !memory["tasks"].is<JsonArrayConst>() || memory["tasks"].size() > kMemoryMaxTasks) return false;
  for (JsonVariantConst task : memory["tasks"].as<JsonArrayConst>())
    if (!task.is<JsonObjectConst>() || !memoryNonemptyText(task["title"])) return false;
  return true;
}

// A syntactically valid fragment from an incomplete response is not a memory.
// Keep this independent of Arduino/network/storage for host regression tests.
inline const char* memoryResponseError(JsonVariantConst response) {
  const char* status = response["status"] | "";
  if (!std::strcmp(status, "incomplete")) {
    const char* reason = response["incomplete_details"]["reason"] | "";
    return !std::strcmp(reason, "max_output_tokens")
      ? "organization: output limit reached" : "organization: incomplete response";
  }
  if (std::strcmp(status, "completed")) return "organization: response not completed";
  bool hasText = false;
  for (JsonObjectConst item : response["output"].as<JsonArrayConst>()) {
    for (JsonObjectConst content : item["content"].as<JsonArrayConst>()) {
      if (!std::strcmp(content["type"] | "", "refusal")) return "organization: request refused";
      if (!std::strcmp(content["type"] | "", "output_text") &&
          content["text"].is<const char*>() && std::strlen(content["text"].as<const char*>())) hasText = true;
    }
  }
  return hasText ? nullptr : "organization: empty response";
}
