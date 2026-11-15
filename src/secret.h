// Passwords in memory: encryption with DPAPI, generation, and wiping.
//
// DPAPI ties each encrypted password to the current Windows user on this
// machine. Any program running as that user can decrypt it, so this
// protects copies of the database file, not the account itself.

#pragma once

#include <stddef.h>
#include <stdint.h>

// A plaintext password: nul-terminated UTF-8 on the heap. len excludes the
// terminator. A zeroed Secret is empty. Release it with secret_free, which
// wipes it, as soon as it is no longer needed.
typedef struct Secret {
  char *text;
  size_t len;
} Secret;

// An encrypted password. Release it with ciphertext_free.
typedef struct Ciphertext {
  uint8_t *data;
  size_t len;
} Ciphertext;

// Generated passwords are this many characters from a 64-character
// alphabet: 6 bits each, 144 bits before the redraws in secret_generate.
// Redrawing a password that lacks a character type or starts with a
// formula character leaves 143.81 bits.
static constexpr size_t PASSWORD_LENGTH = 24;

[[nodiscard]] bool secret_encrypt(const Secret *plaintext, Ciphertext *out);

// secret_decrypt fails for data encrypted by another user, on another
// machine, or with different entropy.
[[nodiscard]] bool secret_decrypt(const uint8_t *data, size_t len, Secret *out);

// secret_generate makes a random password with at least one symbol, digit,
// uppercase and lowercase letter. It fails only if the system RNG does.
[[nodiscard]] bool secret_generate(Secret *out);

// secret_from_wide converts len UTF-16 units of text to a Secret. It does
// not wipe text; the caller owns that buffer.
[[nodiscard]] bool secret_from_wide(const wchar_t *text, size_t len,
                                    Secret *out);

// secret_to_wide returns password as nul-terminated UTF-16 in a malloc'd
// buffer, setting *size to its size in bytes including the terminator, or
// returns nullptr. Release it with wide_secret_free.
[[nodiscard]] wchar_t *secret_to_wide(const Secret *password, size_t *size);
void wide_secret_free(wchar_t *text, size_t size);

void secret_free(Secret *secret);
void ciphertext_free(Ciphertext *ciphertext);
