#pragma once
#include <Arduino.h>
#include <functional>

// Oracle Minutes: meeting recordings streamed live to the Oracle Minutes
// server (resumable upload API), kept on the SD card as a "Meeting" memory,
// and filled with the server's minutes once they are written.
//
// Per meeting note NNN a small sidecar /notes/note_NNN.om keeps:
//   rid=<device recording id>  title=<meeting title>
//   state=recording|recorded|sent|done|failed  job=<server job id>

// Live streaming during the recording -------------------------------------
// Creates the sidecar. When `online`, a background task streams the WAV to
// the server while it is being written.
bool minutesLiveStart(int noteNum, const String& title, bool online);
// Recorder: the file now holds `fileBytes` bytes durably (flushed).
void minutesLivePublish(uint32_t fileBytes);
// Recorder stopped. `keep` = false discards the meeting (too short/failed).
void minutesLiveFinish(bool keep);
// True while the live uploader is connected and keeping up (for the screen).
bool minutesLiveConnected();
// The server's live "brief" from the latest reply: who spoke last, the last
// line heard, and who has talked so far. version changes when it changes.
uint32_t minutesLiveBriefVersion();
void minutesLiveBrief(String& speaker, String& line, String& talked);

// After recording (foreground, Wi-Fi already connected): send what is left
// and ask the server to write the minutes. progress(0..100).
bool minutesFinishUpload(int noteNum, const std::function<void(int)>& progress, String& error);

// Background work -----------------------------------------------------------
// Meetings that still need uploading, completing or their minutes fetched.
int  minutesPendingCount();
// One unit of work for the oldest pending meeting (Wi-Fi connected).
// phase() is told what is happening ("SENDING", "MINUTES") for the screen.
// Returns true when something was achieved.
bool minutesServiceOne(const std::function<void(const char*)>& phase, String& error);
// The note's text is supplied by the server, not by local transcription.
bool minutesNoteAwaitingServer(int noteNum);
String minutesNoteState(int noteNum);

// Speech through the Oracle server's /api/tts (uses the server's voice pick,
// e.g. ElevenLabs). Writes a 16 kHz mono WAV to outputPath.
bool oracleServerTts(const String& text, const char* outputPath);

// Spoken question answered by the Oracle server in one trip (POST /api/voice):
// the server may search the web; its voice plays while it streams in. The
// upper button stops the answer. Leaves the full answer on screen.
bool oracleVoiceAsk(const char* wavPath, String& question, String& answer, String& error);

// Faster variant: the question streams to the server while it is spoken.
// oracleVoiceBegin() before recording, oracleVoicePush() for each audio block,
// then oracleVoiceFinish() (plays the answer; falls back to sending the saved
// WAV if streaming failed) or oracleVoiceCancel().
bool oracleVoiceBegin();
void oracleVoicePush(const int16_t* samples, size_t count);
void oracleVoiceCancel();
bool oracleVoiceFinish(const char* wavPath, String& question, String& answer, String& error);
// Asks the server a typed question (POST /api/ask); used when a voice reply
// carried no answer.
// Typed question on the fast /api/voice stream (server 1.45+), spoken as it arrives.
bool oracleVoiceAskText(const String& text, String& question, String& answer, String& error);
bool oracleTextAsk(const String& question, String& answer, String& error);
// Streams the Oracle server's voice (/api/tts, IMA ADPCM) and plays it while it
// downloads; shows `display` once audio starts. False = nothing played.
bool oracleServerTtsStream(const String& spoken, const String& display, int sourceCount);
// Conversation for follow-up web questions (last 4 turns are sent along).
void oracleVoiceHistoryClear();
void oracleVoiceHistoryAdd(const String& question, const String& answer);
