# Oracle Lite Web Companion API

The Companion is a single-page app (`web/app.html`) served gzipped at `/` and
cached by the browser. All data comes from the JSON endpoints below. All
responses are `application/json` unless noted. Errors: HTTP 4xx/5xx with
`{"ok":false,"error":"human readable message"}`.

POST bodies are JSON (`Content-Type: application/json`) unless noted.
Times are UTC ISO strings (`2026-11-06T03:00:00Z`); the browser shows them in
local time. "dayOnly" means the date is known but not the clock time.

## Boot / status

`GET /api/boot`

```json
{
  "version": "1.0.0-rc021",
  "lang": "en",                 // UI language: en | de | fr | es | it | pl | cs | ru | uk | nl | pt (app only localises en/de; others use en)
  "voice": "eleven",            // eleven | cedar | coral
  "battery": {"pct": 82, "mv": 3950},      // pct -1 when unknown
  "storage": {"used": 123456789, "total": 31914983424},
  "stats": {"memories": 120, "ready": 110, "openTasks": 7, "pending": 3},
  "wifi": {"mode": "sta", "ssid": "Home-WiFi", "ip": "192.168.1.23"},   // mode sta | ap
  "has": {"wifi": true, "openai": true, "minutes": true, "apPassword": true},
  "apFixed": false,
  "minutesUrl": "https://example.com",
  "profiles": ["Home-WiFi", "Office-Guest"]   // saved Wi-Fi names (max 5)
}
```

## Agenda (tasks + appointments)

`GET /api/agenda`

```json
{"items": [
  {"id": "12-ab12cd34", "text": "Meeting with Alex", "type": "event",   // event | todo | reminder
   "due": "2026-11-06T03:00:00Z", "dayOnly": false, "done": false, "note": 12}
]}
```
`due` is "" for undated items. The app derives Today, Tasks and Calendar
from this list (an event counts as past once `due` < now; dayOnly events stay
current for their whole day).

`POST /api/task` `{"id": "12-ab12cd34", "done": true}` → `{"ok": true}`

## Memories

`GET /api/memories?offset=0&limit=12` (newest first)

```json
{"total": 120, "offset": 0, "items": [
  {"num": 120, "created": "2026-11-09T07:12:00Z", "title": "Budget review",
   "summary": "…", "transcript": "…",        // transcript only when not understood yet (first 1200 chars)
   "understood": true, "state": "ready",     // state: localised queue state text (e.g. "Ready", "Waiting for Wi-Fi")
   "error": "", "retries": 0,
   "meeting": false, "hasWav": true, "seconds": 42,
   "moments": [1200, 15800],                 // ms
   "topics": [], "people": [], "projects": [], "tasks": [], "decisions": [], "facts": [], "questions": []}
]}
```

`POST /api/memory/delete` `{"num": 120}` → `{"ok": true}`

`POST /api/memories/delete-all` `{"confirmation": "DELETE ALL"}` → `{"ok": true}`

`POST /api/ask` `{"question": "…"}` (max 240 chars) →
`{"ok": true, "answer": "…", "sources": [12, 40]}` (503 + error when offline).

`GET /api/recap?period=day|month` → `{"count": 4, "text": "…"}`

## Files

`GET /api/files?dir=/notes`

```json
{"dir": "/notes", "used": 123, "total": 456, "entries": [
  {"name": "note_120.wav", "dir": false, "size": 1344044, "mtime": 1762671120,
   "protected": false, "note": 120}           // note: memory number (0 = not a memory file)
]}
```
Folders first, then names descending.

`POST /api/files/delete` `{"paths": ["/notes/note_120.wav", "/notes/.ask.wav"]}` → `{"ok": true, "deleted": 2}`
(a memory's file deletes the whole memory; protected files are skipped)

`GET /api/silent?from=0` → recordings with no speech (memories only, not meetings),
checked for ~6 s per call:
```json
{"total": 120, "checked": 40, "next": 40, "hits": [{"num": 88, "reason": "no speech heard", "seconds": 7}]}
```
`next` is null when everything has been checked.

Downloads (not JSON): `/files/get?path=/notes/note_120.txt` (attachment),
`/audio?num=120` (WAV, supports `Range`), `/wav?num=120` (attachment),
`/txt?num=120` (attachment), `/export.txt` (all transcripts).

## Settings

`GET /api/wifi/scan` →
`{"networks": [{"ssid": "Home-WiFi", "rssi": -48, "channel": 6, "security": "WPA2", "open": false, "saved": true}]}`
(takes ~3 s)

`POST /api/settings/connection` `{"ssid": "", "password": "", "apiKey": ""}` — add a
Wi-Fi network and/or replace the OpenAI key. Empty fields are kept.

`POST /api/wifi/update` `{"ssid": "old", "newSsid": "new", "password": ""}` (empty password keeps it)

`POST /api/wifi/remove` `{"ssid": "Home-WiFi"}`

`POST /api/settings/language` `{"uiLanguage": "en", "speechVoice": "eleven"}`

`POST /api/settings/minutes` `{"url": "https://…", "key": ""}` (empty key keeps it)

`POST /api/settings/hotspot` `{"mode": "fixed" | "dynamic", "password": ""}` (8–63 chars; empty keeps it)

`POST /api/key/clear` → removes the OpenAI key

All of these return `{"ok": true}` or an error.

## Firmware

`POST /firmware` with the raw `.bin` as the request body
(`Content-Type: application/octet-stream`). Response text when done; the
device then restarts. Progress is shown with XHR upload progress.
