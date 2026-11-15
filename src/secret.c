#include "secret.h"

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>
#include <stdlib.h>
#include <string.h>

// Optional entropy passed to every DPAPI call. It ships in the binary, so it
// is not a secret; it only stops other programs from decrypting these blobs
// by accident. Changing it makes every saved password unreadable.
static const uint8_t ENTROPY[16] = {
    0x08, 0x0D, 0xAF, 0xB8, 0x3B, 0x09, 0xDD, 0x25,
    0x29, 0x2B, 0x80, 0x99, 0xF6, 0xED, 0x02, 0x61,
};

// secret_alloc returns a Secret with room for len bytes plus a terminator.
static bool secret_alloc(size_t len, Secret *out) {
  *out = (Secret){};
  char *text = malloc(len + 1);
  if (!text)
    return false;
  text[len] = '\0';
  *out = (Secret){.text = text, .len = len};
  return true;
}

bool secret_encrypt(const Secret *plaintext, Ciphertext *out) {
  *out = (Ciphertext){};
  if (plaintext->len == 0 || plaintext->len > UINT32_MAX)
    return false;

  DATA_BLOB input = {.cbData = (DWORD)plaintext->len,
                     .pbData = (BYTE *)plaintext->text};
  DATA_BLOB entropy = {.cbData = sizeof ENTROPY, .pbData = (BYTE *)ENTROPY};
  DATA_BLOB output = {};
  if (!CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, 0,
                        &output))
    return false;

  *out = (Ciphertext){.data = output.pbData, .len = output.cbData};
  return true;
}

bool secret_decrypt(const uint8_t *data, size_t len, Secret *out) {
  *out = (Secret){};
  if (len == 0 || len > UINT32_MAX)
    return false;

  DATA_BLOB input = {.cbData = (DWORD)len, .pbData = (BYTE *)data};
  DATA_BLOB entropy = {.cbData = sizeof ENTROPY, .pbData = (BYTE *)ENTROPY};
  DATA_BLOB output = {};
  if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
    return false;

  // Copied so the Secret is nul-terminated; DPAPI's buffer is wiped.
  bool ok = secret_alloc(output.cbData, out);
  if (ok)
    memcpy(out->text, output.pbData, output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return ok;
}

// Easily confused characters (0, O, 1, l, I, Q) are left out.
static const char ALPHABET[] =
    "!#%+:=?@"                   // 8 symbols, indices 0 to 7
    "23456789"                   // 8 digits, 8 to 15
    "ABCDEFGHJKLMNPRSTUVWXYZ"    // 23 uppercase, 16 to 38
    "abcdefghijkmnopqrstuvwxyz"; // 25 lowercase, 39 to 63
static_assert(sizeof ALPHABET - 1 == 64);

// Spreadsheets read a CSV field that starts with one of these as a formula,
// even when it is quoted, and re-saving the sheet would then lose the
// password. A generated password never starts with one.
static bool is_formula_start(char c) {
  return c == '=' || c == '+' || c == '-' || c == '@';
}

// Each random byte's low 6 bits pick a character. 64 divides 256, so every
// character is equally likely. A draw missing any character class, or
// starting with a formula character, is redrawn.
bool secret_generate(Secret *out) {
  *out = (Secret){};
  uint8_t random[PASSWORD_LENGTH];
  bool has_symbol = false, has_digit = false, has_upper = false,
       has_lower = false;
  bool safe_start = false;
  bool ok = false;

  while (!(has_symbol && has_digit && has_upper && has_lower && safe_start)) {
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random, sizeof random,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
      goto cleanup;

    has_symbol = has_digit = has_upper = has_lower = false;
    for (size_t i = 0; i < PASSWORD_LENGTH; i++) {
      random[i] %= 64;
      if (random[i] < 8)
        has_symbol = true;
      else if (random[i] < 16)
        has_digit = true;
      else if (random[i] < 39)
        has_upper = true;
      else
        has_lower = true;
    }
    safe_start = !is_formula_start(ALPHABET[random[0]]);
  }

  if (!secret_alloc(PASSWORD_LENGTH, out))
    goto cleanup;
  for (size_t i = 0; i < PASSWORD_LENGTH; i++)
    out->text[i] = ALPHABET[random[i]];
  ok = true;

cleanup:
  SecureZeroMemory(random, sizeof random);
  return ok;
}

bool secret_from_wide(const wchar_t *text, size_t len, Secret *out) {
  *out = (Secret){};
  if (len == 0)
    return true;
  if (len > INT32_MAX)
    return false;

  int32_t n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                  (int32_t)len, nullptr, 0, nullptr, nullptr);
  if (n <= 0 || !secret_alloc((size_t)n, out))
    return false;
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, (int32_t)len,
                          out->text, n, nullptr, nullptr) != n) {
    secret_free(out);
    return false;
  }
  return true;
}

wchar_t *secret_to_wide(const Secret *password, size_t *size) {
  *size = 0;
  if (password->len == 0 || password->len > INT32_MAX)
    return nullptr;
  int32_t len = (int32_t)password->len;
  int32_t n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password->text,
                                  len, nullptr, 0);
  if (n <= 0)
    return nullptr;

  size_t bytes = ((size_t)n + 1) * sizeof(wchar_t);
  wchar_t *text = malloc(bytes);
  if (!text)
    return nullptr;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, password->text, len,
                          text, n) != n) {
    wide_secret_free(text, bytes);
    return nullptr;
  }
  text[n] = L'\0';
  *size = bytes;
  return text;
}

void wide_secret_free(wchar_t *text, size_t size) {
  if (text) {
    SecureZeroMemory(text, size);
    free(text);
  }
}

void secret_free(Secret *secret) {
  if (secret->text) {
    SecureZeroMemory(secret->text, secret->len);
    free(secret->text);
  }
  *secret = (Secret){};
}

void ciphertext_free(Ciphertext *ciphertext) {
  LocalFree(ciphertext->data); // LocalFree(nullptr) is a no-op
  *ciphertext = (Ciphertext){};
}
