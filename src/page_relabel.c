// Relabel page: a shorter list with an Account field and a Save button
// below it. Selecting a row puts its name in the field for editing.

#include "app.h"

#include <stdlib.h>

#include "db.h"

static ListPage list = {.control_id = 2002, .rows = 7};

static constexpr int32_t GAP_AFTER_LIST = 20;
static constexpr int32_t LABEL_HEIGHT = 20;
static constexpr int32_t LABEL_TO_EDIT_GAP = 4;
static constexpr int32_t EDIT_HEIGHT =
    30; // outer height, border included; same as Create
static constexpr int32_t EDIT_TO_BUTTON_GAP = 24; // same as Create
static constexpr int32_t SAVE_BUTTON_WIDTH = 160;
static constexpr int32_t BUTTON_HEIGHT = 34; // same as Create

static constexpr uint16_t ID_ACCOUNT_EDIT = 2010;
static constexpr uint16_t ID_SAVE_BUTTON = 2011;

static HWND account_edit;
static HWND save_button;
static CardBox edit_box;

typedef struct Layout {
  RECT label;
  RECT edit; // the card box; the EDIT control sits inside it
  RECT button;
} Layout;

// layout stacks the form below the list. The field is as wide as the
// list's Account column.
static Layout layout(void) {
  int32_t left = scale(PAGE_MARGIN);
  int32_t edit_w = scale(ACCOUNT_FIELD_WIDTH) + scale(1) * 2;
  Layout l;
  int32_t y = list_box_bottom(&list) + scale(GAP_AFTER_LIST);
  l.label = (RECT){left, y, left + edit_w, y + scale(LABEL_HEIGHT)};
  y = l.label.bottom + scale(LABEL_TO_EDIT_GAP);
  l.edit = (RECT){left, y, left + edit_w, y + scale(EDIT_HEIGHT)};
  y = l.edit.bottom + scale(EDIT_TO_BUTTON_GAP);
  l.button = (RECT){left, y, left + scale(SAVE_BUTTON_WIDTH),
                    y + scale(BUTTON_HEIGHT)};
  return l;
}

static void on_create(HWND hwnd) {
  list_page_create(&list, hwnd);
  Layout l = layout();

  // No native border: on_paint draws a card box around it.
  account_edit = create_control(
      hwnd, WC_EDITW, nullptr, WS_TABSTOP | ES_AUTOHSCROLL,
      edit_control_bounds(l.edit, app.ui_font), ID_ACCOUNT_EDIT, app.ui_font);
  set_edit_margins(account_edit, EDIT_MARGIN);
  SendMessageW(account_edit, EM_LIMITTEXT, ACCOUNT_NAME_MAX_CHARS, 0);
  card_box_set(&edit_box, l.edit);

  // BS_DEFPUSHBUTTON makes Enter in the field press Save.
  save_button = create_control(hwnd, WC_BUTTONW, L"Save New Label",
                               WS_TABSTOP | BS_DEFPUSHBUTTON, l.button,
                               ID_SAVE_BUTTON, app.ui_font);
}

static void on_row_selected(void) {
  wchar_t name[ACCOUNT_NAME_MAX_CHARS + 1];
  if (list_selected_name(&list, name, (int32_t)_countof(name)))
    SetWindowTextW(account_edit, name);
}

// save renames the selected account to the field's text.
static void save(void) {
  int64_t id;
  if (!list_selected_id(&list, &id)) {
    show_error(L"Select an account to relabel.");
    return;
  }
  wchar_t buf[ACCOUNT_NAME_MAX_CHARS + 1];
  const wchar_t *name =
      edit_get_trimmed_text(account_edit, buf, (int32_t)_countof(buf));

  const wchar_t *err;
  if (!db_relabel_account(id, name, &err)) {
    show_error(err);
    return;
  }
  SetWindowTextW(account_edit, L"");
  list_reload_all();
}

static void on_paint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  list_page_paint(&list, hdc);
  draw_label(hdc, L"Account", layout().label, LABEL_COLOR,
             DT_VCENTER | DT_SINGLELINE);
  card_box_paint(hdc, &edit_box);
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    on_create(hwnd);
    return 0;

  case WM_NOTIFY: {
    if (list_row_became_selected(&list, lparam))
      on_row_selected();
    LRESULT result;
    if (list_page_on_notify(&list, lparam, &result))
      return result;
    break;
  }

  case WM_COMMAND:
    if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == ID_SAVE_BUTTON) {
      save();
      return 0;
    }
    break;

  case WM_PAINT:
    on_paint(hwnd);
    return 0;

  case WM_DESTROY:
    list_page_destroy(&list);
    card_box_destroy(&edit_box);
    return 0;
  }
  return page_default_proc(hwnd, msg, wparam, lparam);
}

static void update_for_dpi(void) {
  list_page_update_for_dpi(&list);
  Layout l = layout();
  place_control(account_edit, edit_control_bounds(l.edit, app.ui_font),
                app.ui_font);
  set_edit_margins(account_edit, EDIT_MARGIN);
  place_control(save_button, l.button, app.ui_font);
  card_box_set(&edit_box, l.edit);
}

static HWND focus_target(void) { return list.listview; }
static HWND default_button(void) { return save_button; }
static bool on_key(const MSG *msg) { return list_page_on_key(&list, msg); }

const PageDef relabel_page = {
    .title = L"&Relabel",
    .class_name = L"KeyvokRelabelPage",
    .proc = proc,
    .update_for_dpi = update_for_dpi,
    .focus_target = focus_target,
    .default_button = default_button,
    .on_key = on_key,
};
