// Storage layer: one SQLite connection, used only from the UI thread.
//
// Functions that can fail for a reason the person should see return false
// and set *err_msg to user-facing text (a string literal). SQLite's own
// messages go to stderr in debug builds.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "secret.h"

// One row of the account list. The strings are valid only during the
// callback that receives them.
typedef struct Account {
  int64_t id;
  const wchar_t *name;
  const wchar_t *created; // YYYY-MM-DD, local time
} Account;

typedef void AccountFn(void *ctx, const Account *account);

static constexpr int32_t ACCOUNT_NAME_MAX_CHARS =
    255; // UTF-16 units an account name may have

// db_default_path writes the path of keyvok.db in the executable's
// directory, which can differ from the working directory.
[[nodiscard]] bool db_default_path(wchar_t *buf, uint32_t cap);

// db_open opens or creates the database and its table. Call once at
// startup. On failure the connection is left closed.
[[nodiscard]] bool db_open(const wchar_t *path);

// db_close closes the connection. Safe to call when it is not open.
void db_close(void);

// db_save_password encrypts password and inserts it under account, dated
// today (local time).
[[nodiscard]] bool db_save_password(const wchar_t *account,
                                    const Secret *password,
                                    const wchar_t **err_msg);

// db_each_account calls fn for every account, sorted by name without
// regard to case. False means the list could not be read in full; discard
// any rows fn already received.
[[nodiscard]] bool db_each_account(AccountFn *fn, void *ctx);

// db_get_password decrypts the password of account id into *out.
[[nodiscard]] bool db_get_password(int64_t id, Secret *out);

[[nodiscard]] bool db_relabel_account(int64_t id, const wchar_t *new_name,
                                      const wchar_t **err_msg);
[[nodiscard]] bool db_delete_account(int64_t id, const wchar_t **err_msg);

// db_export_csv writes every account, date and decrypted password to path
// as RFC 4180 CSV with a header row, in UTF-8 with a byte order mark. It
// writes path.tmp and renames it over path, so a failed export leaves no
// partial file and keeps any earlier file at path.
[[nodiscard]] bool db_export_csv(const wchar_t *path, const wchar_t **err_msg);

// What db_import_csv did.
typedef struct ImportStats {
  int32_t added;   // accounts created
  int32_t skipped; // rows whose account already existed, left unchanged
  int32_t line;    // on failure, the line of the file at fault, or 0
} ImportStats;

// db_import_csv adds the accounts in a CSV file laid out like
// db_export_csv's: a header row of Account, Created and Password, then one
// row per account. Fields may be quoted or not; the text may be UTF-8, with
// or without a byte order mark, or the ANSI code page. Names and passwords
// are trimmed. A Created that is not a valid YYYY-MM-DD becomes today. An
// account that already exists, ignoring case, is skipped and keeps its
// password. Any bad row fails the whole import and nothing is added.
[[nodiscard]] bool db_import_csv(const wchar_t *path, ImportStats *stats,
                                 const wchar_t **err_msg);

// db_backup_to writes a consistent copy of the database to path, replacing
// any existing file only once the copy is complete.
[[nodiscard]] bool db_backup_to(const wchar_t *path, const wchar_t **err_msg);
