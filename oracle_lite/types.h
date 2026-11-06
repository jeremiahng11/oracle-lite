#pragma once
#include <vector>

typedef enum {
  STATE_IDLE,
  STATE_RECORDING,
  STATE_TRANSFER,
  STATE_ASKING,
  STATE_AGENDA
} AppState;

struct NoteEntry {
  int num;
  char tag[16];
  bool hasText;
  uint16_t momentCount;
};
