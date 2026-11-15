// Select page: selecting a row copies its password to the clipboard, or
// shows it as a QR code when Alt is held. Only one exposure is live at a
// time: a copy closes any QR popup, and a QR code flushes any copy.

#include "app.h"

#include "clipboard.h"
#include "db.h"

static ListPage list = {.control_id = 2000, .rows = 12};

// The account whose password is currently exposed. clear_highlight uses it
// to ignore a late callback for an exposure that has since been replaced.
static int64_t exposed_id;

// clear_highlight deselects the exposed account's row when its exposure
// ends, unless a different row has been selected since. Failures call it
// too: a row left selected sends no notification when clicked again, so
// it could not be retried.
static void clear_highlight(void) {
  int64_t id;
  if (list_selected_id(&list, &id) && id == exposed_id)
    list_clear_selection(&list);
}

static void copy_password(int64_t id) {
  // Closed before exposed_id changes, so the popup's dismiss callback
  // still sees the account it showed.
  qr_close_if_open();
  exposed_id = id;

  Secret password;
  if (!db_get_password(id, &password)) {
    show_error(L"This password could not be decrypted.");
    clear_highlight();
    return;
  }
  bool copied = clip_copy(app.main_window, &password, clear_highlight);
  secret_free(&password);
  if (!copied) {
    show_error(L"This password could not be copied to the clipboard.");
    clear_highlight();
  }
}

static void show_qr(int64_t id) {
  // Flushed before exposed_id changes, so the flush callback still sees
  // the account it copied.
  clip_flush_pending();
  exposed_id = id;

  Secret password;
  if (!db_get_password(id, &password)) {
    qr_close_if_open(); // an earlier code must not stay up for this selection
    show_error(L"This password could not be decrypted.");
    clear_highlight();
    return;
  }
  QrResult result = qr_show(&password, clear_highlight);
  secret_free(&password);
  if (result == QR_SHOWN)
    return;
  show_error(result == QR_TOO_LONG
                 ? L"This password is too long to display as a QR code."
                 : L"This password could not be displayed as a QR code.");
  clear_highlight();
}

static void on_row_selected(void) {
  int64_t id;
  if (!list_selected_id(&list, &id))
    return;
  // GetKeyState reflects the keys as of the click or key press that led
  // here. Negative means down.
  if (GetKeyState(VK_MENU) < 0)
    show_qr(id);
  else
    copy_password(id);
}

// on_key turns Alt+Enter in the list into a selection of the focused row;
// on_row_selected then sees Alt down and shows the QR code, as for
// Alt+click. Alt makes the key press a WM_SYSKEYDOWN, which the list's own
// key handling never sees. Alt+Space is left alone: it opens the system menu.
static bool on_key(const MSG *msg) {
  if (msg->message == WM_SYSKEYDOWN && msg->wParam == VK_RETURN &&
      GetFocus() == list.listview) {
    list_select_focused_row(&list);
    return true;
  }
  return list_page_on_key(&list, msg);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    list_page_create(&list, hwnd);
    return 0;

  case WM_NOTIFY: {
    if (list_row_became_selected(&list, lparam))
      on_row_selected();
    LRESULT result;
    if (list_page_on_notify(&list, lparam, &result))
      return result;
    break;
  }

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    list_page_paint(&list, hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_DESTROY:
    list_page_destroy(&list);
    return 0;
  }
  return page_default_proc(hwnd, msg, wparam, lparam);
}

static void update_for_dpi(void) { list_page_update_for_dpi(&list); }
static HWND focus_target(void) { return list.listview; }

const PageDef select_page = {
    .title = L"&Select",
    .class_name = L"KeyvokSelectPage",
    .proc = proc,
    .update_for_dpi = update_for_dpi,
    .focus_target = focus_target,
    .on_key = on_key,
};
