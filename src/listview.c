#include "app.h"

#include <stdlib.h>
#include <uxtheme.h>

#include "db.h"

static constexpr int32_t ROW_HEIGHT =
    28; // set through a blank image list; report view has no row height setting
static constexpr int32_t HEADER_HEIGHT =
    28; // the painted "Account" and "Created" strip
static constexpr int32_t SLIVER_PX =
    6; // partial row below the last full one: a hint that the list scrolls
static constexpr int32_t HEADER_INDENT = 8;
static constexpr uint32_t TYPEAHEAD_RESET_MS =
    1000; // a longer pause starts a new search, as in Explorer
static constexpr UINT_PTR SUBCLASS_ID = 1;

static constexpr COLORREF CREATED_COLOR = RGB(0x80, 0x80, 0x80);

// Every list, so list_reload_all can refill them.
static ListPage *lists[3];
static int32_t list_count;

// A brush of alternating pixels, the dotted pattern DrawFocusRect uses,
// which frames the focused row. Created on first use and deleted with the
// last list.
static HBITMAP focus_bitmap;
static HBRUSH focus_brush;

// --- Geometry ------------------------------------------------------------

// box_height returns the card box's height. Rows render one device pixel
// taller than the scaled image list at every scale factor, hence the +1.
static int32_t box_height(const ListPage *list) {
  return list->rows * (scale(ROW_HEIGHT) + 1) + scale(SLIVER_PX) +
         scale(1) * 2 + scale(HEADER_HEIGHT);
}

static RECT box_bounds(const ListPage *list) {
  int32_t x = scale(PAGE_MARGIN), y = scale(PAGE_MARGIN);
  return (RECT){x, y, x + scale(CONTENT_WIDTH), y + box_height(list)};
}

int32_t list_box_bottom(const ListPage *list) {
  return box_bounds(list).bottom;
}

// listview_bounds is inside the border, below the header strip.
static RECT listview_bounds(const ListPage *list) {
  RECT box = box_bounds(list);
  int32_t t = scale(1);
  return (RECT){box.left + t, box.top + t + scale(HEADER_HEIGHT), box.right - t,
                box.bottom - t};
}

// account_column_width is fixed; Created takes the rest of list_w, after
// room for the vertical scrollbar, which is always reserved so that no
// horizontal scrollbar appears when the list fills up.
static int32_t account_column_width(void) { return scale(ACCOUNT_FIELD_WIDTH); }

static int32_t created_column_width(int32_t list_w) {
  return list_w - GetSystemMetricsForDpi(SM_CXVSCROLL, app.dpi) -
         account_column_width();
}

static HIMAGELIST create_row_height_images(void) {
  return ImageList_Create(1, scale(ROW_HEIGHT), 0, 1, 1);
}

// --- Focus and selection -------------------------------------------------

// set_focused_row moves keyboard focus from row current (-1 for none) to
// row next and scrolls it into view. Selection is unchanged.
static void set_focused_row(HWND lv, int32_t current, int32_t next) {
  if (current >= 0 && current != next)
    ListView_SetItemState(lv, current, 0, LVIS_FOCUSED);
  ListView_SetItemState(lv, next, LVIS_FOCUSED, LVIS_FOCUSED);
  InvalidateRect(lv, nullptr, TRUE);
  ListView_EnsureVisible(lv, next, FALSE);
}

static void move_focus(HWND lv, WPARAM key) {
  int32_t count = ListView_GetItemCount(lv);
  if (count <= 0)
    return;

  int32_t current = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
  int32_t page = max_i32(ListView_GetCountPerPage(lv), 1);
  int32_t next = max_i32(current, 0);
  switch (key) {
  case VK_UP:
    next -= 1;
    break;
  case VK_DOWN:
    next += 1;
    break;
  case VK_PRIOR:
    next -= page;
    break;
  case VK_NEXT:
    next += page;
    break;
  case VK_HOME:
    next = 0;
    break;
  case VK_END:
    next = count - 1;
    break;
  default:
    break;
  }
  set_focused_row(lv, current, clamp_i32(next, 0, count - 1));
}

// typeahead adds ch to the search text, or starts a new search after a
// pause, and focuses the next row whose account starts with the text,
// wrapping around. Each search starts from the row focused when it began,
// not the last match, because LVM_FINDITEM skips its start row and so
// would pass over a row that still matches.
static void typeahead(HWND lv, Typeahead *ta, wchar_t ch) {
  uint32_t now = GetTickCount();
  int32_t current = ListView_GetNextItem(lv, -1, LVNI_FOCUSED);
  if (now - ta->last_tick > TYPEAHEAD_RESET_MS ||
      ta->len >= _countof(ta->text) - 1) {
    ta->len = 0;
    ta->anchor = current;
  }
  ta->last_tick = now;
  ta->text[ta->len++] = ch;
  ta->text[ta->len] = L'\0';

  LVFINDINFOW find = {.flags = LVFI_STRING | LVFI_PARTIAL | LVFI_WRAP,
                      .psz = ta->text};
  int32_t found = ListView_FindItem(lv, max_i32(ta->anchor - 1, -1),
                                    &find); // -1: search the anchor too
  if (found >= 0)
    set_focused_row(lv, current, found);
}

void list_select_focused_row(const ListPage *list) {
  int32_t index = ListView_GetNextItem(list->listview, -1, LVNI_FOCUSED);
  if (index >= 0)
    ListView_SetItemState(list->listview, index, LVIS_SELECTED, LVIS_SELECTED);
}

void list_clear_selection(const ListPage *list) {
  int32_t index = ListView_GetNextItem(list->listview, -1, LVNI_SELECTED);
  if (index < 0)
    return;
  ListView_SetItemState(list->listview, index, 0, LVIS_SELECTED);
  InvalidateRect(list->listview, nullptr, TRUE);
}

bool list_selected_id(const ListPage *list, int64_t *id) {
  int32_t index = ListView_GetNextItem(list->listview, -1, LVNI_SELECTED);
  if (index < 0)
    return false;
  LVITEMW item = {.mask = LVIF_PARAM, .iItem = index};
  ListView_GetItem(list->listview, &item);
  *id = (int64_t)item.lParam;
  return true;
}

bool list_selected_name(const ListPage *list, wchar_t *buf, int32_t cap) {
  int32_t index = ListView_GetNextItem(list->listview, -1, LVNI_SELECTED);
  if (index < 0)
    return false;
  ListView_GetItemText(list->listview, index, 0, buf, cap);
  return true;
}

bool list_row_became_selected(const ListPage *list, LPARAM lparam) {
  const NMHDR *hdr = (const NMHDR *)lparam;
  if (hdr->idFrom != list->control_id || hdr->code != (UINT)LVN_ITEMCHANGED)
    return false;
  const NMLISTVIEW *change = (const NMLISTVIEW *)lparam;
  return (change->uChanged & LVIF_STATE) &&
         (change->uNewState & LVIS_SELECTED) &&
         !(change->uOldState & LVIS_SELECTED);
}

bool list_page_on_key(const ListPage *list, const MSG *msg) {
  if (msg->message != WM_KEYDOWN || msg->wParam != VK_RETURN ||
      GetFocus() != list->listview)
    return false;
  list_select_focused_row(list);
  return true;
}

// subclass_proc makes arrow keys and type-ahead move the focused row
// without selecting it. Space selects the focused row, as Enter does
// through list_page_on_key.
static LRESULT CALLBACK subclass_proc(HWND lv, UINT msg, WPARAM wparam,
                                      LPARAM lparam, UINT_PTR id,
                                      DWORD_PTR ref) {
  ListPage *list = (ListPage *)ref;
  switch (msg) {
  case WM_KEYDOWN:
    switch (wparam) {
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_HOME:
    case VK_END:
      move_focus(lv, wparam);
      return 0;
    case VK_SPACE:
      list_select_focused_row(list);
      return 0;
    default:
      break;
    }
    break;

  case WM_CHAR:
    // Every character is consumed, because the control's own handling
    // selects rows as you type.
    if (wparam >= 0x20 && wparam != 0x7F)
      typeahead(lv, &list->typeahead, (wchar_t)wparam);
    return 0;

  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    // The focused-row frame is drawn only while the list has focus.
    InvalidateRect(lv, nullptr, TRUE);
    break;

  case WM_NCDESTROY:
    RemoveWindowSubclass(lv, subclass_proc, id);
    break;
  }
  return DefSubclassProc(lv, msg, wparam, lparam);
}

// --- Rows ----------------------------------------------------------------

static void add_row(void *ctx, const Account *account) {
  HWND lv = ctx;
  LVITEMW item = {
      .mask = LVIF_TEXT | LVIF_PARAM,
      .iItem = ListView_GetItemCount(lv),
      .pszText = (wchar_t *)account->name, // copied by the control
      .lParam = (LPARAM)account->id,
  };
  int32_t index = ListView_InsertItem(lv, &item);
  ListView_SetItemText(lv, index, 1, (wchar_t *)account->created);
}

// load_accounts refills the list and gives row 0 keyboard focus (not
// selection), so the arrow keys work at once.
static void load_accounts(ListPage *list) {
  ListView_DeleteAllItems(list->listview);
  if (!db_each_account(add_row, list->listview)) {
    ListView_DeleteAllItems(
        list->listview); // a partial list would hide accounts
    return;
  }
  if (ListView_GetItemCount(list->listview) > 0)
    ListView_SetItemState(list->listview, 0, LVIS_FOCUSED, LVIS_FOCUSED);
}

void list_reload_all(void) {
  for (int32_t i = 0; i < list_count; i++)
    load_accounts(lists[i]);
}

// --- Drawing -------------------------------------------------------------

static HBRUSH focus_pattern_brush(void) {
  if (!focus_brush) {
    // 8x8 monochrome bitmap. Scanlines are WORD aligned, so each row is
    // a uint16_t with the pattern in its low byte. Alternating 0xAA and
    // 0x55 makes any one-pixel line through it a strict on/off dash.
    static const uint16_t pattern[8] = {0xAA, 0x55, 0xAA, 0x55,
                                        0xAA, 0x55, 0xAA, 0x55};
    focus_bitmap = CreateBitmap(8, 8, 1, 1, pattern);
    focus_brush = CreatePatternBrush(focus_bitmap);
  }
  return focus_brush;
}

// custom_draw greys the Created column and frames the row that has
// keyboard focus but is not selected, which the Explorer theme does not
// mark. The Explorer theme draws the selected row itself. The frame is
// drawn in the control's own paint pass, so it is part of the double
// buffered image.
static LRESULT custom_draw(HWND lv, NMLVCUSTOMDRAW *draw) {
  switch (draw->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;

  case CDDS_ITEMPREPAINT: {
    bool focused_unselected = GetFocus() == lv &&
                              (draw->nmcd.uItemState & CDIS_FOCUS) &&
                              !(draw->nmcd.uItemState & CDIS_SELECTED);
    // Suppress the native focus cue, which Windows turns on after any
    // Tab press. The frame drawn at postpaint replaces it.
    draw->nmcd.uItemState &= ~(UINT)CDIS_FOCUS;
    return focused_unselected ? CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT
                              : CDRF_NOTIFYSUBITEMDRAW;
  }

  case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
    if (draw->iSubItem == 1)
      draw->clrText = CREATED_COLOR;
    return CDRF_DODEFAULT;

  case CDDS_ITEMPOSTPAINT: {
    // nmcd.rc covers one subitem, so ask for the whole row. Postpaint
    // can run more than once per row; it draws the same frame each time.
    RECT row;
    ListView_GetItemRect(lv, (int32_t)draw->nmcd.dwItemSpec, &row, LVIR_BOUNDS);
    HDC hdc = draw->nmcd.hdc;
    int32_t old_mode = SetBkMode(hdc, TRANSPARENT);
    COLORREF old_color = SetTextColor(hdc, RGB(0, 0, 0));
    // Anchor the pattern to the row's corner so repeated draws match.
    POINT old_origin;
    SetBrushOrgEx(hdc, row.left, row.top, &old_origin);
    FrameRect(hdc, &row, focus_pattern_brush());
    SetBrushOrgEx(hdc, old_origin.x, old_origin.y, nullptr);
    SetTextColor(hdc, old_color);
    SetBkMode(hdc, old_mode);
    return CDRF_DODEFAULT;
  }

  default:
    return CDRF_DODEFAULT;
  }
}

bool list_page_on_notify(const ListPage *list, LPARAM lparam, LRESULT *result) {
  const NMHDR *hdr = (const NMHDR *)lparam;
  if (hdr->idFrom != list->control_id || hdr->code != (UINT)NM_CUSTOMDRAW)
    return false;
  *result = custom_draw(list->listview, (NMLVCUSTOMDRAW *)lparam);
  return true;
}

// list_page_paint draws the card box and the header that replaces the
// ListView's native one.
void list_page_paint(const ListPage *list, HDC hdc) {
  card_box_paint(hdc, &list->box);

  RECT lv = listview_bounds(list);
  int32_t header_top = lv.top - scale(HEADER_HEIGHT);
  int32_t account_w = account_column_width();
  UINT format = DT_VCENTER | DT_SINGLELINE;

  RECT account = {lv.left + scale(HEADER_INDENT), header_top,
                  lv.left + account_w, lv.top};
  draw_label(hdc, L"Account", account, LABEL_COLOR, format);
  RECT created = {lv.left + account_w + scale(HEADER_INDENT), header_top,
                  lv.right, lv.top};
  draw_label(hdc, L"Created", created, LABEL_COLOR, format);

  RECT divider = {lv.left, lv.top - scale(1), lv.right, lv.top};
  FillRect(hdc, &divider, app.card_border_brush);
}

// --- Lifetime ------------------------------------------------------------

static void add_column(HWND lv, int32_t index, const wchar_t *title,
                       int32_t width) {
  LVCOLUMNW column = {
      .mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT,
      .fmt = LVCFMT_LEFT,
      .cx = width,
      .pszText = (wchar_t *)title,
  };
  ListView_InsertColumn(lv, index, &column);
}

void list_page_create(ListPage *list, HWND parent) {
  // LVS_SHAREIMAGELISTS: the page owns the row-height image list, so the
  // control must not destroy it.
  RECT bounds = listview_bounds(list);
  list->listview = create_control(parent, WC_LISTVIEWW, nullptr,
                                  WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                      LVS_SINGLESEL | LVS_SHAREIMAGELISTS,
                                  bounds, list->control_id, app.ui_font);
  SetWindowTheme(list->listview, L"Explorer", nullptr);
  // No gridlines: they clash with the Explorer theme's selection border.
  ListView_SetExtendedListViewStyle(list->listview,
                                    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  SetWindowSubclass(list->listview, subclass_proc, SUBCLASS_ID,
                    (DWORD_PTR)list);

  add_column(list->listview, 0, L"Account", account_column_width());
  add_column(list->listview, 1, L"Created",
             created_column_width(rect_width(bounds)));
  list->row_height_images = create_row_height_images();
  ListView_SetImageList(list->listview, list->row_height_images, LVSIL_SMALL);
  card_box_set(&list->box, box_bounds(list));

  ensure(list_count < (int32_t)_countof(lists), L"Too many account lists.");
  lists[list_count++] = list;
  load_accounts(list);
}

void list_page_update_for_dpi(ListPage *list) {
  RECT bounds = listview_bounds(list);
  place_control(list->listview, bounds, app.ui_font);
  ListView_SetColumnWidth(list->listview, 0, account_column_width());
  ListView_SetColumnWidth(list->listview, 1,
                          created_column_width(rect_width(bounds)));
  card_box_set(&list->box, box_bounds(list));

  HIMAGELIST old_images = list->row_height_images;
  list->row_height_images = create_row_height_images();
  ListView_SetImageList(list->listview, list->row_height_images, LVSIL_SMALL);
  ImageList_Destroy(old_images);

  InvalidateRect(list->listview, nullptr, TRUE);
  InvalidateRect(GetParent(list->listview), nullptr,
                 TRUE); // the header is painted by the page
}

void list_page_destroy(ListPage *list) {
  ImageList_Destroy(list->row_height_images);
  list->row_height_images = nullptr;
  card_box_destroy(&list->box);

  for (int32_t i = 0; i < list_count; i++) {
    if (lists[i] == list) {
      lists[i] = lists[--list_count];
      break;
    }
  }
  if (list_count == 0 && focus_brush) {
    DeleteObject(focus_brush);
    DeleteObject(focus_bitmap);
    focus_brush = nullptr;
    focus_bitmap = nullptr;
  }
}
