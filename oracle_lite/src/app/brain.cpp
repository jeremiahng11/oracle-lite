#include "brain.h"
#include "sd_read.h"
#include "minutes.h"

#include "notes.h"
#include "rtc.h"
#include "tasks.h"
#include "date_validation.h"
#include "task_identity.h"
#include "storage_commit.h"
#include "agenda_range.h"
#include "utf8_excerpt.h"
#include "checked_file_write.h"
#include "memory_response.h"
#include "../../config.h"
#include "../../globals.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <algorithm>
#include <math.h>

namespace {

constexpr int kBrainSchemaVersion = 6;
constexpr size_t kSearchTranscriptBytes = 6000;
constexpr size_t kAnswerExcerptBytes = 900;
// Versioned derived indexes can always be rebuilt from the durable note files.
// Bumping the filename prevents an older firmware's layout from being mistaken
// for the current one after an update.
constexpr const char* kSearchIndexPath = NOTES_DIR "/brain.search.v3";
constexpr const char* kVectorIndexPath = NOTES_DIR "/brain.vectors.v2";
constexpr uint16_t kEmbeddingDimensions = 256;

String transcriptPath(int noteNum) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.txt", NOTES_DIR, noteNum);
  return String(path);
}

String embeddingPath(int noteNum) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.brain.vec", NOTES_DIR, noteNum);
  return String(path);
}

String readFilePrefix(const String& path, size_t maxLen) {
  File file = sdOpenRead(path);
  if (!file) return "";
  String text;
  text.reserve(min(maxLen, static_cast<size_t>(4096)));
  while (file.available() && text.length() < maxLen) text += static_cast<char>(file.read());
  file.close();
  return String(utf8Excerpt(std::string(text.c_str(), text.length()), 0, text.length()).c_str());
}

bool explicitlyMeansTomorrow(const String& transcript) {
  String lower = transcript;
  lower.toLowerCase();
  return lower.indexOf("fuer morgen") >= 0 || lower.indexOf("für morgen") >= 0 ||
         lower.indexOf("morgen um") >= 0 || lower.indexOf("morgen gegen") >= 0 ||
         lower.indexOf("morgen habe") >= 0 || lower.indexOf("morgen ist") >= 0 ||
         lower.indexOf("tomorrow") >= 0;
}

String nextCalendarDay(const String& localLabel) {
  if (localLabel.length() < 10) return "";
  struct tm date = {};
  date.tm_year = localLabel.substring(0, 4).toInt() - 1900;
  date.tm_mon = localLabel.substring(5, 7).toInt() - 1;
  date.tm_mday = localLabel.substring(8, 10).toInt();
  time_t next = utcTmToEpoch(date) + 86400;
  struct tm result;
  gmtime_r(&next, &result);
  char day[11];
  strftime(day, sizeof(day), "%Y-%m-%d", &result);
  return String(day);
}

String sourceFingerprint(int noteNum) {
  File file = sdOpenRead(transcriptPath(noteNum));
  if (!file) return "";
  uint32_t hash = 2166136261UL;
  while (file.available()) {
    hash ^= static_cast<uint8_t>(file.read());
    hash *= 16777619UL;
  }
  file.close();
  char encoded[9];
  snprintf(encoded, sizeof(encoded), "%08lx", static_cast<unsigned long>(hash));
  return String(encoded);
}

void readStrings(JsonArrayConst source, std::vector<String>& target) {
  target.clear();
  for (JsonVariantConst item : source) {
    if (!item.is<const char*>()) continue;
    String value = item.as<String>();
    value.trim();
    if (value.length()) target.push_back(value);
  }
}

String normalizedTaskIdentity(String value) {
  value.toLowerCase();
  value.replace("ä", "a"); value.replace("ö", "o"); value.replace("ü", "u");
  value.replace("ß", "ss");
  return String(normalizedTaskUtf8(value.c_str()).c_str());
}

void readTasks(JsonArrayConst source, BrainMemory& memory) {
  memory.tasks.clear(); memory.taskIds.clear(); memory.taskDueUtc.clear(); memory.taskTypes.clear();
  memory.taskDatePrecision.clear(); memory.taskOriginalTimeText.clear(); memory.taskConfidence.clear();
  for (JsonVariantConst item : source) {
    String id, title, due, type = "todo", precision = "none", originalTime;
    int confidence = 50;
    if (item.is<const char*>()) title = item.as<String>();
    else if (item.is<JsonObjectConst>()) {
      id = item["id"] | "";
      title = item["title"] | "";
      due = item["dueUtc"] | "";
      type = item["type"] | "todo";
      precision = item["datePrecision"] | "none";
      originalTime = item["originalTimeText"] | "";
      confidence = constrain(item["confidence"] | 50, 0, 100);
    }
    id.trim(); title.trim(); due.trim(); type.trim();
    if (type != "event" && type != "reminder") type = "todo";
    if (precision != "day" && precision != "time") precision = "none";
    if (!due.length()) precision = "none";
    if (title.length()) {
      // Collapse exact duplicate events, never distinct appointments that
      // happen to share their time phrase or scheduled day.
      int duplicate = -1;
      const String identity = normalizedTaskIdentity(title);
      if (type == "event" && due.length()) {
        for (int i = 0; i < static_cast<int>(memory.tasks.size()); i++) {
          if (memory.taskTypes[i] != "event" || memory.taskDueUtc[i] != due) continue;
          const String previousIdentity = normalizedTaskIdentity(memory.tasks[i]);
          // Two different appointments can legitimately share "tomorrow"
          // and midnight. Only the same title/date/type is a safe duplicate.
          if (sameTaskIdentity(identity.c_str(), previousIdentity.c_str(), type.c_str(),
                               memory.taskTypes[i].c_str(), due.c_str(), memory.taskDueUtc[i].c_str())) {
            duplicate = i;
            break;
          }
        }
      }
      if (duplicate >= 0) {
        if (title.length() > memory.tasks[duplicate].length()) memory.tasks[duplicate] = title;
        if (originalTime.length() > memory.taskOriginalTimeText[duplicate].length())
          memory.taskOriginalTimeText[duplicate] = originalTime;
        memory.taskConfidence[duplicate] = max(memory.taskConfidence[duplicate], confidence);
        if (precision == "time") memory.taskDatePrecision[duplicate] = precision;
        continue;
      }
      memory.tasks.push_back(title);
      // Older memories did not persist task IDs. Keep their former derived ID
      // as a migration fallback so completed-state files remain compatible.
      if (!id.length()) {
        uint32_t hash = 2166136261UL;
        for (size_t c = 0; c < title.length(); c++) {
          hash ^= static_cast<uint8_t>(title[c]);
          hash *= 16777619UL;
        }
        char legacy[24];
        snprintf(legacy, sizeof(legacy), "%d-%08lx", memory.noteNum,
                 static_cast<unsigned long>(hash));
        id = legacy;
      }
      memory.taskIds.push_back(id);
      memory.taskDueUtc.push_back(due);
      memory.taskTypes.push_back(type);
      memory.taskDatePrecision.push_back(precision);
      memory.taskOriginalTimeText.push_back(originalTime);
      memory.taskConfidence.push_back(confidence);
    }
  }
}

void appendSearchable(String& target, const std::vector<String>& values) {
  for (const String& value : values) target += " " + value;
}

String searchable(const BrainMemory& memory) {
  String text = memory.title + " " + memory.summary + " " + memory.kind;
  appendSearchable(text, memory.topics);
  appendSearchable(text, memory.people);
  appendSearchable(text, memory.projects);
  appendSearchable(text, memory.tasks);
  appendSearchable(text, memory.decisions);
  appendSearchable(text, memory.facts);
  appendSearchable(text, memory.questions);
  text.toLowerCase();
  return text;
}

bool isStopWord(const String& term) {
  static const char* words[] = {
    "aber", "alle", "alles", "auch", "eine", "einem", "einen", "einer", "eines",
    "fuer", "habe", "haben", "hatte", "ich", "ist", "mein", "meine", "mich", "mit",
    "oder", "sich", "sind", "ueber", "und", "von", "war", "was", "wenn", "wie", "wir",
    "the", "and", "for", "from", "have", "that", "this", "what", "when", "with"
  };
  for (const char* word : words) if (term == word) return true;
  return false;
}

std::vector<String> queryTerms(String question) {
  question.toLowerCase();
  question.replace("ä", "ae");
  question.replace("ö", "oe");
  question.replace("ü", "ue");
  question.replace("ß", "ss");
  for (size_t i = 0; i < question.length(); i++) {
    const unsigned char c = static_cast<unsigned char>(question[i]);
    if (c < 128 && !isalnum(c)) question.setCharAt(i, ' ');
  }

  std::vector<String> terms;
  int start = 0;
  while (start < static_cast<int>(question.length())) {
    while (start < static_cast<int>(question.length()) && question[start] == ' ') start++;
    int end = question.indexOf(' ', start);
    if (end < 0) end = question.length();
    String term = question.substring(start, end);
    if (term.length() >= 3 && !isStopWord(term) &&
        std::find(terms.begin(), terms.end(), term) == terms.end()) {
      terms.push_back(term);
      // Bridge common natural-language wording to the normalized memory kinds.
      // This keeps exact local retrieval useful even before semantic ranking.
      if (term == "idee" || term == "ideen") terms.push_back("idea");
      else if (term.startsWith("aufgab") || term == "todo") terms.push_back("task");
      else if (term.startsWith("termin") || term == "appointment") terms.push_back("event");
      else if (term.startsWith("entscheid")) terms.push_back("decision");
      else if (term.startsWith("erinner")) terms.push_back("reminder");
    }
    start = end + 1;
  }
  return terms;
}

int occurrences(const String& text, const String& term, int cap) {
  int count = 0;
  int offset = 0;
  while (count < cap) {
    offset = text.indexOf(term, offset);
    if (offset < 0) break;
    count++;
    offset += term.length();
  }
  return count;
}

bool asksForRecent(const String& question) {
  String lower = question;
  lower.toLowerCase();
  return lower.indexOf("zuletzt") >= 0 || lower.indexOf("letzte") >= 0 ||
         lower.indexOf("gerade") >= 0 || lower.indexOf("eben") >= 0 ||
         lower.indexOf("soeben") >= 0 || lower.indexOf("heute") >= 0 ||
         lower.indexOf("recent") >= 0 ||
         lower.indexOf("latest") >= 0;
}

String relevantExcerpt(int noteNum, const std::vector<String>& terms) {
  String transcript = readFilePrefix(transcriptPath(noteNum), kSearchTranscriptBytes);
  if (transcript.length() <= kAnswerExcerptBytes) return transcript;
  String lower = transcript;
  lower.toLowerCase();
  int match = -1;
  for (const String& term : terms) {
    match = lower.indexOf(term);
    if (match >= 0) break;
  }
  if (match < 0) return String(utf8Excerpt(std::string(transcript.c_str(), transcript.length()), 0, kAnswerExcerptBytes).c_str());
  int start = max(0, match - static_cast<int>(kAnswerExcerptBytes / 3));
  int end = min(static_cast<int>(transcript.length()), start + static_cast<int>(kAnswerExcerptBytes));
  return String(utf8Excerpt(std::string(transcript.c_str(), transcript.length()), start, end).c_str());
}

bool replaceAtomically(const String& path, const String& temp) {
  return commitStoredFile(SD_MMC, path, temp);
}

std::vector<float> loadEmbedding(int noteNum) {
  std::vector<float> values;
  recoverStoredFile(SD_MMC, embeddingPath(noteNum));
  File file = sdOpenRead(embeddingPath(noteNum));
  if (!file) return values;
  uint16_t dimensions = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&dimensions), sizeof(dimensions)) != sizeof(dimensions) ||
      dimensions == 0 || dimensions > 512) {
    file.close();
    return values;
  }
  values.resize(dimensions);
  const size_t bytes = dimensions * sizeof(float);
  if (file.size() != sizeof(dimensions) + bytes ||
      file.read(reinterpret_cast<uint8_t*>(values.data()), bytes) != bytes) values.clear();
  for (float value : values) {
    if (!std::isfinite(value)) { values.clear(); break; }
  }
  file.close();
  return values;
}

float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || a.size() != b.size()) return -1.0f;
  float dot = 0.0f;
  float normA = 0.0f;
  float normB = 0.0f;
  for (size_t i = 0; i < a.size(); i++) {
    dot += a[i] * b[i];
    normA += a[i] * a[i];
    normB += b[i] * b[i];
  }
  if (normA <= 0.0f || normB <= 0.0f) return -1.0f;
  return dot / sqrtf(normA * normB);
}

String searchIndexLine(const BrainMemory& memory) {
  String text = searchable(memory);
  String transcript = readFilePrefix(transcriptPath(memory.noteNum), 2000);
  transcript.toLowerCase();
  text += " " + transcript;
  text.replace("|", " "); text.replace("\n", " "); text.replace("\r", " ");
  return String(memory.noteNum) + "|" + memory.createdUtc + "|" + String(memory.importance) + "|" + text;
}

bool rewriteSearchIndex(int replacementNum, const String* replacement) {
  if (!ensureBrainSearchIndex()) return false;
  const String temp = String(kSearchIndexPath) + ".tmp";
  sdRemoveIfExists(temp.c_str());
  File output = SD_MMC.open(temp.c_str(), FILE_WRITE);
  if (!output) return false;
  size_t expected = 0;
  bool ok = true;
  File input = sdOpenRead(kSearchIndexPath);
  if (!input) { output.close(); return false; }
  if (input) {
    while (input.available()) {
      String line = input.readStringUntil('\n'); line.trim();
      int separator = line.indexOf('|');
      if (separator <= 0 || line.substring(0, separator).toInt() == replacementNum) continue;
      line += "\n";
      if (!checkedFileWrite(output, (const uint8_t*)line.c_str(), line.length(), expected)) { ok = false; break; }
    }
    input.close();
  }
  if (ok && replacement && replacement->length()) {
    const String line = *replacement + "\n";
    ok = checkedFileWrite(output, (const uint8_t*)line.c_str(), line.length(), expected);
  }
  ok = checkedFileComplete(output, expected) && ok;
  output.close();
  return ok && replaceAtomically(kSearchIndexPath, temp);
}

bool rewriteVectorIndex(int replacementNum, const std::vector<float>* replacement) {
  if (!ensureBrainVectorIndex()) return false;
  const String temp = String(kVectorIndexPath) + ".tmp";
  sdRemoveIfExists(temp.c_str());
  File output=SD_MMC.open(temp.c_str(),FILE_WRITE);
  if(!output) return false;
  File input=sdOpenRead(kVectorIndexPath);
  if (!input) { output.close(); return false; }
  size_t expected = 0;
  bool ok = true;
  const size_t vectorBytes=kEmbeddingDimensions*sizeof(float);
  std::vector<float> buffer(kEmbeddingDimensions);
  while(input && input.available() >= (int)(sizeof(int32_t)+vectorBytes)) {
    int32_t noteNum=0;
    if(input.read((uint8_t*)&noteNum,sizeof(noteNum))!=sizeof(noteNum) ||
       input.read((uint8_t*)buffer.data(),vectorBytes)!=vectorBytes) { ok = false; break; }
    if(noteNum==replacementNum) continue;
    if (!checkedFileWrite(output, (uint8_t*)&noteNum, sizeof(noteNum), expected) ||
        !checkedFileWrite(output, (uint8_t*)buffer.data(), vectorBytes, expected)) { ok = false; break; }
  }
  if (input && input.available()) ok = false;
  if(input) input.close();
  if(ok && replacement && replacement->size()==kEmbeddingDimensions) {
    int32_t noteNum=replacementNum;
    ok = checkedFileWrite(output, (uint8_t*)&noteNum, sizeof(noteNum), expected) &&
         checkedFileWrite(output, (const uint8_t*)replacement->data(), vectorBytes, expected);
  }
  ok = checkedFileComplete(output, expected) && ok;
  output.close();
  return ok && replaceAtomically(kVectorIndexPath,temp);
}

std::vector<std::pair<int,float>> vectorSimilarities(const std::vector<float>& query) {
  std::vector<std::pair<int,float>> scores;
  if(query.size()!=kEmbeddingDimensions) return scores;
  File file=sdOpenRead(kVectorIndexPath);
  const size_t bytes=kEmbeddingDimensions*sizeof(float);
  std::vector<float> value(kEmbeddingDimensions);
  while(file && file.available() >= (int)(sizeof(int32_t)+bytes)) {
    int32_t noteNum=0;
    if(file.read((uint8_t*)&noteNum,sizeof(noteNum))!=sizeof(noteNum)) break;
    if(file.read((uint8_t*)value.data(),bytes)!=bytes) break;
    scores.push_back({(int)noteNum,cosineSimilarity(query,value)});
  }
  if(file) file.close();
  std::sort(scores.begin(),scores.end(),[](const auto&a,const auto&b){return a.first<b.first;});
  return scores;
}

float indexedSimilarity(const std::vector<std::pair<int,float>>& scores,int noteNum) {
  auto it=std::lower_bound(scores.begin(),scores.end(),noteNum,
    [](const auto& item,int value){return item.first<value;});
  return it!=scores.end()&&it->first==noteNum?it->second:-1.0f;
}

}  // namespace

String brainMemoryPath(int noteNum) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.brain.json", NOTES_DIR, noteNum);
  return String(path);
}

String brainEmbeddingText(const BrainMemory& memory) {
  String text = memory.title + "\n" + memory.summary + "\nType: " + memory.kind;
  auto append = [&text](const char* label, const std::vector<String>& values) {
    if (values.empty()) return;
    text += "\n" + String(label) + ": ";
    for (size_t i = 0; i < values.size(); i++) {
      if (i) text += ", ";
      text += values[i];
    }
  };
  append("Topics", memory.topics);
  append("People", memory.people);
  append("Projects", memory.projects);
  append("Tasks", memory.tasks);
  for (size_t i = 0; i < memory.tasks.size(); i++) {
    text += "\nTask metadata: " + memory.taskTypes[i];
    if (i < memory.taskDueUtc.size() && memory.taskDueUtc[i].length()) text += " due " + memory.taskDueUtc[i];
  }
  append("Decisions", memory.decisions);
  append("Facts", memory.facts);
  append("Questions", memory.questions);
  return text;
}

bool saveBrainEmbedding(int noteNum, const std::vector<float>& embedding) {
  if (embedding.empty() || embedding.size() > 512) return false;
  const String path = embeddingPath(noteNum);
  const String temp = path + ".tmp";
  sdRemoveIfExists(temp);
  File file = SD_MMC.open(temp, FILE_WRITE);
  if (!file) return false;
  const uint16_t dimensions = static_cast<uint16_t>(embedding.size());
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&dimensions), sizeof(dimensions)) == sizeof(dimensions);
  const size_t bytes = embedding.size() * sizeof(float);
  ok = ok && file.write(reinterpret_cast<const uint8_t*>(embedding.data()), bytes) == bytes;
  file.flush();
  ok = ok && file.size() == sizeof(dimensions) + bytes;
  file.close();
  if (!ok || !replaceAtomically(path, temp)) {
    sdRemoveIfExists(temp);
    return false;
  }
  rewriteVectorIndex(noteNum,&embedding);
  return true;
}

bool brainEmbeddingExists(int noteNum) {
  File file = sdOpenRead(embeddingPath(noteNum));
  if (!file) return false;
  uint16_t dimensions = 0;
  const bool valid = file.read(reinterpret_cast<uint8_t*>(&dimensions), sizeof(dimensions)) ==
                       sizeof(dimensions) &&
                     dimensions > 0 && dimensions <= 512 &&
                     file.size() == sizeof(dimensions) + dimensions * sizeof(float);
  file.close();
  return valid;
}

bool brainHasSemanticIndex() {
  if (!ensureBrainVectorIndex()) return false;
  File file = sdOpenRead(kVectorIndexPath);
  if (!file) return false;
  const size_t minimumRecord = sizeof(int32_t) + kEmbeddingDimensions * sizeof(float);
  const bool available = file.size() >= minimumRecord && file.size() % minimumRecord == 0;
  file.close();
  return available;
}

bool saveBrainMemoryJson(int noteNum, const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  // Do not overwrite a usable memory with a malformed or empty model result.
  if (!validMemoryBody(doc.as<JsonVariantConst>())) return false;

  // Preserve identity for the same title, date and type across re-analysis.
  // A genuinely different item must not inherit another item's completion.
  const BrainMemory previous = loadBrainMemory(noteNum);
  doc["noteNum"] = noteNum;
  doc["schemaVersion"] = kBrainSchemaVersion;
  doc["createdUtc"] = noteCreatedUtc(noteNum);
  doc["sourceHash"] = sourceFingerprint(noteNum);
  doc["importance"] = constrain(doc["importance"] | 1, 0, 3);
  // Fail closed on malformed or low-confidence dates. An undated item is much
  // safer than silently placing it on the wrong day.
  int taskIndex = 0;
  std::vector<bool> previousUsed(previous.tasks.size(), false);
  for (JsonObject task : doc["tasks"].as<JsonArray>()) {
    const String title = task["title"] | "";
    String due = task["dueUtc"] | "";
    due.trim();
    const bool isoUtc = validUtcTimestamp(due.c_str());
    const int confidence = constrain(task["confidence"] | 50, 0, 100);
    task["confidence"] = confidence;
    String precision = task["datePrecision"] | "none";
    // Relative dates belong to this item, never to all items in the note.
    const String originalTime = task["originalTimeText"] | "";
    const String capturedLocal = utcToLocalDeviceLabel(doc["createdUtc"].as<String>());
    const String dueLocal = utcToLocalDeviceLabel(due);
    const String expectedDay = capturedLocal.length() >= 10 && capturedLocal != "time not set"
      ? nextCalendarDay(capturedLocal) : "";
    const bool wrongTomorrow = itemRelativeDateMismatch(explicitlyMeansTomorrow(originalTime),
      dueLocal.substring(0, 10).c_str(), expectedDay.c_str());
    if (!isoUtc || confidence < 55 || wrongTomorrow || (precision != "day" && precision != "time")) {
      task["dueUtc"] = "";
      task["datePrecision"] = "none";
    }

    String persistentId;
    const String type = task["type"] | "todo";
    const String identity = normalizedTaskIdentity(title);
    for (int old = 0; old < static_cast<int>(previous.tasks.size()); old++) {
      if (previousUsed[old]) continue;
      const String oldType = old < static_cast<int>(previous.taskTypes.size())
                               ? previous.taskTypes[old] : "todo";
      const String oldIdentity = normalizedTaskIdentity(previous.tasks[old]);
      const String oldDue = old < static_cast<int>(previous.taskDueUtc.size()) ? previous.taskDueUtc[old] : "";
      const String newDue = task["dueUtc"] | "";
      // Two distinct commitments can share a date. Do not transfer a completed
      // state to another task merely because both are due at midnight.
      if (!sameTaskIdentity(identity.c_str(), oldIdentity.c_str(), type.c_str(), oldType.c_str(),
                            newDue.c_str(), oldDue.c_str())) continue;
      if (old < static_cast<int>(previous.taskIds.size())) persistentId = previous.taskIds[old];
      previousUsed[old] = true;
      break;
    }
    if (!persistentId.length()) {
      char generated[28];
      snprintf(generated, sizeof(generated), "%d-t%02d-%08lx", noteNum, taskIndex,
               static_cast<unsigned long>(esp_random()));
      persistentId = generated;
    }
    task["id"] = persistentId;
    taskIndex++;
  }

  const String path = brainMemoryPath(noteNum);
  const String temp = path + ".tmp";
  sdRemoveIfExists(temp);
  File file = SD_MMC.open(temp, FILE_WRITE);
  if (!file) return false;
  const size_t expected = measureJson(doc);
  const size_t count = serializeJson(doc, file);
  file.flush();
  const bool written = count == expected && file.size() == expected;
  file.close();
  if (!written || !replaceAtomically(path, temp)) {
    sdRemoveIfExists(temp);
    return false;
  }
  BrainMemory memory = loadBrainMemory(noteNum);
  if (memory.valid) {
    String line = searchIndexLine(memory);
    rewriteSearchIndex(noteNum, &line);
  }
  return memory.valid;
}

BrainMemory loadBrainMemory(int noteNum) {
  BrainMemory memory;
  memory.noteNum = noteNum;
  recoverStoredFile(SD_MMC, brainMemoryPath(noteNum));
  File file = sdOpenRead(brainMemoryPath(noteNum));
  if (!file) return memory;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) return memory;

  memory.schemaVersion = doc["schemaVersion"] | 1;
  memory.importance = constrain(doc["importance"] | 1, 0, 3);
  memory.title = doc["title"] | "";
  memory.summary = doc["summary"] | "";
  memory.kind = doc["kind"] | "thought";
  memory.language = doc["language"] | "";
  memory.createdUtc = doc["createdUtc"] | noteCreatedUtc(noteNum);
  memory.sourceHash = doc["sourceHash"] | "";
  readStrings(doc["topics"].as<JsonArrayConst>(), memory.topics);
  readStrings(doc["people"].as<JsonArrayConst>(), memory.people);
  readStrings(doc["projects"].as<JsonArrayConst>(), memory.projects);
  readTasks(doc["tasks"].as<JsonArrayConst>(), memory);
  readStrings(doc["decisions"].as<JsonArrayConst>(), memory.decisions);
  readStrings(doc["facts"].as<JsonArrayConst>(), memory.facts);
  readStrings(doc["questions"].as<JsonArrayConst>(), memory.questions);
  memory.valid = memory.title.length() > 0 && memory.summary.length() > 0;
  return memory;
}

bool brainMemoryIsCurrent(int noteNum) {
  BrainMemory memory = loadBrainMemory(noteNum);
  if (!memory.valid) return false;
  if (memory.schemaVersion < kBrainSchemaVersion) return false;
  if (memory.sourceHash.length() == 0) return true;
  return memory.sourceHash == sourceFingerprint(noteNum);
}

bool ensureBrainSearchIndex() {
  recoverStoredFile(SD_MMC, String(kSearchIndexPath));
  if (SD_MMC.exists(kSearchIndexPath)) return true;
  const String temp = String(kSearchIndexPath) + ".tmp";
  sdRemoveIfExists(temp.c_str());
  File output = SD_MMC.open(temp.c_str(), FILE_WRITE);
  if (!output) return false;
  size_t expected = 0;
  bool ok = true;
  for (const NoteEntry& note : noteIndex) {
    BrainMemory memory = loadBrainMemory(note.num);
    if (!memory.valid) continue;
    const String line = searchIndexLine(memory) + "\n";
    if (!checkedFileWrite(output, (const uint8_t*)line.c_str(), line.length(), expected)) { ok = false; break; }
  }
  ok = checkedFileComplete(output, expected) && ok;
  output.close();
  return ok && replaceAtomically(kSearchIndexPath, temp);
}

bool ensureBrainVectorIndex() {
  recoverStoredFile(SD_MMC, String(kVectorIndexPath));
  if(SD_MMC.exists(kVectorIndexPath)) return true;
  const String temp=String(kVectorIndexPath)+".tmp";
  sdRemoveIfExists(temp.c_str());
  File output=SD_MMC.open(temp.c_str(),FILE_WRITE);
  if(!output) return false;
  const size_t bytes=kEmbeddingDimensions*sizeof(float);
  size_t expected = 0;
  bool ok = true;
  for(const NoteEntry& note:noteIndex) {
    std::vector<float> embedding=loadEmbedding(note.num);
    if(embedding.size()!=kEmbeddingDimensions) continue;
    int32_t noteNum=note.num;
    if (!checkedFileWrite(output, (uint8_t*)&noteNum, sizeof(noteNum), expected) ||
        !checkedFileWrite(output, (const uint8_t*)embedding.data(), bytes, expected)) { ok = false; break; }
  }
  ok = checkedFileComplete(output, expected) && ok;
  output.close();
  return ok && replaceAtomically(kVectorIndexPath,temp);
}

void removeBrainSearchIndexEntry(int noteNum) { rewriteSearchIndex(noteNum, nullptr); }

void removeBrainVectorIndexEntry(int noteNum) { rewriteVectorIndex(noteNum, nullptr); }

int pendingBrainMemoryCount() {
  int pending = 0;
  for (const NoteEntry& note : noteIndex) {
    // Embeddings improve semantic ranking but are optional derived data. They
    // must never keep the device awake or pull Home into a processing loop.
    if (!note.hasText && minutesNoteAwaitingServer(note.num)) continue;  // server supplies the text
    if (noteQueueState(note.num) != "blocked" &&
        (!note.hasText || !brainMemoryIsCurrent(note.num))) pending++;
  }
  return pending;
}

BrainStats readBrainStats() {
  BrainStats stats;
  stats.totalBytes = SD_MMC.totalBytes();
  stats.usedBytes = SD_MMC.usedBytes();
  stats.memories = static_cast<int>(noteIndex.size());
  for (const NoteEntry& note : noteIndex) {
    stats.moments += note.momentCount;
    BrainMemory memory = loadBrainMemory(note.num);
    // An embedding improves semantic recall but is a derived background index.
    // A current structured memory is already fully usable and must not appear
    // as pending merely because that optional index is still catching up.
    const bool usable = note.hasText && memory.valid && brainMemoryIsCurrent(note.num);
    if (usable) stats.understood++;
    else stats.pending++;
  }
  stats.openTasks = openBrainTaskCount();
  return stats;
}

std::vector<BrainMemory> retrieveBrainMemories(
    const String& question, size_t limit, const std::vector<float>* queryEmbedding) {
  struct ScoredMemory { float score; BrainMemory memory; };
  std::vector<ScoredMemory> scored;
  const std::vector<String> terms = queryTerms(question);
  const bool recentIntent = asksForRecent(question);

  // Normal queries scan one compact file and only open the few winning JSON
  // records. This avoids thousands of FAT directory lookups as the brain grows.
  if (ensureBrainSearchIndex()) {
    struct IndexedScore { float score; int noteNum; };
    std::vector<IndexedScore> best;
    if(queryEmbedding) ensureBrainVectorIndex();
    const std::vector<std::pair<int,float>> semanticScores =
      queryEmbedding ? vectorSimilarities(*queryEmbedding) : std::vector<std::pair<int,float>>{};
    File file = sdOpenRead(kSearchIndexPath);
    while (file && file.available()) {
      String line = file.readStringUntil('\n'); line.trim();
      int p1=line.indexOf('|'), p2=line.indexOf('|',p1+1), p3=line.indexOf('|',p2+1);
      if (p1<=0||p2<=p1||p3<=p2) continue;
      const int noteNum=line.substring(0,p1).toInt();
      float score=line.substring(p2+1,p3).toInt();
      const String text=line.substring(p3+1);
      int matched=0;
      for (const String& term : terms) {
        const int hits=occurrences(text,term,5);
        if(hits) matched++;
        score += hits*5;
      }
      if (!terms.empty() && matched==(int)terms.size()) score += 12;
      if (recentIntent) score += noteNum*0.001f;
      float semantic=-1.0f;
      if (queryEmbedding && !queryEmbedding->empty()) {
        semantic=indexedSimilarity(semanticScores,noteNum);
        if(semantic>=0) score += semantic*100.0f;
      }
      if (matched>0 || recentIntent || terms.empty() || semantic>=0.25f) {
        best.push_back({score,noteNum});
        if(best.size()>32) {
          std::sort(best.begin(),best.end(),[](const IndexedScore&a,const IndexedScore&b){return a.score>b.score;});
          best.resize(24);
        }
      }
    }
    if(file) file.close();
    std::sort(best.begin(),best.end(),[](const IndexedScore&a,const IndexedScore&b){return a.score!=b.score?a.score>b.score:a.noteNum>b.noteNum;});
    std::vector<BrainMemory> result;
    for(const IndexedScore& candidate:best) {
      if(result.size()>=limit) break;
      BrainMemory memory=loadBrainMemory(candidate.noteNum);
      if(!memory.valid) continue;
      memory.transcriptExcerpt=relevantExcerpt(memory.noteNum,terms);
      result.push_back(memory);
    }
    if(!result.empty()) return result;
  }

  for (size_t index = 0; index < noteIndex.size(); index++) {
    BrainMemory memory = loadBrainMemory(noteIndex[index].num);
    if (!memory.valid) {
      String raw = readFilePrefix(transcriptPath(noteIndex[index].num), kSearchTranscriptBytes);
      raw.trim();
      if (raw.length() == 0) continue;
      memory.noteNum = noteIndex[index].num;
      memory.title = "Memory #" + String(memory.noteNum);
      memory.summary = raw.substring(0, min(static_cast<int>(raw.length()), 700));
      memory.kind = "thought";
      memory.createdUtc = noteCreatedUtc(memory.noteNum);
      memory.valid = true;
    }

    String title = memory.title;
    title.toLowerCase();
    String structured = searchable(memory);
    String transcript = readFilePrefix(transcriptPath(memory.noteNum), kSearchTranscriptBytes);
    transcript.toLowerCase();

    float score = memory.importance;
    int matchedTerms = 0;
    for (const String& term : terms) {
      int termScore = occurrences(title, term, 2) * 10;
      termScore += occurrences(structured, term, 4) * 4;
      termScore += occurrences(transcript, term, 3) * 2;
      if (termScore > 0) matchedTerms++;
      score += termScore;
    }
    if (!terms.empty() && matchedTerms == static_cast<int>(terms.size())) score += 12;
    if (recentIntent) score += static_cast<int>((index * 8) / max<size_t>(1, noteIndex.size()));
    float semantic = -1.0f;
    if (queryEmbedding && !queryEmbedding->empty()) {
      semantic = cosineSimilarity(*queryEmbedding, loadEmbedding(memory.noteNum));
      if (semantic >= 0.0f) score += semantic * 100.0f;
    }
    if (matchedTerms > 0 || recentIntent || terms.empty() || semantic >= 0.25f) {
      scored.push_back({score, memory});
    }
  }

  std::sort(scored.begin(), scored.end(), [](const ScoredMemory& a, const ScoredMemory& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.memory.noteNum > b.memory.noteNum;
  });

  std::vector<BrainMemory> result;
  for (size_t i = 0; i < scored.size() && result.size() < limit; i++) {
    scored[i].memory.transcriptExcerpt = relevantExcerpt(scored[i].memory.noteNum, terms);
    result.push_back(scored[i].memory);
  }
  return result;
}


// ─── One-time time-zone correction ──────────────────────────────────────────
// Earlier builds used German time (CET/CEST) as the device's local
// time, so a spoken "11am" was stored as 11:00 German time. The device now
// runs on Singapore time; move every stored due time so it keeps the wall-clock
// time that was said (11:00 CET -> 11:00 SGT). Runs once per SD card.
static bool cestAt(long long epoch) {
  // CEST: last Sunday of March 01:00 UTC until last Sunday of October 01:00 UTC.
  const long day = static_cast<long>(epoch / 86400);
  int y, m, d;
  agenda::civilFromDays(day, y, m, d);
  auto lastSunday = [&](int month) {
    const long last = agenda::daysFromCivil(month == 12 ? y + 1 : y, month == 12 ? 1 : month + 1, 1) - 1;
    return last - ((agenda::weekdayMon0(last) + 1) % 7);   // Monday=0 ... Sunday=6
  };
  const long long start = static_cast<long long>(lastSunday(3)) * 86400 + 3600;
  const long long end = static_cast<long long>(lastSunday(10)) * 86400 + 3600;
  return epoch >= start && epoch < end;
}

static String isoFromEpoch(long long epoch) {
  int y, m, d;
  agenda::civilFromDays(static_cast<long>(epoch / 86400), y, m, d);
  const long secs = static_cast<long>(epoch % 86400);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02ld:%02ld:%02ldZ", y, m, d, secs / 3600, (secs / 60) % 60, secs % 60);
  return String(buf);
}

void migrateDueTimesToSingapore() {
  const char* marker = NOTES_DIR "/.tz_sgt";
  if (SD_MMC.exists(marker)) return;
  int moved = 0, failed = 0;
  for (const NoteEntry& note : noteIndex) {
    const String path = brainMemoryPath(note.num);
    File in = sdOpenRead(path.c_str());
    if (!in) continue;
    JsonDocument doc;
    const bool parsed = !deserializeJson(doc, in);
    in.close();
    if (!parsed) continue;
    bool changed = false;
    for (JsonObject task : doc["tasks"].as<JsonArray>()) {
      const String due = task["dueUtc"] | "";
      if (!validUtcTimestamp(due.c_str())) continue;
      const long long epoch = static_cast<long long>(agenda::daysFromCivil(due.substring(0, 4).toInt(),
          due.substring(5, 7).toInt(), due.substring(8, 10).toInt())) * 86400 +
          due.substring(11, 13).toInt() * 3600 + due.substring(14, 16).toInt() * 60 +
          (due.length() >= 19 ? due.substring(17, 19).toInt() : 0);
      const int germanOffset = cestAt(epoch) ? 7200 : 3600;
      task["dueUtc"] = isoFromEpoch(epoch + germanOffset - 8 * 3600);
      changed = true;
      moved++;
    }
    if (!changed) continue;
    const String temp = path + ".tmp";
    sdRemoveIfExists(temp.c_str());
    File out = SD_MMC.open(temp.c_str(), FILE_WRITE);
    const size_t expected = measureJson(doc);
    const bool ok = out && serializeJson(doc, out) == expected;
    if (out) { out.flush(); out.close(); }
    if (!ok || !replaceAtomically(path, temp)) { sdRemoveIfExists(temp.c_str()); failed++; }
  }
  if (failed) {
    Serial.printf("[Time] moving due times to Singapore time: %d files failed, will retry\n", failed);
    return;
  }
  File done = SD_MMC.open(marker, FILE_WRITE);
  if (done) { done.print("Agenda times converted from German time to Singapore time\n"); done.close(); }
  storageChanged();
  Serial.printf("[Time] %d agenda times moved to Singapore time\n", moved);
}
