#pragma once
#include <Arduino.h>

// Spoken reminders shortly before timed appointments.
constexpr uint32_t REMINDER_LEAD_SECONDS = 15 * 60;

// Rescan the agenda for the next appointment to remind about (reads the SD).
void reminderRefresh();
// True when a reminder is due now.
bool reminderDue();
// Text for the due reminder ("Meeting with San in 15 minutes") and mark it
// done so it is never repeated, also across deep sleep.
String reminderTakeMessage();
// Seconds until the next reminder (0 = none). Used to wake from deep sleep.
uint32_t reminderSecondsUntilNext();
