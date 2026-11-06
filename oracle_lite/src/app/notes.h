#pragma once
#include <Arduino.h>
#include <vector>

void   loadIndex(bool recoverOrphans = false);
bool   saveIndex();
bool   addToIndex(int num, const char* tag, bool hasText);
bool   updateIndexHasText(int num);
bool   deleteNote(int num);
bool   deleteAllNotes();
int    nextNoteNumber();
bool   saveTag(int num, const char* tag);

String noteMetaPath(int num);
String readNoteMetaValue(int num, const char* key);
bool   writeNoteMeta(int num, const char* tag);
String noteQueueState(int num);
String noteQueueError(int num);
int    noteQueueRetries(int num);
int    noteQueueFailureCount(int num);
String noteQueueUpdatedUtc(int num);
bool   noteQueueRetryDue(int num);
void   resumeBlockedCloudNotes();
bool   writeNoteQueueState(int num, const char* state, const char* error = nullptr,
                           bool incrementRetry = false, bool progressMade = false);

bool   saveNoteMoments(int num, const std::vector<uint32_t>& momentsMs);
std::vector<uint32_t> loadNoteMoments(int num);
int    noteMomentCount(int num);
void   updateIndexMomentCount(int num, int count);

String noteCreatedUtc(int num);
String utcToLocalDeviceLabel(const String& utcIso);
int    localUtcOffsetMinutes(const String& utcIso);
String noteCreatedDeviceLabel(int num);
String currentUtcIso();

String notePreviewText(int num, size_t maxLen = 90);
