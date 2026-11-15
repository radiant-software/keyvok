// Small UI helpers shared by the pages: control creation, labels, card
// boxes and edit fields.

#include "app.h"

#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

HWND create_control(HWND parent, const wchar_t *class_name, const wchar_t *text,
                    DWORD style, RECT bounds, uint16_t id, HFONT font) {
  HWND control = CreateWindowExW(
      0, class_name, text, WS_CHILD | WS_VISIBLE | style, bounds.left,
      bounds.top, rect_width(bounds), rect_height(bounds), parent,
      (HMENU)(uintptr_t)id, GetModuleHandleW(nullptr), nullptr);
  ensure(control != nullptr, L"Control creation failed.");
  set_font(control, font);
  return control;
}

void place_control(HWND control, RECT bounds, HFONT font) {
  MoveWindow(control, bounds.left, bounds.top, rect_width(bounds),
             rect_height(bounds), TRUE);
  set_font(control, font);
}

void draw_label(HDC hdc, const wchar_t *text, RECT bounds, COLORREF color,
                UINT format) {
  HGDIOBJ old_font = SelectObject(hdc, app.ui_font);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, color);
  DrawTextW(hdc, text, -1, &bounds, format);
  SelectObject(hdc, old_font);
}

// --- Card boxes ----------------------------------------------------------

// Corner cut in device pixels. Not scaled.
static constexpr int32_t CARD_CHAMFER = 2;

// chamfered_region returns an octagon: r with a 45-degree cut of c pixels
// at each corner. GDI's rounded regions are not symmetric at small radii,
// so straight cuts are used instead.
static HRGN chamfered_region(RECT r, int32_t c) {
  const POINT points[] = {
      {r.left, r.top + c},    {r.left + c, r.top},     {r.right - c, r.top},
      {r.right, r.top + c},   {r.right, r.bottom - c}, {r.right - c, r.bottom},
      {r.left + c, r.bottom}, {r.left, r.bottom - c},
  };
  return CreatePolygonRgn(points, (int32_t)_countof(points), ALTERNATE);
}

void card_box_set(CardBox *box, RECT bounds) {
  card_box_destroy(box);
  int32_t t = scale(1); // border thickness
  box->fill = chamfered_region(bounds, CARD_CHAMFER);

  // The border is the box minus a copy inset by its thickness, so a single
  // FillRgn paints it.
  RECT inset = {bounds.left + t, bounds.top + t, bounds.right - t,
                bounds.bottom - t};
  HRGN inner = chamfered_region(inset, max_i32(CARD_CHAMFER - t, 0));
  box->border = CreateRectRgn(0, 0, 0, 0);
  CombineRgn(box->border, box->fill, inner, RGN_DIFF);
  DeleteObject(inner);
}

// Fill first, so the border covers its edge.
void card_box_paint(HDC hdc, const CardBox *box) {
  FillRgn(hdc, box->fill, app.card_fill_brush);
  FillRgn(hdc, box->border, app.card_border_brush);
}

void card_box_destroy(CardBox *box) {
  if (box->fill)
    DeleteObject(box->fill);
  if (box->border)
    DeleteObject(box->border);
  *box = (CardBox){};
}

// --- Edit fields ---------------------------------------------------------

RECT edit_control_bounds(RECT outer, HFONT font) {
  int32_t t = scale(1);
  RECT content = {outer.left + t, outer.top + t, outer.right - t,
                  outer.bottom - t};

  // The screen DC works for text metrics, so the control need not exist yet.
  HDC hdc = GetDC(nullptr);
  HGDIOBJ old_font = SelectObject(hdc, font);
  TEXTMETRICW metrics;
  GetTextMetricsW(hdc, &metrics);
  SelectObject(hdc, old_font);
  ReleaseDC(nullptr, hdc);

  int32_t content_h = rect_height(content);
  int32_t edit_h = min_i32(metrics.tmHeight + scale(4),
                           content_h); // a little room around the text
  int32_t pad_top = max_i32(content_h - edit_h - 1, 0);
  return (RECT){content.left, content.top + pad_top, content.right,
                content.top + pad_top + edit_h};
}

void set_edit_margins(HWND edit, int32_t px) {
  WORD margin = (WORD)scale(px);
  SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
               MAKELONG(margin, margin));
}

// trim removes leading and trailing whitespace from s in place and
// returns the first remaining character.
static wchar_t *trim(wchar_t *s) {
  while (iswspace(*s))
    s++;
  wchar_t *end = s + wcslen(s);
  while (end > s && iswspace(end[-1]))
    end--;
  *end = L'\0';
  return s;
}

wchar_t *edit_get_trimmed_text(HWND edit, wchar_t *buf, int32_t cap) {
  if (GetWindowTextW(edit, buf, cap) <= 0)
    buf[0] = L'\0';
  return trim(buf);
}

bool edit_get_secret(HWND edit, Secret *out) {
  *out = (Secret){};
  int32_t len = GetWindowTextLengthW(edit);
  if (len <= 0)
    return true;

  size_t size = ((size_t)len + 1) * sizeof(wchar_t);
  wchar_t *buf = malloc(size);
  if (!buf)
    return false;
  if (GetWindowTextW(edit, buf, len + 1) <= 0)
    buf[0] = L'\0';
  const wchar_t *text = trim(buf);
  bool ok = secret_from_wide(text, wcslen(text), out);
  wide_secret_free(buf, size);
  return ok;
}

bool edit_set_secret(HWND edit, const Secret *password) {
  size_t size = 0;
  wchar_t *text = secret_to_wide(password, &size);
  if (!text)
    return false;
  SetWindowTextW(edit, text);
  wide_secret_free(text, size);
  return true;
}
