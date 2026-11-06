#pragma once
#include <Arduino.h>
#include <vector>

struct BrainMemory {
  int noteNum = -1;
  int schemaVersion = 0;
  int importance = 1;
  String title;
  String summary;
  String kind;
  String language;
  String createdUtc;
  String sourceHash;
  String transcriptExcerpt;
  std::vector<String> topics;
  std::vector<String> people;
  std::vector<String> projects;
  std::vector<String> tasks;
  std::vector<String> taskIds;
  std::vector<String> taskDueUtc;
  std::vector<String> taskTypes;
  std::vector<String> taskDatePrecision;
  std::vector<String> taskOriginalTimeText;
  std::vector<int> taskConfidence;
  std::vector<String> decisions;
  std::vector<String> facts;
  std::vector<String> questions;
  bool valid = false;
};

struct BrainStats {
  uint64_t totalBytes = 0;
  uint64_t usedBytes = 0;
  int memories = 0;
  int understood = 0;
  int moments = 0;
  int openTasks = 0;
  int pending = 0;
};

String brainMemoryPath(int noteNum);
String brainEmbeddingText(const BrainMemory& memory);
bool saveBrainMemoryJson(int noteNum, const String& json);
bool saveBrainEmbedding(int noteNum, const std::vector<float>& embedding);
bool brainEmbeddingExists(int noteNum);
bool brainHasSemanticIndex();
BrainMemory loadBrainMemory(int noteNum);
bool brainMemoryIsCurrent(int noteNum);
bool ensureBrainSearchIndex();
bool ensureBrainVectorIndex();
void removeBrainSearchIndexEntry(int noteNum);
void removeBrainVectorIndexEntry(int noteNum);
int pendingBrainMemoryCount();
BrainStats readBrainStats();
std::vector<BrainMemory> retrieveBrainMemories(
  const String& question,
  size_t limit = 5,
  const std::vector<float>* queryEmbedding = nullptr
);

// One-time move of stored due times from German time to Singapore time.
void migrateDueTimesToSingapore();
