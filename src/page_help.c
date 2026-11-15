// Help page: a topic name on the left and its text on the right, one row
// per topic. The page's only controls are the Backup topic's buttons, each
// row under the paragraph it belongs to: Copy Passwords File, then Export
// as CSV and Import CSV. The page scrolls, and paints into a back buffer so
// a scrollbar drag does not flicker.

#include "app.h"

#include <objbase.h> // CoTaskMemFree, for the paths the file dialogs return
#include <stdlib.h>
#include <wchar.h>

#include "db.h"

static constexpr int32_t LABEL_COLUMN_WIDTH = 100; // fits every topic name
static constexpr int32_t LABEL_TO_TEXT_GAP = 16;
static constexpr int32_t LABEL_HEIGHT = 24; // topic names never wrap
static constexpr int32_t ROW_GAP = 20;
static constexpr int32_t PARAGRAPH_GAP = 10; // between paragraphs of one topic
static constexpr int32_t BUTTON_GAP = 16; // between a paragraph and its buttons
static constexpr int32_t BUTTON_ROW_GAP = 12; // between buttons in one row
static constexpr int32_t BUTTON_HEIGHT =
    34; // same as the Create and Relabel buttons
static constexpr int32_t SCROLL_LINE = 24; // one line step
static constexpr int32_t WHEEL_LINES =
    3; // per wheel notch, the usual system default
static constexpr int32_t MEASURE_HEIGHT =
    4000; // room for DT_CALCRECT to report a paragraph's real height
static constexpr size_t PARAGRAPH_SLOTS =
    4; // most paragraphs one topic can have

static constexpr uint16_t ID_COPY_BUTTON = 2029;
static constexpr uint16_t ID_EXPORT_BUTTON = 2030;
static constexpr uint16_t ID_IMPORT_BUTTON = 2031;

// A row of buttons under a paragraph.
typedef enum ButtonRow : uint8_t {
  NO_BUTTONS,
  COPY_ROW,
  CSV_ROW,
} ButtonRow;

// Each row is laid out left to right in this order.
typedef struct ButtonDef {
  const wchar_t *text;
  uint16_t id;
  int32_t width;
  ButtonRow row;
} ButtonDef;

static const ButtonDef BUTTONS[] = {
    {L"Copy Passwords File", ID_COPY_BUTTON, 188, COPY_ROW},
    // The two CSV buttons and the gap between them fill the text column.
    {L"Export as CSV", ID_EXPORT_BUTTON, 174, CSV_ROW},
    {L"Import CSV", ID_IMPORT_BUTTON, 174, CSV_ROW},
};

// "\r\n" forces a line break inside a paragraph; DrawTextW still wraps
// each line.
typedef struct Topic {
  const wchar_t *name;
  const wchar_t
      *paragraphs[PARAGRAPH_SLOTS]; // ends at the first nullptr, or when full
  ButtonRow buttons_after[PARAGRAPH_SLOTS]; // the row under each paragraph
} Topic;

// In nav order, then Security and Backup.
static const Topic TOPICS[] = {
    {.name = L"Select",
     .paragraphs =
         {
             L"Select an account to copy its password to the clipboard. Hold "
             L"Alt while selecting to generate a QR code "
             L"that can be used to copy the password to your phone. The "
             L"clipboard or QR code will be cleared "
             L"automatically after 30 seconds, or immediately by pressing "
             L"Escape.",
         }},
    {.name = L"Create",
     .paragraphs =
         {
             L"Enter and save an account label and password. Account labels "
             L"can include usernames. You can use the "
             L"Generate Strong Password button to create a high-entropy, "
             L"143.8-bit password.",
             L"If a website rejects a generated password, simply create "
             L"another and manually edit it to meet the "
             L"site's specific requirements before saving.",
         }},
    {.name = L"Relabel",
     .paragraphs =
         {
             L"Select an account and enter a new label. This only changes the "
             L"label, not the password.",
         }},
    {.name = L"Delete",
     .paragraphs =
         {
             L"Select an account to delete it. You will be prompted with a "
             L"confirmation box; once deleted, the "
             L"password cannot be recovered.",
         }},
    {.name = L"Security",
     .paragraphs =
         {
             L"Passwords are encrypted using Windows Data Protection and "
             L"stored in a local file. They can only be "
             L"decrypted on your computer by someone logged in as you. Some "
             L"forms of malware can act as you, so keep "
             L"your security software up to date.",
         }},
    {.name = L"Backup",
     .paragraphs =
         {
             L"The passwords file is in your AppData folder. Use the Copy "
             L"Passwords File button below for a one-\r\n"
             L"time copy, or, if you run your own local backup routine, point "
             L"it directly at:\r\n"
             L"    C:\\Users\\<username>\\AppData\\Local\r\n"
             L"    \\Programs\\Keyvok\\keyvok.db\r\n"
             L"The AppData folder is hidden by default.",
             L"A backup of this file is only usable on your computer by "
             L"someone logged in as you. It will not work "
             L"after reinstalling Windows or replacing the motherboard.",
             L"To keep a portable copy of your passwords, export them as an "
             L"unencrypted CSV file, ideally to a flash "
             L"drive kept in a safe place. On a new computer, use Import CSV "
             L"to add the passwords from an exported "
             L"file.",
         },
     .buttons_after = {[1] = COPY_ROW, [2] = CSV_ROW}},
};

static HWND buttons[_countof(BUTTONS)];

// The window's scrollbar holds the scroll position; scroll_y mirrors it so
// layout need not query the scrollbar.
static int32_t scroll_y;

// Where everything goes at scroll position 0. Paragraph heights depend on
// word wrapping, so they are measured with DT_CALCRECT here, once per size
// or DPI change; painting and scrolling only shift these rects.
typedef struct Layout {
  RECT labels[_countof(TOPICS)];
  RECT paragraphs[_countof(TOPICS)]
                 [PARAGRAPH_SLOTS]; // empty past a topic's last paragraph
  RECT buttons[_countof(BUTTONS)];
  int32_t content_height; // height of everything on the page
} Layout;

static Layout top_layout;

// Reused across paints; rebuilt when the client size changes.
typedef struct BackBuffer {
  HDC dc;
  HBITMAP bitmap;
  HGDIOBJ stock_bitmap; // reselected before the bitmap is deleted
  int32_t width, height;
} BackBuffer;

static BackBuffer back_buffer;

// measure lays the page out at scroll position 0, measuring the paragraphs
// on the screen DC with the UI font.
static Layout measure(void) {
  int32_t label_x = scale(PAGE_MARGIN);
  int32_t label_w = scale(LABEL_COLUMN_WIDTH);
  int32_t text_x = label_x + label_w + scale(LABEL_TO_TEXT_GAP);
  int32_t text_w = scale(CONTENT_WIDTH) - label_w - scale(LABEL_TO_TEXT_GAP);

  HDC hdc = GetDC(nullptr);
  HGDIOBJ old_font = SelectObject(hdc, app.ui_font);

  Layout l = {};
  int32_t y = scale(PAGE_MARGIN);
  int32_t last_bottom = y;
  for (size_t i = 0; i < _countof(TOPICS); i++) {
    int32_t top = y;
    l.labels[i] =
        (RECT){label_x, top, label_x + label_w, top + scale(LABEL_HEIGHT)};

    int32_t paragraph_y = top;
    int32_t text_bottom = top;
    for (size_t j = 0; j < PARAGRAPH_SLOTS && TOPICS[i].paragraphs[j]; j++) {
      RECT r = {text_x, paragraph_y, text_x + text_w,
                paragraph_y + scale(MEASURE_HEIGHT)};
      DrawTextW(hdc, TOPICS[i].paragraphs[j], -1, &r,
                DT_WORDBREAK | DT_CALCRECT);
      l.paragraphs[i][j] = r;
      text_bottom = r.bottom;

      ButtonRow row = TOPICS[i].buttons_after[j];
      if (row != NO_BUTTONS) {
        int32_t button_top = text_bottom + scale(BUTTON_GAP);
        int32_t button_x = text_x;
        for (size_t k = 0; k < _countof(BUTTONS); k++) {
          if (BUTTONS[k].row != row)
            continue;
          l.buttons[k] =
              (RECT){button_x, button_top, button_x + scale(BUTTONS[k].width),
                     button_top + scale(BUTTON_HEIGHT)};
          button_x = l.buttons[k].right + scale(BUTTON_ROW_GAP);
        }
        text_bottom = button_top + scale(BUTTON_HEIGHT);
        // Farther from the next paragraph than from its own, so
        // the row reads as part of the paragraph above it.
        paragraph_y = text_bottom + scale(ROW_GAP);
      } else {
        paragraph_y = text_bottom + scale(PARAGRAPH_GAP);
      }
    }

    last_bottom = max_i32(text_bottom, l.labels[i].bottom);
    y = last_bottom + scale(ROW_GAP);
  }
  SelectObject(hdc, old_font);
  ReleaseDC(nullptr, hdc);

  l.content_height = last_bottom + scale(PAGE_MARGIN);
  return l;
}

static RECT shifted_up(RECT r, int32_t dy) {
  return (RECT){r.left, r.top - dy, r.right, r.bottom - dy};
}

// draw paints the labels and paragraphs that overlap dirty, in client
// coordinates, at the current scroll position. The buttons draw themselves.
static void draw(HDC hdc, RECT dirty) {
  HGDIOBJ old_font = SelectObject(hdc, app.ui_font);
  SetBkMode(hdc, TRANSPARENT);
  RECT overlap;
  for (size_t i = 0; i < _countof(TOPICS); i++) {
    RECT label = shifted_up(top_layout.labels[i], scroll_y);
    if (IntersectRect(&overlap, &label, &dirty)) {
      SetTextColor(hdc, TEXT_COLOR);
      DrawTextW(hdc, TOPICS[i].name, -1, &label, DT_SINGLELINE);
    }
    for (size_t j = 0; j < PARAGRAPH_SLOTS && TOPICS[i].paragraphs[j]; j++) {
      RECT r = shifted_up(top_layout.paragraphs[i][j], scroll_y);
      if (IntersectRect(&overlap, &r, &dirty)) {
        SetTextColor(hdc, LABEL_COLOR);
        DrawTextW(hdc, TOPICS[i].paragraphs[j], -1, &r, DT_WORDBREAK);
      }
    }
  }
  SelectObject(hdc, old_font);
}

static void place_buttons(void) {
  for (size_t k = 0; k < _countof(BUTTONS); k++)
    place_control(buttons[k], shifted_up(top_layout.buttons[k], scroll_y),
                  app.ui_font);
}

// update_scroll_range measures the content, sets the scrollbar's range
// and page size, and clamps the position if the page grew.
static void update_scroll_range(HWND hwnd) {
  top_layout = measure();

  RECT client;
  GetClientRect(hwnd, &client);
  int32_t page_h = max_i32(rect_height(client), 0);

  SCROLLINFO si = {
      .cbSize = sizeof si,
      .fMask = SIF_RANGE | SIF_PAGE,
      .nMax = max_i32(top_layout.content_height - 1, 0),
      .nPage = (UINT)page_h,
  };
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

  int32_t clamped =
      clamp_i32(scroll_y, 0, max_i32(top_layout.content_height - page_h, 0));
  if (clamped != scroll_y) {
    scroll_y = clamped;
    si.fMask = SIF_POS;
    si.nPos = clamped;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  }

  place_buttons();
  InvalidateRect(hwnd, nullptr, FALSE);
}

// scroll_to scrolls to requested, clamped to the scrollbar's range, and
// moves the buttons and repaints if the position changed.
static void scroll_to(HWND hwnd, int32_t requested) {
  SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL};
  GetScrollInfo(hwnd, SB_VERT, &si);
  int32_t max_pos = max_i32(si.nMax - (int32_t)si.nPage + 1, si.nMin);
  int32_t pos = clamp_i32(requested, si.nMin, max_pos);
  if (pos == si.nPos)
    return;

  scroll_y = pos;
  si.fMask = SIF_POS;
  si.nPos = pos;
  SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
  place_buttons();
  InvalidateRect(hwnd, nullptr, FALSE);
}

static void on_vscroll(HWND hwnd, WPARAM wparam) {
  SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL};
  GetScrollInfo(hwnd, SB_VERT, &si);
  int32_t line = scale(SCROLL_LINE);
  switch (LOWORD(wparam)) {
  case SB_LINEUP:
    scroll_to(hwnd, si.nPos - line);
    break;
  case SB_LINEDOWN:
    scroll_to(hwnd, si.nPos + line);
    break;
  case SB_PAGEUP:
    scroll_to(hwnd, si.nPos - (int32_t)si.nPage);
    break;
  case SB_PAGEDOWN:
    scroll_to(hwnd, si.nPos + (int32_t)si.nPage);
    break;
  case SB_THUMBTRACK:
  case SB_THUMBPOSITION:
    scroll_to(hwnd, si.nTrackPos);
    break;
  case SB_TOP:
    scroll_to(hwnd, si.nMin);
    break;
  case SB_BOTTOM:
    scroll_to(hwnd, si.nMax);
    break;
  default:
    break;
  }
}

// on_mousewheel scrolls by the wheel rotation, a signed multiple of
// WHEEL_DELTA; negative scrolls down.
static void on_mousewheel(HWND hwnd, WPARAM wparam) {
  int32_t delta = GET_WHEEL_DELTA_WPARAM(wparam);
  scroll_to(hwnd,
            scroll_y - delta * scale(SCROLL_LINE) * WHEEL_LINES / WHEEL_DELTA);
}

// on_key scrolls for Up, Down, Page Up, Page Down, Home and End, whichever
// control on the page has focus.
static bool on_key(const MSG *msg) {
  if (msg->message != WM_KEYDOWN)
    return false;
  HWND hwnd = app.pages[PAGE_HELP];
  SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL};
  GetScrollInfo(hwnd, SB_VERT, &si);
  int32_t line = scale(SCROLL_LINE);
  switch (msg->wParam) {
  case VK_UP:
    scroll_to(hwnd, si.nPos - line);
    return true;
  case VK_DOWN:
    scroll_to(hwnd, si.nPos + line);
    return true;
  case VK_PRIOR:
    scroll_to(hwnd, si.nPos - (int32_t)si.nPage);
    return true;
  case VK_NEXT:
    scroll_to(hwnd, si.nPos + (int32_t)si.nPage);
    return true;
  case VK_HOME:
    scroll_to(hwnd, si.nMin);
    return true;
  case VK_END:
    scroll_to(hwnd, si.nMax);
    return true;
  default:
    return false;
  }
}

static void destroy_back_buffer(void) {
  if (!back_buffer.dc)
    return;
  SelectObject(back_buffer.dc, back_buffer.stock_bitmap);
  DeleteObject(back_buffer.bitmap);
  DeleteDC(back_buffer.dc);
  back_buffer = (BackBuffer){};
}

// on_paint redraws the invalid part of the page in the back buffer and
// copies it to the screen in one BitBlt, so a scrollbar drag does not
// flicker.
static void on_paint(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);

  RECT client;
  GetClientRect(hwnd, &client);
  int32_t w = rect_width(client), h = rect_height(client);
  RECT dirty = ps.rcPaint;
  if (!back_buffer.dc || back_buffer.width != w || back_buffer.height != h) {
    // A new buffer holds nothing yet, so all of it is drawn.
    destroy_back_buffer();
    back_buffer.dc = CreateCompatibleDC(hdc);
    back_buffer.bitmap = CreateCompatibleBitmap(hdc, w, h);
    back_buffer.stock_bitmap = SelectObject(back_buffer.dc, back_buffer.bitmap);
    back_buffer.width = w;
    back_buffer.height = h;
    dirty = client;
  }

  // Clipped to dirty: text that crosses its edge would otherwise be drawn
  // again over its own pixels outside it, darkening the anti-aliasing.
  int32_t saved = SaveDC(back_buffer.dc);
  IntersectClipRect(back_buffer.dc, dirty.left, dirty.top, dirty.right,
                    dirty.bottom);
  FillRect(back_buffer.dc, &dirty, app.page_brush);
  draw(back_buffer.dc, dirty);
  RestoreDC(back_buffer.dc, saved);
  BitBlt(hdc, dirty.left, dirty.top, rect_width(dirty), rect_height(dirty),
         back_buffer.dc, dirty.left, dirty.top, SRCCOPY);
  EndPaint(hwnd, &ps);
}

// copy_database backs the database up to keyvok.db in a folder the person
// picks. The name is fixed so restoring means putting the file back. The
// copy stays encrypted.
static void copy_database(void) {
  PWSTR folder;
  if (!pick_folder_dialog(&folder))
    return;
  static const wchar_t NAME[] = L"\\keyvok.db";
  size_t len = wcslen(folder) + _countof(NAME);
  wchar_t *dest = malloc(len * sizeof *dest);
  if (dest)
    swprintf(dest, len, L"%ls%ls", folder, NAME);
  CoTaskMemFree(folder);
  if (!dest) {
    show_error(L"The backup file could not be written.");
    return;
  }

  bool proceed =
      GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES ||
      MessageBoxW(app.main_window,
                  L"A keyvok.db file already exists there. Overwrite it?",
                  L"Keyvok", MB_YESNO | MB_ICONWARNING) == IDYES;
  const wchar_t *err;
  if (proceed && !db_backup_to(dest, &err))
    show_error(err);
  free(dest);
}

static void export_csv(void) {
  PWSTR path;
  if (!save_file_dialog(L"passwords.csv", &path))
    return;
  const wchar_t *err;
  if (!db_export_csv(path, &err))
    show_error(err);
  CoTaskMemFree(path);
}

// import_csv adds the accounts in a CSV file the person picks and says
// how many were added and skipped.
static void import_csv(void) {
  PWSTR path;
  if (!open_file_dialog(&path))
    return;
  ImportStats stats;
  const wchar_t *err;
  bool ok = db_import_csv(path, &stats, &err);
  CoTaskMemFree(path);

  wchar_t msg[256];
  if (!ok) {
    if (stats.line > 0) {
      swprintf(msg, _countof(msg), L"Line %d: %ls\n\nNothing was imported.",
               stats.line, err);
      err = msg;
    }
    show_error(err);
    return;
  }

  list_reload_all();
  int32_t n = swprintf(msg, _countof(msg), L"Imported %d %ls.", stats.added,
                       stats.added == 1 ? L"account" : L"accounts");
  if (stats.skipped > 0 && n > 0) {
    swprintf(msg + n, _countof(msg) - (size_t)n,
             L"\n\nSkipped %d that already %ls, keeping %ls password.",
             stats.skipped, stats.skipped == 1 ? L"exists" : L"exist",
             stats.skipped == 1 ? L"its saved" : L"their saved");
  }
  MessageBoxW(app.main_window, msg, L"Keyvok", MB_OK | MB_ICONINFORMATION);
}

static void on_create(HWND hwnd) {
  // The scroll range is set on the first WM_SIZE, once the page's real
  // size is known.
  top_layout = measure();
  for (size_t k = 0; k < _countof(BUTTONS); k++) {
    buttons[k] =
        create_control(hwnd, WC_BUTTONW, BUTTONS[k].text, WS_TABSTOP,
                       top_layout.buttons[k], BUTTONS[k].id, app.ui_font);
  }
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam,
                             LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    on_create(hwnd);
    return 0;

  case WM_SIZE:
    update_scroll_range(hwnd);
    return 0;

  case WM_VSCROLL:
    on_vscroll(hwnd, wparam);
    return 0;

  case WM_MOUSEWHEEL:
    on_mousewheel(hwnd, wparam);
    return 0;

  case WM_COMMAND:
    if (HIWORD(wparam) != BN_CLICKED)
      break;
    if (LOWORD(wparam) == ID_COPY_BUTTON) {
      copy_database();
      return 0;
    }
    if (LOWORD(wparam) == ID_EXPORT_BUTTON) {
      export_csv();
      return 0;
    }
    if (LOWORD(wparam) == ID_IMPORT_BUTTON) {
      import_csv();
      return 0;
    }
    break;

  case WM_ERASEBKGND:
    return 1; // on_paint fills the whole back buffer

  case WM_PAINT:
    on_paint(hwnd);
    return 0;

  case WM_DESTROY:
    destroy_back_buffer();
    return 0;
  }
  return page_default_proc(hwnd, msg, wparam, lparam);
}

// update_for_dpi measures again, which also moves the buttons and
// applies the new font to them.
static void update_for_dpi(void) { update_scroll_range(app.pages[PAGE_HELP]); }

const PageDef help_page = {
    .title = L"&Help",
    .class_name = L"KeyvokHelpPage",
    .style = WS_VSCROLL,
    .proc = proc,
    .update_for_dpi = update_for_dpi,
    .on_key = on_key,
};
