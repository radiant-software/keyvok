// The nav strip: one button per page across the top of the main window,
// an underline under the active one, and the keyboard handling that ties
// the buttons to their pages.

#include "app.h"

static constexpr int32_t NAV_BUTTON_TOP = 5;
static constexpr int32_t NAV_BUTTON_WIDTH = 76;
static constexpr int32_t NAV_BUTTON_HEIGHT = 30;
static constexpr int32_t NAV_BUTTON_GAP = 4;
static constexpr int32_t ACTIVE_UNDERLINE_HEIGHT = 3;

// The control id of page P's nav button is ID_NAV_BASE + P.
static constexpr uint16_t ID_NAV_BASE = 1000;

// nav_create makes the buttons; nav_layout places them.
void nav_create(HWND parent) {
  for (Page page = 0; page < PAGE_COUNT; page++) {
    app.nav_buttons[page] =
        create_control(parent, WC_BUTTONW, PAGES[page]->title, WS_TABSTOP,
                       (RECT){}, (uint16_t)(ID_NAV_BASE + page), app.ui_font);
  }
  nav_layout();
}

// nav_layout places the buttons left to right at the current DPI.
void nav_layout(void) {
  int32_t x = scale(PAGE_MARGIN);
  for (Page page = 0; page < PAGE_COUNT; page++) {
    MoveWindow(app.nav_buttons[page], x, scale(NAV_BUTTON_TOP),
               scale(NAV_BUTTON_WIDTH), scale(NAV_BUTTON_HEIGHT), TRUE);
    app.nav_button_x[page] = x;
    x += scale(NAV_BUTTON_WIDTH) + scale(NAV_BUTTON_GAP);
  }
}

// nav_paint draws the line between the strip and the page, and the bar
// under the active page's button.
void nav_paint(HDC hdc) {
  RECT client;
  GetClientRect(app.main_window, &client);
  RECT divider = {
      .left = 0,
      .top = scale(NAV_HEIGHT) - scale(1),
      .right = client.right,
      .bottom = scale(NAV_HEIGHT),
  };
  FillRect(hdc, &divider, app.card_border_brush);

  int32_t x = app.nav_button_x[app.active_page];
  RECT underline = {
      .left = x,
      .top = scale(NAV_HEIGHT) - scale(ACTIVE_UNDERLINE_HEIGHT),
      .right = x + scale(NAV_BUTTON_WIDTH),
      .bottom = scale(NAV_HEIGHT),
  };
  FillRect(hdc, &underline, app.underline_brush);
}

// nav_update_for_dpi re-applies the rebuilt UI font. WM_SIZE repositions
// the buttons.
void nav_update_for_dpi(void) {
  for (Page page = 0; page < PAGE_COUNT; page++)
    set_font(app.nav_buttons[page], app.ui_font);
}

bool nav_on_command(uint16_t id) {
  if (id < ID_NAV_BASE || id >= ID_NAV_BASE + PAGE_COUNT)
    return false;
  nav_set_active_page((Page)(id - ID_NAV_BASE));
  return true;
}

void nav_focus_page_default(Page page) {
  const PageDef *def = PAGES[page];
  HWND target = def->focus_target ? def->focus_target() : nullptr;
  SetFocus(target ? target : app.nav_buttons[page]);
}

void nav_set_active_page(Page page) {
  ShowWindow(app.pages[app.active_page], SW_HIDE);
  app.active_page = page;
  ShowWindow(app.pages[page], SW_SHOW);
  InvalidateRect(app.pages[page], nullptr, TRUE);

  nav_focus_page_default(page);

  // Repaint the strip so the underline moves.
  RECT client;
  GetClientRect(app.main_window, &client);
  RECT strip = {
      .left = 0, .top = 0, .right = client.right, .bottom = scale(NAV_HEIGHT)};
  InvalidateRect(app.main_window, &strip, TRUE);
}

// on_enter clicks the focused button, or else the page's default button.
// It runs before IsDialogMessageW, whose default-button handling does not
// reliably click a focused button.
static bool on_enter(void) {
  HWND focused = GetFocus();
  if (focused && (SendMessageW(focused, WM_GETDLGCODE, 0, 0) & DLGC_BUTTON)) {
    SendMessageW(focused, BM_CLICK, 0, 0);
    return true;
  }

  const PageDef *def = PAGES[app.active_page];
  HWND button = def->default_button ? def->default_button() : nullptr;
  if (!button)
    return false;
  SendMessageW(button, BM_CLICK, 0, 0);
  return true;
}

static bool is_key_down(int32_t vk) { return GetKeyState(vk) < 0; }

static bool on_tab(void) {
  // Ctrl+Tab and Ctrl+Shift+Tab switch pages from anywhere.
  if (is_key_down(VK_CONTROL)) {
    int32_t step = is_key_down(VK_SHIFT) ? -1 : 1;
    nav_set_active_page(
        (Page)((app.active_page + step + PAGE_COUNT) % PAGE_COUNT));
    return true;
  }

  // IsDialogMessageW does not move from the active nav button into its
  // page's content. Shift+Tab is left to it.
  const PageDef *def = PAGES[app.active_page];
  if (!is_key_down(VK_SHIFT) &&
      GetFocus() == app.nav_buttons[app.active_page] && def->focus_target) {
    HWND target = def->focus_target();
    if (target) {
      SetFocus(target);
      return true;
    }
  }
  return false;
}

bool nav_handle_key(const MSG *msg) {
  const PageDef *def = PAGES[app.active_page];
  if (def->on_key && def->on_key(msg))
    return true;

  if (msg->message != WM_KEYDOWN)
    return false;
  switch (msg->wParam) {
  case VK_RETURN:
    return on_enter();
  case VK_TAB:
    return on_tab();
  default:
    return false;
  }
}
