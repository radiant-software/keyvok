#include "sq.h"

#include <windows.h>

#include <stdlib.h>

// STRICT tables, which keyvok's schema uses, arrived in 3.37.0.
static_assert(SQLITE_VERSION_NUMBER >= 3'037'000);

// utf8_from_wide returns text as UTF-8 in a malloc'd buffer, or nullptr.
static char *utf8_from_wide(const wchar_t *text) {
  int32_t n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                  nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return nullptr;
  char *utf8 = malloc((size_t)n);
  if (utf8 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8,
                                  n, nullptr, nullptr) != n) {
    free(utf8);
    utf8 = nullptr;
  }
  return utf8;
}

int32_t sq_open(const wchar_t *path, sqlite3 **out) {
  *out = nullptr;

  // Required: the library is built with SQLITE_OMIT_AUTOINIT. Idempotent.
  int32_t rc = sqlite3_initialize();
  if (rc != SQLITE_OK)
    return rc;

  char *path_utf8 = utf8_from_wide(path);
  if (!path_utf8)
    return SQLITE_CANTOPEN;

  // open_v2 can allocate a handle even when it fails, so db is closed on
  // every failure path. sqlite3_close_v2(nullptr) is a no-op.
  sqlite3 *db = nullptr;
  rc = sqlite3_open_v2(path_utf8, &db,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  free(path_utf8);
  if (rc != SQLITE_OK)
    goto fail;

  sqlite3_extended_result_codes(db, 1);

  // The build already defaults to these settings. Applying them here
  // keeps the connection strict whatever the build flags, and reading
  // each one back confirms it took.
  static const struct {
    int32_t op;
    int32_t want;
  } settings[] = {
      {SQLITE_DBCONFIG_ENABLE_FKEY, 1},
      {SQLITE_DBCONFIG_DQS_DML, 0},
      {SQLITE_DBCONFIG_DQS_DDL, 0},
  };
  for (size_t i = 0; i < _countof(settings); i++) {
    int32_t actual = -1;
    sqlite3_db_config(db, settings[i].op, settings[i].want, &actual);
    if (actual != settings[i].want) {
      rc = SQLITE_ERROR;
      goto fail;
    }
  }

  *out = db;
  return SQLITE_OK;

fail:
  sqlite3_close_v2(db);
  return rc;
}
