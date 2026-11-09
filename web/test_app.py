#!/usr/bin/env python3
"""End-to-end test of web/app.html against mock_server.py (Playwright, Chromium).

    python3 test_app.py            # screenshots go to web/screens/

Starts the mock server, screenshots every page at desktop + phone in light +
dark, then exercises every action and settings form. Exits non-zero on failure.
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

from playwright.sync_api import expect, sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = os.path.join(HERE, "screens")
PHONE_UA = ("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 "
            "(KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1")
ERRORS = []
ALLOW_HTTP_ERRORS = [False]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


PORT = free_port()
BASE = "http://127.0.0.1:%d" % PORT


def get(path):
    with urllib.request.urlopen(urllib.request.Request(BASE + path, headers={"X-Mock-Test": "1"})) as r:
        return json.loads(r.read())


def post(path, body=None):
    req = urllib.request.Request(BASE + path, data=json.dumps(body or {}).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "X-Mock-Test": "1"})
    with urllib.request.urlopen(req) as r:
        return json.loads(r.read())


def wait_until(fn, timeout=8.0, what="condition"):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return
        time.sleep(0.1)
    raise AssertionError("timed out waiting for " + what)


def step(name):
    print("  -", name, flush=True)


def watch(page, label):
    def on_console(msg):
        if msg.type != "error":
            return
        if ALLOW_HTTP_ERRORS[0] and "Failed to load resource" in msg.text:
            return
        ERRORS.append("%s console: %s" % (label, msg.text))
    page.on("console", on_console)
    page.on("pageerror", lambda e: ERRORS.append("%s pageerror: %s" % (label, e)))
    page.on("dialog", lambda d: d.accept())


def go(page, hash_):
    page.evaluate("h => { location.hash = h }", hash_)


def toast(page, text, bad=False):
    sel = "#toast.show" + (".bad" if bad else ":not(.bad)")
    expect(page.locator(sel)).to_contain_text(text, timeout=10000)


ROUTES = [
    ("today", "#/today", ".memory-card"),
    ("memories", "#/memories", ".memory-card"),
    ("tasks", "#/tasks", ".taskrow"),
    ("calendar", "#/calendar", ".agendarow"),
    ("files", "#/files?dir=/notes", ".ftable .fsel"),
    ("files-root", "#/files?dir=/", ".ftable .n a"),
    ("silent", "#/silent", ".silentrow"),
    ("setup", "#/setup", "form[data-f=conn]"),
    ("wifi-scan", "#/wifi-scan", ".netwrap"),
    ("recap-day", "#/recap?period=day", ".card .text"),
    ("recap-month", "#/recap?period=month", ".card .text"),
]


def screenshots(p, browser):
    os.makedirs(SHOTS, exist_ok=True)
    for vp in ("desktop", "phone"):
        for scheme in ("light", "dark"):
            opts = dict(color_scheme=scheme)
            if vp == "desktop":
                opts.update(viewport={"width": 1280, "height": 900})
            else:
                opts.update(viewport={"width": 390, "height": 844}, device_scale_factor=2, is_mobile=True,
                            has_touch=True, user_agent=PHONE_UA)
            ctx = browser.new_context(**opts)
            page = ctx.new_page()
            label = "%s-%s" % (vp, scheme)
            watch(page, label)
            step("screenshots " + label)
            page.goto(BASE + "/#/today")
            for name, hash_, sel in ROUTES:
                go(page, hash_)
                page.wait_for_selector(sel, timeout=15000)
                page.wait_for_timeout(250)
                page.screenshot(path=os.path.join(SHOTS, "%s_%s_%s.png" % (vp, scheme, name)), full_page=True)
            # viewport-only shot of today, shows the floating nav where the user sees it
            go(page, "#/today")
            page.wait_for_selector(".memory-card")
            page.screenshot(path=os.path.join(SHOTS, "%s_%s_today-viewport.png" % (vp, scheme)))
            # ask
            page.fill("form[data-f=ask] input", "What is over budget?")
            page.press("form[data-f=ask] input", "Enter")
            page.wait_for_selector("text=Oracle remembers")
            page.screenshot(path=os.path.join(SHOTS, "%s_%s_ask.png" % (vp, scheme)), full_page=True)
            ctx.close()


def functional(browser):
    post("/__mock/reset")
    ctx = browser.new_context(viewport={"width": 1280, "height": 900})
    page = ctx.new_page()
    watch(page, "functional")
    page.goto(BASE + "/")
    page.wait_for_selector(".memory-card")
    page.evaluate("window.__marker = 42")

    step("nav switches pages instantly")
    for href, h1 in [("#/tasks", "Tasks."), ("#/calendar", "Calendar."), ("#/files?dir=/notes", "Files."),
                     ("#/setup", "Setup."), ("#/memories", "Today."), ("#/today", "Today.")]:
        page.click('.navlink[href="%s"]' % href)
        expect(page.locator("main h1").first).to_have_text(h1)
        expect(page.locator('.navlink[href="%s"]' % href)).to_have_class("navlink active")
    # second visit renders from memory without waiting for the network
    page.click('.navlink[href="#/tasks"]')
    page.wait_for_selector(".taskrow")
    page.click('.navlink[href="#/today"]')
    rows = page.evaluate("""() => new Promise(r => {
        addEventListener('hashchange', () => r(document.querySelectorAll('.taskrow').length), {once: true});
        document.querySelector('.navlink[href="#/tasks"]').click(); })""")
    assert rows > 0, "tasks were not rendered synchronously from memory"

    step("tick a task (optimistic, in place)")
    row_id, state = page.evaluate("""() => {
        const b = document.querySelector('.taskrow:not(.completed) .check');
        const id = b.dataset.id; b.click();
        const row = document.querySelector('[data-id="' + id + '"]').closest('.taskrow');
        return [id, row.classList.contains('completed')];
    }""")
    assert state, "task row did not become completed synchronously"
    toast(page, "Done.")
    wait_until(lambda: next(a for a in get("/api/agenda")["items"] if a["id"] == row_id)["done"], what="task done on server")
    # tick it back
    page.click('[data-id="%s"]' % row_id)
    wait_until(lambda: not next(a for a in get("/api/agenda")["items"] if a["id"] == row_id)["done"], what="task reopened")

    step("tick rolls back on error")
    ALLOW_HTTP_ERRORS[0] = True
    page.route("**/api/task", lambda r: r.fulfill(status=500, content_type="application/json",
                                                  body='{"ok":false,"error":"SD card busy"}'))
    page.click('[data-id="%s"]' % row_id)
    toast(page, "SD card busy", bad=True)
    expect(page.locator('[data-id="%s"]' % row_id)).to_have_attribute("aria-pressed", "false")
    page.unroute("**/api/task")
    ALLOW_HTTP_ERRORS[0] = False

    step("tick a task on Today")
    go(page, "#/today")
    page.wait_for_selector(".todayitem .check")
    text, gone = page.evaluate("""() => {
        const b = document.querySelector('.todayitem .check'); const text = b.parentNode.textContent;
        b.click();
        return [text, ![...document.querySelectorAll('.todayitem')].some(e => e.textContent === text)];
    }""")
    assert gone, "today task did not disappear synchronously"
    wait_until(lambda: next(a for a in get("/api/agenda")["items"] if a["text"] == text)["done"], what="today task done")

    step("delete a memory")
    total = get("/api/memories?offset=0&limit=1")["total"]
    num, removed = page.evaluate("""() => {
        const c = document.querySelector('.memory-card'); const num = c.id.slice(2);
        c.querySelector('[data-a=mdel]').click();
        return [+num, !document.getElementById('m-' + num)];
    }""")
    assert removed, "memory card not removed synchronously"
    toast(page, "Memory forgotten.")
    wait_until(lambda: get("/api/memories?offset=0&limit=1")["total"] == total - 1, what="memory deleted")
    assert num not in [m["num"] for m in get("/api/memories?offset=0&limit=50")["items"]]

    step("search + infinite scroll")
    page.fill("#memory-search", "budget")
    expect(page.locator(".memory-card:not(.hidden)").first).to_contain_text("udget")
    page.fill("#memory-search", "")
    page.locator("#more").scroll_into_view_if_needed()
    wait_until(lambda: page.locator(".memory-card").count() > 12, what="next memories page")

    step("files: protected, select all, multi-delete")
    go(page, "#/files?dir=/notes")
    page.wait_for_selector(".ftable .fsel")
    assert page.locator("tr:has-text('index.csv') input").count() == 0, "protected file has a checkbox"
    assert page.locator("tr:has-text('tasks.state') input").count() == 0
    page.check("#fall")
    assert page.locator(".fsel:not(:checked)").count() == 0
    expect(page.locator("form[data-f=fdel] [type=submit]")).to_be_enabled()
    page.uncheck("#fall")
    assert page.locator(".fsel:checked").count() == 0
    expect(page.locator("form[data-f=fdel] [type=submit]")).to_be_disabled()
    victim = page.locator(".fsel[value$='.txt']").first.get_attribute("value")
    vnum = int(victim.split("note_")[1][:3])
    page.check(".fsel[value='%s']" % victim)              # page 1
    page.click("[data-a=fpg][data-d='1']")
    page.check(".fsel[value='/notes/.ask.wav']")            # page 2: selection spans pages
    expect(page.locator("form[data-f=fdel] [type=submit]")).to_have_text("Delete selected (2)")
    page.click("form[data-f=fdel] [type=submit]")
    expect(page.locator(".fsel[value='/notes/.ask.wav']")).to_have_count(0)
    expect(page.locator(".fsel[value^='/notes/note_%03d']" % vnum)).to_have_count(0)
    toast(page, "2 deleted")
    names = [e["name"] for e in get("/api/files?dir=/notes")["entries"]]
    assert ".ask.wav" not in names and not any(n.startswith("note_%03d" % vnum) for n in names)
    step("files: folder navigation + breadcrumb")
    page.click(".crumb a:has-text('SD')")
    page.click(".ftable .n a:has-text('ota/')")
    expect(page.locator(".ftable")).to_contain_text("firmware.bin")
    page.click(".ftable .n a:has-text('..')")
    expect(page.locator(".ftable")).to_contain_text("notes/")

    step("no-speech flow incl. check more")
    page.click('.navlink[href="#/files?dir=/notes"]')
    page.click("text=Find recordings with no speech")
    page.wait_for_selector(".silentrow")
    while page.locator("[data-a=smore]").count():
        page.locator("[data-a=smore]:not([disabled])").click(timeout=15000)
        page.wait_for_timeout(100)
        page.wait_for_selector("[data-a=smore]:not([disabled]), form[data-f=sdel]:not(:has([data-a=smore]))", timeout=15000)
    expect(page.locator("main")).to_contain_text("Checked: %d / %d" % ((total - 2,) * 2))
    hits = [int(x) for x in page.eval_on_selector_all(".ssel", "els => els.map(e => e.dataset.num)")]
    assert len(hits) >= 3, hits
    page.uncheck(".ssel[data-num='%d']" % hits[0])
    page.click("text=Delete selected memories")
    toast(page, "%d memories deleted" % (len(hits) - 1))
    left = [m["num"] for m in get("/api/memories?offset=0&limit=50")["items"]]
    assert hits[0] in left and not any(h in left for h in hits[1:])

    step("ask")
    go(page, "#/today")
    page.fill("form[data-f=ask] input", "What is over budget?")
    page.click("form[data-f=ask] button")
    expect(page.locator("main")).to_contain_text("Oracle remembers", timeout=15000)
    expect(page.locator("main")).to_contain_text("<b>order rev C</b>")  # escaped, not HTML
    assert page.locator("main .card .text b").count() == 0
    ALLOW_HTTP_ERRORS[0] = True
    page.fill("form[data-f=ask] input", "are you offline?")
    page.press("form[data-f=ask] input", "Enter")
    expect(page.locator("main")).to_contain_text("Oracle could not answer")
    ALLOW_HTTP_ERRORS[0] = False

    step("recap")
    go(page, "#/today")
    page.click("text=Recap: today")
    expect(page.locator("main h1")).to_have_text("today, recalled.")
    page.wait_for_selector(".card .text")
    page.click(".recaptabs >> text=This month")
    expect(page.locator("main h1")).to_have_text("this month.")
    expect(page.locator("main .card .text")).to_contain_text("Progress", timeout=15000)

    step("settings: connection")
    go(page, "#/setup")
    f = page.locator("form[data-f=conn]")
    f.locator("[name=ssid]").fill("Test-Net")
    f.locator("[name=password]").fill("password123")
    f.locator("[name=apiKey]").fill("sk-test")
    f.locator("[type=submit]").click()
    toast(page, "Settings saved.")
    assert "Test-Net" in get("/api/boot")["profiles"]
    expect(page.locator("#st-nets")).to_contain_text("Test-Net")
    expect(f.locator("[name=ssid]")).to_have_value("")

    step("settings: edit Wi-Fi")
    w = page.locator(".netwrap", has=page.locator("b", has_text="Test-Net"))
    w.locator("summary").click()
    w.locator("[name=newSsid]").fill("Test-Net-2")
    w.locator("[type=submit]").click()
    toast(page, "Wi-Fi updated.")
    assert "Test-Net-2" in get("/api/boot")["profiles"]

    step("settings: remove Wi-Fi")
    page.locator(".netwrap", has=page.locator("b", has_text="Test-Net-2")).locator("[data-a=wrm]").click()
    toast(page, "Wi-Fi removed.")
    assert "Test-Net-2" not in get("/api/boot")["profiles"]
    expect(page.locator("#st-nets")).not_to_contain_text("Test-Net-2")

    step("settings: scan + save a scanned network")
    page.click("text=See nearby networks")
    page.wait_for_selector(".netwrap", timeout=15000)
    n = page.locator(".netwrap", has=page.locator("b", has_text="Neighbour 2.4G"))
    n.locator("summary").click()
    n.locator("[name=password]").fill("neighbour-pass")
    n.locator("[type=submit]").click()
    toast(page, "Wi-Fi saved.")
    assert "Neighbour 2.4G" in get("/api/boot")["profiles"]
    expect(page.locator(".netwrap", has=page.locator("b", has_text="Neighbour 2.4G"))).to_contain_text("Saved")
    page.click("[data-a=rescan]")
    expect(page.locator("[data-a=rescan]")).to_have_text("Scan again", timeout=15000)

    step("settings: language + voice")
    go(page, "#/setup")
    lf = page.locator("form[data-f=lang]")
    lf.locator("[name=uiLanguage]").select_option("de")
    lf.locator("[name=speechVoice]").select_option("coral")
    lf.locator("[type=submit]").click()
    expect(page.locator(".navlink[href='#/today']")).to_contain_text("Heute")
    b = get("/api/boot")
    assert b["lang"] == "de" and b["voice"] == "coral"
    page.screenshot(path=os.path.join(SHOTS, "desktop_light_setup-de.png"), full_page=True)
    go(page, "#/today")
    expect(page.locator("main h1").first).to_have_text("Heute.")
    page.screenshot(path=os.path.join(SHOTS, "desktop_light_today-de.png"))
    go(page, "#/setup")
    lf = page.locator("form[data-f=lang]")
    lf.locator("[name=uiLanguage]").select_option("en")
    lf.locator("[type=submit]").click()
    expect(page.locator(".navlink[href='#/today']")).to_contain_text("Today")

    step("settings: Oracle Minutes")
    mf = page.locator("form[data-f=minutes]")
    mf.locator("[name=url]").fill("https://minutes.test.local")
    mf.locator("[name=key]").fill("key_123")
    mf.locator("[type=submit]").click()
    toast(page, "Settings saved.")
    wait_until(lambda: get("/api/boot")["minutesUrl"] == "https://minutes.test.local", what="minutes url")

    step("settings: hotspot fixed / dynamic")
    hf = page.locator("form[data-f=hotspot]")
    hf.locator("[name=mode]").select_option("fixed")
    hf.locator("[name=password]").fill("short")
    hf.locator("[type=submit]").click()
    assert hf.locator("[name=password]").evaluate("e => e.validity.tooShort"), "short password accepted"
    page.wait_for_timeout(300)
    assert get("/api/boot")["apFixed"] is False
    hf.locator("[name=password]").fill("my-oracle-2026")
    hf.locator("[type=submit]").click()
    toast(page, "Hotspot saved.")
    assert get("/api/boot")["apFixed"] is True
    hf = page.locator("form[data-f=hotspot]")
    expect(hf.locator("[name=mode]")).to_have_value("fixed")
    hf.locator("[name=mode]").select_option("dynamic")
    hf.locator("[type=submit]").click()
    wait_until(lambda: get("/api/boot")["apFixed"] is False, what="hotspot dynamic")

    step("settings: clear API key")
    page.click("[data-a=keyclear]")
    expect(page.locator("#st-status")).to_contain_text("API key missing")
    expect(page.locator("[data-a=keyclear]")).to_have_count(0)
    toast(page, "API key removed.")
    assert get("/api/boot")["has"]["openai"] is False

    step("firmware upload")
    page.set_input_files("#fw", files=[{"name": "firmware.bin", "mimeType": "application/octet-stream",
                                        "buffer": b"\xe9" + b"\0" * 300000}])
    page.click("#fwgo")
    expect(page.locator("#fwmsg")).to_contain_text("Update OK (300001 bytes)", timeout=15000)
    expect(page.locator("#fwbar")).to_have_attribute("aria-valuenow", "100")

    step("delete all memories")
    df = page.locator("form[data-f=wipe]")
    df.locator("[name=confirmation]").fill("DELETE")
    df.locator("[type=submit]").click()
    toast(page, "did not match", bad=True)
    assert get("/api/boot")["stats"]["memories"] > 0
    df.locator("[name=confirmation]").fill("DELETE ALL")
    df.locator("[type=submit]").click()
    toast(page, "Every memory was deleted.")
    assert get("/api/boot")["stats"]["memories"] == 0
    page.click('.navlink[href="#/today"]')
    expect(page.locator("main")).to_contain_text("No memories saved yet.")

    step("no full page reloads happened")
    assert page.evaluate("window.__marker") == 42, "page was reloaded"
    ctx.close()


def phone(browser):
    post("/__mock/reset")
    ctx = browser.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=2, is_mobile=True,
                              has_touch=True, user_agent=PHONE_UA)
    page = ctx.new_page()
    watch(page, "phone")
    page.goto(BASE + "/")
    page.wait_for_selector(".memory-card")
    step("phone: bottom nav + tick")
    box = page.locator("nav.nav").bounding_box()
    assert box["y"] > 700, "nav should float at the bottom on phones"
    page.tap('.navlink[href="#/tasks"]')
    expect(page.locator("main h1")).to_have_text("Tasks.")
    page.locator(".taskrow:not(.completed) .check").first.tap()
    toast(page, "Done.")
    page.tap('.navlink[href="#/calendar"]')
    page.tap("[data-a=mon][data-d='1']")
    page.tap("[data-a=mon][data-d='0']")
    expect(page.locator(".calday.today")).to_have_count(1)
    width = page.evaluate("document.documentElement.scrollWidth")
    for h in ("#/today", "#/files?dir=/notes", "#/setup", "#/calendar"):
        go(page, h)
        page.wait_for_timeout(400)
        width = page.evaluate("document.documentElement.scrollWidth")
        assert width <= 390, "horizontal overflow on %s: %d" % (h, width)
    ctx.close()


def main():
    env = dict(os.environ, MOCK_DELAY_MS=os.environ.get("MOCK_DELAY_MS", "150"), MOCK_SCAN_MS="1200")
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "mock_server.py"), str(PORT)], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        wait_until(lambda: _up(), 10, "mock server")
        with sync_playwright() as p:
            browser = p.chromium.launch()
            print("screenshots", flush=True)
            screenshots(p, browser)
            print("functional (desktop)", flush=True)
            functional(browser)
            print("phone", flush=True)
            phone(browser)
            browser.close()
        stats = get("/__mock/stats")
        print("max concurrent API requests:", stats["max_inflight"])
        assert stats["max_inflight"] <= 2, "more than 2 requests at once"
        size = os.path.getsize(os.path.join(HERE, "app.html"))
        print("app.html: %d bytes" % size)
        if ERRORS:
            raise AssertionError("console errors:\n" + "\n".join(ERRORS))
        print("PASS")
    finally:
        srv.terminate()
        srv.wait()


def _up():
    try:
        get("/__mock/stats")
        return True
    except Exception:
        return False


if __name__ == "__main__":
    main()
