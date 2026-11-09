// Web Companion server: serves the single-page app (web/app.html, gzipped
// into the firmware) once, and a small JSON API that the app calls.
//
// Built on ESP-IDF's esp_http_server: it runs in its own task, keeps
// connections alive and serves several browser sockets, so the Arduino loop
// (buttons, screen) never waits for the browser and vice versa. While the
// Companion is open the main loop does no background work, so handlers can
// use the note catalog and SD card directly.
#include <Arduino.h>
#include "psram_json.h"
#include <esp_http_server.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <map>
#include <vector>
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include "../../config.h"
#include "../../globals.h"
#include "network.h"
#include "notes.h"
#include "brain.h"
#include "tasks.h"
#include "battery.h"
#include "settings.h"
#include "ui.h"
#include "sd_read.h"
#include "storage_commit.h"
#include "speech_detect.h"
#include "viewer_i18n.h"
#include "viewer_request_policy.h"
#include "web_app_gz.h"   // generated from web/app.html by scripts/web_embed.py

void resetActivity();

static httpd_handle_t server = nullptr;

// ─── Small helpers ──────────────────────────────────────────────────────────

static String urlDecode(const char* in) {
  String out;
  for (const char* p = in; *p; p++) {
    if (*p == '+') out += ' ';
    else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      char hex[3] = {p[1], p[2], 0};
      out += (char)strtol(hex, nullptr, 16);
      p += 2;
    } else out += *p;
  }
  return out;
}

static String queryArg(httpd_req_t* req, const char* key) {
  const size_t len = httpd_req_get_url_query_len(req);
  if (!len) return "";
  std::vector<char> query(len + 1);
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) return "";
  std::vector<char> value(len + 1);
  if (httpd_query_key_value(query.data(), key, value.data(), value.size()) != ESP_OK) return "";
  return urlDecode(value.data());
}

static String header(httpd_req_t* req, const char* name) {
  const size_t len = httpd_req_get_hdr_value_len(req, name);
  if (!len) return "";
  std::vector<char> value(len + 1);
  if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) return "";
  return String(value.data());
}

static const char* statusLine(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 206: return "206 Partial Content";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 413: return "413 Payload Too Large";
    case 416: return "416 Range Not Satisfiable";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
  }
}

static esp_err_t sendText(httpd_req_t* req, int status, const char* type, const String& body) {
  httpd_resp_set_status(req, statusLine(status));
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t sendJson(httpd_req_t* req, JsonDocument& doc, int status = 200) {
  String body;
  serializeJson(doc, body);
  return sendText(req, status, "application/json", body);
}

static esp_err_t sendError(httpd_req_t* req, int status, const String& message) {
  JsonDocument doc(psramJson());
  doc["ok"] = false;
  doc["error"] = message;
  return sendJson(req, doc, status);
}

static esp_err_t sendOk(httpd_req_t* req) {
  return sendText(req, 200, "application/json", "{\"ok\":true}");
}

// Writes only from the Companion page itself (blocks cross-site form posts).
static bool writeAllowed(httpd_req_t* req) {
  return viewerWriteAllowed(header(req, "Host").c_str(), header(req, "Origin").c_str(),
                            header(req, "Referer").c_str(), header(req, "Sec-Fetch-Site").c_str());
}

static bool readJsonBody(httpd_req_t* req, JsonDocument& doc, size_t maxBytes = 8192) {
  if (req->content_len == 0 || req->content_len > maxBytes) return false;
  std::vector<char> body(req->content_len + 1, 0);
  size_t got = 0;
  while (got < req->content_len) {
    const int n = httpd_req_recv(req, body.data() + got, req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;
    got += n;
  }
  return !deserializeJson(doc, body.data(), got);
}

// Every handler: counts as activity (keeps the Companion awake) and, for
// POSTs, checks the request comes from the Companion page.
#define COMPANION_GET(name) static esp_err_t name##Impl(httpd_req_t* req); \
  static esp_err_t name(httpd_req_t* req) { resetActivity(); const esp_err_t r = name##Impl(req); resetActivity(); return r; } \
  static esp_err_t name##Impl(httpd_req_t* req)
#define COMPANION_POST(name) static esp_err_t name##Impl(httpd_req_t* req); \
  static esp_err_t name(httpd_req_t* req) { resetActivity(); \
    if (!writeAllowed(req)) return sendError(req, 403, "Open the Companion directly to make changes."); \
    const esp_err_t r = name##Impl(req); resetActivity(); return r; } \
  static esp_err_t name##Impl(httpd_req_t* req)

// ─── App ────────────────────────────────────────────────────────────────────

COMPANION_GET(handleApp) {
  // Same build = same app: let the browser reuse its copy.
  const String etag = String("\"") + kWebAppEtag + "\"";
  if (header(req, "If-None-Match") == etag) {
    httpd_resp_set_status(req, statusLine(304));
    httpd_resp_set_hdr(req, "ETag", etag.c_str());
    return httpd_resp_send(req, nullptr, 0);
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "ETag", etag.c_str());
  return httpd_resp_send(req, reinterpret_cast<const char*>(kWebAppGz), sizeof(kWebAppGz));
}

// ─── Boot / status ──────────────────────────────────────────────────────────

COMPANION_GET(handleBoot) {
  loadIndex();
  JsonDocument doc(psramJson());
  doc["version"] = FIRMWARE_VERSION;
  doc["lang"] = oracleUiLanguage();
  doc["voice"] = oracleSpeechVoice();
  const BatteryReading battery = readBattery();
  doc["battery"]["pct"] = battery.valid ? battery.percent : -1;
  doc["battery"]["mv"] = battery.valid ? battery.millivolts : 0;
  doc["storage"]["used"] = SD_MMC.usedBytes();
  doc["storage"]["total"] = SD_MMC.totalBytes();
  const BrainStats stats = readBrainStats();
  doc["stats"]["memories"] = stats.memories;
  doc["stats"]["ready"] = stats.understood;
  doc["stats"]["openTasks"] = stats.openTasks;
  doc["stats"]["pending"] = stats.pending;
  const bool ap = WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
  doc["wifi"]["mode"] = ap && WiFi.status() != WL_CONNECTED ? "ap" : "sta";
  doc["wifi"]["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  doc["wifi"]["ip"] = transferUrl;
  doc["has"]["wifi"] = oracleHasWifi();
  doc["has"]["openai"] = oracleHasOpenAiKey();
  doc["has"]["minutes"] = oracleHasMinutesKey();
  doc["has"]["apPassword"] = oracleHasApPassword();
  doc["apFixed"] = oracleApPasswordFixed();
  doc["minutesUrl"] = oracleMinutesUrl();
  doc["vocabulary"] = oracleVocabulary();
  // Memory health, for diagnosing (not shown in the app).
  doc["mem"]["internalFree"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  doc["mem"]["internalLargest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  doc["mem"]["psramFree"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  JsonArray profiles = doc["profiles"].to<JsonArray>();
  for (int i = 0; i < oracleWifiProfileCount(); i++) profiles.add(oracleWifiProfile(i).ssid);
  return sendJson(req, doc);
}

// ─── Agenda ─────────────────────────────────────────────────────────────────

COMPANION_GET(handleAgenda) {
  loadIndex();
  JsonDocument doc(psramJson());
  JsonArray items = doc["items"].to<JsonArray>();
  for (const BrainTaskRecord& task : loadBrainTasks()) {
    JsonObject item = items.add<JsonObject>();
    item["id"] = task.id;
    item["text"] = task.text;
    item["type"] = task.type;
    item["due"] = task.dueUtc;
    item["dayOnly"] = task.datePrecision == "day";
    item["done"] = task.done;
    item["note"] = task.noteNum;
  }
  return sendJson(req, doc);
}

COMPANION_POST(handleTaskStatus) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  const String id = body["id"] | "";
  if (!setBrainTaskDone(id, body["done"] | false)) return sendError(req, 400, "Unknown task");
  return sendOk(req);
}

// ─── Memories (cards cached in PSRAM for the whole Companion session) ───────

static std::map<int, char*> cardCache;

static void cardCacheClear() {
  for (auto& entry : cardCache) heap_caps_free(entry.second);
  cardCache.clear();
}

static void cardCacheDrop(int num) {
  auto it = cardCache.find(num);
  if (it == cardCache.end()) return;
  heap_caps_free(it->second);
  cardCache.erase(it);
}

static const char* cardJson(const NoteEntry& note) {
  auto it = cardCache.find(note.num);
  if (it != cardCache.end()) return it->second;
  const int num = note.num;
  char path[64];
  JsonDocument doc(psramJson());
  doc["num"] = num;
  doc["created"] = noteCreatedUtc(num);
  BrainMemory memory = loadBrainMemory(num);
  doc["understood"] = memory.valid;
  String title;
  if (memory.valid) {
    title = memory.title;
    doc["summary"] = memory.summary;
  } else {
    snprintf(path, sizeof(path), "%s/note_%03d.txt", NOTES_DIR, num);
    String transcript = readSmallFile(path, 1200);
    if (!transcript.length())
      transcript = note.hasText ? String(viewerText("(leeres Transkript)", "(empty transcript)"))
                                : String(viewerText("Noch nicht transkribiert.", "Not transcribed yet."));
    doc["transcript"] = transcript;
    title = transcript;
    title.replace("\n", " ");
    title.trim();
    if (title.length() > 58) title = title.substring(0, 58) + "...";
    if (!title.length() || !note.hasText) title = String(viewerText("Gedanke ", "Memory ")) + String(num);
  }
  doc["title"] = title;
  doc["state"] = localizedQueueState(noteQueueState(num));
  const String error = noteQueueError(num);
  doc["error"] = error.length() ? localizedQueueError(error) : String("");
  doc["retries"] = error.length() ? noteQueueRetries(num) : 0;
  snprintf(path, sizeof(path), "%s/note_%03d.om", NOTES_DIR, num);
  doc["meeting"] = sdExists(path);
  snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, num);
  File wav = sdOpenRead(path);
  doc["hasWav"] = (bool)wav;
  doc["seconds"] = wav && wav.size() > 44 ? (uint32_t)((wav.size() - 44) / (SAMPLE_RATE * 2)) : 0;
  if (wav) wav.close();
  JsonArray moments = doc["moments"].to<JsonArray>();
  for (uint32_t ms : loadNoteMoments(num)) moments.add(ms);
  auto addValues = [&doc](const char* key, const std::vector<String>& values) {
    JsonArray array = doc[key].to<JsonArray>();
    for (const String& value : values) array.add(value);
  };
  addValues("topics", memory.topics);
  addValues("people", memory.people);
  addValues("projects", memory.projects);
  addValues("tasks", memory.tasks);
  addValues("decisions", memory.decisions);
  addValues("facts", memory.facts);
  addValues("questions", memory.questions);
  const size_t len = measureJson(doc);
  char* json = static_cast<char*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!json) return nullptr;
  serializeJson(doc, json, len + 1);
  cardCache[num] = json;
  return json;
}

COMPANION_GET(handleMemories) {
  loadIndex();
  const int total = noteIndex.size();
  const int offset = constrain((int)queryArg(req, "offset").toInt(), 0, total);
  const int limitArg = queryArg(req, "limit").toInt();
  const int limit = constrain(limitArg > 0 ? limitArg : 12, 1, 50);
  const int end = std::min(total, offset + limit);
  String body;
  body.reserve(512 + (end - offset) * 1600);
  body += "{\"total\":" + String(total) + ",\"offset\":" + String(offset) + ",\"items\":[";
  bool first = true;
  for (int v = offset; v < end; v++) {
    const char* card = cardJson(noteIndex[total - 1 - v]);
    if (!card) continue;
    if (!first) body += ',';
    body += card;
    first = false;
  }
  body += "]}";
  return sendText(req, 200, "application/json", body);
}

COMPANION_POST(handleMemoryDelete) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  const int num = body["num"] | 0;
  if (num <= 0) return sendError(req, 400, "Invalid memory");
  cardCacheDrop(num);
  if (!deleteNote(num)) return sendError(req, 500, "Storage error: deletion incomplete. Please check the SD card.");
  return sendOk(req);
}

COMPANION_POST(handleDeleteAll) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String confirmation = body["confirmation"] | "";
  confirmation.trim();
  if (confirmation != "DELETE ALL") return sendError(req, 400, "Confirmation did not match. Nothing was deleted.");
  cardCacheClear();
  if (!deleteAllNotes()) { loadIndex(); return sendError(req, 500, "Could not remove every memory. Please try again."); }
  lastRecNum = -1;
  return sendOk(req);
}

COMPANION_POST(handleAsk) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String question = body["question"] | "";
  question.trim();
  if (!question.length() || question.length() > 240) return sendError(req, 400, "Invalid question");
  String answer;
  std::vector<int> sources;
  if (WiFi.status() != WL_CONNECTED || !answerFromBrain(question, answer, sources))
    return sendError(req, 503, viewerText(
      "Oracle konnte nicht antworten. Verbinde das Gerät mit WLAN, hinterlege einen API-Key und synchronisiere zuerst mindestens einen verstandenen Gedanken.",
      "Oracle could not answer. Connect the device to Wi-Fi, add an API key, and sync at least one Smart Memory first."));
  JsonDocument doc(psramJson());
  doc["ok"] = true;
  doc["answer"] = answer;
  JsonArray list = doc["sources"].to<JsonArray>();
  for (int n : sources) list.add(n);
  return sendJson(req, doc);
}

COMPANION_GET(handleRecap) {
  loadIndex();
  const String period = queryArg(req, "period") == "month" ? "month" : "day";
  const String localNow = utcToLocalDeviceLabel(currentUtcIso());
  const String prefix = period == "month" ? localNow.substring(0, std::min(7, (int)localNow.length()))
                                          : localNow.substring(0, std::min(10, (int)localNow.length()));
  const int latest = noteIndex.empty() ? 0 : noteIndex.back().num;
  const String cacheKey = period + "|" + prefix + "|" + String(noteIndex.size()) + "|" + String(latest);
  const String cachePath = String(NOTES_DIR) + "/.recap-" + period + ".txt";
  String recap, context;
  File cached = sdOpenRead(cachePath);
  if (cached) {
    String storedKey = cached.readStringUntil('\n');
    storedKey.trim();
    if (storedKey == cacheKey) recap = cached.readString();
    cached.close();
  }
  int included = 0;
  for (int i = (int)noteIndex.size() - 1; i >= 0 && included < 40; i--) {
    BrainMemory memory = loadBrainMemory(noteIndex[i].num);
    if (!memory.valid || !utcToLocalDeviceLabel(memory.createdUtc).startsWith(prefix)) continue;
    if (!recap.length()) {
      context += "\n[" + memory.createdUtc + "] " + memory.title + "\n" + memory.summary;
      for (const String& decision : memory.decisions) context += "\nDecision: " + decision;
      for (const String& task : memory.tasks) context += "\nTask: " + task;
    }
    included++;
  }
  if (!recap.length() && included > 0) {
    if (WiFi.status() == WL_CONNECTED) {
      JsonDocument request(psramJson());
      request["model"] = "gpt-4o-mini";
      request["store"] = false;
      request["max_output_tokens"] = 500;
      request["instructions"] = "Create a concise private second-brain recap in the language of the memories. Cover progress, decisions, open loops, patterns, and next actions. Use only supplied memories. Use short sections and bullets. Always address the user informally: use du/dein in German, never Sie/Ihr.";
      request["input"] = String("Period: ") + period + "\nMemories:" + context;
      String payload, response;
      serializeJson(request, payload);
      if (openAiJsonPost("/v1/responses", payload, response)) recap = responseOutputText(response);
    }
    if (!recap.length()) recap = context;
    if (recap.length()) {
      sdRemoveIfExists(cachePath.c_str());
      File file = sdCreate(cachePath.c_str());
      if (file) { file.println(cacheKey); file.print(recap); file.close(); }
    }
  }
  JsonDocument doc(psramJson());
  doc["count"] = included;
  doc["text"] = recap;
  return sendJson(req, doc);
}

// ─── Downloads and audio (with Range, in bounded pieces) ────────────────────

static esp_err_t sendFile(httpd_req_t* req, const String& path, const char* type, const String& attachmentName,
                          bool allowRange) {
  File f = sdOpenRead(path.c_str());
  if (!f || f.isDirectory()) { if (f) f.close(); return sendError(req, 404, "File not found"); }
  const size_t size = f.size();
  size_t start = 0, end = size ? size - 1 : 0;
  bool partial = false;
  const String range = allowRange ? header(req, "Range") : String("");
  if (range.startsWith("bytes=") && size) {
    const int dash = range.indexOf('-');
    const String a = range.substring(6, dash), b = range.substring(dash + 1);
    if (a.length()) { start = a.toInt(); if (b.length()) end = std::min((size_t)b.toInt(), size - 1); }
    else if (b.length()) { const size_t n = std::min((size_t)b.toInt(), size); start = size - n; }
    if (start >= size || start > end) {
      f.close();
      httpd_resp_set_status(req, statusLine(416));
      const String cr = "bytes */" + String(size);
      httpd_resp_set_hdr(req, "Content-Range", cr.c_str());
      return httpd_resp_send(req, nullptr, 0);
    }
    // Answer long open-ended ranges in 512 KB pieces so one recording never
    // ties up the server; the browser asks for the next piece itself.
    end = std::min(end, start + 512 * 1024 - 1);
    partial = true;
  }
  httpd_resp_set_type(req, type);
  String contentRange, disposition;
  if (allowRange) httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  if (partial) {
    httpd_resp_set_status(req, statusLine(206));
    contentRange = "bytes " + String(start) + "-" + String(end) + "/" + String(size);
    httpd_resp_set_hdr(req, "Content-Range", contentRange.c_str());
  }
  if (attachmentName.length()) {
    disposition = "attachment; filename=\"" + attachmentName + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
  }
  f.seek(start);
  size_t left = size ? end - start + 1 : 0;
  static uint8_t* buf = nullptr;
  constexpr size_t kChunk = 8192;
  if (!buf) buf = static_cast<uint8_t*>(heap_caps_malloc(kChunk, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  if (!buf) { f.close(); return sendError(req, 500, "Out of memory"); }
  esp_err_t result = ESP_OK;
  while (left > 0) {
    const int n = f.read(buf, std::min(left, kChunk));
    if (n <= 0) break;
    if ((result = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(buf), n)) != ESP_OK) break;
    left -= n;
    resetActivity();
  }
  f.close();
  if (result == ESP_OK) result = httpd_resp_send_chunk(req, nullptr, 0);
  return result;
}

static String notePath(httpd_req_t* req, const char* ext, int& num) {
  num = queryArg(req, "num").toInt();
  char path[64];
  snprintf(path, sizeof(path), "%s/note_%03d.%s", NOTES_DIR, num, ext);
  return path;
}

COMPANION_GET(handleAudio) {
  int num; const String path = notePath(req, "wav", num);
  if (num <= 0) return sendError(req, 400, "Invalid memory");
  return sendFile(req, path, "audio/wav", "", true);
}
COMPANION_GET(handleWavDownload) {
  int num; const String path = notePath(req, "wav", num);
  if (num <= 0) return sendError(req, 400, "Invalid memory");
  return sendFile(req, path, "audio/wav", "note_" + String(num) + ".wav", true);
}
COMPANION_GET(handleTxtDownload) {
  int num; const String path = notePath(req, "txt", num);
  if (num <= 0) return sendError(req, 400, "Invalid memory");
  return sendFile(req, path, "text/plain; charset=utf-8", "note_" + String(num) + ".txt", false);
}

COMPANION_GET(handleExport) {
  loadIndex();
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"oracle_notes_export.txt\"");
  httpd_resp_sendstr_chunk(req, "Oracle Lite Export\n------------------------------\n\n");
  for (int v = 0; v < (int)noteIndex.size(); v++) {
    const NoteEntry& note = noteIndex[noteIndex.size() - 1 - v];
    char path[64];
    snprintf(path, sizeof(path), "%s/note_%03d.txt", NOTES_DIR, note.num);
    String transcript = readSmallFile(path, 16000);
    if (!transcript.length())
      transcript = note.hasText ? String(viewerText("(leeres Transkript)", "(empty transcript)"))
                                : String(viewerText("Noch nicht transkribiert.", "Not transcribed yet."));
    String block = "#" + String(note.num) + " · " + String(note.tag) + "\n" + noteCreatedUtc(note.num) +
                   "\n\n" + transcript + "\n\n------------------------------\n\n";
    if (httpd_resp_sendstr_chunk(req, block.c_str()) != ESP_OK) return ESP_FAIL;
    resetActivity();
  }
  return httpd_resp_sendstr_chunk(req, nullptr);
}

// ─── Files ──────────────────────────────────────────────────────────────────

static bool filesPathOk(const String& path) {
  return path.length() > 0 && path.length() < 200 && path[0] == '/' && path.indexOf("..") < 0;
}

static bool filesProtected(const String& path) {
  return path == INDEX_FILE || path == "/notes/index.tmp" || path == "/notes/index.bak" ||
         path == NOTES_DIR "/tasks.state" || path.startsWith(NOTES_DIR "/brain.search") ||
         path.startsWith(NOTES_DIR "/brain.vectors") || path == NOTES_DIR "/.tz_sgt";
}

// note_012.anything -> 12, otherwise 0.
static int filesNoteNumber(const String& path) {
  const String name = path.substring(path.lastIndexOf('/') + 1);
  if (!path.startsWith(NOTES_DIR "/") || !name.startsWith("note_") || name.length() < 9) return 0;
  int n = 0, i = 5;
  for (; i < (int)name.length() && isDigit(name[i]); i++) n = n * 10 + (name[i] - '0');
  return (i > 5 && i < (int)name.length() && name[i] == '.') ? n : 0;
}

COMPANION_GET(handleFiles) {
  String dir = queryArg(req, "dir");
  if (!dir.length() || !filesPathOk(dir)) dir = NOTES_DIR;
  if (dir.length() > 1 && dir.endsWith("/")) dir.remove(dir.length() - 1);
  File root = SD_MMC.open(dir.c_str());
  if (!root || !root.isDirectory()) { if (root) root.close(); return sendError(req, 404, "Folder not found"); }
  struct Entry { String name; bool folder; uint64_t size; time_t mtime; };
  std::vector<Entry> entries;
  for (File f = root.openNextFile(); f && entries.size() < 3000; f = root.openNextFile()) {
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    entries.push_back({name, f.isDirectory(), f.isDirectory() ? 0 : (uint64_t)f.size(), f.getLastWrite()});
    f.close();
  }
  root.close();
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.folder != b.folder) return a.folder;
    return a.name > b.name;
  });
  // Large folders: stream the JSON instead of building one big document.
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  JsonDocument head(psramJson());
  head["dir"] = dir;
  String start;
  serializeJson(head, start);
  start.remove(start.length() - 1);   // drop '}'
  start += ",\"used\":" + String((unsigned long long)SD_MMC.usedBytes()) +
           ",\"total\":" + String((unsigned long long)SD_MMC.totalBytes()) + ",\"entries\":[";
  httpd_resp_sendstr_chunk(req, start.c_str());
  String chunk;
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry& e = entries[i];
    const String path = (dir == "/" ? String("") : dir) + "/" + e.name;
    JsonDocument item(psramJson());
    item["name"] = e.name;
    item["dir"] = e.folder;
    item["size"] = e.size;
    item["mtime"] = (long long)e.mtime;
    item["protected"] = e.folder || filesProtected(path);
    item["note"] = filesNoteNumber(path);
    if (i) chunk += ',';
    serializeJson(item, chunk);
    if (chunk.length() > 3000) { httpd_resp_sendstr_chunk(req, chunk.c_str()); chunk = ""; }
  }
  chunk += "]}";
  httpd_resp_sendstr_chunk(req, chunk.c_str());
  return httpd_resp_sendstr_chunk(req, nullptr);
}

COMPANION_GET(handleFileGet) {
  const String path = queryArg(req, "path");
  if (!filesPathOk(path)) return sendError(req, 400, "Invalid path");
  const String name = path.substring(path.lastIndexOf('/') + 1);
  const char* type = name.endsWith(".wav") ? "audio/wav" : name.endsWith(".json") ? "application/json"
                   : (name.endsWith(".txt") || name.endsWith(".csv")) ? "text/plain; charset=utf-8" : "application/octet-stream";
  return sendFile(req, path, type, name, true);
}

COMPANION_POST(handleFilesDelete) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body, 64 * 1024)) return sendError(req, 400, "Invalid request");
  int deleted = 0;
  std::vector<int> notes;
  for (JsonVariant value : body["paths"].as<JsonArray>()) {
    const String path = value.as<String>();
    if (!filesPathOk(path) || filesProtected(path)) continue;
    const int num = filesNoteNumber(path);
    if (num > 0) {
      if (std::find(notes.begin(), notes.end(), num) == notes.end()) notes.push_back(num);
      continue;
    }
    File f = SD_MMC.open(path.c_str());
    const bool isFile = f && !f.isDirectory();
    if (f) f.close();
    if (isFile && sdRemoveIfExists(path.c_str())) { storageChanged(); deleted++; }
  }
  loadIndex();
  for (int num : notes) {
    cardCacheDrop(num);
    if (deleteNote(num)) deleted++;
  }
  JsonDocument doc(psramJson());
  doc["ok"] = true;
  doc["deleted"] = deleted;
  return sendJson(req, doc);
}

// Recordings that hold only background noise (memories, not meetings).
COMPANION_GET(handleSilent) {
  loadIndex();
  const uint32_t started = millis();
  const int total = noteIndex.size();
  int index = std::max(0, (int)queryArg(req, "from").toInt());
  JsonDocument doc(psramJson());
  JsonArray hits = doc["hits"].to<JsonArray>();
  std::vector<int16_t> buf(2048);
  for (; index < total; index++) {
    if (millis() - started > 6000UL) break;
    const NoteEntry& note = noteIndex[total - 1 - index];
    char path[64];
    snprintf(path, sizeof(path), "%s/note_%03d.om", NOTES_DIR, note.num);
    if (sdExists(path)) continue;
    snprintf(path, sizeof(path), "%s/note_%03d.wav", NOTES_DIR, note.num);
    File wav = sdOpenRead(path);
    if (!wav) continue;
    const uint32_t seconds = wav.size() > 44 ? (wav.size() - 44) / (SAMPLE_RATE * 2) : 0;
    char txt[64];
    snprintf(txt, sizeof(txt), "%s/note_%03d.txt", NOTES_DIR, note.num);
    String transcript = readSmallFile(txt, 200);
    transcript.trim();
    const char* reason = nullptr;
    if (note.hasText && !transcript.length()) reason = viewerText("leeres Transkript", "empty transcript");
    else if (!note.hasText) {
      SpeechDetector speech;
      wav.seek(44);
      while (!speech.heardSpeech()) {
        const int got = wav.read(reinterpret_cast<uint8_t*>(buf.data()), buf.size() * 2);
        if (got <= 0) break;
        speech.feed(buf.data(), got / 2);
      }
      if (!speech.heardSpeech()) reason = viewerText("keine Sprache erkannt", "no speech heard");
    }
    wav.close();
    if (reason) {
      JsonObject hit = hits.add<JsonObject>();
      hit["num"] = note.num;
      hit["reason"] = reason;
      hit["seconds"] = seconds;
    }
  }
  doc["total"] = total;
  doc["checked"] = index;
  if (index < total) doc["next"] = index; else doc["next"] = nullptr;
  return sendJson(req, doc);
}

// ─── Settings ───────────────────────────────────────────────────────────────

COMPANION_GET(handleWifiScan) {
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);   // scanning needs the station side
  oracleApplyWifiRegion();
  const int found = WiFi.scanNetworks(false, true);
  struct Seen { String ssid; int rssi; int channel; wifi_auth_mode_t auth; };
  std::vector<Seen> seen;
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    auto same = std::find_if(seen.begin(), seen.end(), [&](const Seen& s) { return s.ssid == ssid && ssid.length(); });
    if (same != seen.end()) { if (WiFi.RSSI(i) > same->rssi) same->rssi = WiFi.RSSI(i); continue; }
    seen.push_back({ssid, WiFi.RSSI(i), static_cast<int>(WiFi.channel(i)), WiFi.encryptionType(i)});
  }
  WiFi.scanDelete();
  std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.rssi > b.rssi; });
  auto security = [](wifi_auth_mode_t auth) -> const char* {
    switch (auth) {
      case WIFI_AUTH_OPEN: return "Open";
      case WIFI_AUTH_WEP: return "WEP";
      case WIFI_AUTH_WPA_PSK: return "WPA";
      case WIFI_AUTH_WPA2_PSK: return "WPA2";
      case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
      case WIFI_AUTH_WPA3_PSK: return "WPA3";
      case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
      case WIFI_AUTH_WPA2_ENTERPRISE: return "Enterprise (not supported)";
      default: return "Secured";
    }
  };
  JsonDocument doc(psramJson());
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (const Seen& network : seen) {
    bool saved = false;
    for (int p = 0; p < oracleWifiProfileCount(); ++p) if (oracleWifiProfile(p).ssid == network.ssid) saved = true;
    JsonObject item = networks.add<JsonObject>();
    item["ssid"] = network.ssid;
    item["rssi"] = network.rssi;
    item["channel"] = network.channel;
    item["security"] = security(network.auth);
    item["open"] = network.auth == WIFI_AUTH_OPEN;
    item["saved"] = saved;
  }
  return sendJson(req, doc);
}

COMPANION_POST(handleConnection) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String ssid = body["ssid"] | "", password = body["password"] | "", apiKey = body["apiKey"] | "";
  ssid.trim();
  apiKey.trim();
  // An empty password keeps the saved one when this network already exists.
  if (!password.length()) {
    for (int i = 0; i < oracleWifiProfileCount(); i++) {
      const OracleWifiProfile profile = oracleWifiProfile(i);
      if (profile.ssid == ssid) { password = profile.password; break; }
    }
  }
  if (!oracleSaveSettings(ssid, password, apiKey, oracleSpeechVoice())) return sendError(req, 400, "Invalid settings");
  return sendOk(req);
}

COMPANION_POST(handleWifiUpdate) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String ssid = body["ssid"] | "", newSsid = body["newSsid"] | "";
  const String password = body["password"] | "";
  ssid.trim();
  newSsid.trim();
  if (!oracleUpdateWifiProfile(ssid, newSsid.length() ? newSsid : ssid, password))
    return sendError(req, 400, "Could not update this Wi-Fi (unknown network, or the new name is already saved).");
  return sendOk(req);
}

COMPANION_POST(handleWifiRemove) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String ssid = body["ssid"] | "";
  ssid.trim();
  if (!oracleRemoveWifiProfile(ssid)) return sendError(req, 400, "Unknown Wi-Fi");
  return sendOk(req);
}

COMPANION_POST(handleLanguage) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String voice = body["speechVoice"] | "", language = body["uiLanguage"] | "";
  voice.trim();
  language.trim();
  if (voice.length() && !oracleSetSpeechVoice(voice)) return sendError(req, 400, "Invalid voice");
  if (!oracleSetUiLanguage(language)) return sendError(req, 400, "Invalid language");
  cardCacheClear();   // cards carry translated texts
  return sendOk(req);
}

COMPANION_POST(handleMinutes) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  String url = body["url"] | "", key = body["key"] | "";
  url.trim();
  key.trim();
  if (!oracleSaveMinutes(url, key)) return sendError(req, 400, "Invalid Oracle Minutes server or key");
  return sendOk(req);
}

COMPANION_POST(handleHotspot) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  const bool fixed = String(body["mode"] | "dynamic") == "fixed";
  const String password = body["password"] | "";
  if (!oracleSetApPassword(fixed, password))
    return sendError(req, 400, fixed && !password.length() && !oracleHasApPassword()
      ? "Enter a fixed password (8-63 characters) first."
      : "The password must be 8-63 characters (letters, numbers and symbols).");
  return sendOk(req);
}

COMPANION_POST(handleVocabulary) {
  JsonDocument body(psramJson());
  if (!readJsonBody(req, body)) return sendError(req, 400, "Invalid request");
  if (!oracleSetVocabulary(body["words"] | "")) return sendError(req, 500, "Could not save the words.");
  return sendOk(req);
}

COMPANION_POST(handleKeyClear) {
  if (!oracleClearOpenAiKey()) return sendError(req, 500, "Could not clear the API key. Please try again.");
  return sendOk(req);
}

// ─── Firmware update (raw .bin body) ────────────────────────────────────────
// Only Oracle Lite builds contain this marker text; any other image is
// refused before it is marked bootable.
static const char kOtaMarker[] = "Oracle-Lite-firmware-image:v1";

COMPANION_POST(handleFirmware) {
  const size_t total = req->content_len;
  if (total < 64 * 1024) return sendError(req, 400, "This file is too small to be firmware.");
  if (!Update.begin(total, U_FLASH)) return sendError(req, 400, Update.errorString());
  showAskProgress("UPDATING", "FIRMWARE");
  std::vector<uint8_t> buf(4096);
  size_t got = 0, markerPos = 0;
  int shownStep = -1;
  bool headerChecked = false;
  String error;
  while (got < total) {
    const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf.data()), std::min(buf.size(), total - got));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) { error = "Upload interrupted."; break; }
    if (!headerChecked) {
      headerChecked = true;
      const size_t descAt = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
      const esp_app_desc_t* desc = reinterpret_cast<const esp_app_desc_t*>(buf.data() + descAt);
      if (n < (int)(descAt + sizeof(esp_app_desc_t)) || buf[0] != ESP_IMAGE_HEADER_MAGIC ||
          desc->magic_word != ESP_APP_DESC_MAGIC_WORD) { error = "This is not an ESP32 firmware file (use firmware.bin)."; break; }
    }
    for (int i = 0; i < n && markerPos < sizeof(kOtaMarker) - 1; i++) {
      if (buf[i] == (uint8_t)kOtaMarker[markerPos]) markerPos++;
      else markerPos = buf[i] == (uint8_t)kOtaMarker[0] ? 1 : 0;
    }
    if (Update.write(buf.data(), n) != (size_t)n) { error = Update.errorString(); break; }
    got += n;
    resetActivity();
    const int percent = (int)((uint64_t)got * 100 / total);
    if (percent / 20 != shownStep) {
      shownStep = percent / 20;
      char object[24];
      snprintf(object, sizeof(object), "FIRMWARE %d%%", percent);
      showAskProgress("UPDATING", object);
    }
  }
  if (!error.length() && markerPos < sizeof(kOtaMarker) - 1) error = "This firmware is not for Oracle Lite.";
  if (!error.length() && !Update.end(true)) error = Update.errorString();
  if (error.length()) {
    Update.abort();
    Serial.printf("[OTA] failed: %s\n", error.c_str());
    showError("UPDATE FAILED");
    return sendError(req, 400, error);
  }
  Serial.printf("[OTA] %u bytes written; restarting\n", (unsigned)total);
  showAskProgress("UPDATED", "RESTARTING");
  httpd_resp_set_hdr(req, "Connection", "close");
  sendText(req, 200, "text/plain", "Updated. Oracle Lite is restarting…");
  delay(1200);
  ESP.restart();
  return ESP_OK;
}

// ─── Server ─────────────────────────────────────────────────────────────────

bool companionServerStart() {
  if (server) return true;
  cardCacheClear();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 16384;          // Ask/recap use TLS to the AI service
  config.max_uri_handlers = 40;
  config.max_open_sockets = 6;
  config.lru_purge_enable = true;      // a new browser socket replaces the oldest idle one
  config.recv_wait_timeout = 15;
  config.send_wait_timeout = 15;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.core_id = 0;                  // Wi-Fi core; the Arduino loop stays on core 1
  if (httpd_start(&server, &config) != ESP_OK) { server = nullptr; return false; }
  struct Route { const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); };
  const Route routes[] = {
    {"/api/boot", HTTP_GET, handleBoot},
    {"/api/agenda", HTTP_GET, handleAgenda},
    {"/api/task", HTTP_POST, handleTaskStatus},
    {"/api/memories", HTTP_GET, handleMemories},
    {"/api/memory/delete", HTTP_POST, handleMemoryDelete},
    {"/api/memories/delete-all", HTTP_POST, handleDeleteAll},
    {"/api/ask", HTTP_POST, handleAsk},
    {"/api/recap", HTTP_GET, handleRecap},
    {"/api/files", HTTP_GET, handleFiles},
    {"/api/files/delete", HTTP_POST, handleFilesDelete},
    {"/api/silent", HTTP_GET, handleSilent},
    {"/api/wifi/scan", HTTP_GET, handleWifiScan},
    {"/api/settings/connection", HTTP_POST, handleConnection},
    {"/api/wifi/update", HTTP_POST, handleWifiUpdate},
    {"/api/wifi/remove", HTTP_POST, handleWifiRemove},
    {"/api/settings/language", HTTP_POST, handleLanguage},
    {"/api/settings/minutes", HTTP_POST, handleMinutes},
    {"/api/settings/hotspot", HTTP_POST, handleHotspot},
    {"/api/key/clear", HTTP_POST, handleKeyClear},
    {"/api/settings/vocabulary", HTTP_POST, handleVocabulary},
    {"/firmware", HTTP_POST, handleFirmware},
    {"/audio", HTTP_GET, handleAudio},
    {"/wav", HTTP_GET, handleWavDownload},
    {"/txt", HTTP_GET, handleTxtDownload},
    {"/export.txt", HTTP_GET, handleExport},
    {"/files/get", HTTP_GET, handleFileGet},
    {"/*", HTTP_GET, handleApp},       // the app for every other page
  };
  for (const Route& route : routes) {
    httpd_uri_t uri = {};
    uri.uri = route.uri;
    uri.method = route.method;
    uri.handler = route.handler;
    httpd_register_uri_handler(server, &uri);
  }
  Serial.println("[Companion] server started");
  return true;
}

void companionServerStop() {
  if (!server) return;
  httpd_stop(server);
  server = nullptr;
  cardCacheClear();
}
