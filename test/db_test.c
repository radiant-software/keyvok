// Tests for sq, secret and db. Run by `make test` with a scratch directory
// as its argument. Prints counts only, never account names or passwords.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "db.h"
#include "secret.h"
#include "sq.h"

static int32_t failures;

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

static void check(bool ok, const char *expr, const char *file, int32_t line) {
  if (!ok) {
    failures++;
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }
}

static const wchar_t *scratch_dir;

// scratch_path returns dir\name in a static buffer, overwritten by the next
// call.
static const wchar_t *scratch_path(const wchar_t *name) {
  static wchar_t buf[MAX_PATH];
  swprintf(buf, _countof(buf), L"%ls\\%ls", scratch_dir, name);
  return buf;
}

static Secret secret_of(const wchar_t *text) {
  Secret s = {};
  CHECK(secret_from_wide(text, wcslen(text), &s));
  return s;
}

// --- sq ----------------------------------------------------------------------

static void test_sq_is_strict(void) {
  sqlite3 *conn = nullptr;
  CHECK(sq_open(L":memory:", &conn) == SQLITE_OK);
  CHECK(sqlite3_exec(
            conn,
            "CREATE TABLE parent (id INTEGER PRIMARY KEY);"
            "CREATE TABLE child (parent_id INTEGER REFERENCES parent (id));",
            nullptr, nullptr, nullptr) == SQLITE_OK);

  // Foreign keys are enforced, and extended codes say which constraint failed.
  CHECK(sqlite3_exec(conn, "INSERT INTO child VALUES (42);", nullptr, nullptr,
                     nullptr) == SQLITE_CONSTRAINT_FOREIGNKEY);

  // A double-quoted name that matches no column is an error, not a string.
  sqlite3_stmt *stmt = nullptr;
  CHECK(sqlite3_prepare_v2(conn, "SELECT \"no_such_column\";", -1, &stmt,
                           nullptr) == SQLITE_ERROR);
  CHECK(stmt == nullptr);

  CHECK(sqlite3_close_v2(conn) == SQLITE_OK);
}

// --- secret ------------------------------------------------------------------

static void test_generate(void) {
  // 2000 rounds: without the redraw, about 94 would start with = + or @.
  for (int32_t round = 0; round < 2000; round++) {
    Secret pw = {};
    CHECK(secret_generate(&pw));
    CHECK(pw.len == PASSWORD_LENGTH && pw.text[pw.len] == '\0');

    bool symbol = false, digit = false, upper = false, lower = false,
         confusable = false;
    for (size_t i = 0; i < pw.len; i++) {
      char c = pw.text[i];
      symbol |= strchr("!#%+:=?@", c) != nullptr;
      digit |= c >= '2' && c <= '9';
      upper |= c >= 'A' && c <= 'Z';
      lower |= c >= 'a' && c <= 'z';
      confusable |= strchr("0O1lIQ", c) != nullptr;
    }
    CHECK(symbol && digit && upper && lower && !confusable);
    CHECK(strchr("=+-@", pw.text[0]) ==
          nullptr); // spreadsheets read these as formulas
    secret_free(&pw);
    CHECK(pw.text == nullptr && pw.len == 0);
  }
}

static void test_encrypt_round_trip(void) {
  Secret plain = secret_of(
      L"pässwörd \U0001F511"); // non-ASCII, including a surrogate pair
  CHECK(plain.len == 15);      // UTF-8: 1+2+3+2+2+1+4

  Ciphertext sealed = {};
  CHECK(secret_encrypt(&plain, &sealed));
  CHECK(sealed.len > plain.len);

  Secret opened = {};
  CHECK(secret_decrypt(sealed.data, sealed.len, &opened));
  CHECK(opened.len == plain.len &&
        memcmp(opened.text, plain.text, plain.len) == 0);
  CHECK(opened.text[opened.len] == '\0');

  // Tampered ciphertext fails to decrypt.
  sealed.data[sealed.len / 2] ^= 0x01;
  Secret tampered = {};
  CHECK(!secret_decrypt(sealed.data, sealed.len, &tampered));
  CHECK(tampered.text == nullptr);

  // Empty input is refused, and an empty conversion is an empty Secret.
  Secret empty = {};
  Ciphertext none = {};
  CHECK(!secret_encrypt(&empty, &none));
  CHECK(secret_from_wide(L"", 0, &empty) && empty.text == nullptr);

  secret_free(&plain);
  secret_free(&opened);
  ciphertext_free(&sealed);
}

// --- db ----------------------------------------------------------------------

typedef struct AccountList {
  int32_t count;
  int64_t ids[8];
  wchar_t names[8][64];
  wchar_t created[8][16];
} AccountList;

static void collect(void *ctx, const Account *account) {
  AccountList *list = ctx;
  if (list->count < 8) {
    list->ids[list->count] = account->id;
    wcsncpy_s(list->names[list->count], 64, account->name, _TRUNCATE);
    wcsncpy_s(list->created[list->count], 16, account->created, _TRUNCATE);
  }
  list->count++;
}

static AccountList list_accounts(void) {
  AccountList list = {};
  CHECK(db_each_account(collect, &list));
  return list;
}

static bool save(const wchar_t *account, const wchar_t *password,
                 const wchar_t **err_msg) {
  Secret pw = secret_of(password);
  bool ok = db_save_password(account, &pw, err_msg);
  secret_free(&pw);
  return ok;
}

static bool write_file(const wchar_t *path, const char *text) {
  FILE *f = nullptr;
  if (_wfopen_s(&f, path, L"wb") != 0)
    return false;
  bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
  return fclose(f) == 0 && ok;
}

static bool file_equals(const wchar_t *path, const char *expected) {
  char buf[1024] = {};
  FILE *f = nullptr;
  if (_wfopen_s(&f, path, L"rb") != 0)
    return false;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  return n == strlen(expected) && memcmp(buf, expected, n) == 0;
}

static void test_db_accounts(void) {
  const wchar_t *err = nullptr;
  const wchar_t *path = scratch_path(L"accounts.db");
  DeleteFileW(path);
  CHECK(db_open(path));

  CHECK(save(L"Bob", L"hunter2", &err) && err == nullptr);
  CHECK(save(L"alice", L"pa\"ss,word", &err));

  // Names are unique without regard to case.
  CHECK(!save(L"BOB", L"x", &err) &&
        wcscmp(err, L"An account with this name already exists.") == 0);
  CHECK(!save(L" \t ", L"x", &err) &&
        wcscmp(err, L"Enter an account name.") == 0);
  CHECK(!save(L"Carol", L"", &err) &&
        wcscmp(err, L"Enter or generate a password.") == 0);

  // Sorted without regard to case, dated YYYY-MM-DD.
  AccountList list = list_accounts();
  CHECK(list.count == 2);
  CHECK(wcscmp(list.names[0], L"alice") == 0 &&
        wcscmp(list.names[1], L"Bob") == 0);
  CHECK(wcslen(list.created[0]) == 10 && list.created[0][4] == L'-' &&
        list.created[0][7] == L'-');
  int64_t alice = list.ids[0], bob = list.ids[1];

  Secret pw = {};
  CHECK(db_get_password(bob, &pw) && pw.len == 7 &&
        memcmp(pw.text, "hunter2", 7) == 0);
  secret_free(&pw);
  CHECK(!db_get_password(999, &pw) && pw.text == nullptr);

  CHECK(db_relabel_account(bob, L"Say \"hi\"", &err) && err == nullptr);
  CHECK(!db_relabel_account(bob, L"ALICE", &err) &&
        wcscmp(err, L"An account with this name already exists.") == 0);
  CHECK(!db_relabel_account(bob, L"  ", &err) &&
        wcscmp(err, L"Enter an account name.") == 0);
  CHECK(!db_relabel_account(999, L"Dave", &err) &&
        wcscmp(err, L"This account no longer exists.") == 0);

  // UTF-8 with a byte order mark. Fields are quoted, with quotes doubled;
  // the password keeps its comma. An earlier export is replaced.
  wchar_t csv[MAX_PATH], csv_tmp[MAX_PATH]; // scratch_path reuses its buffer
  wcscpy_s(csv, _countof(csv), scratch_path(L"export.csv"));
  wcscpy_s(csv_tmp, _countof(csv_tmp), scratch_path(L"export.csv.tmp"));
  CHECK(write_file(csv, "earlier export"));
  CHECK(db_export_csv(csv, &err) && err == nullptr);
  CHECK(GetFileAttributesW(csv_tmp) == INVALID_FILE_ATTRIBUTES);
  char expected[256];
  snprintf(expected, sizeof expected,
           "\xEF\xBB\xBF\"Account\",\"Created\",\"Password\"\r\n"
           "\"alice\",\"%ls\",\"pa\"\"ss,word\"\r\n"
           "\"Say \"\"hi\"\"\",\"%ls\",\"hunter2\"\r\n",
           list.created[0], list.created[1]);
  CHECK(file_equals(csv, expected));

  // A failed export keeps the earlier file and leaves no temp file. A
  // directory in the way of the temp file makes the export fail.
  CHECK(CreateDirectoryW(csv_tmp, nullptr));
  CHECK(!db_export_csv(csv, &err) && err != nullptr);
  CHECK(file_equals(csv, expected));
  CHECK(RemoveDirectoryW(csv_tmp));

  // The backup is a complete, openable copy.
  CHECK(db_backup_to(scratch_path(L"backup.db"), &err) && err == nullptr);
  CHECK(GetFileAttributesW(scratch_path(L"backup.db.tmp")) ==
        INVALID_FILE_ATTRIBUTES);

  CHECK(db_delete_account(alice, &err) && err == nullptr);
  CHECK(!db_delete_account(alice, &err) &&
        wcscmp(err, L"This account no longer exists.") == 0);
  CHECK(list_accounts().count == 1);
  db_close();

  CHECK(db_open(scratch_path(L"backup.db")));
  CHECK(list_accounts().count == 2);
  CHECK(db_get_password(bob, &pw) && pw.len == 7);
  secret_free(&pw);
  db_close();
  db_close(); // safe when already closed
}

static bool password_is(int64_t id, const char *expected) {
  Secret pw = {};
  bool ok = db_get_password(id, &pw) && pw.len == strlen(expected) &&
            memcmp(pw.text, expected, pw.len) == 0;
  secret_free(&pw);
  return ok;
}

static bool import(const char *csv, ImportStats *stats, const wchar_t **err) {
  wchar_t path[MAX_PATH];
  wcscpy_s(path, _countof(path), scratch_path(L"import.csv"));
  return write_file(path, csv) && db_import_csv(path, stats, err);
}

static void test_db_import(void) {
  const wchar_t *err = nullptr;
  ImportStats stats = {};
  DeleteFileW(scratch_path(L"import.db"));
  CHECK(db_open(scratch_path(L"import.db")));
  CHECK(save(L"Bob", L"kept", &err));
  AccountList list = list_accounts();
  wchar_t today[16];
  wcscpy_s(today, _countof(today), list.created[0]);

  // An existing name in another case is skipped and keeps its password.
  // Quoted and bare fields, LF and CRLF, blank lines, padding, a quoted
  // line break and a date Excel reformatted.
  CHECK(import("\xEF\xBB\xBF\"Account\",\"Created\",\"Password\"\r\n"
               "\"alice\",\"2020-01-02\",\"pa\"\"ss,word\"\r\n"
               "\r\n"
               "BOB,2021-05-06,replaced\n"
               "  carol  , 1/2/2020 ,  open sesame  \n"
               "\"dave\nsmith\",2020-02-30,x\n",
               &stats, &err) &&
        err == nullptr);
  CHECK(stats.added == 3 && stats.skipped == 1 && stats.line == 0);
  list = list_accounts();
  CHECK(list.count == 4);
  CHECK(wcscmp(list.names[0], L"alice") == 0 &&
        wcscmp(list.created[0], L"2020-01-02") == 0);
  CHECK(wcscmp(list.names[1], L"Bob") == 0 &&
        wcscmp(list.created[1], today) == 0);
  CHECK(wcscmp(list.names[2], L"carol") == 0 &&
        wcscmp(list.created[2], today) == 0);
  CHECK(wcscmp(list.names[3], L"dave\nsmith") == 0 &&
        wcscmp(list.created[3], today) == 0);
  CHECK(password_is(list.ids[0], "pa\"ss,word"));
  CHECK(password_is(list.ids[1], "kept"));
  CHECK(password_is(list.ids[2], "open sesame"));

  // A bad row fails the whole import at its line, adding nothing.
  CHECK(!import("Account,Created,Password\r\nerin,,one\r\nfrank,,\r\n", &stats,
                &err));
  CHECK(stats.line == 3 && stats.added == 0 &&
        wcscmp(err, L"This row has no password.") == 0);
  CHECK(!import("Account,Created,Password\r\nerin,,one,extra\r\n", &stats,
                &err) &&
        stats.line == 2);
  CHECK(!import("Account,Created,Password\r\n,,one\r\n", &stats, &err) &&
        stats.line == 2);
  CHECK(!import("Account,Created,Password\r\n\"erin,,one\r\n", &stats, &err) &&
        stats.line == 2);
  CHECK(
      !import("Account,Created,Password\r\n\"erin\"x,,one\r\n", &stats, &err) &&
      stats.line == 2);
  CHECK(!import("Name,Password\r\nerin,one\r\n", &stats, &err) &&
        stats.line == 0);
  CHECK(!import("", &stats, &err) && stats.line == 0);
  CHECK(list_accounts().count == 4);

  // Text that is not UTF-8 is read in the ANSI code page.
  if (GetACP() == 1252) {
    CHECK(import("Account,Created,Password\r\ncaf\xE9,,cr\xE8me\r\n", &stats,
                 &err) &&
          stats.added == 1);
    list = list_accounts();
    CHECK(list.count == 5 && wcscmp(list.names[2], L"café") == 0);
    CHECK(password_is(list.ids[2], "cr\xC3\xA8me"));
  }

  // An export imports into an empty database unchanged.
  wchar_t csv[MAX_PATH];
  wcscpy_s(csv, _countof(csv), scratch_path(L"round-trip.csv"));
  CHECK(db_export_csv(csv, &err));
  AccountList before = list_accounts();
  db_close();
  DeleteFileW(scratch_path(L"import2.db"));
  CHECK(db_open(scratch_path(L"import2.db")));
  CHECK(db_import_csv(csv, &stats, &err) && stats.added == before.count &&
        stats.skipped == 0);
  AccountList after = list_accounts();
  CHECK(after.count == before.count);
  for (int32_t i = 0; i < after.count && i < 8; i++)
    CHECK(wcscmp(after.names[i], before.names[i]) == 0 &&
          wcscmp(after.created[i], before.created[i]) == 0);
  CHECK(password_is(after.ids[0], "pa\"ss,word"));
  db_close();
}

int wmain(int argc, wchar_t **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: db_test <scratch directory>\n");
    return 2;
  }
  scratch_dir = argv[1];

  test_sq_is_strict();
  test_generate();
  test_encrypt_round_trip();
  test_db_accounts();
  test_db_import();

  if (failures > 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
