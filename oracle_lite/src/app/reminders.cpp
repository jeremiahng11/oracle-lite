#include "reminders.h"
#include "tasks.h"
#include "notes.h"
#include "settings.h"
#include "agenda_range.h"
#include <time.h>

// Recent reminders, kept through deep sleep so a timer wake never repeats one.
RTC_DATA_ATTR static uint32_t remindedRing[8];
RTC_DATA_ATTR static uint8_t remindedNext = 0;

static bool cached = false;
static time_t cachedRemindAt = 0, cachedDueAt = 0;
static uint32_t cachedHash = 0;
static String cachedText;

static uint32_t fnv(const String& a, const String& b) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < a.length(); i++) { h ^= (uint8_t)a[i]; h *= 16777619UL; }
  h ^= '|'; h *= 16777619UL;
  for (size_t i = 0; i < b.length(); i++) { h ^= (uint8_t)b[i]; h *= 16777619UL; }
  return h ? h : 1;
}

static bool alreadyReminded(uint32_t hash) {
  for (uint32_t h : remindedRing) if (h == hash) return true;
  return false;
}

static void markReminded(uint32_t hash) {
  remindedRing[remindedNext % 8] = hash;
  remindedNext = (remindedNext + 1) % 8;
}

// "2026-11-06T03:00:00Z" -> epoch seconds (UTC).
static time_t isoToEpoch(const String& iso) {
  if (iso.length() < 16) return 0;
  const long days = agenda::daysFromCivil(iso.substring(0, 4).toInt(), iso.substring(5, 7).toInt(),
                                          iso.substring(8, 10).toInt());
  const int sec = iso.length() >= 19 ? iso.substring(17, 19).toInt() : 0;
  return (time_t)days * 86400 + iso.substring(11, 13).toInt() * 3600 + iso.substring(14, 16).toInt() * 60 + sec;
}

static time_t nowEpoch() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? now : 0;
}

void reminderRefresh() {
  cached = false;
  const time_t now = nowEpoch();
  if (!now) return;
  for (const BrainTaskRecord& item : loadBrainTasks()) {
    if (item.done || item.type != "event" || !item.dueUtc.length()) continue;
    // Only appointments with a real clock time get a reminder.
    const String local = utcToLocalDeviceLabel(item.dueUtc);
    if (local.length() < 16) continue;
    if (item.datePrecision == "day" && local.substring(11, 16) == "00:00") continue;
    const time_t due = isoToEpoch(item.dueUtc);
    if (due <= now) continue;
    const uint32_t hash = fnv(item.id, item.dueUtc);
    if (alreadyReminded(hash)) continue;
    if (cached && due >= cachedDueAt) continue;
    cached = true;
    cachedDueAt = due;
    cachedRemindAt = due > (time_t)REMINDER_LEAD_SECONDS ? due - REMINDER_LEAD_SECONDS : due;
    cachedHash = hash;
    cachedText = item.text;
  }
}

bool reminderDue() {
  const time_t now = nowEpoch();
  return cached && now && now >= cachedRemindAt;
}

String reminderTakeMessage() {
  if (!cached) return "";
  cached = false;
  markReminded(cachedHash);
  const time_t now = nowEpoch();
  // Missed while busy (e.g. a long recording): stay quiet once it has started.
  if (!now || now > cachedDueAt) return "";
  const int minutes = (int)((cachedDueAt - now + 59) / 60);
  const bool german = oracleUiLanguage() == "de";
  if (minutes <= 1) return german ? "Jetzt: " + cachedText + "." : cachedText + " is starting now.";
  return german ? "In " + String(minutes) + " Minuten: " + cachedText + "."
                : cachedText + " in " + String(minutes) + " minutes.";
}

uint32_t reminderSecondsUntilNext() {
  const time_t now = nowEpoch();
  if (!cached || !now) return 0;
  return cachedRemindAt > now ? (uint32_t)(cachedRemindAt - now) : 1;
}
