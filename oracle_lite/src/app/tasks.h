#pragma once

#include <Arduino.h>
#include <vector>

struct BrainTaskRecord {
  String id;
  int noteNum = -1;
  String text;
  String createdUtc;
  String dueUtc;
  String type = "todo";
  String datePrecision = "none";
  String updatedUtc;
  bool done = false;
};

std::vector<BrainTaskRecord> loadBrainTasks();
std::vector<BrainTaskRecord> priorityBrainTasks(size_t limit = 3, int* openCount = nullptr);
bool setBrainTaskDone(const String& id, bool done);
int openBrainTaskCount();
void clearBrainTaskState();
bool applyBrainTaskCommand(const String& speech, String& response, int& sourceNote);
