#pragma once
#include <Arduino.h>

// Chip-locked encryption for saved secrets (Wi-Fi passwords, API keys).
// A random 256-bit key is burned once into a spare eFuse key block with the
// HMAC purpose (read-protected: software can never read it back). The data
// key is derived from it with the chip's HMAC peripheral, so a copy of the
// flash is useless on any other chip or without this chip's hardware.
bool secureStoreBegin();
bool secureStoreReady();
bool secureIsEncrypted(const String& stored);
// AES-256-GCM, bound to the settings key name. Returns "" on failure.
String secureEncrypt(const String& name, const String& plain);
bool secureDecrypt(const String& name, const String& stored, String& plain);
