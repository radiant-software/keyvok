// Tests for the clipboard module, run by `make test-clipboard`. They use the
// real Windows clipboard, replacing whatever is on it, and leave it empty.
// One test waits for the clearing timer, so the run takes about 30 seconds.

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "clipboard.h"
#include "secret.h"

static int32_t failures;

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

static void check(bool ok, const char *expr, const char *file, int32_t line) {
  if (!ok) {
    failures++;
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", file, line, expr);
  }
}

static HWND owner;

// open_clipboard retries like the module does: WSLg's clipboard sync and
// other listeners hold the clipboard briefly after every change.
static bool open_clipboard(void) {
  for (int32_t attempt = 0; attempt < 20; attempt++) {
    if (OpenClipboard(owner))
      return true;
    Sleep(5);
  }
  return false;
}

static Secret secret_of(const wchar_t *text) {
  Secret s = {};
  CHECK(secret_from_wide(text, wcslen(text), &s));
  return s;
}

// clipboard_equals reports whether the clipboard holds exactly text.
static bool clipboard_equals(const wchar_t *text) {
  bool equal = false;
  if (open_clipboard()) {
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *current = data ? GlobalLock(data) : nullptr;
    if (current) {
      equal = wcscmp(current, text) == 0;
      GlobalUnlock(data);
    }
    CloseClipboard();
  }
  return equal;
}

static bool clipboard_has_text(void) {
  return IsClipboardFormatAvailable(CF_UNICODETEXT);
}

// set_other_text stands in for the person copying something else.
static void set_other_text(const wchar_t *text) {
  size_t size = (wcslen(text) + 1) * sizeof *text;
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
  memcpy(GlobalLock(mem), text, size);
  GlobalUnlock(mem);
  CHECK(open_clipboard());
  EmptyClipboard();
  CHECK(SetClipboardData(CF_UNICODETEXT, mem) != nullptr);
  CloseClipboard();
}

static bool copy(const wchar_t *text, ClipFlushFn *on_flush) {
  Secret s = secret_of(text);
  bool ok = clip_copy(owner, &s, on_flush);
  secret_free(&s);
  return ok;
}

static int32_t flushes_a, flushes_b;
static void on_flush_a(void) { flushes_a++; }
static void on_flush_b(void) { flushes_b++; }

static void test_copy_and_flush(void) {
  flushes_a = 0;
  CHECK(copy(L"päss \U0001F511", on_flush_a));
  CHECK(clip_has_pending());
  CHECK(clipboard_equals(L"päss \U0001F511"));
  CHECK(IsClipboardFormatAvailable(RegisterClipboardFormatW(
      L"ExcludeClipboardContentFromMonitorProcessing")));

  clip_flush_pending();
  CHECK(!clip_has_pending());
  CHECK(!clipboard_has_text());
  CHECK(flushes_a == 1);

  clip_flush_pending(); // nothing pending: no-op
  CHECK(flushes_a == 1);
}

static void test_superseded_copy_drops_its_callback(void) {
  flushes_a = flushes_b = 0;
  CHECK(copy(L"first", on_flush_a));
  CHECK(copy(L"second", on_flush_b));
  CHECK(clipboard_equals(L"second"));
  clip_flush_pending();
  CHECK(flushes_a == 0 && flushes_b == 1);
  CHECK(!clipboard_has_text());
}

static void test_flush_leaves_newer_clipboard_content(void) {
  flushes_a = 0;
  CHECK(copy(L"secret", on_flush_a));
  set_other_text(L"copied since");
  clip_flush_pending();
  CHECK(flushes_a == 1);
  CHECK(clipboard_equals(L"copied since"));
}

static void copy_again(void) { CHECK(copy(L"from callback", nullptr)); }

static void test_callback_may_start_a_new_copy(void) {
  CHECK(copy(L"secret", copy_again));
  clip_flush_pending();
  CHECK(clip_has_pending());
  CHECK(clipboard_equals(L"from callback"));
  clip_flush_pending();
  CHECK(!clipboard_has_text());
}

static void test_timer_clears_clipboard(void) {
  flushes_a = 0;
  CHECK(copy(L"timed", on_flush_a));

  // Pump messages, as the app's loop would, until the timer flushes.
  ULONGLONG deadline = GetTickCount64() + CLIPBOARD_CLEAR_DELAY_MS + 5'000;
  MSG msg;
  while (flushes_a == 0 && GetTickCount64() < deadline) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&msg);
    Sleep(50);
  }
  CHECK(flushes_a == 1);
  CHECK(!clip_has_pending());
  CHECK(!clipboard_has_text());
}

// A Holder stands in for another program, such as a clipboard manager,
// that keeps the clipboard open. It holds it on its own thread until
// release is set or hold_ms passes.
typedef struct Holder {
  HANDLE thread, held, release;
  DWORD hold_ms;
} Holder;

static DWORD WINAPI hold_clipboard(void *arg) {
  Holder *h = arg;
  while (!OpenClipboard(nullptr))
    Sleep(5);
  SetEvent(h->held);
  WaitForSingleObject(h->release, h->hold_ms);
  CloseClipboard();
  return 0;
}

static Holder start_holding(DWORD hold_ms) {
  Holder h = {.hold_ms = hold_ms};
  h.held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  h.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  h.thread = CreateThread(nullptr, 0, hold_clipboard, &h, 0, nullptr);
  CHECK(h.thread && WaitForSingleObject(h.held, 5'000) == WAIT_OBJECT_0);
  return h;
}

// stop_holding waits for the holder to close the clipboard.
static void stop_holding(Holder *h) {
  SetEvent(h->release);
  WaitForSingleObject(h->thread, INFINITE);
  CloseHandle(h->thread);
  CloseHandle(h->held);
  CloseHandle(h->release);
}

// pump_until dispatches messages, as the app's loop would, until done
// returns true or timeout_ms passes.
static void pump_until(bool (*done)(void), DWORD timeout_ms) {
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  MSG msg;
  while (!done() && GetTickCount64() < deadline) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&msg);
    Sleep(20);
  }
}

static bool never(void) { return false; }
static bool clipboard_is_empty(void) { return !clipboard_has_text(); }

static void test_held_clipboard_is_cleared_later(void) {
  flushes_a = 0;
  CHECK(copy(L"held secret", on_flush_a));
  Holder h = start_holding(INFINITE);
  clip_flush_pending();
  // The flush ends the copy for the UI even though the clear failed.
  CHECK(flushes_a == 1);
  CHECK(!clip_has_pending());
  stop_holding(&h);

  // No messages have run, so the retry has not either.
  CHECK(clipboard_equals(L"held secret"));
  pump_until(clipboard_is_empty, 3'000);
  CHECK(!clipboard_has_text());
}

static void test_retry_leaves_newer_copy(void) {
  CHECK(copy(L"held secret", nullptr));
  Holder h = start_holding(INFINITE);
  clip_flush_pending();
  stop_holding(&h);

  // A new copy replaces the uncleared one; the retry must not clear it.
  CHECK(copy(L"newer", nullptr));
  pump_until(never, 1'500);
  CHECK(clip_has_pending());
  CHECK(clipboard_equals(L"newer"));
  clip_flush_pending();
  CHECK(!clipboard_has_text());
}

static void test_shutdown_waits_for_held_clipboard(void) {
  CHECK(copy(L"held secret", nullptr));
  Holder h = start_holding(300); // let go during clip_shutdown's last attempt
  clip_shutdown();
  stop_holding(&h);
  CHECK(!clip_has_pending());
  CHECK(!clipboard_has_text());
}

int wmain(void) {
  // A message-only window is enough to own the clipboard and its timer.
  owner = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                          nullptr, nullptr, nullptr);
  if (!owner) {
    fprintf(stderr, "could not create the owner window\n");
    return 2;
  }

  test_copy_and_flush();
  test_superseded_copy_drops_its_callback();
  test_flush_leaves_newer_clipboard_content();
  test_callback_may_start_a_new_copy();
  test_held_clipboard_is_cleared_later();
  test_retry_leaves_newer_copy();
  test_shutdown_waits_for_held_clipboard();
  test_timer_clears_clipboard();

  // Leave the clipboard empty, whatever the tests did.
  if (open_clipboard()) {
    EmptyClipboard();
    CloseClipboard();
  }
  DestroyWindow(owner);

  if (failures > 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("all clipboard tests passed\n");
  return 0;
}
