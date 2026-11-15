// Delete page: selecting a row asks for confirmation, then deletes the
// account.

#include "app.h"

#include <stdlib.h>
#include <wchar.h>

#include "db.h"

static ListPage list = {.control_id = 2001, .rows = 12};

// Posted to the page to show the confirmation. The ListView reports the
// selection while it still holds mouse capture from the click; a modal
// dialog opened then would leave the button release queued, and it would
// land on whichever row moved under the cursor, selecting it and opening
// the dialog again.
static constexpr UINT CONFIRM_DELETE_MSG = WM_APP + 1;

// confirm_and_delete asks, with Cancel as the default button, and deletes
// the selected account on OK. Cancel clears the row's selection.
static void confirm_and_delete(void) {
  int64_t id;
  wchar_t name[ACCOUNT_NAME_MAX_CHARS + 1];
  if (!list_selected_id(&list, &id) ||
      !list_selected_name(&list, name, (int32_t)_countof(name)))
    return;

  wchar_t prompt[ACCOUNT_NAME_MAX_CHARS + 64];
  swprintf(prompt, _countof(prompt), L"%ls will be permanently deleted.", name);
  int32_t choice = MessageBoxW(app.main_window, prompt, L"Keyvok",
                               MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
  // The message box returns focus to the main window, not the list.
  SetFocus(list.listview);
  if (choice != IDOK) {
    list_clear_selection(&list);
    return;
  }

  const wchar_t *err;
  if (!db_delete_account(id, &err)) {
    // A row left selected sends no notification when clicked again, so
    // the delete could not be retried.
    list_clear_selection(&list);
    show_error(err);
    return;
  }
  list_reload_all();
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    list_page_create(&list, hwnd);
    return 0;

  case WM_NOTIFY: {
    if (list_row_became_selected(&list, lparam))
      PostMessageW(hwnd, CONFIRM_DELETE_MSG, 0, 0);
    LRESULT result;
    if (list_page_on_notify(&list, lparam, &result))
      return result;
    break;
  }

  case CONFIRM_DELETE_MSG:
    confirm_and_delete();
    return 0;

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
static bool on_key(const MSG *msg) { return list_page_on_key(&list, msg); }

const PageDef delete_page = {
    .title = L"&Delete",
    .class_name = L"KeyvokDeletePage",
    .proc = proc,
    .update_for_dpi = update_for_dpi,
    .focus_target = focus_target,
    .on_key = on_key,
};
