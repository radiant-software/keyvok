// Create page: Account and Password fields, then the Generate Strong
// Password and Save Account and Password buttons.

#include "app.h"

#include <stdlib.h>

#include "db.h"

static constexpr int32_t LABEL_HEIGHT = 20;
static constexpr int32_t LABEL_TO_EDIT_GAP = 4;
static constexpr int32_t EDIT_HEIGHT =
    30; // outer height, border included; same as Relabel
static constexpr int32_t GAP_AFTER_ACCOUNT_FIELD = 20;
static constexpr int32_t EDIT_TO_BUTTONS_GAP = 24;
static constexpr int32_t BUTTON_GAP = 16;
static constexpr int32_t BUTTON_HEIGHT = 34;

static constexpr uint16_t ID_ACCOUNT_EDIT = 2020;
static constexpr uint16_t ID_PASSWORD_EDIT = 2021;
static constexpr uint16_t ID_GENERATE_BUTTON = 2022;
static constexpr uint16_t ID_SAVE_BUTTON = 2023;

static HWND account_edit;
static HWND password_edit;
static HWND generate_button;
static HWND save_button;
static HFONT password_font; // monospace, so each character of a generated
                            // password is easy to read
static CardBox account_box;
static CardBox password_box;

typedef struct Layout {
  RECT account_label, account_edit; // edit rects are the card boxes
  RECT password_label, password_edit;
  RECT generate_button, save_button;
} Layout;

// layout stacks the form from the top. Password is as wide as the content;
// Account matches the lists' Account column.
static Layout layout(void) {
  int32_t left = scale(PAGE_MARGIN);
  int32_t full_w = scale(CONTENT_WIDTH);
  int32_t account_w = scale(ACCOUNT_FIELD_WIDTH);
  Layout l;

  int32_t y = scale(PAGE_MARGIN);
  l.account_label = (RECT){left, y, left + account_w, y + scale(LABEL_HEIGHT)};
  y = l.account_label.bottom + scale(LABEL_TO_EDIT_GAP);
  l.account_edit = (RECT){left, y, left + account_w, y + scale(EDIT_HEIGHT)};

  y = l.account_edit.bottom + scale(GAP_AFTER_ACCOUNT_FIELD);
  l.password_label = (RECT){left, y, left + full_w, y + scale(LABEL_HEIGHT)};
  y = l.password_label.bottom + scale(LABEL_TO_EDIT_GAP);
  l.password_edit = (RECT){left, y, left + full_w, y + scale(EDIT_HEIGHT)};

  // The buttons split the width; Save takes any rounding remainder so the
  // pair spans it exactly.
  y = l.password_edit.bottom + scale(EDIT_TO_BUTTONS_GAP);
  int32_t gap = scale(BUTTON_GAP);
  int32_t generate_w = (full_w - gap) / 2;
  l.generate_button =
      (RECT){left, y, left + generate_w, y + scale(BUTTON_HEIGHT)};
  l.save_button = (RECT){l.generate_button.right + gap, y, left + full_w,
                         y + scale(BUTTON_HEIGHT)};
  return l;
}

static void on_create(HWND hwnd) {
  Layout l = layout();

  // No native borders: on_paint draws card boxes instead.
  account_edit =
      create_control(hwnd, WC_EDITW, nullptr, WS_TABSTOP | ES_AUTOHSCROLL,
                     edit_control_bounds(l.account_edit, app.ui_font),
                     ID_ACCOUNT_EDIT, app.ui_font);
  set_edit_margins(account_edit, EDIT_MARGIN);
  SendMessageW(account_edit, EM_LIMITTEXT, ACCOUNT_NAME_MAX_CHARS, 0);

  // Not masked, so the person can see what they will save.
  password_font = build_mono_font();
  password_edit =
      create_control(hwnd, WC_EDITW, nullptr, WS_TABSTOP | ES_AUTOHSCROLL,
                     edit_control_bounds(l.password_edit, password_font),
                     ID_PASSWORD_EDIT, password_font);
  set_edit_margins(password_edit, EDIT_MARGIN);

  card_box_set(&account_box, l.account_edit);
  card_box_set(&password_box, l.password_edit);

  generate_button =
      create_control(hwnd, WC_BUTTONW, L"Generate Strong Password", WS_TABSTOP,
                     l.generate_button, ID_GENERATE_BUTTON, app.ui_font);
  // BS_DEFPUSHBUTTON makes Enter in either field press Save.
  save_button = create_control(hwnd, WC_BUTTONW, L"Save Account and Password",
                               WS_TABSTOP | BS_DEFPUSHBUTTON, l.save_button,
                               ID_SAVE_BUTTON, app.ui_font);
}

// save stores the two fields as a new account, then clears them and
// refreshes the lists.
static void save(void) {
  wchar_t buf[ACCOUNT_NAME_MAX_CHARS + 1];
  const wchar_t *account =
      edit_get_trimmed_text(account_edit, buf, (int32_t)_countof(buf));

  Secret password;
  if (!edit_get_secret(password_edit, &password)) {
    show_error(L"This password could not be read.");
    return;
  }
  const wchar_t *err;
  bool saved = db_save_password(account, &password, &err);
  secret_free(&password);
  if (!saved) {
    show_error(err);
    return;
  }

  SetWindowTextW(account_edit, L"");
  SetWindowTextW(password_edit, L"");
  SendMessageW(password_edit, EM_EMPTYUNDOBUFFER, 0, 0);
  list_reload_all();
}

static void generate(void) {
  Secret password;
  if (!secret_generate(&password)) {
    show_error(L"A password could not be generated.");
    return;
  }
  if (!edit_set_secret(password_edit, &password))
    show_error(L"A password could not be generated.");
  secret_free(&password);
}

static void on_paint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  Layout l = layout();
  UINT format = DT_VCENTER | DT_SINGLELINE;
  draw_label(hdc, L"Account", l.account_label, LABEL_COLOR, format);
  draw_label(hdc, L"Password", l.password_label, LABEL_COLOR, format);
  card_box_paint(hdc, &account_box);
  card_box_paint(hdc, &password_box);
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    on_create(hwnd);
    return 0;

  case WM_COMMAND:
    if (HIWORD(wparam) != BN_CLICKED)
      break;
    if (LOWORD(wparam) == ID_GENERATE_BUTTON) {
      generate();
      return 0;
    }
    if (LOWORD(wparam) == ID_SAVE_BUTTON) {
      save();
      return 0;
    }
    break;

  case WM_PAINT:
    on_paint(hwnd);
    return 0;

  case WM_DESTROY:
    DeleteObject(password_font);
    card_box_destroy(&account_box);
    card_box_destroy(&password_box);
    return 0;
  }
  return page_default_proc(hwnd, msg, wparam, lparam);
}

static void update_for_dpi(void) {
  Layout l = layout();
  place_control(account_edit, edit_control_bounds(l.account_edit, app.ui_font),
                app.ui_font);
  set_edit_margins(account_edit, EDIT_MARGIN);

  HFONT old_font = password_font;
  password_font = build_mono_font();
  place_control(password_edit,
                edit_control_bounds(l.password_edit, password_font),
                password_font);
  set_edit_margins(password_edit, EDIT_MARGIN);
  DeleteObject(old_font);

  place_control(generate_button, l.generate_button, app.ui_font);
  place_control(save_button, l.save_button, app.ui_font);
  card_box_set(&account_box, l.account_edit);
  card_box_set(&password_box, l.password_edit);
  InvalidateRect(app.pages[PAGE_CREATE], nullptr, TRUE);
}

static HWND focus_target(void) { return account_edit; }
static HWND default_button(void) { return save_button; }

const PageDef create_page = {
    .title = L"&Create",
    .class_name = L"KeyvokCreatePage",
    .proc = proc,
    .update_for_dpi = update_for_dpi,
    .focus_target = focus_target,
    .default_button = default_button,
};
