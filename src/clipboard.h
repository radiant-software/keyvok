// Copying a password to the clipboard, with automatic clearing.
//
// A copy stays on the clipboard for CLIPBOARD_CLEAR_DELAY_MS, or until it
// is flushed early (Escape, or the app closing). Flushing clears the
// clipboard only if it still holds the copied password, so anything the
// person copied since is left alone. UI thread only.

#pragma once

#include <windows.h>

#include <stdint.h>

#include "secret.h"

static constexpr uint32_t CLIPBOARD_CLEAR_DELAY_MS = 30'000;

// Runs when a pending copy is flushed, whether or not the clipboard still
// held it.
typedef void ClipFlushFn(void);

// clip_copy puts password on the clipboard, owned by owner, and arms the
// clearing timer. A copy still pending is replaced, and its on_flush is
// dropped without being called. on_flush may be nullptr. False means
// the password is not left on the clipboard (it was never copied, or the
// clearing timer could not be armed), and on_flush is not called.
bool clip_copy(HWND owner, const Secret *password, ClipFlushFn *on_flush);

bool clip_has_pending(void);

// clip_flush_pending ends the pending copy now: it clears the clipboard if
// it still holds the password, wipes the app's copy and runs on_flush.
// No-op when nothing is pending. If another program holds the clipboard
// open, the clear is retried in the background (every 500 ms) until it
// succeeds or a new copy replaces it; the copy no longer counts as pending.
void clip_flush_pending(void);

// clip_shutdown flushes the pending copy and makes a last attempt, of up to
// about 2 s, at any clear still being retried. Call it while the owner
// window still exists, as the app closes.
void clip_shutdown(void);
