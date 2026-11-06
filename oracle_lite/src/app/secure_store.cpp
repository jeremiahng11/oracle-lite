#include "secure_store.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <vector>
#include <Preferences.h>

static const char kPrefix[] = "enc1:";
static const char kLabel[] = "oracle-lite/settings/v1";
static uint8_t dataKey[32];
static bool ready = false;

static void wipe(void* p, size_t n) {
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n--) *v++ = 0;
}

bool secureStoreBegin() {
  if (ready) return true;
  esp_efuse_block_t block = EFUSE_BLK_KEY_MAX;
  if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &block)) {
    // Burn at most once. A flag is saved before burning; if a burn was
    // started but no HMAC key exists (e.g. power cut mid-write), never use up
    // another eFuse block automatically.
    Preferences marker;
    if (!marker.begin("oracle_sec", false)) return false;
    if (marker.getBool("burn_try", false)) {
      marker.end();
      Serial.println("[Secure] an earlier key burn did not finish; settings stay unencrypted");
      return false;
    }
    block = esp_efuse_find_unused_key_block();
    if (block == EFUSE_BLK_KEY_MAX) {
      marker.end();
      Serial.println("[Secure] no free eFuse key block; settings stay unencrypted");
      return false;
    }
    if (!marker.putBool("burn_try", true)) { marker.end(); return false; }
    marker.end();
    // One-time: create the chip key from the hardware random generator.
    uint8_t key[32];
    bootloader_random_enable();
    esp_fill_random(key, sizeof(key));
    bootloader_random_disable();
    const esp_err_t burned = esp_efuse_write_key(block, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, key, sizeof(key));
    wipe(key, sizeof(key));
    if (burned != ESP_OK) {
      Serial.printf("[Secure] eFuse key write failed: %s\n", esp_err_to_name(burned));
      return false;
    }
    Serial.printf("[Secure] chip key created in eFuse key block %d\n", (int)(block - EFUSE_BLK_KEY0));
  }
  const hmac_key_id_t id = static_cast<hmac_key_id_t>(block - EFUSE_BLK_KEY0);
  if (esp_hmac_calculate(id, kLabel, strlen(kLabel), dataKey) != ESP_OK) {
    Serial.println("[Secure] HMAC unavailable; settings stay unencrypted");
    wipe(dataKey, sizeof(dataKey));
    return false;
  }
  ready = true;
  return true;
}

bool secureStoreReady() { return ready; }

bool secureIsEncrypted(const String& stored) { return stored.startsWith(kPrefix); }

String secureEncrypt(const String& name, const String& plain) {
  if (!ready) return "";
  const size_t n = plain.length();
  std::vector<uint8_t> blob(12 + n + 16);
  esp_fill_random(blob.data(), 12);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, dataKey, 256) == 0 &&
            mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, n, blob.data(), 12,
              reinterpret_cast<const uint8_t*>(name.c_str()), name.length(),
              reinterpret_cast<const uint8_t*>(plain.c_str()), blob.data() + 12,
              16, blob.data() + 12 + n) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok) return "";
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, blob.data(), blob.size());
  std::vector<uint8_t> b64(outLen + 1);
  if (mbedtls_base64_encode(b64.data(), b64.size(), &outLen, blob.data(), blob.size()) != 0) return "";
  b64[outLen] = 0;
  return String(kPrefix) + reinterpret_cast<const char*>(b64.data());
}

bool secureDecrypt(const String& name, const String& stored, String& plain) {
  plain = "";
  if (!ready || !secureIsEncrypted(stored)) return false;
  const char* b64 = stored.c_str() + strlen(kPrefix);
  size_t len = 0;
  mbedtls_base64_decode(nullptr, 0, &len, reinterpret_cast<const uint8_t*>(b64), strlen(b64));
  std::vector<uint8_t> blob(len + 1);
  if (mbedtls_base64_decode(blob.data(), blob.size(), &len, reinterpret_cast<const uint8_t*>(b64), strlen(b64)) != 0 ||
      len < 12 + 16) return false;
  const size_t n = len - 12 - 16;
  std::vector<uint8_t> out(n + 1, 0);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, dataKey, 256) == 0 &&
                  mbedtls_gcm_auth_decrypt(&gcm, n, blob.data(), 12,
                    reinterpret_cast<const uint8_t*>(name.c_str()), name.length(),
                    blob.data() + 12 + n, 16, blob.data() + 12, out.data()) == 0;
  mbedtls_gcm_free(&gcm);
  if (ok) plain = String(reinterpret_cast<const char*>(out.data()));
  wipe(out.data(), out.size());
  return ok;
}
