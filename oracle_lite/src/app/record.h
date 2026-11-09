#pragma once

bool record();
// Whether the last record()/recordQuestion() contained speech (not only noise).
bool lastRecordingHadSpeech();
int lastRecordingSpeechMs();
bool recordQuestion(const char* path, int holdPin);
// Records until either button is pressed (after the starting press is
// released) or maxMs passes. Shows the Ask "listening" screen.
bool recordQuestionUntilPress(const char* path, uint32_t maxMs);
// Records a spoken question and stops by itself ~1 s after the speaker stops
// (or on a button press / maxMs). onAudio receives each mono block as it is
// recorded (for streaming). heardSpeech tells whether anyone spoke.
bool recordQuestionUntilSilence(const char* path, uint32_t maxMs,
                                void (*onAudio)(const int16_t*, size_t), bool& heardSpeech);
bool playWavFile(const char* path);
// Records a meeting until the top button is pressed again. onFlushed(bytes) is
// called whenever the file durably holds `bytes` bytes (for live streaming).
bool recordMeeting(const char* path, uint32_t& durationMs, void (*onFlushed)(uint32_t) = nullptr);
