// Creates a database of fake accounts for trying the app by hand, run by
// `make seed`. Usage: seed.exe <csv> <db>. Refuses to touch an existing
// database, so it can never overwrite real passwords.

#include <windows.h>

#include <stdio.h>

#include "db.h"

int wmain(int argc, wchar_t **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: seed <csv> <db>\n");
    return 2;
  }
  const wchar_t *csv = argv[1];
  const wchar_t *path = argv[2];

  if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
    fprintf(stderr, "seed: %ls already exists; delete it first\n", path);
    return 1;
  }
  if (!db_open(path)) {
    fprintf(stderr, "seed: cannot create %ls\n", path);
    return 1;
  }

  ImportStats stats = {};
  const wchar_t *err = nullptr;
  bool ok = db_import_csv(csv, &stats, &err);
  db_close();
  if (!ok) {
    fprintf(stderr, "seed: import failed at line %d: %ls\n", stats.line,
            err ? err : L"unknown error");
    DeleteFileW(path);
    return 1;
  }
  printf("seed: %d accounts written to %ls\n", stats.added, path);
  return 0;
}
