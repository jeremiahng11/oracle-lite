#include "Arduino.h"
#include "sd_read.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "notes.h"
#include "rtc.h"
#include "draw.h"
#include "brain.h"
#include "SD_MMC.h"
#include <algorithm>
#include <map>
#include "retry_policy.h"
#include "storage_commit.h"

// ─── Index ────────────────────────────────────────────────────────────────

namespace {
constexpr const char* kIndexTemp = "/notes/index.tmp";
constexpr const char* kIndexBackup = "/notes/index.bak";

bool replaceNoteFileAtomically(const String& path, const String& temp) {
  return commitStoredFile(sdFs, path, temp);
}
}

void loadIndex(bool recoverOrphans) {
  noteIndex.clear();
  // Recover the last complete catalog after an interrupted FAT rename.
  if (!sdExists(INDEX_FILE)) {
    if (sdExists(kIndexBackup)) sdRename(kIndexBackup, INDEX_FILE);
    else if (sdExists(kIndexTemp)) sdRename(kIndexTemp, INDEX_FILE);
  }
  File f = sdOpenRead(INDEX_FILE);
  if (f) {
    while (f.available()) {
      String ln = f.readStringUntil('\n'); ln.trim();
      if (!ln.length()) continue;
      int c1=ln.indexOf(','), c2=ln.indexOf(',',c1+1), c3=ln.indexOf(',',c2+1);
      if (c1<0||c2<0) continue;
      NoteEntry e = {};
      e.num = ln.substring(0,c1).toInt();
      if (e.num <= 0) continue;
      strncpy(e.tag, ln.substring(c1+1,c2).c_str(), sizeof(e.tag)-1);
      e.tag[sizeof(e.tag)-1]='\0';
      e.hasText = (ln.substring(c2+1, c3 >= 0 ? c3 : ln.length()).toInt()==1);
      e.momentCount = c3 >= 0 ? (uint16_t)ln.substring(c3+1).toInt() : 0;
      noteIndex.push_back(e);
    }
    f.close();
  }

  // The WAV is the source of truth. Reinsert an orphaned recording if power
  // failed after audio close but before the small catalog was committed.
  bool repaired = false;
  File directory;
  if (recoverOrphans) directory = sdOpenRead(NOTES_DIR);
  if (directory && directory.isDirectory()) {
    File entry;
    while ((entry = directory.openNextFile())) {
      String name = entry.name();
      const bool usableAudio = !entry.isDirectory() && entry.size() > 1044;
      entry.close();
      const int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith("note_") || !name.endsWith(".wav")) continue;
      if (!usableAudio) continue;
      const int num = name.substring(5, name.length() - 4).toInt();
      if (num <= 0) continue;
      bool known = false;
      for (const NoteEntry& item : noteIndex) if (item.num == num) { known = true; break; }
      if (known) continue;
      NoteEntry recovered = {};
      recovered.num = num;
      strncpy(recovered.tag, "Inbox", sizeof(recovered.tag) - 1);
      char textPath[64];
      snprintf(textPath, sizeof(textPath), "%s/note_%03d.txt", NOTES_DIR, num);
      recovered.hasText = sdExists(textPath);
      recovered.momentCount = static_cast<uint16_t>(noteMomentCount(num));
      noteIndex.push_back(recovered);
      repaired = true;
      Serial.printf("[Storage] recovered note %03d into catalog\n", num);
    }
    directory.close();
  }
  std::sort(noteIndex.begin(), noteIndex.end(),
            [](const NoteEntry& a, const NoteEntry& b) { return a.num < b.num; });
  if (recoverOrphans) {
    for (NoteEntry& note : noteIndex) {
      char textPath[64];
      snprintf(textPath, sizeof(textPath), "%s/note_%03d.txt", NOTES_DIR, note.num);
      recoverStoredFile(sdFs, String(textPath));
      File text = sdOpenRead(textPath);
      const bool hasText = text && text.size() > 0;
      if (text) text.close();
      if (note.hasText != hasText) { note.hasText = hasText; repaired = true; }
    }
  }
  if (repaired || !sdExists(INDEX_FILE)) saveIndex();
}

bool saveIndex() {
  if (sdExists(kIndexTemp)) sdRemoveIfExists(kIndexTemp);
  File f = sdCreate(kIndexTemp);
  if (!f) return false;
  size_t expected = 0;
  bool complete = true;
  // A blank line represents an empty catalog and is ignored by loadIndex.
  // Materialize it explicitly instead of relying on a zero-write FAT file.
  if (noteIndex.empty()) {
    expected = 1;
    complete = f.print("\n") == 1;
  }
  for (const NoteEntry& note : noteIndex) {
    const String line = String(note.num) + "," + note.tag + "," + (note.hasText ? "1" : "0") + "," + String(note.momentCount) + "\n";
    expected += line.length();
    complete &= f.print(line) == line.length();
  }
  f.flush();
  complete &= f.size() == expected;
  f.close();
  return complete && replaceNoteFileAtomically(INDEX_FILE, kIndexTemp);
}

bool addToIndex(int num, const char* tag, bool hasText) {
  NoteEntry e;
  e.num = num;
  strncpy(e.tag, tag, sizeof(e.tag)-1);
  e.tag[sizeof(e.tag)-1]='\0';
  e.hasText = hasText;
  e.momentCount = (uint16_t)noteMomentCount(num);
  noteIndex.push_back(e);
  if (!saveIndex()) { noteIndex.pop_back(); return false; }
  removeBrainSearchIndexEntry(num);
  removeBrainVectorIndexEntry(num);
  return true;
}

bool updateIndexHasText(int num) {
  const char* foundTag = "";
  for (int i=0; i<(int)noteIndex.size(); i++) {
    if (noteIndex[i].num==num) {
      noteIndex[i].hasText=true;
      foundTag = noteIndex[i].tag;
      break;
    }
  }
  if (!saveIndex()) {
    for (NoteEntry& note : noteIndex) if (note.num == num) note.hasText = false;
    return false;
  }
  return writeNoteMeta(num, foundTag);
}

bool deleteNote(int num) {
  storageChanged();
  const char* exts[] = {"txt", "meta", "queue", "moments", "brain.json", "brain.vec", "wav", "om", nullptr};
  char path[64];
  for (int e = 0; exts[e]; e++) {
    snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, exts[e]);
    for (const char* suffix : {".tmp", ".bak", ""}) {
      const String target = String(path) + suffix;
      if (sdExists(target.c_str()) && !sdRemoveIfExists(target.c_str())) return false;
    }
  }
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].num == num) {
      noteIndex.erase(noteIndex.begin() + i);
      break;
    }
  }
  const bool saved = saveIndex();
  removeBrainSearchIndexEntry(num);
  removeBrainVectorIndexEntry(num);
  return saved;
}

bool deleteAllNotes() {
  bool success = true;
  int removed = 0;

  // Reopen the directory for every entry. Mutating an open FAT directory can
  // otherwise skip the item that follows the deleted one.
  while (removed < 10000) {
    File directory = sdOpenRead(NOTES_DIR);
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      success = false;
      break;
    }

    File entry = directory.openNextFile();
    if (!entry) {
      directory.close();
      break;
    }

    String path = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    directory.close();
    if (!path.startsWith("/")) path = String(NOTES_DIR) + "/" + path;

    if (isDirectory || !sdRemoveIfExists(path.c_str())) {
      success = false;
      break;
    }
    removed++;
  }

  if (removed >= 10000) success = false;
  if (!success) {
    // Preserve discoverability of surviving audio after partial deletion.
    loadIndex(true);
    noteIndex.erase(std::remove_if(noteIndex.begin(), noteIndex.end(), [](const NoteEntry& note) {
      char path[64];
      snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, note.num);
      return !sdExists(path);
    }), noteIndex.end());
    saveIndex();
    return false;
  }
  noteIndex.clear();
  return saveIndex();
}

int nextNoteNumber() {
  int maxNum = 0;
  for (int i=0; i<(int)noteIndex.size(); i++) if (noteIndex[i].num > maxNum) maxNum = noteIndex[i].num;
  char path[64];
  do {
    if (maxNum == INT32_MAX) return -1;
    ++maxNum;
    snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, maxNum);
  } while (sdExists(path));
  return maxNum;
}

bool saveTag(int num, const char* tag) {
  if (!writeNoteMeta(num, tag)) return false;
  if (!addToIndex(num, tag, false)) return false;
  return writeNoteQueueState(num, "queued");
}

// ─── Tags ─────────────────────────────────────────────────────────────────

// ─── Meta ─────────────────────────────────────────────────────────────────

String noteMetaPath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.meta", NOTES_DIR, num);
  return String(path);
}

String readNoteMetaValue(int num, const char* key) {
  String path = noteMetaPath(num);
  recoverStoredFile(sdFs, path);
  File f = sdOpenRead(path.c_str());
  if (!f) return "";
  String prefix = String(key) + "=";
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.startsWith(prefix)) { f.close(); return line.substring(prefix.length()); }
  }
  f.close();
  return "";
}

bool writeNoteMeta(int num, const char* tag) {
  String path = noteMetaPath(num);
  String temp = path + ".tmp";
  String existingCreated = readNoteMetaValue(num, "created_utc");
  String created = existingCreated.length() ? existingCreated : currentUtcIso();
  sdRemoveIfExists(temp.c_str());
  File f = sdCreate(temp.c_str());
  if (!f) return false;
  bool hasText = false;
  for (int i = 0; i < (int)noteIndex.size(); i++)
    if (noteIndex[i].num == num) { hasText = noteIndex[i].hasText; break; }
  const String contents = "created_utc=" + created + "\ntag=" + (tag ? tag : "") + "\nsynced=" + (hasText ? "1\n" : "0\n");
  const size_t written = f.print(contents);
  f.flush();
  const bool complete = written == contents.length() && f.size() == contents.length();
  f.close();
  return complete && replaceNoteFileAtomically(path, temp);
}

static String queuePath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.queue", NOTES_DIR, num);
  return String(path);
}

static String readQueueValue(int num, const char* key) {
  recoverStoredFile(sdFs, queuePath(num));
  File file = sdOpenRead(queuePath(num));
  if (!file) return "";
  const String prefix = String(key) + "=";
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.startsWith(prefix)) {
      file.close();
      return line.substring(prefix.length());
    }
  }
  file.close();
  return "";
}

String noteQueueState(int num) {
  const String state = readQueueValue(num, "state");
  return state.length() ? state : "queued";
}

String noteQueueError(int num) { return readQueueValue(num, "error"); }
int noteQueueRetries(int num) { return readQueueValue(num, "retries").toInt(); }
int noteQueueFailureCount(int num) { return readQueueValue(num, "failures").toInt(); }
String noteQueueUpdatedUtc(int num) { return readQueueValue(num, "updated_utc"); }

static time_t queueUtcEpoch(const String& iso) {
  if (iso.length() < 19) return 0;
  struct tm value = {};
  value.tm_year = iso.substring(0, 4).toInt() - 1900;
  value.tm_mon = iso.substring(5, 7).toInt() - 1;
  value.tm_mday = iso.substring(8, 10).toInt();
  value.tm_hour = iso.substring(11, 13).toInt();
  value.tm_min = iso.substring(14, 16).toInt();
  value.tm_sec = iso.substring(17, 19).toInt();
  return utcTmToEpoch(value);
}

static std::map<int, uint32_t> retryStartedMs;
void resumeBlockedCloudNotes() {
  for (const NoteEntry& note : noteIndex) {
    if (noteQueueState(note.num) != "blocked") continue;
    const String error = noteQueueError(note.num);
    if (error == "upload limit exceeded" || error == "empty recording" || error == "no speech recognized" || error == "recording unavailable" ||
        error == "organization: transcript exceeds 32000 bytes") continue;
    writeNoteQueueState(note.num, "queued", nullptr, false, true);
  }
}

bool noteQueueRetryDue(int num) {
  if (noteQueueState(num) == "blocked") return false;
  if (noteQueueState(num) != "retry" && noteQueueFailureCount(num) <= 0) return true;
  const int failures = max(1, noteQueueFailureCount(num));
  String failedUtc = readQueueValue(num, "last_failure_utc");
  if (!failedUtc.length()) failedUtc = noteQueueUpdatedUtc(num); // old queue files
  const time_t updated = queueUtcEpoch(failedUtc);
  const time_t now = time(nullptr);
  // After a reboot without wall-clock time, allow one recovery attempt after
  // two minutes; otherwise a 30-minute backoff would outlast the awake budget.
  const uint32_t recoveredStart = millis() - (queueRetryDelaySeconds(failures) - 120U) * 1000U;
  const auto start = retryStartedMs.emplace(num, recoveredStart);
  return queueRetryElapsed(failures, updated, now, start.first->second, millis());
}

bool writeNoteQueueState(int num, const char* state, const char* error, bool incrementRetry, bool progressMade) {
  const String path = queuePath(num);
  const String temp = path + ".tmp";
  int retries = noteQueueRetries(num);
  int failures = noteQueueFailureCount(num);
  if (incrementRetry && retries < 1000000) retries++;
  progressMade = progressMade || (state && String(state) == "ready");
  failures = queueNextFailures(failures, incrementRetry, progressMade);
  String failedUtc = readQueueValue(num, "last_failure_utc");
  if (!failedUtc.length() && failures > 0) failedUtc = noteQueueUpdatedUtc(num);
  const uint32_t failureStarted = millis();
  if (incrementRetry) failedUtc = currentUtcIso();
  if (progressMade) failedUtc = "";
  sdRemoveIfExists(temp.c_str());
  File file = sdCreate(temp.c_str());
  if (!file) { Serial.printf("[Storage] queue open failed #%d\n", num); return false; }
  const String contents = String("state=") + (state ? state : "queued") + "\nretries=" + String(retries) +
      "\nfailures=" + String(failures) + "\nlast_failure_utc=" + failedUtc + "\nerror=" +
      (error ? error : "") + "\nupdated_utc=" + currentUtcIso() + "\n";
  const size_t written = file.print(contents);
  file.flush();
  const bool complete = written == contents.length() && file.size() == contents.length();
  file.close();
  const bool saved = complete && replaceNoteFileAtomically(path, temp);
  // In-memory backoff must describe committed metadata, not a failed write.
  if (saved) {
    if (progressMade) retryStartedMs.erase(num);
    else if (incrementRetry) retryStartedMs[num] = failureStarted;
  }
  if (!saved) Serial.printf("[Storage] queue commit failed #%d\n", num);
  return saved;
}

// ─── Moments ────────────────────────────────────────────────────────────────

static String noteMomentsPath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.moments", NOTES_DIR, num);
  return String(path);
}

bool saveNoteMoments(int num, const std::vector<uint32_t>& momentsMs) {
  String path = noteMomentsPath(num);
  String temp = path + ".tmp";
  if (sdExists(temp.c_str())) sdRemoveIfExists(temp.c_str());
  File f = sdCreate(temp.c_str());
  if (!f) return false;
  size_t expected = 0;
  bool complete = true;
  for (uint32_t momentMs : momentsMs) {
    const String line = String(momentMs) + "\n";
    expected += line.length();
    complete &= f.print(line) == line.length();
  }
  f.flush();
  complete &= f.size() == expected;
  f.close();
  if (!complete || !replaceNoteFileAtomically(path, temp)) return false;
  updateIndexMomentCount(num, (int)momentsMs.size());
  return true;
}

std::vector<uint32_t> loadNoteMoments(int num) {
  std::vector<uint32_t> result;
  String path = noteMomentsPath(num);
  recoverStoredFile(sdFs, path);
  File f = sdOpenRead(path.c_str());
  if (!f) return result;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length()) result.push_back((uint32_t)strtoul(line.c_str(), nullptr, 10));
  }
  f.close();
  return result;
}

int noteMomentCount(int num) {
  return (int)loadNoteMoments(num).size();
}

void updateIndexMomentCount(int num, int count) {
  for (int i = 0; i < (int)noteIndex.size(); i++) {
    if (noteIndex[i].num == num) {
      noteIndex[i].momentCount = (uint16_t)max(0, count);
      saveIndex();
      return;
    }
  }
}

// ─── Time helpers ─────────────────────────────────────────────────────────

String noteCreatedUtc(int num) {
  return readNoteMetaValue(num, "created_utc");
}

String utcToLocalDeviceLabel(const String& utcIso) {
  if (utcIso.length() < 16) return "time not set";
  struct tm utc;
  memset(&utc, 0, sizeof(struct tm));
  utc.tm_year = utcIso.substring(0, 4).toInt() - 1900;
  utc.tm_mon  = utcIso.substring(5, 7).toInt() - 1;
  utc.tm_mday = utcIso.substring(8, 10).toInt();
  utc.tm_hour = utcIso.substring(11, 13).toInt();
  utc.tm_min  = utcIso.substring(14, 16).toInt();
  utc.tm_sec  = 0;
  time_t epoch = utcTmToEpoch(utc);
  struct tm localTm;
  setenv("TZ", LOCAL_TZ_RULE, 1);
  tzset();
  localtime_r(&epoch, &localTm);
  char buf[22];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &localTm);
  return String(buf);
}

int localUtcOffsetMinutes(const String& utcIso) {
  if (utcIso.length() < 16) return 0;
  struct tm utc = {};
  utc.tm_year = utcIso.substring(0, 4).toInt() - 1900;
  utc.tm_mon  = utcIso.substring(5, 7).toInt() - 1;
  utc.tm_mday = utcIso.substring(8, 10).toInt();
  utc.tm_hour = utcIso.substring(11, 13).toInt();
  utc.tm_min  = utcIso.substring(14, 16).toInt();
  const time_t epoch = utcTmToEpoch(utc);
  setenv("TZ", LOCAL_TZ_RULE, 1);
  tzset();
  struct tm localTm = {};
  localtime_r(&epoch, &localTm);
  return static_cast<int>((utcTmToEpoch(localTm) - epoch) / 60);
}

String noteCreatedDeviceLabel(int num) {
  String utc = noteCreatedUtc(num);
  if (utc.length() < 16) return "time not set";
  return utcToLocalDeviceLabel(utc);
}

String currentUtcIso() {
  String fromRtc = rtcUtcIso();
  if (fromRtc.length() > 0) { timeReady = true; return fromRtc; }
  time_t now = time(nullptr);
  if (!timeReady || now < 1700000000) return "";
  struct tm utc;
  gmtime_r(&now, &utc);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buf);
}

// ─── Ticker + preview ─────────────────────────────────────────────────────

String notePreviewText(int num, size_t maxLen) {
  char txtPath[64];
  snprintf(txtPath, sizeof(txtPath), "%s/note_%03d.txt", NOTES_DIR, num);
  File f = sdOpenRead(txtPath);
  if (!f) return "";
  String out = "";
  while (f.available() && out.length() < maxLen) {
    char c = (char)f.read();
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    out += c;
  }
  f.close();
  return normalizeForDisplay(out);
}

// ─── Filtering ────────────────────────────────────────────────────────────
