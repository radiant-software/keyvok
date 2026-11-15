// Shared declarations for keyvok's UI modules.
//
// The main window has a row of nav buttons; each shows one page, a child
// window filling the area below them. Every page is described by a
// PageDef and has its own window class and procedure.

#pragma once

#include <windows.h>

#include <commctrl.h>
#include <stdint.h>

#include "secret.h"

// --- Pages ---------------------------------------------------------------

typedef enum Page : int32_t {
  PAGE_SELECT,
  PAGE_CREATE,
  PAGE_RELABEL,
  PAGE_DELETE,
  PAGE_HELP,
  PAGE_ABOUT,
} Page;

static constexpr int32_t PAGE_COUNT = PAGE_ABOUT + 1;

// What the shell needs to know about a page. Optional fields are nullptr.
typedef struct PageDef {
  const wchar_t *title; // nav button text; & marks the Alt mnemonic
  const wchar_t *class_name;
  DWORD style;  // extra window styles, such as WS_VSCROLL
  WNDPROC proc; // passes what it does not handle to page_default_proc

  // Optional. Re-lays out the page after a DPI change. app.dpi and
  // app.ui_font are already updated.
  void (*update_for_dpi)(void);

  // Optional. The control focused when the page opens; without one, or
  // when it returns nullptr, the page's nav button gets focus.
  HWND (*focus_target)(void);

  // Optional. The button Enter clicks when focus is not on a button.
  HWND (*default_button)(void);

  // Optional. Sees each key message before the shell's own handling
  // while the page is active. Returns true when it handled the message.
  bool (*on_key)(const MSG *msg);
} PageDef;

// Indexed by Page. Defined in main.c.
extern const PageDef *const PAGES[PAGE_COUNT];

extern const PageDef select_page, create_page, relabel_page, delete_page,
    help_page, about_page;

// --- App state -----------------------------------------------------------

typedef struct App {
  HWND main_window;
  HWND pages[PAGE_COUNT];
  HWND nav_buttons[PAGE_COUNT];
  int32_t
      nav_button_x[PAGE_COUNT]; // set by nav_layout; positions the underline
  Page active_page;
  uint32_t dpi;  // every layout constant is scaled by it
  HFONT ui_font; // shared by every control; rebuilt on DPI change
  HACCEL accelerators;

  // Created at startup, deleted at exit.
  HBRUSH nav_bar_brush;     // main window background
  HBRUSH page_brush;        // page background
  HBRUSH card_fill_brush;   // card box interior
  HBRUSH card_border_brush; // card box border and divider lines
  HBRUSH underline_brush;   // active nav underline
  HBRUSH qr_module_brush;   // dark QR modules
} App;

extern App app;

// --- Layout --------------------------------------------------------------
// Sizes are pixels at 96 DPI; scale() converts them to the current DPI.

static constexpr int32_t PAGE_MARGIN =
    28; // left and right inset of the nav and pages; top inset of page content
static constexpr int32_t CONTENT_WIDTH =
    476; // list boxes, Create's Password field and buttons, Help and About text
static constexpr int32_t ACCOUNT_FIELD_WIDTH =
    348; // Account column and the fields that edit it
static constexpr int32_t NAV_HEIGHT = 40;

static constexpr int32_t EDIT_MARGIN =
    4; // left and right text margin of edit fields

static constexpr COLORREF TEXT_COLOR = RGB(0, 0, 0);
static constexpr COLORREF LABEL_COLOR = RGB(0x4D, 0x4D, 0x4D);

static inline int32_t scale(int32_t px) { return px * (int32_t)app.dpi / 96; }

static inline int32_t min_i32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max_i32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  return min_i32(max_i32(v, lo), hi);
}

static inline int32_t rect_width(RECT r) { return r.right - r.left; }
static inline int32_t rect_height(RECT r) { return r.bottom - r.top; }

static inline void set_font(HWND control, HFONT font) {
  SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

// --- main.c --------------------------------------------------------------

// show_error shows a modal error message owned by the main window.
void show_error(const wchar_t *msg);

// ensure stops the app with a message when a call that does not fail in
// practice, such as creating a window, does. Unlike assert, it stays in
// release builds.
void ensure(bool ok, const wchar_t *what);

// page_default_proc handles what every page window shares, then defers to
// DefWindowProcW. Page procedures pass it the messages they do not handle.
LRESULT CALLBACK page_default_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam);

// --- fonts.c -------------------------------------------------------------

// Both fonts are 16px at 96 DPI, scaled to app.dpi. The caller owns them.
HFONT build_ui_font(void);
HFONT build_mono_font(void); // for the Password field

// --- nav.c ---------------------------------------------------------------

void nav_create(HWND parent);
void nav_layout(void);
void nav_paint(HDC hdc);
void nav_update_for_dpi(void);

// nav_on_command handles a click on a nav button. Returns false for other ids.
bool nav_on_command(uint16_t id);

// nav_handle_key handles keys IsDialogMessageW gets wrong or does not
// know: the active page's own keys, Enter, Ctrl+Tab, and Tab from a nav
// button into its page. Returns true when the message was handled.
bool nav_handle_key(const MSG *msg);

void nav_set_active_page(Page page);
void nav_focus_page_default(Page page);

// --- widgets.c -----------------------------------------------------------

// create_control creates a visible child control with the given font.
// style is added to WS_CHILD | WS_VISIBLE.
HWND create_control(HWND parent, const wchar_t *class_name, const wchar_t *text,
                    DWORD style, RECT bounds, uint16_t id, HFONT font);

// place_control moves a control and applies a font, after a DPI change.
void place_control(HWND control, RECT bounds, HFONT font);

void draw_label(HDC hdc, const wchar_t *text, RECT bounds, COLORREF color,
                UINT format);

// A card box: the light box with small cut corners drawn around every
// list and edit field. A zeroed CardBox is empty.
typedef struct CardBox {
  HRGN fill;
  HRGN border;
} CardBox;

void card_box_set(CardBox *box, RECT bounds); // replaces any earlier bounds
void card_box_paint(HDC hdc, const CardBox *box);
void card_box_destroy(CardBox *box);

// edit_control_bounds returns where an EDIT control using font goes
// inside the card box outer. It is one line tall, sitting one pixel above
// the bottom border, because an EDIT control does not center its text
// vertically in extra height.
RECT edit_control_bounds(RECT outer, HFONT font);

// set_edit_margins sets equal left and right text margins, px at 96 DPI.
// The default margins depend on the font.
void set_edit_margins(HWND edit, int32_t px);

// edit_get_trimmed_text reads an edit field into buf and returns it with
// surrounding whitespace removed. The result points into buf.
wchar_t *edit_get_trimmed_text(HWND edit, wchar_t *buf, int32_t cap);

// edit_get_secret reads a password field, trimmed, into *out. Its
// intermediate UTF-16 copy is wiped. An empty field gives an empty Secret.
[[nodiscard]] bool edit_get_secret(HWND edit, Secret *out);

// edit_set_secret puts a password in an edit field, wiping its own copies.
[[nodiscard]] bool edit_set_secret(HWND edit, const Secret *password);

// --- listview.c ----------------------------------------------------------
// Select, Delete and Relabel each show the account list: a borderless
// report ListView inside a card box, under painted "Account" and "Created"
// headers. Arrow keys and type-ahead move the focused row without selecting
// it, because selecting a row is what triggers each page's action.

// Per-list type-ahead search state. Private to listview.c.
typedef struct Typeahead {
  wchar_t text[33]; // 32 characters and a terminator
  size_t len;
  uint32_t last_tick;
  int32_t anchor; // row focused when the search began
} Typeahead;

typedef struct ListPage {
  uint16_t control_id;
  int32_t rows; // full rows visible
  HWND listview;
  HIMAGELIST
      row_height_images; // sets the row height; owned here, not by the control
  CardBox box;
  Typeahead typeahead;
} ListPage;

// list_page_create builds the list inside parent and fills it.
void list_page_create(ListPage *list, HWND parent);
void list_page_update_for_dpi(ListPage *list);
void list_page_destroy(ListPage *list);
void list_page_paint(const ListPage *list, HDC hdc);

// list_page_on_notify handles the list's custom draw. It returns false,
// leaving *result alone, for any other notification.
bool list_page_on_notify(const ListPage *list, LPARAM lparam, LRESULT *result);

// list_page_on_key makes Enter in the list select the focused row.
bool list_page_on_key(const ListPage *list, const MSG *msg);

// list_row_became_selected reports whether a WM_NOTIFY is this list
// gaining a selected row, by a click or by the keyboard.
bool list_row_became_selected(const ListPage *list, LPARAM lparam);

void list_select_focused_row(const ListPage *list);
void list_clear_selection(const ListPage *list); // keyboard focus stays put

// The selected row's account id or name. False when no row is selected.
bool list_selected_id(const ListPage *list, int64_t *id);
bool list_selected_name(const ListPage *list, wchar_t *buf, int32_t cap);

// list_box_bottom returns the y just below the list's card box.
int32_t list_box_bottom(const ListPage *list);

// list_reload_all refills every account list, after an account is added,
// renamed or deleted.
void list_reload_all(void);

// --- qr_popup.c ----------------------------------------------------------

typedef void QrDismissFn(void);

typedef enum {
  QR_SHOWN,
  QR_TOO_LONG, // password does not fit in the largest QR code
  QR_FAILED,   // anything else, such as the dismiss timer failing
} QrResult;

// qr_show shows password as a QR code in a popup centered on the main
// window, replacing any code already shown. The popup closes on a click,
// Escape or after CLIPBOARD_CLEAR_DELAY_MS; on_dismiss (may be nullptr)
// runs then, whatever closed it. On failure no popup is left open and
// on_dismiss is not called.
[[nodiscard]] QrResult qr_show(const Secret *password, QrDismissFn *on_dismiss);

// qr_close_if_open closes the popup and reports whether one was open.
bool qr_close_if_open(void);

// qr_update_for_dpi re-sizes and re-centers an open popup after the main
// window's DPI changes. The popup also handles its own WM_DPICHANGED, but
// a scale change does not reliably deliver one to it.
void qr_update_for_dpi(void);

// --- file_dialog.c -------------------------------------------------------
// The dialogs start in This PC and return false if the person cancels or
// anything fails. Free *path with CoTaskMemFree.

[[nodiscard]] bool save_file_dialog(const wchar_t *default_name, PWSTR *path);
[[nodiscard]] bool open_file_dialog(PWSTR *path); // a CSV file
[[nodiscard]] bool pick_folder_dialog(PWSTR *path);
