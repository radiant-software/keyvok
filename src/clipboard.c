#include "clipboard.h"

#include <string.h>
#include <wchar.h>

// A timer on the UI thread, rather than a background thread, lets the app
// clear the clipboard itself when it closes before the timer fires.
static constexpr UINT_PTR CLEAR_TIMER_ID =
    0x4B56; // "KV"; unique among the owner's timers
static constexpr UINT_PTR RETRY_TIMER_ID = 0x4B57;

// How often a clear that found the clipboard held open is retried.
static constexpr UINT RETRY_INTERVAL_MS = 500;

// The pending copy as UTF-16 with its terminator, kept so a flush can check
// the clipboard still holds it. nullptr means nothing is pending.
static wchar_t *pending;
static size_t pending_size; // bytes, including the terminator
static HWND pending_owner;
static ClipFlushFn *pending_on_flush;

// A flushed copy that could not be cleared because another program held
// the clipboard open. Its text is kept so a retry can still check that the
// clipboard holds it. nullptr means none.
static wchar_t *uncleared;
static size_t uncleared_size;
static HWND uncleared_owner;

// Attempts 5 ms apart: OPEN_ATTEMPTS is about 100 ms, EXIT_OPEN_ATTEMPTS,
// for the last chance when the app closes, about 2 s.
static constexpr int32_t OPEN_ATTEMPTS = 20;
static constexpr int32_t EXIT_OPEN_ATTEMPTS = 400;

// open_clipboard opens the clipboard, trying up to attempts times.
// Clipboard listeners, such as WSLg's clipboard sync (msrdc.exe),
// Remote Desktop and clipboard managers, hold it open briefly after every
// change, and OpenClipboard fails meanwhile.
static bool open_clipboard(HWND owner, int32_t attempts) {
  for (int32_t attempt = 0; attempt < attempts; attempt++) {
    if (OpenClipboard(owner))
      return true;
    if (attempt + 1 < attempts)
      Sleep(5);
  }
  return false;
}

// clear_if_holds empties the clipboard if it still holds text. False means
// the clipboard could not be opened, so it may still hold text.
//
// The clipboard's memory belongs to the system and cannot be wiped here,
// only released by EmptyClipboard. wcscmp stops at the first difference,
// so it never reads past the clipboard's terminator.
static bool clear_if_holds(HWND owner, const wchar_t *text, int32_t attempts) {
  if (!open_clipboard(owner, attempts))
    return false;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  const wchar_t *current = data ? GlobalLock(data) : nullptr;
  if (current) {
    bool still_ours = wcscmp(current, text) == 0;
    GlobalUnlock(data);
    if (still_ours)
      EmptyClipboard();
  }
  CloseClipboard();
  return true;
}

static void discard_uncleared(void) {
  if (uncleared)
    KillTimer(uncleared_owner, RETRY_TIMER_ID);
  wide_secret_free(uncleared, uncleared_size);
  uncleared = nullptr;
  uncleared_size = 0;
}

// A single attempt per tick, so a clipboard held open for long does not
// stall the UI thread.
static void CALLBACK on_retry_timer(HWND, UINT, UINT_PTR, DWORD) {
  if (uncleared && clear_if_holds(uncleared_owner, uncleared, 1))
    discard_uncleared();
}

bool clip_has_pending(void) { return pending != nullptr; }

static void discard_pending(void) {
  wide_secret_free(pending, pending_size);
  pending = nullptr;
  pending_size = 0;
  pending_on_flush = nullptr;
}

static void wipe_global(HGLOBAL mem, size_t size) {
  void *p = GlobalLock(mem);
  if (p) {
    SecureZeroMemory(p, size);
    GlobalUnlock(mem);
  }
}

// mark_excluded_from_history adds the documented
// ExcludeClipboardContentFromMonitorProcessing format, which keeps the
// entry out of Clipboard History (Win+V) and Cloud Clipboard. The clipboard
// must still be open from the copy. Failure is ignored, since the password
// is already on the clipboard.
static void mark_excluded_from_history(void) {
  UINT format =
      RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
  if (format == 0)
    return;

  // A real one-byte block: a null handle would request delayed rendering.
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, 1);
  if (mem && !SetClipboardData(format, mem))
    GlobalFree(mem);
}

// set_clipboard_text places size bytes of nul-terminated UTF-16 on the
// clipboard as CF_UNICODETEXT. On failure, its copy is wiped and freed.
static bool set_clipboard_text(HWND owner, const wchar_t *text, size_t size) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!mem)
    return false;
  void *dst = GlobalLock(mem);
  if (!dst) {
    GlobalFree(mem);
    return false;
  }
  memcpy(dst, text, size);
  GlobalUnlock(mem);

  // SetClipboardData fails if the clipboard was opened without an owner.
  if (!open_clipboard(owner, OPEN_ATTEMPTS))
    goto fail;
  EmptyClipboard();
  if (!SetClipboardData(CF_UNICODETEXT, mem)) {
    CloseClipboard();
    goto fail;
  }
  // The clipboard owns mem now.
  mark_excluded_from_history();
  CloseClipboard();
  return true;

fail:
  wipe_global(mem, size);
  GlobalFree(mem);
  return false;
}

static void CALLBACK on_clear_timer(HWND, UINT, UINT_PTR, DWORD) {
  clip_flush_pending();
}

bool clip_copy(HWND owner, const Secret *password, ClipFlushFn *on_flush) {
  size_t size = 0;
  wchar_t *text = secret_to_wide(password, &size);
  if (!text)
    return false;
  if (!set_clipboard_text(owner, text, size)) {
    wide_secret_free(text, size);
    return false;
  }

  // The new copy has replaced whatever the clipboard held.
  discard_uncleared();
  discard_pending();
  pending = text;
  pending_size = size;
  pending_owner = owner;

  // Re-arming an existing timer restarts it, so the new copy gets its
  // full time. Without the timer nothing would clear the clipboard, so
  // the copy is taken back off it. on_flush is not set yet, so it does
  // not run for a copy that failed.
  if (!SetTimer(owner, CLEAR_TIMER_ID, CLIPBOARD_CLEAR_DELAY_MS,
                on_clear_timer)) {
    clip_flush_pending();
    return false;
  }
  pending_on_flush = on_flush;
  return true;
}

void clip_flush_pending(void) {
  if (!pending)
    return;
  KillTimer(pending_owner, CLEAR_TIMER_ID);

  if (clear_if_holds(pending_owner, pending, OPEN_ATTEMPTS)) {
    wide_secret_free(pending, pending_size);
  } else {
    // Hand the copy over to be retried until the clipboard can be
    // opened, so the password is not left on it. A failed SetTimer
    // leaves the retry to clip_shutdown.
    discard_uncleared();
    uncleared = pending;
    uncleared_size = pending_size;
    uncleared_owner = pending_owner;
    SetTimer(uncleared_owner, RETRY_TIMER_ID, RETRY_INTERVAL_MS,
             on_retry_timer);
  }
  pending = nullptr;
  pending_size = 0;

  // The callback runs last, once this copy is gone, so it may start a
  // new one.
  ClipFlushFn *on_flush = pending_on_flush;
  pending_on_flush = nullptr;
  if (on_flush)
    on_flush();
}

void clip_shutdown(void) {
  clip_flush_pending();
  if (uncleared) {
    clear_if_holds(uncleared_owner, uncleared, EXIT_OPEN_ATTEMPTS);
    discard_uncleared();
  }
}
