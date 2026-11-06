#include "minutes.h"
#include "sd_read.h"
#include "../../config.h"
#include "../../globals.h"
#include "../../types.h"
#include "notes.h"
#include "settings.h"
#include "storage_commit.h"
#include "SD_MMC.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "HTTPClient.h"
#include <ArduinoJson.h>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include "speech_stream.h"
#include "http_response.h"
#include "ui.h"
#include "../../sounds.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"

extern const uint8_t oracle_ca_bundle_start[] asm("_binary_certs_oracle_ca_bundle_pem_start");

namespace {

constexpr size_t kLiveChunk = 128UL * 1024UL;     // per live PUT (~4 s of audio)
constexpr size_t kCatchUpChunk = 1024UL * 1024UL; // per PUT afterwards
constexpr uint32_t kMinutesCheckEveryMs = 3UL * 60UL * 1000UL;
constexpr size_t kMaxMinutesText = 30000;

// ─── Sidecar ────────────────────────────────────────────────────────────────
using Fields = std::map<String, String>;
std::map<int, uint32_t> nextMinutesCheck;

String sidecarPath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.om", NOTES_DIR, num);
  return path;
}

String wavPath(int num) {
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  return path;
}

bool readSidecar(int num, Fields& fields) {
  fields.clear();
  File file = sdOpenRead(sidecarPath(num).c_str());
  if (!file) return false;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    const int eq = line.indexOf('=');
    if (eq > 0) fields[line.substring(0, eq)] = line.substring(eq + 1);
  }
  file.close();
  return fields.count("rid") > 0;
}

bool writeSidecar(int num, const Fields& fields) {
  const String path = sidecarPath(num);
  const String temp = path + ".tmp";
  sdRemoveIfExists(temp.c_str());
  File file = SD_MMC.open(temp.c_str(), FILE_WRITE);
  if (!file) return false;
  bool ok = true;
  for (const auto& field : fields) {
    String value = field.second;
    value.replace("\n", " ");
    ok = file.print(field.first + "=" + value + "\n") > 0 && ok;
  }
  file.flush();
  file.close();
  return ok && commitStoredFile(SD_MMC, path, temp);
}

bool setSidecarField(int num, const char* key, const String& value) {
  Fields fields;
  if (!readSidecar(num, fields)) return false;
  fields[key] = value;
  return writeSidecar(num, fields);
}

String deviceId() {
  char id[16];
  snprintf(id, sizeof(id), "%012llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
  return id;
}

String urlEncode(const String& text) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < text.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// ─── HTTP ───────────────────────────────────────────────────────────────────
struct Conn {
  WiFiClientSecure secure;
  WiFiClient plain;
  bool tls = true;
  explicit Conn() {
    tls = oracleMinutesUrl().startsWith("https://");
    if (tls) {
      secure.setCACert(reinterpret_cast<const char*>(oracle_ca_bundle_start));
      secure.setHandshakeTimeout(15);
    }
  }
  void reset() { if (tls) secure.stop(); else plain.stop(); }
};

// Returns the HTTP status (negative = connection/timeout error).
int call(Conn& conn, const char* method, const String& path, const char* contentType,
         const uint8_t* data, size_t length, Stream* stream, size_t streamLength,
         long xOffset, String* body, uint16_t timeoutMs = 30000,
         bool live = false, const char* liveState = nullptr) {
  HTTPClient http;
  http.setReuse(true);
  const String url = oracleMinutesUrl() + path;
  const bool begun = conn.tls ? http.begin(conn.secure, url) : http.begin(conn.plain, url);
  if (!begun) return -100;
  http.setConnectTimeout(10000);
  http.setTimeout(timeoutMs);
  http.addHeader("Authorization", "Bearer " + oracleMinutesKey());
  http.addHeader("Accept-Encoding", "identity");
  if (contentType) http.addHeader("Content-Type", contentType);
  if (xOffset >= 0) http.addHeader("X-Offset", String(xOffset));
  // Live: the server transcribes each piece as it arrives (minutes come fast).
  if (live) http.addHeader("X-Live", "1");
  if (liveState) http.addHeader("X-Live-State", liveState);
  int status;
  if (stream) status = http.sendRequest(method, stream, streamLength);
  else status = http.sendRequest(method, const_cast<uint8_t*>(data), length);
  String response = status > 0 ? http.getString() : String();
  http.end();
  if (status <= 0) conn.reset();
  if (body) *body = response;
  if (status < 200 || status >= 300)
    Serial.printf("[Minutes] %s %s -> %d %s\n", method, path.c_str(), status,
                  response.substring(0, 200).c_str());
  return status;
}

String describe(int status) {
  if (status < 0) return "SERVER UNREACHABLE";
  if (status == 401 || status == 403) return "MINUTES KEY REJECTED";
  if (status == 404) return "NOT FOUND ON SERVER";
  if (status == 413) return "MEETING TOO LARGE";
  if (status >= 500) return String("SERVER ERROR ") + status;
  return String("SERVER HTTP ") + status;
}

long sizeFromVariant(JsonVariantConst value) {
  if (value.is<long long>()) return static_cast<long>(value.as<long long>());
  if (value.is<double>()) return static_cast<long>(value.as<double>());
  if (value.is<JsonObjectConst>()) {
    for (const char* key : {"size", "received", "offset", "bytes", "have", "length", "next", "detail"}) {
      const long found = sizeFromVariant(value[key]);
      if (found >= 0) return found;
    }
  }
  if (value.is<const char*>()) {
    // e.g. {"detail": "offset mismatch: server has 123456 bytes"} -> last number
    const String text = value.as<const char*>();
    long number = -1;
    for (int i = 0; i < static_cast<int>(text.length());) {
      if (isdigit(static_cast<unsigned char>(text[i]))) {
        int j = i;
        while (j < static_cast<int>(text.length()) && isdigit(static_cast<unsigned char>(text[j]))) j++;
        number = text.substring(i, j).toInt();
        i = j;
      } else i++;
    }
    return number;
  }
  return -1;
}

long parseServerSize(const String& body) {
  String text = body;
  text.trim();
  if (!text.length()) return -1;
  if (isdigit(static_cast<unsigned char>(text[0]))) return text.toInt();
  JsonDocument doc;
  if (deserializeJson(doc, text)) return -1;
  return sizeFromVariant(doc.as<JsonVariantConst>());
}

// How many bytes of this recording the server already has (-1 = unknown).
long serverOffset(Conn& conn, const String& rid, String& error) {
  String body;
  const int status = call(conn, "GET", "/api/uploads/" + rid, nullptr, nullptr, 0, nullptr, 0, -1, &body);
  if (status == 404) return 0;
  if (status < 200 || status >= 300) { error = describe(status); return -1; }
  const long size = parseServerSize(body);
  return size < 0 ? 0 : size;
}

// A WAV header for a stream whose length is not known yet (0xFFFFFFFF sizes:
// decoders read to the end of the data).
void streamingHeader(uint8_t* header) {
  const uint32_t unknown = 0xFFFFFFFFUL, fmtLength = 16, sampleRate = SAMPLE_RATE,
                 byteRate = SAMPLE_RATE * 2;
  const uint16_t audioFormat = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(header, "RIFF", 4); memcpy(header + 4, &unknown, 4);
  memcpy(header + 8, "WAVEfmt ", 8); memcpy(header + 16, &fmtLength, 4);
  memcpy(header + 20, &audioFormat, 2); memcpy(header + 22, &channels, 2);
  memcpy(header + 24, &sampleRate, 4); memcpy(header + 28, &byteRate, 4);
  memcpy(header + 32, &blockAlign, 2); memcpy(header + 34, &bits, 2);
  memcpy(header + 36, "data", 4); memcpy(header + 40, &unknown, 4);
}

// A request body made of a small buffer followed by part of a file: the live
// stream's first piece is its WAV header together with the first audio.
class HeadedFileStream : public Stream {
 public:
  HeadedFileStream(const uint8_t* head, size_t headLength, File& file, size_t fileLength)
      : head_(head), headLength_(headLength), file_(file), fileLength_(fileLength) {}
  int available() override {
    return static_cast<int>((headLength_ - headPos_) + (fileLength_ - filePos_));
  }
  int read() override {
    if (headPos_ < headLength_) return head_[headPos_++];
    if (filePos_ >= fileLength_) return -1;
    const int c = file_.read();
    if (c >= 0) filePos_++;
    return c;
  }
  int peek() override {
    if (headPos_ < headLength_) return head_[headPos_];
    return filePos_ < fileLength_ ? file_.peek() : -1;
  }
  size_t readBytes(char* buffer, size_t length) override {
    size_t copied = 0;
    while (copied < length && headPos_ < headLength_) buffer[copied++] = head_[headPos_++];
    if (copied < length && filePos_ < fileLength_) {
      const size_t want = std::min(length - copied, fileLength_ - filePos_);
      const size_t got = file_.read(reinterpret_cast<uint8_t*>(buffer) + copied, want);
      filePos_ += got;
      copied += got;
    }
    return copied;
  }
  size_t write(uint8_t) override { return 0; }
 private:
  const uint8_t* head_;
  size_t headLength_, headPos_ = 0;
  File& file_;
  size_t fileLength_, filePos_ = 0;
};

std::mutex briefLock;
String briefSpeaker, briefLine, briefTalked;
std::atomic<uint32_t> briefVersion{0};

String firstString(JsonVariantConst object, std::initializer_list<const char*> keys) {
  for (const char* key : keys)
    if (object[key].is<const char*>() && strlen(object[key].as<const char*>()))
      return object[key].as<const char*>();
  return "";
}

// Every live reply carries "brief": the last line heard, who said it, who has talked.
void takeBrief(const String& body) {
  if (body.indexOf("brief") < 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  JsonVariantConst brief = doc["brief"];
  String speaker, line, talked;
  if (brief.is<const char*>()) {
    line = brief.as<const char*>();
  } else if (brief.is<JsonObjectConst>()) {
    line = firstString(brief, {"line", "text", "last", "heard", "last_line"});
    JsonVariantConst lastLine = brief["last"];
    if (!line.length() && lastLine.is<JsonObjectConst>()) {
      line = firstString(lastLine, {"text", "line"});
      speaker = firstString(lastLine, {"speaker", "who", "name"});
    }
    if (!speaker.length()) speaker = firstString(brief, {"speaker", "who", "name", "by", "said_by"});
    for (const char* key : {"talked", "speakers", "people", "who_talked", "voices"}) {
      if (!brief[key].is<JsonArrayConst>()) continue;
      for (JsonVariantConst person : brief[key].as<JsonArrayConst>()) {
        String name = person.is<const char*>() ? String(person.as<const char*>())
                                               : firstString(person, {"name", "speaker", "who"});
        if (!name.length()) continue;
        if (talked.length()) talked += ", ";
        talked += name;
      }
      break;
    }
  } else {
    return;
  }
  line.trim();
  if (!line.length() && !speaker.length() && !talked.length()) return;
  std::lock_guard<std::mutex> guard(briefLock);
  if (line == briefLine && speaker == briefSpeaker && talked == briefTalked) return;
  briefLine = line; briefSpeaker = speaker; briefTalked = talked;
  briefVersion++;
}

// Sends one PUT of bytes [sent, limit). Returns the server's new size or -1.
long pushRange(Conn& conn, const String& rid, const String& path, long sent, long limit,
               size_t maxChunk, bool syntheticHeader, String& error) {
  String body;
  int status;
  long sending;
  if (sent == 0 && syntheticHeader && limit > 44) {
    // First live piece: streaming WAV header + the first audio, one request.
    uint8_t header[44];
    streamingHeader(header);
    File file = sdOpenRead(path.c_str());
    if (!file) { error = "RECORDING MISSING"; return -1; }
    const size_t audio = std::min<long>(limit - 44, static_cast<long>(maxChunk));
    if (!file.seek(44)) { file.close(); error = "RECORDING MISSING"; return -1; }
    HeadedFileStream piece(header, sizeof(header), file, audio);
    sending = 44 + audio;
    status = call(conn, "PUT", "/api/uploads/" + rid, "application/octet-stream",
                  nullptr, 0, &piece, sending, 0, &body, 60000, true);
    file.close();
  } else if (sent < 44 && syntheticHeader) {
    uint8_t header[44];
    streamingHeader(header);
    sending = 44 - sent;
    status = call(conn, "PUT", "/api/uploads/" + rid, "application/octet-stream",
                  header + sent, sending, nullptr, 0, sent, &body, 30000, true);
  } else {
    File file = sdOpenRead(path.c_str());
    if (!file) { error = "RECORDING MISSING"; return -1; }
    sending = std::min<long>(limit - sent, static_cast<long>(maxChunk));
    if (sending <= 0 || !file.seek(sent)) { file.close(); return sent; }
    status = call(conn, "PUT", "/api/uploads/" + rid, "application/octet-stream",
                  nullptr, 0, &file, sending, sent, &body, 60000, true);
    file.close();
  }
  takeBrief(body);
  if (status >= 200 && status < 300) {
    const long reported = parseServerSize(body);
    return reported > sent ? reported : sent + sending;
  }
  if (status == 409) {
    const long reported = parseServerSize(body);
    if (reported >= 0) return reported;
  }
  error = describe(status);
  return -1;
}

// ─── Live session ───────────────────────────────────────────────────────────
struct Live {
  std::atomic<uint32_t> flushed{0};
  std::atomic<bool> finished{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> running{false};
  std::atomic<bool> connected{false};
  std::atomic<long> sent{0};
  int noteNum = -1;
  String rid;
  String path;
} live;

void liveTask(void*) {
  Conn conn;
  String error;
  long sent = serverOffset(conn, live.rid, error);
  if (sent < 0) sent = 0;
  live.sent = sent;
  int failures = 0;
  uint32_t lastPutAt = millis();
  while (!live.stop.load()) {
    const uint32_t available = live.flushed.load();
    const bool finished = live.finished.load();
    if (finished && sent >= static_cast<long>(available)) break;
    if (WiFi.status() != WL_CONNECTED) { live.connected = false; delay(1000); continue; }
    const bool worthSending = static_cast<long>(available) - sent >= 8L * 1024L ||
                              (finished && static_cast<long>(available) > sent) || sent < 44;
    if (!worthSending || static_cast<long>(available) <= sent) {
      // Nothing new (e.g. the card is busy): tell the page we are still here
      // so it doesn't show "Paused"; the reply refreshes the brief too.
      if (sent >= 44 && millis() - lastPutAt > 10000UL) {
        String body;
        call(conn, "PUT", "/api/uploads/" + live.rid, "application/octet-stream",
             nullptr, 0, nullptr, 0, sent, &body, 15000, true, "quiet");
        takeBrief(body);
        lastPutAt = millis();
      }
      delay(300);
      continue;
    }
    const long next = pushRange(conn, live.rid, live.path, sent, available, kLiveChunk, true, error);
    if (next < 0) {
      live.connected = false;
      failures++;
      conn.reset();
      // Wrong key will not fix itself while recording; keep the audio local.
      if (error == "MINUTES KEY REJECTED") break;
      delay(std::min(15000, 1000 * failures));
      continue;
    }
    failures = 0;
    sent = next;
    live.sent = sent;
    live.connected = true;
    lastPutAt = millis();
  }
  conn.reset();
  live.connected = false;
  live.running = false;
  vTaskDelete(nullptr);
}

// ─── Minutes text ───────────────────────────────────────────────────────────
bool jobReady(const String& body, String& minutesText, bool& failed) {
  failed = false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonVariantConst root = doc.as<JsonVariantConst>();
  if (root["job"].is<JsonObjectConst>()) root = root["job"];
  for (const char* key : {"minutes", "summary"}) {
    if (root[key].is<const char*>() && strlen(root[key].as<const char*>()) > 40) {
      minutesText = root[key].as<const char*>();
      return true;
    }
  }
  String status = root["status"] | "";
  status.toLowerCase();
  if (status == "error" || status == "failed") { failed = true; return false; }
  if (root["progress"].is<double>() && root["progress"].as<double>() >= 100.0) return true;
  return status == "done" || status == "complete" || status == "completed" || status == "finished" ||
         status == "ready" || status == "summarized" || status == "success" || status == "ok";
}

String answerFrom(const String& body, String& askId, bool& failed) {
  failed = false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return "";
  if (doc["id"].is<const char*>()) askId = doc["id"].as<const char*>();
  else if (doc["id"].is<long long>()) askId = String(static_cast<long>(doc["id"].as<long long>()));
  String status = doc["status"] | "";
  status.toLowerCase();
  if (status == "error" || status == "failed") failed = true;
  for (const char* key : {"answer", "text", "result"}) {
    if (doc[key].is<const char*>() && strlen(doc[key].as<const char*>())) return doc[key].as<const char*>();
  }
  return "";
}

bool askForMinutes(Conn& conn, const String& rid, String& minutes, String& error) {
  JsonDocument request;
  JsonObject message = request["messages"].to<JsonArray>().add<JsonObject>();
  message["role"] = "user";
  message["content"] =
    "Write the complete minutes of this meeting as plain text (no markdown): the date and attendees "
    "if known, a short summary, the key points discussed, decisions made, and action items with "
    "owner and due date. Be specific and keep names, numbers and dates exactly as said.";
  request["recording"] = rid;
  String payload;
  serializeJson(request, payload);
  String body, askId;
  bool failed = false;
  int status = call(conn, "POST", "/api/ask?wait=15", "application/json",
                    reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
                    nullptr, 0, -1, &body, 25000);
  if (status < 200 || status >= 300) { error = describe(status); return false; }
  minutes = answerFrom(body, askId, failed);
  for (int poll = 0; !minutes.length() && !failed && askId.length() && poll < 4; ++poll) {
    status = call(conn, "GET", "/api/ask/" + urlEncode(askId) + "?wait=15", nullptr, nullptr, 0,
                  nullptr, 0, -1, &body, 25000);
    if (status < 200 || status >= 300) { error = describe(status); return false; }
    minutes = answerFrom(body, askId, failed);
  }
  if (failed) error = "MINUTES FAILED";
  else if (!minutes.length()) error = "MINUTES NOT READY";
  return minutes.length() > 0;
}

bool storeMinutes(int num, const String& title, const String& minutes) {
  String text = "Meeting minutes: " + title + "\n\n" + minutes;
  if (text.length() > kMaxMinutesText) text = text.substring(0, kMaxMinutesText);
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.txt", NOTES_DIR, num);
  const String temp = String(path) + ".tmp";
  sdRemoveIfExists(temp.c_str());
  File file = SD_MMC.open(temp.c_str(), FILE_WRITE);
  if (!file) return false;
  const size_t written = file.print(text);
  file.flush();
  file.close();
  if (written != text.length() || !commitStoredFile(SD_MMC, String(path), temp)) return false;
  if (!updateIndexHasText(num)) return false;
  // The normal memory pipeline now organises the minutes (tasks, decisions).
  writeNoteQueueState(num, "queued");
  return true;
}

bool completeUpload(Conn& conn, int num, Fields& fields, String& error) {
  const String rid = fields["rid"];
  String query = "?filename=" + urlEncode("note_" + String(num) + ".wav");
  if (fields["title"].length()) query += "&title=" + urlEncode(fields["title"]);
  String body;
  const int status = call(conn, "POST", "/api/uploads/" + rid + "/complete" + query, nullptr,
                          nullptr, 0, nullptr, 0, -1, &body, 60000);
  if (status < 200 || status >= 300) { error = describe(status); return false; }
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    for (const char* key : {"id", "job", "jid", "job_id"}) {
      if (doc[key].is<const char*>()) { fields["job"] = doc[key].as<const char*>(); break; }
    }
  }
  fields["state"] = "sent";
  fields["sent_ms"] = String(millis());
  writeSidecar(num, fields);
  nextMinutesCheck[num] = millis() + 60000UL;   // live parts are already transcribed
  return true;
}

// Uploads whatever the server is missing, then completes. Wi-Fi must be up.
bool uploadAndComplete(int num, Fields& fields, const std::function<void(int)>& progress, String& error) {
  Conn conn;
  const String path = wavPath(num);
  File file = sdOpenRead(path.c_str());
  if (!file) { error = "RECORDING MISSING"; return false; }
  const long total = file.size();
  file.close();
  long sent = serverOffset(conn, fields["rid"], error);
  if (sent < 0) return false;
  int lastPercent = -1;
  int failures = 0;
  while (sent < total) {
    const int percent = static_cast<int>((sent * 100LL) / std::max<long>(total, 1));
    if (progress && percent != lastPercent) { progress(percent); lastPercent = percent; }
    // A finished file has its real header; the server may already hold the
    // live stream's header, which is equivalent.
    const long next = pushRange(conn, fields["rid"], path, sent, total, kCatchUpChunk, false, error);
    if (next < 0) {
      if (++failures >= 3 || error == "MINUTES KEY REJECTED") return false;
      conn.reset();
      delay(1500);
      continue;
    }
    failures = 0;
    sent = next;
  }
  if (progress) progress(100);
  return completeUpload(conn, num, fields, error);
}

}  // namespace

// ─── Public API ─────────────────────────────────────────────────────────────
bool minutesLiveStart(int noteNum, const String& title, bool online) {
  char suffix[12];
  snprintf(suffix, sizeof(suffix), "%08lx", static_cast<unsigned long>(esp_random()));
  Fields fields;
  fields["rid"] = "oraclelite-" + deviceId() + "-" + String(noteNum) + "-" + suffix;
  fields["title"] = title;
  fields["state"] = "recording";
  if (!writeSidecar(noteNum, fields)) return false;

  live.stop = false;
  live.finished = false;
  live.connected = false;
  live.flushed = 0;
  live.sent = 0;
  {
    std::lock_guard<std::mutex> guard(briefLock);
    briefSpeaker = ""; briefLine = ""; briefTalked = "";
  }
  live.noteNum = noteNum;
  live.rid = fields["rid"];
  live.path = wavPath(noteNum);
  if (!online || !oracleHasMinutesKey()) return true;
  live.running = true;
  // Core 0 with Wi-Fi; the recorder keeps the microphone drained on core 1.
  if (xTaskCreatePinnedToCore(liveTask, "minutes-live", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    live.running = false;
    Serial.println("[Minutes] live uploader could not start; recording locally");
  }
  return true;
}

void minutesLivePublish(uint32_t fileBytes) { live.flushed = fileBytes; }

uint32_t minutesLiveBriefVersion() { return briefVersion.load(); }

void minutesLiveBrief(String& speaker, String& line, String& talked) {
  std::lock_guard<std::mutex> guard(briefLock);
  speaker = briefSpeaker; line = briefLine; talked = briefTalked;
}

void minutesLiveFinish(bool keep) {
  live.finished = true;
  if (!keep) {
    live.stop = true;
    while (live.running.load()) delay(10);
    sdRemoveIfExists(sidecarPath(live.noteNum).c_str());
    return;
  }
  setSidecarField(live.noteNum, "state", "recorded");
}

bool minutesLiveConnected() { return live.connected.load(); }

bool minutesFinishUpload(int noteNum, const std::function<void(int)>& progress, String& error) {
  // Let the live uploader drain what it has; give up if it stalls.
  File file = sdOpenRead(wavPath(noteNum).c_str());
  const long total = file ? static_cast<long>(file.size()) : 0;
  if (file) file.close();
  long lastSent = live.sent.load();
  uint32_t lastProgressAt = millis();
  while (live.running.load()) {
    const long sent = live.sent.load();
    if (sent != lastSent) { lastSent = sent; lastProgressAt = millis(); }
    if (progress && total > 0) progress(static_cast<int>((sent * 100LL) / total));
    if (millis() - lastProgressAt > 20000UL) live.stop = true;
    delay(500);
  }
  if (!oracleHasMinutesKey()) { error = "ADD MINUTES KEY"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "NO WIFI"; return false; }
  Fields fields;
  if (!readSidecar(noteNum, fields)) { error = "MEETING INFO MISSING"; return false; }
  return uploadAndComplete(noteNum, fields, progress, error);
}

bool minutesNoteAwaitingServer(int noteNum) {
  if (!SD_MMC.exists(sidecarPath(noteNum).c_str())) return false;
  Fields fields;
  if (!readSidecar(noteNum, fields)) return false;
  return fields["state"] != "done" && fields["state"] != "failed";
}

String minutesNoteState(int noteNum) {
  Fields fields;
  return readSidecar(noteNum, fields) ? fields["state"] : String();
}

int minutesPendingCount() {
  int count = 0;
  for (const NoteEntry& note : noteIndex) {
    if (strcmp(note.tag, "Meeting") != 0) continue;
    if (note.num == live.noteNum && live.running.load()) continue;
    if (minutesNoteAwaitingServer(note.num)) count++;
  }
  return count;
}

bool minutesServiceOne(const std::function<void(const char*)>& phase, String& error) {
  for (const NoteEntry& note : noteIndex) {
    if (strcmp(note.tag, "Meeting") != 0) continue;
    if (note.num == live.noteNum && live.running.load()) continue;
    Fields fields;
    if (!readSidecar(note.num, fields)) continue;
    const String state = fields["state"];
    if (state == "recording" || state == "recorded") {
      if (phase) phase("SENDING");
      return uploadAndComplete(note.num, fields, nullptr, error);
    }
    if (state != "sent") continue;
    auto due = nextMinutesCheck.find(note.num);
    if (due != nextMinutesCheck.end() && static_cast<int32_t>(millis() - due->second) < 0) continue;
    nextMinutesCheck[note.num] = millis() + kMinutesCheckEveryMs;
    if (phase) phase("MINUTES");
    Conn conn;
    String body, minutes;
    bool failed = false;
    const int status = call(conn, "GET", "/api/recordings/" + fields["rid"], nullptr, nullptr, 0,
                            nullptr, 0, -1, &body);
    if (status == 404) { error = "NOT ON SERVER YET"; return false; }
    if (status < 200 || status >= 300) { error = describe(status); return false; }
    const bool ready = jobReady(body, minutes, failed);
    if (failed) {
      fields["state"] = "failed";
      writeSidecar(note.num, fields);
      error = "SERVER COULD NOT PROCESS";
      return false;
    }
    if (!ready) {
      // Stop asking after ~40 checks (each is a wake-up, 3+ minutes apart).
      const int checks = fields["checks"].toInt() + 1;
      fields["checks"] = String(checks);
      if (checks >= 40) fields["state"] = "failed";
      writeSidecar(note.num, fields);
      error = "MINUTES NOT READY";
      return false;
    }
    if (!minutes.length() && !askForMinutes(conn, fields["rid"], minutes, error)) return false;
    if (!storeMinutes(note.num, fields["title"], minutes)) { error = "STORAGE FULL"; return false; }
    fields["state"] = "done";
    writeSidecar(note.num, fields);
    nextMinutesCheck.erase(note.num);
    return true;
  }
  return false;
}

bool oracleServerTts(const String& text, const char* outputPath) {
  if (!oracleHasMinutesKey() || WiFi.status() != WL_CONNECTED) return false;
  const uint32_t startedAt = millis();
  JsonDocument request;
  request["text"] = text;
  request["lang"] = oracleUiLanguage();
  String payload;
  serializeJson(request, payload);
  Conn conn;
  HTTPClient http;
  const String url = oracleMinutesUrl() + "/api/tts";
  if (!(conn.tls ? http.begin(conn.secure, url) : http.begin(conn.plain, url))) return false;
  http.setConnectTimeout(10000);
  http.setTimeout(45000);
  http.addHeader("Authorization", "Bearer " + oracleMinutesKey());
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  const int status = http.POST(payload);
  bool ok = false;
  if (status == 200) {
    const String temp = String(outputPath) + ".tmp";
    sdRemoveIfExists(temp.c_str());
    File file = SD_MMC.open(temp.c_str(), FILE_WRITE);
    if (file) {
      const int written = http.writeToStream(&file);
      file.flush();
      const size_t stored = file.size();
      file.close();
      ok = written > 44 && stored > 44 && commitStoredFile(SD_MMC, String(outputPath), temp);
      if (!ok) sdRemoveIfExists(temp.c_str());
    }
  } else {
    Serial.printf("[Minutes] TTS -> %d %s\n", status, http.getString().substring(0, 160).c_str());
  }
  http.end();
  Serial.printf("[Minutes] TTS %s in %lu ms\n", ok ? "ok" : "failed",
                static_cast<unsigned long>(millis() - startedAt));
  return ok;
}

// ─── Spoken web question through /api/voice ────────────────────────────────
// One trip: the WAV goes up, the reply streams back as lines:
//   T {"text"} what was heard   P {"text"} answer sentence   B <base64> voice
//   A {"answer"} whole answer   E {} / E {"error"} end
// The voice is 16 kHz IMA ADPCM, decoded and played while it arrives.
namespace {
const int8_t kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int16_t kImaStep[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
  449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

class AdpcmPlayer {
 public:
  explicit AdpcmPlayer(OracleSpeechStream& stream) : stream_(stream) {}
  bool feed(const uint8_t* data, size_t length) {
    pending_.insert(pending_.end(), data, data + length);
    if (!headerChecked_ && !skipWavHeader()) return true;   // need more bytes
    if (pcm_) {   // the server sent plain 16-bit PCM: play it as is
      const size_t even = pending_.size() & ~static_cast<size_t>(1);
      for (size_t i = 0; i < even; ++i)
        if (!stream_.put(static_cast<char>(pending_[i]))) return false;
      samples_ += even / 2;
      pending_.erase(pending_.begin(), pending_.begin() + even);
      return true;
    }
    size_t offset = 0;
    while (pending_.size() - offset >= blockAlign_) {
      if (!decodeBlock(&pending_[offset], blockAlign_)) return false;
      offset += blockAlign_;
    }
    pending_.erase(pending_.begin(), pending_.begin() + offset);
    return true;
  }
  bool flush() {
    if (pcm_) { pending_.clear(); return true; }
    const bool ok = pending_.size() < 4 || decodeBlock(pending_.data(), pending_.size());
    pending_.clear();
    return ok;
  }
  bool hasAudio() const { return samples_ > 0; }
 private:
  // The first piece may carry a WAV header: skip to its data, keep the block size.
  bool skipWavHeader() {
    if (pending_.size() < 12) return false;
    if (memcmp(pending_.data(), "RIFF", 4) != 0) { headerChecked_ = true; return true; }
    size_t pos = 12;
    while (pos + 8 <= pending_.size()) {
      const uint32_t size = pending_[pos + 4] | (pending_[pos + 5] << 8) |
                            (pending_[pos + 6] << 16) | (static_cast<uint32_t>(pending_[pos + 7]) << 24);
      if (!memcmp(&pending_[pos], "fmt ", 4) && pos + 8 + 14 <= pending_.size()) {
        pcm_ = (pending_[pos + 8] | (pending_[pos + 9] << 8)) == 1;
        const size_t align = pending_[pos + 8 + 12] | (pending_[pos + 8 + 13] << 8);
        if (align >= 8 && align <= 4096) blockAlign_ = align;
      }
      if (!memcmp(&pending_[pos], "data", 4)) {
        pending_.erase(pending_.begin(), pending_.begin() + pos + 8);
        headerChecked_ = true;
        return true;
      }
      pos += 8 + size + (size & 1);
    }
    return false;
  }
  bool put(int16_t sample) {
    samples_++;
    return stream_.put(static_cast<char>(sample & 0xFF)) && stream_.put(static_cast<char>((sample >> 8) & 0xFF));
  }
  bool decodeBlock(const uint8_t* block, size_t length) {
    if (length < 4) return true;
    int predictor = static_cast<int16_t>(block[0] | (block[1] << 8));
    int index = std::min<int>(block[2], 88);
    if (!put(static_cast<int16_t>(predictor))) return false;
    for (size_t i = 4; i < length; ++i) {
      for (int half = 0; half < 2; ++half) {
        const int nibble = half ? (block[i] >> 4) : (block[i] & 0x0F);
        const int step = kImaStep[index];
        int diff = step >> 3;
        if (nibble & 4) diff += step;
        if (nibble & 2) diff += step >> 1;
        if (nibble & 1) diff += step >> 2;
        predictor += (nibble & 8) ? -diff : diff;
        predictor = std::max(-32768, std::min(32767, predictor));
        index = std::max(0, std::min(88, index + kImaIndex[nibble]));
        if (!put(static_cast<int16_t>(predictor))) return false;
      }
    }
    return true;
  }
  OracleSpeechStream& stream_;
  std::vector<uint8_t> pending_;
  size_t blockAlign_ = 256;
  bool headerChecked_ = false;
  bool pcm_ = false;
  uint32_t samples_ = 0;
};

String deviceLanguageName() {
  const String code = oracleUiLanguage();
  if (code == "de") return "German";
  if (code == "fr") return "French";
  if (code == "es") return "Spanish";
  if (code == "it") return "Italian";
  if (code == "pl") return "Polish";
  if (code == "cs") return "Czech";
  if (code == "ru") return "Russian";
  if (code == "uk") return "Ukrainian";
  if (code == "nl") return "Dutch";
  if (code == "pt") return "Portuguese";
  return "English";
}

String jsonText(const String& json, const char* key) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "";
  return doc[key] | "";
}
}  // namespace

// The request target and headers (everything except how the body is framed).
namespace {
struct VoiceTarget {
  String host;
  String prefix;
  int port = 443;
  String headers;   // request line + headers, without the final blank line
};

// Recent web questions and answers, so a follow-up ("and how old is it?")
// is understood. Cleared when a new conversation starts.
std::vector<std::pair<String, String>> voiceHistory;

VoiceTarget voiceTarget() {
  VoiceTarget target;
  String host = oracleMinutesUrl();
  const bool tls = host.startsWith("https://");
  host = host.substring(tls ? 8 : 7);
  String prefix;
  target.port = tls ? 443 : 80;
  const int slash = host.indexOf('/');
  if (slash >= 0) { prefix = host.substring(slash); host = host.substring(0, slash); }
  const int colon = host.indexOf(':');
  if (colon >= 0) { target.port = host.substring(colon + 1).toInt(); host = host.substring(0, colon); }
  target.host = host;
  target.prefix = prefix;

  JsonDocument ask;
  JsonArray messages = ask["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] =
    "You are Oracle, a voice assistant on a small e-paper device with a speaker, like a phone "
    "assistant: answer everyday questions (news, weather, facts, prices, maths, definitions) directly, "
    "searching the web whenever the answer may have changed. Answer in at most 3 short sentences of "
    "plain text (no markdown, no lists), natural when spoken. Always reply in " + deviceLanguageName() +
    " unless the user asks for another language.";
  // Earlier turns of this conversation (last 4), then no user message: the
  // server adds the transcribed question itself. An empty user turn here made
  // it answer with an empty string.
  const size_t from = voiceHistory.size() > 4 ? voiceHistory.size() - 4 : 0;
  for (size_t i = from; i < voiceHistory.size(); ++i) {
    JsonObject asked = messages.add<JsonObject>();
    asked["role"] = "user";
    asked["content"] = voiceHistory[i].first.substring(0, 400);
    JsonObject replied = messages.add<JsonObject>();
    replied["role"] = "assistant";
    replied["content"] = voiceHistory[i].second.substring(0, 400);
  }
  String askJson;
  serializeJson(ask, askJson);
  size_t encodedLength = 0;
  mbedtls_base64_encode(nullptr, 0, &encodedLength,
                        reinterpret_cast<const uint8_t*>(askJson.c_str()), askJson.length());
  std::vector<unsigned char> encoded(encodedLength + 1, 0);
  mbedtls_base64_encode(encoded.data(), encoded.size(), &encodedLength,
                        reinterpret_cast<const uint8_t*>(askJson.c_str()), askJson.length());
  // language= fixes speech recognition (auto-detect heard Singapore English
  // as Malay); it follows the device language, as the Idea Saver does.
  const String path = prefix + "/api/voice?speak=1&intents=0&language=" + oracleUiLanguage() +
                      "&tz=" + String(localUtcOffsetMinutes(currentUtcIso()));
  target.headers = "POST " + path + " HTTP/1.1\r\nHost: " + host +
    "\r\nAuthorization: Bearer " + oracleMinutesKey() +
    "\r\nX-Ask: " + String(reinterpret_cast<char*>(encoded.data())) +
    "\r\nContent-Type: audio/wav\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
  return target;
}

// The recorded question as a 16 kHz mono IMA ADPCM WAV (256-byte blocks,
// 505 samples each): a quarter of the PCM size, so it uploads 4x faster.
bool buildAdpcmWav(const char* wavPath, std::vector<uint8_t>& out) {
  File file = sdOpenRead(wavPath);
  if (!file || file.size() <= 44) { if (file) file.close(); return false; }
  const uint32_t samples = (file.size() - 44) / 2;
  constexpr uint32_t kSamplesPerBlock = 505, kBlockAlign = 256;
  const uint32_t blocks = (samples + kSamplesPerBlock - 1) / kSamplesPerBlock;
  const uint32_t dataBytes = blocks * kBlockAlign;
  out.assign(60 + dataBytes, 0);
  auto put16 = [&](size_t at, uint16_t v) { out[at] = v & 0xFF; out[at + 1] = v >> 8; };
  auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = (v >> (8 * i)) & 0xFF; };
  memcpy(&out[0], "RIFF", 4); put32(4, static_cast<uint32_t>(out.size() - 8)); memcpy(&out[8], "WAVE", 4);
  memcpy(&out[12], "fmt ", 4); put32(16, 20);
  put16(20, 0x11); put16(22, 1); put32(24, SAMPLE_RATE);
  put32(28, SAMPLE_RATE * kBlockAlign / kSamplesPerBlock); put16(32, kBlockAlign); put16(34, 4);
  put16(36, 2); put16(38, kSamplesPerBlock);
  memcpy(&out[40], "fact", 4); put32(44, 4); put32(48, samples);
  memcpy(&out[52], "data", 4); put32(56, dataBytes);

  file.seek(44);
  std::vector<int16_t> pcm(kSamplesPerBlock);
  int index = 0;
  uint8_t* block = &out[60];
  for (uint32_t b = 0; b < blocks; ++b, block += kBlockAlign) {
    const size_t got = file.read(reinterpret_cast<uint8_t*>(pcm.data()), kSamplesPerBlock * 2) / 2;
    for (size_t i = got; i < kSamplesPerBlock; ++i) pcm[i] = 0;
    int predictor = pcm[0];
    block[0] = predictor & 0xFF; block[1] = (predictor >> 8) & 0xFF;
    block[2] = static_cast<uint8_t>(index); block[3] = 0;
    for (uint32_t i = 1; i < kSamplesPerBlock; ++i) {
      int diff = pcm[i] - predictor;
      int nibble = 0;
      if (diff < 0) { nibble = 8; diff = -diff; }
      int step = kImaStep[index];
      int delta = step >> 3;
      if (diff >= step) { nibble |= 4; diff -= step; delta += step; }
      step >>= 1;
      if (diff >= step) { nibble |= 2; diff -= step; delta += step; }
      step >>= 1;
      if (diff >= step) { nibble |= 1; delta += step; }
      predictor += (nibble & 8) ? -delta : delta;
      predictor = std::max(-32768, std::min(32767, predictor));
      index = std::max(0, std::min(88, index + kImaIndex[nibble]));
      const uint32_t at = 4 + (i - 1) / 2;
      if ((i - 1) & 1) block[at] |= nibble << 4; else block[at] = nibble;
    }
  }
  file.close();
  return true;
}

bool readWholeFile(const char* path, std::vector<uint8_t>& out) {
  File file = sdOpenRead(path);
  if (!file) return false;
  out.resize(file.size());
  const size_t got = file.read(out.data(), out.size());
  file.close();
  return got == out.size();
}


bool writeAll(Client& client, const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const size_t written = client.write(data + offset, length - offset);
    if (!written) return false;
    offset += written;
  }
  return true;
}

// Reads the streamed reply (T/P/B/A/E lines), plays the voice as it arrives.
// Returns the HTTP status (0 = no reply).
int readVoiceReply(Client& client, uint32_t startedAt, uint32_t uploadedAt,
                   String& question, String& answer, String& error) {
  OracleSpeechStream stream;
  stream.from24k = false;
  const bool streamReady = stream.begin();
  AdpcmPlayer player(stream);
  String line, partialAnswer, serverError, errorBody, shownText;
  line.reserve(8192);
  bool ended = false, shown = false, playbackFailed = false;
  uint32_t firstAudioAt = 0;

  auto feedBase64 = [&](const String& text) {
    if (!streamReady || playbackFailed || text.length() < 4) return;
    size_t decodedLength = 0;
    std::vector<uint8_t> decoded(text.length() * 3 / 4 + 4);
    if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decodedLength,
                              reinterpret_cast<const uint8_t*>(text.c_str()), text.length()) != 0) return;
    if (!player.feed(decoded.data(), decodedLength)) playbackFailed = true;
    if (!shown && stream.ready()) {
      shownText = partialAnswer.length() ? partialAnswer : question;
      showAskSpeaking(shownText, 0);
      shown = true;
      audioPlaying = true;
      stream.start();
      firstAudioAt = millis();
    }
  };
  int lineCount = 0;
  auto anyText = [](const String& payload, std::initializer_list<const char*> keys) {
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return String();
    if (doc.is<const char*>()) return String(doc.as<const char*>());
    for (const char* key : keys)
      if (doc[key].is<const char*>() && strlen(doc[key].as<const char*>())) return String(doc[key].as<const char*>());
    return String();
  };
  auto handleLine = [&](String raw) {
    raw.trim();
    if (raw.startsWith("data:")) { raw = raw.substring(5); raw.trim(); }   // tolerate SSE framing
    if (raw.length() < 1) return;
    const char kind = raw[0];
    const String payload = raw.length() > 2 ? raw.substring(2) : String();
    if (kind != 'B' && lineCount < 40)   // see exactly what the server sends
      Serial.printf("[Voice] < %c %s\n", kind, payload.substring(0, 160).c_str());
    lineCount++;
    if (kind == 'T') { question = anyText(payload, {"text", "question", "q"}); if (!shown) showAskProgress("BUILDING", nullptr); }
    else if (kind == 'P') { const String part = anyText(payload, {"text", "t", "sentence", "answer"}); if (part.length()) partialAnswer += (partialAnswer.length() ? " " : "") + part; }
    else if (kind == 'B') feedBase64(payload);
    else if (kind == 'A') answer = anyText(payload, {"answer", "text", "a"});
    else if (kind == 'E') { ended = true; serverError = anyText(payload, {"error", "detail", "message"}); }
  };

  OracleHttpResponse response(16UL * 1024UL * 1024UL);
  response.sink = [&](char c) {
    if (response.status != 200) { if (errorBody.length() < 300) errorBody += c; return true; }
    if (c == '\n') { handleLine(line); line = ""; return !stream.wasCancelled(); }
    if (c == '\r') return true;
    line += c;
    // Decode long voice lines in pieces so playback starts early and RAM stays small.
    if (line.length() >= 8194 && line.startsWith("B ") && (line.length() - 2) % 4 == 0) {
      feedBase64(line.substring(2));
      line = "B ";
    }
    return true;
  };
  oracleSoundSetEnabled(false);
  uint32_t lastByteAt = millis();
  const uint32_t waitStarted = millis();
  bool receivedAny = false;
  while (!response.done() && !response.failed() && !ended) {
    if (streamReady && !stream.poll()) break;   // upper button stops the answer
    if (!receivedAny && millis() - waitStarted > 25000UL) break;   // server silent: do not hang
    int budget = 4096;
    while (budget-- > 0 && client.available() && !response.done() && !response.failed()) {
      const int value = client.read();
      if (value < 0) break;
      response.feed(static_cast<char>(value));
      lastByteAt = millis();
      receivedAny = true;
    }
    if (!client.available()) {
      if (!client.connected()) { response.eof(); break; }
      if (receivedAny && millis() - lastByteAt > 30000UL) break;
      delay(1);
    }
  }
  if (line.length() && !stream.wasCancelled()) handleLine(line);
  client.stop();
  if (streamReady) player.flush();
  if (!answer.length()) answer = partialAnswer;
  answer.trim();
  const bool complete = ended || response.done();
  if (streamReady && complete && !shown && player.hasAudio()) {
    shownText = answer;
    showAskSpeaking(answer, 0);
    shown = true;
    audioPlaying = true;
  }
  /* Wi-Fi stays on while awake (key feature); sleep turns it off */
  if (streamReady) stream.finish(complete && !playbackFailed);
  audioPlaying = false;
  oracleSoundSetEnabled(true);
  Serial.printf("[Voice] status=%d upload done=%lu ms first audio=%lu ms total=%lu ms heard=\"%s\"\n",
                response.status, static_cast<unsigned long>(uploadedAt - startedAt),
                static_cast<unsigned long>(firstAudioAt ? firstAudioAt - startedAt : 0),
                static_cast<unsigned long>(millis() - startedAt), question.c_str());
  if (response.status != 200) {
    error = response.status > 0 ? describe(response.status) : String(receivedAny ? "SERVER TIMEOUT" : "SERVER SLOW");
    Serial.printf("[Voice] error body: %s\n", errorBody.c_str());
    return response.status;
  }
  if (!answer.length()) {
    error = "NO ANSWER";
    Serial.printf("[Voice] reply had no answer (%d lines) %s\n", lineCount, serverError.c_str());
  }
  // Leave the complete answer on screen for paging.
  if (answer.length() && shownText != answer) showAskSpeaking(answer, 0);
  return 200;
}

// ─── Streaming the question while it is spoken ─────────────────────────────
// The recorder pushes audio into a PSRAM ring; a core-0 task opens the TLS
// connection as soon as Wi-Fi is up and sends the audio with chunked transfer
// encoding while the user is still talking. When they stop, only the last
// fraction of a second remains to be sent.
struct VoiceUpload {
  Conn* conn = nullptr;
  uint8_t* ring = nullptr;
  uint32_t capacity = 0;
  std::atomic<uint32_t> written{0};
  std::atomic<uint32_t> sent{0};
  std::atomic<bool> ended{false};
  std::atomic<bool> abort{false};
  std::atomic<bool> running{false};
  std::atomic<bool> ok{false};
  uint32_t startedAt = 0;
  uint32_t uploadedAt = 0;
  VoiceTarget target;
} voiceUpload;

Client& voiceClient() {
  return voiceUpload.conn->tls ? static_cast<Client&>(voiceUpload.conn->secure)
                               : static_cast<Client&>(voiceUpload.conn->plain);
}


// Opens the TLS connection while the question is being spoken, so only the
// (sub-second) upload remains when the speaker stops.
void voiceUploadTask(void*) {
  VoiceUpload& v = voiceUpload;
  const uint32_t waitStarted = millis();
  while (WiFi.status() != WL_CONNECTED && !v.abort.load() && millis() - waitStarted < 12000UL) delay(20);
  if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(false);   // power-save slows uploads ~3x
  const bool connected = !v.abort.load() && WiFi.status() == WL_CONNECTED &&
                         voiceClient().connect(v.target.host.c_str(), v.target.port);
  v.uploadedAt = millis();
  v.ok = connected && !v.abort.load();
  v.running = false;
  vTaskDelete(nullptr);
}

void releaseVoiceUpload() {
  VoiceUpload& v = voiceUpload;
  if (v.conn) { voiceClient().stop(); delete v.conn; v.conn = nullptr; }
  if (v.ring) { heap_caps_free(v.ring); v.ring = nullptr; }
}
}  // namespace

bool oracleVoiceBegin() {
  VoiceUpload& v = voiceUpload;
  releaseVoiceUpload();
  if (!oracleHasMinutesKey()) return false;
  v.conn = new Conn();
  v.target = voiceTarget();
  v.written = 0; v.sent = 0;
  v.ended = false; v.abort = false; v.ok = false;
  v.startedAt = millis();
  v.running = true;
  if (xTaskCreatePinnedToCore(voiceUploadTask, "voice-upload", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    v.running = false;
    releaseVoiceUpload();
    return false;
  }
  return true;
}

void oracleVoicePush(const int16_t* samples, size_t count) {
  // Audio is sent once the question is complete (exact length, standard
  // upload); the connection itself is already open by then.
  VoiceUpload& v = voiceUpload;
  if (!v.ring) return;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(samples);
  const size_t length = count * 2;
  uint32_t position = v.written.load();
  if (position - v.sent.load() + length > v.capacity) return;   // never in 30 s
  for (size_t i = 0; i < length; ++i) v.ring[(position + i) % v.capacity] = bytes[i];
  v.written = position + static_cast<uint32_t>(length);
}

void oracleVoiceCancel() {
  VoiceUpload& v = voiceUpload;
  v.abort = true;
  while (v.running.load()) delay(5);
  releaseVoiceUpload();
}

bool oracleVoiceFinish(const char* wavPath, String& question, String& answer, String& error) {
  VoiceUpload& v = voiceUpload;
  question = ""; answer = ""; error = "";
  v.ended = true;
  const uint32_t waitStarted = millis();
  while (v.running.load() && millis() - waitStarted < 15000UL) delay(5);
  if (v.running.load()) { v.abort = true; while (v.running.load()) delay(5); }
  std::vector<uint8_t> body;
  const bool compressed = buildAdpcmWav(wavPath, body);
  if (!compressed && !readWholeFile(wavPath, body)) { releaseVoiceUpload(); error = "RECORDING MISSING"; return false; }
  if (v.ok.load() && v.conn && voiceClient().connected()) {
    Client& client = voiceClient();
    const uint32_t startedAt = millis();
    const String head = v.target.headers + "Content-Length: " + String(body.size()) + "\r\n\r\n";
    if (writeAll(client, reinterpret_cast<const uint8_t*>(head.c_str()), head.length()) &&
        writeAll(client, body.data(), body.size())) {
      const uint32_t uploadedAt = millis();
      Serial.printf("[Voice] sent %u bytes (%s) in %lu ms\n", static_cast<unsigned>(body.size()),
                    compressed ? "ADPCM" : "PCM", static_cast<unsigned long>(uploadedAt - startedAt));
      const int status = readVoiceReply(client, startedAt, uploadedAt, question, answer, error);
      releaseVoiceUpload();
      if (answer.length()) return true;
      // The server could not use the compressed form: send plain PCM once.
      if (compressed && (status == 400 || status == 415 || status == 422 || status >= 500)) {
        Serial.printf("[Voice] server refused ADPCM (%d); sending PCM\n", status);
        error = "";
        return oracleVoiceAsk(wavPath, question, answer, error);
      }
      // Retry only if the connection was closed at once (dropped while the
      // question was spoken); a server that answered is not asked twice.
      if (status != 0 || millis() - uploadedAt > 5000UL) return false;
    }
    Serial.println("[Voice] pre-opened connection unusable; retrying on a fresh one");
  }
  releaseVoiceUpload();
  if (WiFi.status() != WL_CONNECTED) { if (!error.length()) error = "NO WIFI"; return false; }
  error = "";
  return oracleVoiceAsk(wavPath, question, answer, error);
}

bool oracleVoiceAsk(const char* wavPath, String& question, String& answer, String& error) {
  question = ""; answer = ""; error = "";
  if (!oracleHasMinutesKey()) { error = "ADD MINUTES KEY"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "NO WIFI"; return false; }
  File file = sdOpenRead(wavPath);
  if (!file) { error = "RECORDING MISSING"; return false; }
  const size_t fileSize = file.size();
  const VoiceTarget target = voiceTarget();
  Conn conn;
  Client& client = conn.tls ? static_cast<Client&>(conn.secure) : static_cast<Client&>(conn.plain);
  const uint32_t startedAt = millis();
  if (!client.connect(target.host.c_str(), target.port)) { file.close(); error = "SERVER UNREACHABLE"; return false; }
  const String head = target.headers + "Content-Length: " + String(fileSize) + "\r\n\r\n";
  bool sent = writeAll(client, reinterpret_cast<const uint8_t*>(head.c_str()), head.length());
  std::vector<uint8_t> chunk(4096);   // heap: the loop task's stack also holds TLS
  while (sent && file.available()) {
    const int n = file.read(chunk.data(), chunk.size());
    if (n <= 0) break;
    sent = writeAll(client, chunk.data(), static_cast<size_t>(n));
  }
  file.close();
  if (!sent) { client.stop(); error = "UPLOAD FAILED"; return false; }
  const int status = readVoiceReply(client, startedAt, millis(), question, answer, error);
  return status == 200 && answer.length() > 0;
}

bool oracleTextAsk(const String& question, String& answer, String& error) {
  answer = ""; error = "";
  if (!oracleHasMinutesKey()) { error = "ADD MINUTES KEY"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "NO WIFI"; return false; }
  JsonDocument request;
  JsonArray messages = request["messages"].to<JsonArray>();
  JsonObject system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = "Search the web when the question needs current information. Answer in one to three "
                      "short spoken sentences, no lists or markdown, in " + deviceLanguageName() + ".";
  JsonObject user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = question;
  String payload;
  serializeJson(request, payload);
  Conn conn;
  String body, askId;
  bool failed = false;
  int status = call(conn, "POST", "/api/ask?wait=15", "application/json",
                    reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
                    nullptr, 0, -1, &body, 25000);
  if (status < 200 || status >= 300) { error = describe(status); return false; }
  answer = answerFrom(body, askId, failed);
  for (int poll = 0; !answer.length() && !failed && askId.length() && poll < 3; ++poll) {
    status = call(conn, "GET", "/api/ask/" + urlEncode(askId) + "?wait=15", nullptr, nullptr, 0,
                  nullptr, 0, -1, &body, 25000);
    if (status < 200 || status >= 300) { error = describe(status); return false; }
    answer = answerFrom(body, askId, failed);
  }
  answer.trim();
  if (!answer.length()) error = failed ? "NO ANSWER" : "SERVER SLOW";
  return answer.length() > 0;
}

// Speech from the Oracle server (its voice pick, e.g. ElevenLabs) requested as
// IMA ADPCM (4x smaller) and played while it downloads. Shows the answer at
// the first audio. Returns false if nothing could be played (caller falls back).
bool oracleServerTtsStream(const String& spoken, const String& display, int sourceCount) {
  if (!oracleHasMinutesKey() || WiFi.status() != WL_CONNECTED) return false;
  const uint32_t startedAt = millis();
  const VoiceTarget target = voiceTarget();
  JsonDocument request;
  request["text"] = spoken;
  request["lang"] = oracleUiLanguage();
  request["format"] = "adpcm";
  String payload;
  serializeJson(request, payload);
  Conn conn;
  Client& client = conn.tls ? static_cast<Client&>(conn.secure) : static_cast<Client&>(conn.plain);
  if (!client.connect(target.host.c_str(), target.port)) return false;
  const String head = "POST " + target.prefix + "/api/tts HTTP/1.1\r\nHost: " + target.host +
    "\r\nAuthorization: Bearer " + oracleMinutesKey() +
    "\r\nContent-Type: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\nContent-Length: " +
    String(payload.length()) + "\r\n\r\n";
  if (!writeAll(client, reinterpret_cast<const uint8_t*>(head.c_str()), head.length()) ||
      !writeAll(client, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length())) {
    client.stop();
    return false;
  }
  OracleSpeechStream stream;
  stream.from24k = false;
  if (!stream.begin()) { client.stop(); return false; }
  AdpcmPlayer player(stream);
  bool shown = false, playbackFailed = false;
  uint32_t firstAudioAt = 0;
  std::vector<uint8_t> pending;
  pending.reserve(1024);
  OracleHttpResponse response(16UL * 1024UL * 1024UL);
  response.sink = [&](char c) {
    if (response.status != 200) return true;
    pending.push_back(static_cast<uint8_t>(c));
    if (pending.size() >= 512) {
      if (!player.feed(pending.data(), pending.size())) playbackFailed = true;
      pending.clear();
      if (!shown && stream.ready()) {
        showAskSpeaking(display, sourceCount);
        shown = true;
        audioPlaying = true;
        stream.start();
        firstAudioAt = millis();
      }
    }
    return !stream.wasCancelled() && !playbackFailed;
  };
  oracleSoundSetEnabled(false);
  uint32_t lastByteAt = millis();
  bool receivedAny = false;
  while (!response.done() && !response.failed()) {
    if (!stream.poll()) break;   // upper button stops the answer
    if (!receivedAny && millis() - startedAt > 30000UL) break;
    int budget = 4096;
    while (budget-- > 0 && client.available() && !response.done() && !response.failed()) {
      const int value = client.read();
      if (value < 0) break;
      response.feed(static_cast<char>(value));
      lastByteAt = millis();
      receivedAny = true;
    }
    if (!client.available()) {
      if (!client.connected()) { response.eof(); break; }
      if (receivedAny && millis() - lastByteAt > 20000UL) break;
      delay(1);
    }
  }
  client.stop();
  if (!pending.empty() && !playbackFailed) player.feed(pending.data(), pending.size());
  player.flush();
  const bool complete = response.done() && response.status == 200 && player.hasAudio();
  if (complete && !shown) { showAskSpeaking(display, sourceCount); shown = true; audioPlaying = true; }
  const bool played = stream.finish(complete && !playbackFailed);
  audioPlaying = false;
  oracleSoundSetEnabled(true);
  Serial.printf("[Minutes] TTS stream status=%d first audio=%lu ms total=%lu ms\n", response.status,
                static_cast<unsigned long>(firstAudioAt ? firstAudioAt - startedAt : 0),
                static_cast<unsigned long>(millis() - startedAt));
  return stream.wasCancelled() || (complete && played) || shown;
}

void oracleVoiceHistoryClear() { voiceHistory.clear(); }
void oracleVoiceHistoryAdd(const String& question, const String& answer) {
  if (!question.length() || !answer.length()) return;
  voiceHistory.emplace_back(question, answer);
  if (voiceHistory.size() > 4) voiceHistory.erase(voiceHistory.begin());
}
