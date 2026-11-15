#include "db.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "sq.h"

// The app's only connection. Opened by db_open at startup and closed by
// db_close when the main window is destroyed. Every other function
// requires it to be open.
static sqlite3 *db;

static const char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS passwords ("
    "    id       INTEGER PRIMARY KEY,"
    "    account  TEXT COLLATE NOCASE NOT NULL UNIQUE CHECK (account != ''),"
    "    created  TEXT NOT NULL,"
    "    password BLOB NOT NULL"
    ") STRICT;";

// log_failure reports SQLite's message for the latest failure on conn.
static void log_failure([[maybe_unused]] sqlite3 *conn,
                        [[maybe_unused]] const char *what,
                        [[maybe_unused]] int32_t rc) {
#ifdef KEYVOK_DEBUG
  fprintf(stderr, "keyvok db: %s failed: %s (%s)\n", what, sqlite3_errstr(rc),
          conn ? sqlite3_errmsg(conn) : "no connection");
#endif
}

static bool is_blank(const wchar_t *s) {
  for (; *s; s++) {
    if (!iswspace(*s))
      return false;
  }
  return true;
}

// prepare compiles sql on the app's connection, or returns nullptr.
static sqlite3_stmt *prepare(const char *what, const char *sql) {
  sqlite3_stmt *stmt = nullptr;
  int32_t rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    log_failure(db, what, rc);
  return stmt;
}

// run_and_finalize runs a statement that returns no rows and finalizes it.
// rc is the result of binding its parameters; the statement only runs if
// that is SQLITE_OK. Returns SQLITE_DONE on success.
static int32_t run_and_finalize(sqlite3_stmt *stmt, int32_t rc,
                                const char *what) {
  if (rc == SQLITE_OK)
    rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE)
    log_failure(db, what, rc);
  sqlite3_finalize(stmt);
  return rc;
}

bool db_default_path(wchar_t *buf, uint32_t cap) {
  static const wchar_t NAME[] = L"keyvok.db";

  DWORD n = GetModuleFileNameW(nullptr, buf, cap);
  if (n == 0 || n == cap) // n == cap: truncated
    return false;
  wchar_t *slash = wcsrchr(buf, L'\\');
  if (!slash || (size_t)(slash + 1 - buf) + _countof(NAME) > cap)
    return false;
  memcpy(slash + 1, NAME, sizeof NAME);
  return true;
}

bool db_open(const wchar_t *path) {
  int32_t rc = sq_open(path, &db);
  if (rc != SQLITE_OK) {
    log_failure(nullptr, "open", rc);
    return false;
  }

  // Wait out brief locks, such as a backup tool reading the file.
  const char *what = "busy_timeout";
  rc = sqlite3_busy_timeout(db, 5000);
  if (rc == SQLITE_OK) {
    // Overwrite deleted content, so old labels and ciphertext do not
    // linger in the file's free pages.
    what = "secure_delete";
    rc = sqlite3_exec(db, "PRAGMA secure_delete = ON;", nullptr, nullptr,
                      nullptr);
  }
  if (rc == SQLITE_OK) {
    what = "create table";
    rc = sqlite3_exec(db, SCHEMA, nullptr, nullptr, nullptr);
  }
  if (rc != SQLITE_OK) {
    log_failure(db, what, rc);
    db_close();
    return false;
  }
  return true;
}

void db_close(void) {
  sqlite3_close_v2(db); // no-op on nullptr
  db = nullptr;
}

// Bound values below outlive their statements, so they are bound with
// SQLITE_STATIC rather than copied.

bool db_save_password(const wchar_t *account, const Secret *password,
                      const wchar_t **err_msg) {
  if (is_blank(account)) {
    *err_msg = L"Enter an account name.";
    return false;
  }
  if (password->len == 0) {
    *err_msg = L"Enter or generate a password.";
    return false;
  }

  Ciphertext encrypted = {};
  if (!secret_encrypt(password, &encrypted)) {
    *err_msg = L"This password could not be encrypted.";
    return false;
  }

  int32_t rc = SQLITE_ERROR;
  sqlite3_stmt *stmt = prepare(
      "prepare insert", "INSERT INTO passwords (account, created, password) "
                        "VALUES (?1, date('now', 'localtime'), ?2);");
  if (stmt) {
    rc = sqlite3_bind_text16(stmt, 1, account, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_blob64(stmt, 2, encrypted.data, encrypted.len,
                               SQLITE_STATIC);
    rc = run_and_finalize(stmt, rc, "insert");
  }
  ciphertext_free(&encrypted);

  if (rc == SQLITE_CONSTRAINT_UNIQUE) {
    *err_msg = L"An account with this name already exists.";
    return false;
  }
  if (rc != SQLITE_DONE) {
    *err_msg = L"This password data could not be saved.";
    return false;
  }
  *err_msg = nullptr;
  return true;
}

bool db_each_account(AccountFn *fn, void *ctx) {
  sqlite3_stmt *stmt =
      prepare("prepare account list",
              "SELECT id, account, created FROM passwords ORDER BY account;");
  if (!stmt)
    return false;

  int32_t rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    Account account = {
        .id = sqlite3_column_int64(stmt, 0),
        .name = sqlite3_column_text16(stmt, 1),
        .created = sqlite3_column_text16(stmt, 2),
    };
    if (!account.name || !account.created) { // conversion out of memory
      rc = SQLITE_NOMEM;
      break;
    }
    fn(ctx, &account);
  }
  if (rc != SQLITE_DONE)
    log_failure(db, "read account list", rc);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool db_get_password(int64_t id, Secret *out) {
  *out = (Secret){};
  sqlite3_stmt *stmt = prepare("prepare password read",
                               "SELECT password FROM passwords WHERE id = ?1;");
  if (!stmt)
    return false;

  bool ok = false;
  int32_t rc = sqlite3_bind_int64(stmt, 1, id);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    // The blob is valid until finalize; decrypting copies it.
    ok = secret_decrypt(sqlite3_column_blob(stmt, 0),
                        (size_t)sqlite3_column_bytes(stmt, 0), out);
  } else if (rc != SQLITE_DONE) {
    log_failure(db, "read password", rc);
  }
  sqlite3_finalize(stmt);
  return ok;
}

bool db_relabel_account(int64_t id, const wchar_t *new_name,
                        const wchar_t **err_msg) {
  if (is_blank(new_name)) {
    *err_msg = L"Enter an account name.";
    return false;
  }

  int32_t rc = SQLITE_ERROR;
  sqlite3_stmt *stmt = prepare(
      "prepare relabel", "UPDATE passwords SET account = ?1 WHERE id = ?2;");
  if (stmt) {
    rc = sqlite3_bind_text16(stmt, 1, new_name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(stmt, 2, id);
    rc = run_and_finalize(stmt, rc, "relabel");
  }

  if (rc == SQLITE_CONSTRAINT_UNIQUE) {
    *err_msg = L"An account with this name already exists.";
    return false;
  }
  if (rc != SQLITE_DONE) {
    *err_msg = L"This password could not be relabeled.";
    return false;
  }
  if (sqlite3_changes64(db) == 0) {
    *err_msg = L"This account no longer exists.";
    return false;
  }
  *err_msg = nullptr;
  return true;
}

bool db_delete_account(int64_t id, const wchar_t **err_msg) {
  int32_t rc = SQLITE_ERROR;
  sqlite3_stmt *stmt =
      prepare("prepare delete", "DELETE FROM passwords WHERE id = ?1;");
  if (stmt)
    rc = run_and_finalize(stmt, sqlite3_bind_int64(stmt, 1, id), "delete");

  if (rc != SQLITE_DONE) {
    *err_msg = L"This password could not be deleted.";
    return false;
  }
  if (sqlite3_changes64(db) == 0) {
    *err_msg = L"This account no longer exists.";
    return false;
  }
  *err_msg = nullptr;
  return true;
}

// csv_field_len returns the length of s as a quoted CSV field.
static size_t csv_field_len(const char *s, size_t len) {
  size_t n = len + 2;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"')
      n++;
  }
  return n;
}

// csv_put_field writes s as a quoted CSV field, doubling its quotes, and
// returns the number of bytes written: csv_field_len(s, len).
static size_t csv_put_field(char *buf, const char *s, size_t len) {
  size_t n = 0;
  buf[n++] = '"';
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '"')
      buf[n++] = '"';
    buf[n++] = s[i];
  }
  buf[n++] = '"';
  return n;
}

// temp_path_for returns path with ".tmp" appended, to be freed by the
// caller, or nullptr when out of memory. A file is written there first and
// renamed over path only once complete, so a failure never touches a file
// already at path.
static wchar_t *temp_path_for(const wchar_t *path) {
  static const wchar_t SUFFIX[] = L".tmp";
  size_t path_len = wcslen(path);
  wchar_t *temp_path =
      malloc((path_len + _countof(SUFFIX)) * sizeof *temp_path);
  if (temp_path) {
    memcpy(temp_path, path, path_len * sizeof *path);
    memcpy(temp_path + path_len, SUFFIX, sizeof SUFFIX);
  }
  return temp_path;
}

static bool write_all(HANDLE file, const char *data, size_t len) {
  DWORD written = 0;
  return len <= MAXDWORD &&
         WriteFile(file, data, (DWORD)len, &written, nullptr) && written == len;
}

// Rows are written one at a time from exactly sized buffers that are wiped
// after each write, so no growing buffer leaves plaintext behind.
bool db_export_csv(const wchar_t *path, const wchar_t **err_msg) {
  // The UTF-8 byte order mark makes Excel read the file as UTF-8 rather
  // than the ANSI code page, which would garble non-ASCII text.
  static const char HEADER[] =
      "\xEF\xBB\xBF\"Account\",\"Created\",\"Password\"\r\n";

  sqlite3_stmt *stmt = prepare(
      "prepare export",
      "SELECT account, created, password FROM passwords ORDER BY account;");
  if (!stmt) {
    *err_msg = L"The password list could not be read.";
    return false;
  }

  const wchar_t *msg = L"The file could not be written.";
  bool ok = false;
  Secret plaintext = {};
  char *row = nullptr;
  size_t row_cap = 0;

  HANDLE file = INVALID_HANDLE_VALUE;
  bool created = false;
  wchar_t *temp_path = temp_path_for(path);
  if (!temp_path)
    goto cleanup;
  file = CreateFileW(temp_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
  created = file != INVALID_HANDLE_VALUE;
  if (!created || !write_all(file, HEADER, sizeof HEADER - 1))
    goto cleanup;

  int32_t rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    // These views are valid until the next step.
    const char *account = (const char *)sqlite3_column_text(stmt, 0);
    size_t account_len = (size_t)sqlite3_column_bytes(stmt, 0);
    const char *date = (const char *)sqlite3_column_text(stmt, 1);
    size_t date_len = (size_t)sqlite3_column_bytes(stmt, 1);
    if (!account || !date) {
      msg = L"A password could not be read.";
      goto cleanup;
    }
    if (!secret_decrypt(sqlite3_column_blob(stmt, 2),
                        (size_t)sqlite3_column_bytes(stmt, 2), &plaintext)) {
      msg = L"A password could not be decrypted.";
      goto cleanup;
    }

    // Two commas and CRLF.
    row_cap = csv_field_len(account, account_len) +
              csv_field_len(date, date_len) +
              csv_field_len(plaintext.text, plaintext.len) + 4;
    row = malloc(row_cap);
    if (!row)
      goto cleanup;
    size_t n = csv_put_field(row, account, account_len);
    row[n++] = ',';
    n += csv_put_field(row + n, date, date_len);
    row[n++] = ',';
    n += csv_put_field(row + n, plaintext.text, plaintext.len);
    row[n++] = '\r';
    row[n++] = '\n';

    bool written = write_all(file, row, n);
    SecureZeroMemory(row, row_cap);
    free(row);
    row = nullptr;
    secret_free(&plaintext);
    if (!written)
      goto cleanup;
  }
  if (rc != SQLITE_DONE) {
    log_failure(db, "read export row", rc);
    msg = L"A password could not be read.";
    goto cleanup;
  }

  // Flushed before the rename, so a flash drive pulled soon after never
  // holds a renamed but incomplete file.
  bool flushed = FlushFileBuffers(file);
  bool closed = CloseHandle(file);
  file = INVALID_HANDLE_VALUE;
  ok = flushed && closed &&
       MoveFileExW(temp_path, path,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

cleanup:
  if (row) {
    SecureZeroMemory(row, row_cap);
    free(row);
  }
  secret_free(&plaintext);
  sqlite3_finalize(stmt);
  if (file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
  if (!ok && created)
    DeleteFileW(temp_path);
  free(temp_path);
  *err_msg = ok ? nullptr : msg;
  return ok;
}

bool db_backup_to(const wchar_t *path, const wchar_t **err_msg) {
  const wchar_t *msg = L"The backup file could not be written.";
  bool ok = false;
  sqlite3 *dest = nullptr;

  wchar_t *temp_path = temp_path_for(path);
  if (!temp_path)
    goto cleanup;
  DeleteFileW(temp_path);

  int32_t rc = sq_open(temp_path, &dest);
  if (rc != SQLITE_OK) {
    msg = L"The backup file could not be created.";
    goto cleanup;
  }

  sqlite3_backup *backup = sqlite3_backup_init(dest, "main", db, "main");
  if (backup) {
    rc = sqlite3_backup_step(backup, -1); // -1: every page in one step
    int32_t finish_rc = sqlite3_backup_finish(backup);
    rc = rc == SQLITE_DONE ? finish_rc : rc;
  } else {
    rc = sqlite3_extended_errcode(dest);
  }
  if (rc != SQLITE_OK) {
    log_failure(dest, "backup", rc);
    goto cleanup;
  }

  rc = sqlite3_close_v2(dest);
  dest = nullptr;
  if (rc != SQLITE_OK ||
      !MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING))
    goto cleanup;
  ok = true;

cleanup:
  sqlite3_close_v2(dest);
  if (!ok && temp_path)
    DeleteFileW(temp_path);
  free(temp_path);
  *err_msg = ok ? nullptr : msg;
  return ok;
}

// --- Import --------------------------------------------------------------

// Far beyond any real password list; a bigger file is not one.
static constexpr DWORD IMPORT_MAX_BYTES = 16 * 1024 * 1024;

// read_text_file reads path into a malloc'd, nul-terminated UTF-16 buffer
// of *size bytes, which the caller wipes and frees. The text is UTF-8,
// after any byte order mark, or else the ANSI code page. The raw bytes are
// wiped before they are freed.
static wchar_t *read_text_file(const wchar_t *path, size_t *size,
                               const wchar_t **err_msg) {
  *size = 0;
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *err_msg = L"The file could not be opened.";
    return nullptr;
  }

  *err_msg = L"The file could not be read.";
  wchar_t *text = nullptr;
  char *bytes = nullptr;
  DWORD len = 0;
  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(file, &file_size))
    goto cleanup;
  if (file_size.QuadPart > IMPORT_MAX_BYTES) {
    *err_msg = L"This file is too large to be a password list.";
    goto cleanup;
  }
  len = (DWORD)file_size.QuadPart;
  bytes = malloc(len + 1); // + 1: malloc(0) may return nullptr
  DWORD got = 0;
  if (!bytes || !ReadFile(file, bytes, len, &got, nullptr) || got != len)
    goto cleanup;

  const char *start = bytes;
  int32_t n = (int32_t)len;
  if (n >= 3 && memcmp(start, "\xEF\xBB\xBF", 3) == 0) {
    start += 3;
    n -= 3;
  }
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int32_t wide_len =
      n > 0 ? MultiByteToWideChar(code_page, flags, start, n, nullptr, 0) : 0;
  if (n > 0 && wide_len == 0) {
    code_page = CP_ACP;
    flags = 0;
    wide_len = MultiByteToWideChar(code_page, flags, start, n, nullptr, 0);
    if (wide_len == 0)
      goto cleanup;
  }

  size_t text_size = ((size_t)wide_len + 1) * sizeof *text;
  text = malloc(text_size);
  if (!text)
    goto cleanup;
  if (n > 0 && MultiByteToWideChar(code_page, flags, start, n, text,
                                   wide_len) != wide_len) {
    SecureZeroMemory(text, text_size);
    free(text);
    text = nullptr;
    goto cleanup;
  }
  text[wide_len] = L'\0';
  *size = text_size;

  // A nul inside the text means it is not a text file, such as UTF-16.
  if (wcslen(text) != (size_t)wide_len) {
    SecureZeroMemory(text, text_size);
    free(text);
    text = nullptr;
    *size = 0;
    *err_msg = L"This file is not a CSV text file.";
    goto cleanup;
  }
  *err_msg = nullptr;

cleanup:
  if (bytes) {
    SecureZeroMemory(bytes, len);
    free(bytes);
  }
  CloseHandle(file);
  return text;
}

// A CSV parser that works in place: it unquotes each field over its own
// text, which only shrinks it, and ends it with a nul.
typedef struct CsvReader {
  wchar_t *p;   // start of the next record
  int32_t line; // line of the file p is on, from 1
} CsvReader;

typedef enum CsvStatus {
  CSV_RECORD,
  CSV_END,
  CSV_BAD_QUOTE, // an unterminated quote, or text after a closing one
} CsvStatus;

static bool ends_field(wchar_t c) {
  return c == L',' || c == L'\r' || c == L'\n' || c == L'\0';
}

// csv_next reads one record, storing up to cap field pointers in fields
// and the record's real field count in *count.
static CsvStatus csv_next(CsvReader *r, wchar_t **fields, int32_t cap,
                          int32_t *count) {
  if (*r->p == L'\0')
    return CSV_END;
  *count = 0;
  wchar_t *in = r->p;
  for (;;) {
    wchar_t *start = in;
    wchar_t *out = in;
    if (*in == L'"') {
      in++;
      for (;;) {
        if (*in == L'\0')
          return CSV_BAD_QUOTE;
        if (*in == L'"') {
          if (in[1] != L'"') {
            in++;
            break;
          }
          in++; // a doubled quote is one quote
        }
        if (*in == L'\n')
          r->line++;
        *out++ = *in++;
      }
      while (*in == L' ' || *in == L'\t')
        in++;
      if (!ends_field(*in))
        return CSV_BAD_QUOTE;
    } else {
      while (!ends_field(*in))
        *out++ = *in++;
    }

    // out can equal in, so the nul may overwrite the delimiter.
    wchar_t delimiter = *in;
    *out = L'\0';
    if (*count < cap)
      fields[*count] = start;
    (*count)++;

    if (delimiter == L',') {
      in++;
      continue;
    }
    if (delimiter == L'\r' && in[1] == L'\n')
      in++;
    if (delimiter != L'\0') {
      in++;
      r->line++;
    }
    r->p = in;
    return CSV_RECORD;
  }
}

static wchar_t *trim(wchar_t *s) {
  while (iswspace(*s))
    s++;
  wchar_t *end = s + wcslen(s);
  while (end > s && iswspace(end[-1]))
    end--;
  *end = L'\0';
  return s;
}

// import_row encrypts password and inserts it under account, setting
// *added to whether the account was new. Returns nullptr on success, or
// user-facing text saying what is wrong with the row.
static const wchar_t *import_row(sqlite3_stmt *stmt, const wchar_t *account,
                                 const wchar_t *created,
                                 const wchar_t *password, bool *added) {
  if (*account == L'\0')
    return L"This row has no account name.";
  if (wcslen(account) > ACCOUNT_NAME_MAX_CHARS)
    return L"This account name is longer than 255 characters.";
  if (*password == L'\0')
    return L"This row has no password.";

  Secret plaintext = {};
  Ciphertext encrypted = {};
  bool sealed = secret_from_wide(password, wcslen(password), &plaintext) &&
                secret_encrypt(&plaintext, &encrypted);
  secret_free(&plaintext);
  if (!sealed) {
    ciphertext_free(&encrypted);
    return L"This password could not be encrypted.";
  }

  int32_t rc = sqlite3_bind_text16(stmt, 1, account, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text16(stmt, 2, created, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_blob64(stmt, 3, encrypted.data, encrypted.len,
                             SQLITE_STATIC);
  if (rc == SQLITE_OK)
    rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE)
    log_failure(db, "import row", rc);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  ciphertext_free(&encrypted);

  if (rc != SQLITE_DONE)
    return L"This password data could not be saved.";
  *added = sqlite3_changes64(db) == 1;
  return nullptr;
}

bool db_import_csv(const wchar_t *path, ImportStats *stats,
                   const wchar_t **err_msg) {
  *stats = (ImportStats){};
  size_t text_size = 0;
  wchar_t *text = read_text_file(path, &text_size, err_msg);
  if (!text)
    return false;

  const wchar_t *msg = L"The passwords could not be saved.";
  bool ok = false;
  bool in_transaction = false;
  sqlite3_stmt *stmt = nullptr;

  CsvReader reader = {.p = text, .line = 1};
  wchar_t *fields[3];
  int32_t count = 0;
  int32_t line = reader.line;
  CsvStatus status = csv_next(&reader, fields, 3, &count);
  if (status != CSV_RECORD || count != 3 ||
      _wcsicmp(trim(fields[0]), L"Account") != 0 ||
      _wcsicmp(trim(fields[1]), L"Created") != 0 ||
      _wcsicmp(trim(fields[2]), L"Password") != 0) {
    msg = L"This is not a Keyvok CSV export. Its first row must be Account, "
          L"Created, Password.";
    goto cleanup;
  }

  // An existing account, ignoring case, is left alone. A date that
  // date() does not give back unchanged is not a valid YYYY-MM-DD.
  stmt = prepare(
      "prepare import",
      "INSERT INTO passwords (account, created, password) VALUES (?1, "
      "CASE WHEN date(?2) IS ?2 THEN ?2 ELSE date('now', 'localtime') END, ?3) "
      "ON CONFLICT (account) DO NOTHING;");
  if (!stmt)
    goto cleanup;
  int32_t rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    log_failure(db, "begin import", rc);
    goto cleanup;
  }
  in_transaction = true;

  for (;;) {
    line = reader.line;
    status = csv_next(&reader, fields, 3, &count);
    if (status == CSV_END)
      break;
    if (status == CSV_BAD_QUOTE) {
      msg = L"This row has a quote mark out of place.";
      stats->line = line;
      goto cleanup;
    }
    if (count == 1 && *trim(fields[0]) == L'\0')
      continue; // a blank line
    if (count != 3) {
      msg = L"This row does not have exactly three fields: Account, Created "
            L"and Password.";
      stats->line = line;
      goto cleanup;
    }
    bool added = false;
    const wchar_t *row_msg = import_row(stmt, trim(fields[0]), trim(fields[1]),
                                        trim(fields[2]), &added);
    if (row_msg) {
      msg = row_msg;
      stats->line = line;
      goto cleanup;
    }
    if (added)
      stats->added++;
    else
      stats->skipped++;
  }

  rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
  if (rc != SQLITE_OK) {
    log_failure(db, "commit import", rc);
    goto cleanup;
  }
  in_transaction = false;
  ok = true;

cleanup:
  sqlite3_finalize(stmt);
  if (in_transaction)
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
  if (!ok) {
    stats->added = 0;
    stats->skipped = 0;
  }
  SecureZeroMemory(text, text_size);
  free(text);
  *err_msg = ok ? nullptr : msg;
  return ok;
}
