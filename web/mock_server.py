#!/usr/bin/env python3
"""Mock Oracle Lite device for developing web/app.html (stdlib only).

    python3 mock_server.py [port]        # default 8080
    MOCK_DELAY_MS=150                    # artificial delay per API request
    MOCK_SCAN_MS=2500                    # extra delay of /api/wifi/scan

Serves app.html at / and implements every endpoint in API.md with in-memory
state; mutations change that state. Test helpers (not part of the device API):
GET /__mock/stats (max concurrent API requests seen; requests with an
X-Mock-Test header are not counted or delayed), POST /__mock/reset.
"""
import json
import os
import random
import struct
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
DELAY = int(os.environ.get("MOCK_DELAY_MS", "150")) / 1000.0
SCAN_DELAY = int(os.environ.get("MOCK_SCAN_MS", "2500")) / 1000.0
LOCK = threading.Lock()
STATS = {"inflight": 0, "max_inflight": 0, "requests": 0}


def iso(dt):
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


TITLES = [
    ("Budget review", "Went through Q4 numbers with Alex; travel budget is 12% over.", ["finance", "Q4"], ["Alex"], ["Acme"]),
    ("Kiosk voice latency", "Voice wake-word round trip is 1.8 s; target is under 1 s.", ["kiosk", "latency"], [], ["AI Concierge"]),
    ("Pinhole back plate idea", "Use a bayonet-style twist lock instead of magnets for the back plate.", ["3D print"], [], ["Pinhole 135"]),
    ("Groceries", "Milk, eggs, coffee beans and batteries for the label printer.", ["errands"], [], []),
    ("Badminton court pricing", "Peak slots RM 30, off-peak RM 20, members get 10% off.", ["pricing"], ["Sam"], ["Club"]),
    ("E-ink front light", "Front light bleeds at the bottom edge; try a thinner diffuser.", ["hardware"], [], ["Board-X"]),
    ("Call with auditor", "Jordan needs the access review evidence by Friday.", ["compliance"], ["Jordan"], ["MAS"]),
    ("Gift for Mia", "Mia mentioned she likes pour-over coffee gear.", ["personal"], ["Mia"], []),
    ("Firmware rollout plan", "Ship rc021 to the test units first, then everyone after two days.", ["firmware"], [], ["Oracle Lite"]),
    ("Running notes", "5 km in 27 minutes, knee felt fine.", ["health"], [], []),
]
MEETINGS = [
    ("Weekly product sync", "Agreed to freeze the booking app scope; payment gateway next sprint.",
     ["Freeze scope for v1", "Use Stripe test mode"]),
    ("Hardware review", "Reviewed CM4 carrier rev B; power rail fix confirmed.", ["Order rev C at JLCPCB"]),
    ("Kick-off: cafeteria system", "Hardware list and invoice schedule agreed with The Wok People.", ["Deposit invoice this week"]),
]


def build_state():
    rnd = random.Random(7)
    now = datetime.now(timezone.utc).replace(second=0, microsecond=0)
    mems = []
    first, count = 81, 40
    meeting_nums = {118, 104, 90}
    silent_nums = {112, 97, 93, 85}
    pending_nums = {120, 119}
    for i, num in enumerate(range(first + count - 1, first - 1, -1)):
        created = now - timedelta(hours=i * 11 + rnd.randint(0, 5), minutes=rnd.randint(0, 59))
        if num == 120:
            created = now - timedelta(minutes=12)
        m = {
            "num": num, "created": iso(created), "title": "", "summary": "", "transcript": "",
            "understood": True, "state": "Ready", "error": "", "retries": 0,
            "meeting": num in meeting_nums, "hasWav": True,
            "seconds": rnd.randint(6, 95), "moments": [],
            "topics": [], "people": [], "projects": [], "tasks": [], "decisions": [], "facts": [], "questions": [],
        }
        if num in meeting_nums:
            t = MEETINGS[len([x for x in mems if x["meeting"]])]
            m.update(title=t[0], summary=t[1], decisions=t[2], seconds=rnd.randint(900, 3600),
                     moments=[60000, 412000, 905000], topics=["meeting"], people=["Alex", "Sam"],
                     tasks=["Send minutes to the team"])
        elif num in silent_nums:
            m.update(understood=False, state="Ready", seconds=rnd.randint(3, 9), transcript="")
        elif num in pending_nums:
            m.update(understood=False, state="Waiting for Wi-Fi",
                     transcript="Remember to check whether the hotspot password survives a reboot, and ask Alex about the VPN.")
            if num == 119:
                m.update(state="Retrying", error="OpenAI timed out", retries=2)
        else:
            t = TITLES[i % len(TITLES)]
            m.update(title=t[0], summary=t[1], topics=t[2], people=t[3], projects=t[4],
                     moments=[1200, 15800] if i % 3 == 0 else [],
                     facts=["Prices include SST"] if "pricing" in t[2] else [],
                     questions=["Who signs off the travel budget?"] if "finance" in t[2] else [],
                     tasks=["Follow up next week"] if i % 4 == 0 else [])
        if num == 99:
            m["hasWav"] = False
        mems.append(m)

    def at(days, hour=None, minute=0):
        local = now + timedelta(days=days)
        if hour is None:
            return iso(local.replace(hour=12, minute=0)), True
        return iso(local.replace(hour=hour, minute=minute)), False

    # Today's events relative to "now": one past, two upcoming.
    h = now.hour
    raw = [
        ("Stand-up with infra team", "event", (0, max(0, h - 2)), False),
        ("Meeting with Alex", "event", (0, min(23, h + 1), 30), False),
        ("Dinner at Sam's", "event", (0, min(23, h + 3)), False),
        ("Public holiday prep", "event", (0, None), False),
        ("Dentist", "event", (3, 9, 15), False),
        ("JLCPCB order review", "event", (8, 14), False),
        ("Board demo day", "event", (24, None), False),
        ("Club proposal call", "event", (30, 10), False),
        ("Send access review evidence to Jordan", "todo", (-1, None), False),
        ("Order rev C carrier boards", "todo", (0, 17), False),
        ("Buy coffee beans", "todo", None, False),
        ("Renew domain", "reminder", (5, 9), False),
        ("Reply to Mia about gift", "todo", None, False),
        ("Draft cafeteria invoice", "todo", (-2, None), True),
        ("Book flights for summit", "todo", (12, None), False),
        ("Pay electricity bill", "reminder", (-3, 9), True),
        ("Update kiosk firmware", "todo", (2, 11), True),
    ]
    agenda = []
    for idx, (text, typ, when, done) in enumerate(raw):
        due, day_only = ("", False) if when is None else at(*when)
        note = mems[(idx * 3) % len(mems)]["num"]
        agenda.append({"id": "%d-%08x" % (note, 0xab12cd00 + idx), "text": text, "type": typ,
                       "due": due, "dayOnly": day_only, "done": done, "note": note})

    t0 = int(now.timestamp())
    extras = {
        "/notes/index.csv": [2048, t0 - 300, True],
        "/notes/tasks.state": [912, t0 - 600, True],
        "/notes/.ask.wav": [96044, t0 - 4000, False],
        "/notes/.recap-day.txt": [1800, t0 - 7200, False],
        "/oracle.log": [48211, t0 - 60, False],
        "/ota/firmware.bin": [1702345, t0 - 86400 * 4, False],
        "/ota/notes-old.txt": [3112, t0 - 86400 * 30, False],
    }
    return {
        "boot": {
            "version": "1.0.0-rc021", "lang": "en", "voice": "eleven",
            "battery": {"pct": 82, "mv": 3950},
            "storage": {"used": 0, "total": 31914983424},
            "wifi": {"mode": "sta", "ssid": "Home-WiFi", "ip": "192.168.1.23"},
            "has": {"wifi": True, "openai": True, "minutes": True, "apPassword": True},
            "apFixed": False, "minutesUrl": "https://minutes.example.com",
            "profiles": ["Home-WiFi", "Office-Guest"],
        },
        "memories": mems, "agenda": agenda, "extras": extras, "now": now,
    }


STATE = build_state()


def mem_files(m):
    created = int(datetime.strptime(m["created"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp())
    n = "/notes/note_%03d" % m["num"]
    out = {}
    if m["hasWav"]:
        out[n + ".wav"] = (44 + m["seconds"] * 32000, created)
    out[n + ".txt"] = (0 if not m["transcript"] and not m["understood"] else 300 + len(m["summary"]) * 3, created + 40)
    out[n + ".meta"] = (96, created)
    out[n + ".queue"] = (48, created + 41)
    if m["understood"]:
        out[n + ".brain.json"] = (1400 + len(m["summary"]) * 4, created + 60)
    if m["meeting"]:
        out[n + ".om"] = (220, created)
    return out


def all_files():
    files = {}
    for m in STATE["memories"]:
        for p, (size, mt) in mem_files(m).items():
            files[p] = {"size": size, "mtime": mt, "protected": False, "note": m["num"]}
    for p, (size, mt, prot) in STATE["extras"].items():
        files[p] = {"size": size, "mtime": mt, "protected": prot, "note": 0}
    return files


def used_bytes():
    return 412 * 1024 * 1024 + sum(f["size"] for f in all_files().values())


def boot():
    b = json.loads(json.dumps(STATE["boot"]))
    mems = STATE["memories"]
    b["storage"]["used"] = used_bytes()
    b["stats"] = {
        "memories": len(mems),
        "ready": len([m for m in mems if m["understood"]]),
        "openTasks": len([a for a in STATE["agenda"] if a["type"] != "event" and not a["done"]]),
        "pending": len([m for m in mems if m["state"] != "Ready"]),
    }
    return b


def silent_wav(seconds=1, rate=16000):
    data = b"\x00\x00" * rate * seconds
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data)


def note_of(path):
    name = path.rsplit("/", 1)[-1]
    if path.startswith("/notes/note_"):
        try:
            return int(name[5:8])
        except ValueError:
            return 0
    return 0


def delete_memory(num):
    before = len(STATE["memories"])
    STATE["memories"] = [m for m in STATE["memories"] if m["num"] != num]
    return len(STATE["memories"]) != before


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if os.environ.get("MOCK_LOG"):
            sys.stderr.write("mock: " + fmt % args + "\n")

    # helpers -------------------------------------------------------------
    def send(self, code, body, ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def ok(self, extra=None):
        d = {"ok": True}
        d.update(extra or {})
        self.send(200, d)

    def err(self, code, msg):
        self.send(code, {"ok": False, "error": msg})

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def json_body(self):
        try:
            return json.loads(self.body() or b"{}")
        except ValueError:
            return None

    def track(self, fn):
        api = (self.path.startswith("/api/") or self.path.startswith("/firmware")) and not self.headers.get("X-Mock-Test")
        if api:
            with LOCK:
                STATS["inflight"] += 1
                STATS["requests"] += 1
                STATS["max_inflight"] = max(STATS["max_inflight"], STATS["inflight"])
            time.sleep(DELAY)
        try:
            fn()
        finally:
            if api:
                with LOCK:
                    STATS["inflight"] -= 1

    def do_GET(self):
        self.track(self.get)

    def do_POST(self):
        self.track(self.post)

    # GET -----------------------------------------------------------------
    def get(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        p = u.path
        if p in ("/", "/app.html"):
            with open(os.path.join(HERE, "app.html"), "rb") as f:
                return self.send(200, f.read(), "text/html; charset=utf-8")
        if p == "/favicon.ico":
            return self.send(204, b"", "image/x-icon")
        if p == "/__mock/stats":
            return self.send(200, STATS)
        with LOCK:
            if p == "/api/boot":
                return self.send(200, boot())
            if p == "/api/agenda":
                return self.send(200, {"items": STATE["agenda"]})
            if p == "/api/memories":
                off, lim = int(q.get("offset", 0)), max(1, min(50, int(q.get("limit", 12))))
                mems = STATE["memories"]
                items = []
                for m in mems[off:off + lim]:
                    m = dict(m)
                    if m["understood"]:
                        m["transcript"] = ""
                    items.append(m)
                return self.send(200, {"total": len(mems), "offset": off, "items": items})
            if p == "/api/recap":
                period = "month" if q.get("period") == "month" else "day"
                mems = [m for m in STATE["memories"] if m["understood"]]
                now = STATE["now"]
                pre = now.strftime("%Y-%m") if period == "month" else now.strftime("%Y-%m-%d")
                sel = [m for m in mems if m["created"].startswith(pre)][:40]
                if not sel:
                    return self.send(200, {"count": 0, "text": ""})
                text = ("Progress\n" + "\n".join("• " + m["title"] + ": " + m["summary"] for m in sel[:6]) +
                        "\n\nOpen loops\n• Follow up with Alex on the budget\n• Reply to Jordan\n\nNext actions\n• Order rev C boards")
                return self.send(200, {"count": len(sel), "text": text})
            if p == "/api/files":
                d = q.get("dir", "/notes").rstrip("/") or "/"
                files = all_files()
                entries, dirs = [], set()
                for path, f in files.items():
                    parent, name = path.rsplit("/", 1)
                    parent = parent or "/"
                    if parent == d:
                        entries.append(dict(f, name=name, dir=False))
                    elif path.startswith(("" if d == "/" else d) + "/"):
                        rest = path[len("" if d == "/" else d) + 1:]
                        dirs.add(rest.split("/", 1)[0])
                if not entries and not dirs and d not in ("/", "/notes"):
                    return self.err(404, "Folder not found")
                for name in dirs:
                    entries.append({"name": name, "dir": True, "size": 0, "mtime": int(STATE["now"].timestamp()),
                                    "protected": False, "note": 0})
                entries.sort(key=lambda e: e["name"], reverse=True)
                entries.sort(key=lambda e: not e["dir"])
                return self.send(200, {"dir": d, "used": used_bytes(), "total": STATE["boot"]["storage"]["total"],
                                       "entries": entries})
            if p == "/api/silent":
                start = int(q.get("from", 0) or 0)
                mems = STATE["memories"]
                end = min(len(mems), start + 15)
                hits, checked = [], 0
                for m in mems[start:end]:
                    if m["meeting"] or not m["hasWav"]:
                        continue
                    checked += 1
                    if not m["understood"] and not m["transcript"]:
                        hits.append({"num": m["num"], "reason": "no speech heard", "seconds": m["seconds"]})
                return self.send(200, {"total": len(mems), "checked": checked,
                                       "next": end if end < len(mems) else None, "hits": hits})
        if p == "/api/wifi/scan":
            time.sleep(SCAN_DELAY)
            with LOCK:
                saved = set(STATE["boot"]["profiles"])
            nets = [("Home-WiFi", -48, 6, "WPA2"), ("Office-Guest", -61, 11, "WPA2"), ("Cafe Free WiFi", -70, 1, "Open"),
                    ("Neighbour 2.4G", -77, 6, "WPA2/WPA3"), ("", -82, 3, "WPA2"), ("Office-Ent", -85, 11, "Enterprise (not supported)")]
            return self.send(200, {"networks": [{"ssid": s, "rssi": r, "channel": c, "security": sec, "open": sec == "Open",
                                                 "saved": s in saved} for s, r, c, sec in nets]})
        if p in ("/audio", "/wav"):
            wav = silent_wav()
            hdr = {"Accept-Ranges": "bytes"}
            if p == "/wav":
                hdr["Content-Disposition"] = 'attachment; filename="note_%03d.wav"' % int(q.get("num", 0))
            return self.send(200, wav, "audio/wav", hdr)
        if p == "/txt":
            num = int(q.get("num", 0))
            with LOCK:
                m = next((m for m in STATE["memories"] if m["num"] == num), None)
            if not m:
                return self.send(404, "Not found", "text/plain")
            return self.send(200, m["transcript"] or m["summary"], "text/plain",
                             {"Content-Disposition": 'attachment; filename="note_%03d.txt"' % num})
        if p == "/files/get":
            path = q.get("path", "")
            name = path.rsplit("/", 1)[-1]
            return self.send(200, "mock content of " + path, "application/octet-stream",
                             {"Content-Disposition": 'attachment; filename="%s"' % name})
        if p == "/export.txt":
            with LOCK:
                txt = "\n\n".join("#%d %s\n%s" % (m["num"], m["title"], m["summary"] or m["transcript"]) for m in STATE["memories"])
            return self.send(200, txt, "text/plain", {"Content-Disposition": 'attachment; filename="oracle-export.txt"'})
        self.err(404, "Not found")

    # POST ----------------------------------------------------------------
    def post(self):
        global STATE
        p = urlparse(self.path).path
        if p == "/firmware":
            data = self.body()
            if self.headers.get("Content-Type") != "application/octet-stream":
                return self.send(400, "Expected application/octet-stream", "text/plain")
            return self.send(200, "Update OK (%d bytes). Oracle restarts now." % len(data), "text/plain")
        if p == "/__mock/reset":
            with LOCK:
                STATE = build_state()
            return self.ok()
        b = self.json_body()
        if b is None:
            return self.err(400, "Invalid JSON")
        with LOCK:
            st, boot_ = STATE, STATE["boot"]
            if p == "/api/task":
                item = next((a for a in st["agenda"] if a["id"] == b.get("id")), None)
                if not item:
                    return self.err(400, "Invalid task")
                item["done"] = bool(b.get("done"))
                return self.ok()
            if p == "/api/memory/delete":
                if not delete_memory(int(b.get("num", 0))):
                    return self.err(404, "Memory not found")
                return self.ok()
            if p == "/api/memories/delete-all":
                if (b.get("confirmation") or "").strip() != "DELETE ALL":
                    return self.err(400, "Confirmation did not match. Nothing was deleted.")
                st["memories"] = []
                st["extras"] = {k: v for k, v in st["extras"].items() if not k.startswith("/notes/") or v[2]}
                return self.ok()
            if p == "/api/ask":
                qn = (b.get("question") or "").strip()
                if not qn or len(qn) > 240:
                    return self.err(400, "Invalid question")
                if "offline" in qn.lower():
                    return self.err(503, "Oracle is offline. Connect it to Wi-Fi first.")
                src = [m["num"] for m in st["memories"] if m["understood"]][:2]
                return self.ok({"answer": "From what you told me: the travel budget is 12% over, and Alex wants a "
                                          "revised plan by Friday. You also noted to <b>order rev C</b> boards.",
                                "sources": src})
            if p == "/api/files/delete":
                files = all_files()
                deleted, notes = 0, set()
                for path in b.get("paths") or []:
                    f = files.get(path)
                    if not f or f["protected"]:
                        continue
                    if f["note"]:
                        notes.add(f["note"])
                    elif path in st["extras"]:
                        del st["extras"][path]
                        deleted += 1
                for n in notes:
                    deleted += delete_memory(n)
                return self.ok({"deleted": deleted})
            if p == "/api/settings/connection":
                ssid, key = (b.get("ssid") or "").strip(), (b.get("apiKey") or "").strip()
                if ssid:
                    if ssid not in boot_["profiles"]:
                        if len(boot_["profiles"]) >= 5:
                            return self.err(400, "Five networks are saved already. Remove one first.")
                        boot_["profiles"].append(ssid)
                    boot_["has"]["wifi"] = True
                if key:
                    boot_["has"]["openai"] = True
                return self.ok()
            if p == "/api/wifi/update":
                old, new = b.get("ssid"), (b.get("newSsid") or "").strip()
                if old not in boot_["profiles"] or not new:
                    return self.err(400, "Unknown network")
                boot_["profiles"][boot_["profiles"].index(old)] = new
                if boot_["wifi"]["ssid"] == old:
                    boot_["wifi"]["ssid"] = new
                return self.ok()
            if p == "/api/wifi/remove":
                if b.get("ssid") not in boot_["profiles"]:
                    return self.err(400, "Unknown network")
                boot_["profiles"].remove(b.get("ssid"))
                boot_["has"]["wifi"] = bool(boot_["profiles"])
                return self.ok()
            if p == "/api/settings/language":
                lang, voice = b.get("uiLanguage"), b.get("speechVoice")
                if lang not in "en de fr es it pl cs ru uk nl pt".split() or voice not in ("eleven", "cedar", "coral"):
                    return self.err(400, "Unknown language or voice")
                boot_["lang"], boot_["voice"] = lang, voice
                return self.ok()
            if p == "/api/settings/minutes":
                boot_["minutesUrl"] = (b.get("url") or "").strip()
                if b.get("key"):
                    boot_["has"]["minutes"] = True
                return self.ok()
            if p == "/api/settings/hotspot":
                mode, pw = b.get("mode"), b.get("password") or ""
                if mode not in ("fixed", "dynamic"):
                    return self.err(400, "Unknown mode")
                if pw and not 8 <= len(pw) <= 63:
                    return self.err(400, "The password needs 8 to 63 characters.")
                if mode == "fixed" and not pw and not boot_["has"]["apPassword"]:
                    return self.err(400, "Enter a fixed password first.")
                if pw:
                    boot_["has"]["apPassword"] = True
                boot_["apFixed"] = mode == "fixed"
                return self.ok()
            if p == "/api/key/clear":
                boot_["has"]["openai"] = False
                return self.ok()
        self.err(404, "Not found")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.daemon_threads = True
    print("Mock Oracle Lite on http://127.0.0.1:%d (delay %d ms)" % (port, DELAY * 1000), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
