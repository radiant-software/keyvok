// About page: one line per item. Source has a painted label followed by a
// read-only EDIT control holding the value, so only the value can be selected
// and copied.

#include "app.h"
#include "version.h"

#include <wchar.h>

static constexpr int32_t LINE_HEIGHT = 24;

typedef enum Row : int32_t {
  ROW_TITLE,
  ROW_VERSION,
  ROW_SOURCE,
  ROW_COPYRIGHT,
} Row;

// A selectable value. The label's trailing space separates it from the value.
typedef struct Field {
  Row row;
  uint16_t id;
  const wchar_t *label;
  const wchar_t *value;
} Field;

static const Field FIELDS[] = {
    {ROW_SOURCE, 2051, L"Source: ", L"github.com/radiant-software/keyvok"},
};
static constexpr int32_t FIELD_COUNT = sizeof FIELDS / sizeof FIELDS[0];

static HWND value_edits[FIELD_COUNT];

static RECT row_rect(Row row) {
  int32_t top = scale(PAGE_MARGIN) + row * scale(LINE_HEIGHT);
  int32_t left = scale(PAGE_MARGIN);
  return (RECT){left, top, left + scale(CONTENT_WIDTH),
                top + scale(LINE_HEIGHT)};
}

// value_rect returns the row's bounds after its label, measured in the UI
// font on the screen DC.
static RECT value_rect(const Field *field) {
  HDC hdc = GetDC(nullptr);
  HGDIOBJ old_font = SelectObject(hdc, app.ui_font);
  SIZE label;
  GetTextExtentPoint32W(hdc, field->label, (int32_t)wcslen(field->label),
                        &label);
  SelectObject(hdc, old_font);
  ReleaseDC(nullptr, hdc);

  RECT r = row_rect(field->row);
  r.left += label.cx;
  return r;
}

static void on_create(HWND hwnd) {
  for (int32_t i = 0; i < FIELD_COUNT; i++) {
    // Read-only EDIT controls ask for STATIC colors, which
    // page_default_proc answers with the page background.
    value_edits[i] =
        create_control(hwnd, WC_EDITW, FIELDS[i].value,
                       WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                       value_rect(&FIELDS[i]), FIELDS[i].id, app.ui_font);
    set_edit_margins(value_edits[i],
                     0); // the value starts right after its label
  }
}

static void on_paint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  draw_label(hdc, L"Keyvok", row_rect(ROW_TITLE), TEXT_COLOR, DT_SINGLELINE);
  draw_label(hdc, L"Version: " KEYVOK_VERSION_STRING, row_rect(ROW_VERSION),
             TEXT_COLOR, DT_SINGLELINE);
  for (int32_t i = 0; i < FIELD_COUNT; i++)
    draw_label(hdc, FIELDS[i].label, row_rect(FIELDS[i].row), TEXT_COLOR,
               DT_SINGLELINE);
  draw_label(hdc, L"© " KEYVOK_COPYRIGHT_YEAR " " KEYVOK_COMPANY,
             row_rect(ROW_COPYRIGHT), TEXT_COLOR, DT_SINGLELINE);
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    on_create(hwnd);
    return 0;

  case WM_PAINT:
    on_paint(hwnd);
    return 0;
  }
  return page_default_proc(hwnd, msg, wparam, lparam);
}

// update_for_dpi moves each value after its label, measured again in the
// new font.
static void update_for_dpi(void) {
  for (int32_t i = 0; i < FIELD_COUNT; i++)
    place_control(value_edits[i], value_rect(&FIELDS[i]), app.ui_font);
}

const PageDef about_page = {
    .title = L"&About",
    .class_name = L"KeyvokAboutPage",
    .proc = proc,
    .update_for_dpi = update_for_dpi,
};
