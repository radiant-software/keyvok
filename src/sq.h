// sq: strict SQLite connections.
//
// The rest of the SQLite C API is used directly. Result codes are SQLite's
// own, and with extended result codes on, a failed constraint reports
// which kind it was (SQLITE_CONSTRAINT_UNIQUE and so on).

#pragma once

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

// sq_open opens or creates the database at path and configures the
// connection:
//   - extended result codes on
//   - foreign keys enforced (PRAGMA foreign_keys = OFF turns this off)
//   - double quotes quote identifiers only, so a mistyped column name is
//     an error instead of a string
// On failure *out is nullptr; on success close it with sqlite3_close_v2.
[[nodiscard]] int32_t sq_open(const wchar_t *path, sqlite3 **out);
