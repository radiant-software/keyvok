// Keyvok: a password manager for one Windows user. Entry point, main
// window, and the message loop.

#include "app.h"

#ifdef KEYVOK_DEBUG
#include <crtdbg.h>
#endif

#include "clipboard.h"
#include "db.h"

App app;

const PageDef *const PAGES[PAGE_COUNT] = {
    [PAGE_SELECT] = &select_page,   [PAGE_CREATE] = &create_page,
    [PAGE_RELABEL] = &relabel_page, [PAGE_DELETE] = &delete_page,
    [PAGE_HELP] = &help_page,       [PAGE_ABOUT] = &about_page,
};

static const wchar_t MAIN_CLASS_NAME[] = L"KeyvokAppWindow";

// The layout is fixed, so the window cannot be maximized or resized.
static constexpr DWORD MAIN_WINDOW_STYLE =
    WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME);

// Client size at 96 DPI. The height leaves PAGE_MARGIN below the 12-row
// list pages, so it must change if either of those does.
static constexpr int32_t CLIENT_WIDTH = 532;
static constexpr int32_t CLIENT_HEIGHT = 480;

static constexpr uint16_t ICON_RESOURCE_ID = 2; // in resource.rc
static constexpr uint16_t ID_APP_EXIT = 999;    // Ctrl+Q

void show_error(const wchar_t *msg) {
  MessageBoxW(app.main_window, msg, L"Keyvok", MB_OK | MB_ICONERROR);
}

void ensure(bool ok, const wchar_t *what) {
  if (!ok) {
    MessageBoxW(nullptr, what, L"Keyvok", MB_OK | MB_ICONERROR);
    ExitProcess(1);
  }
}

LRESULT CALLBACK page_default_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam) {
  if (msg == WM_CTLCOLORSTATIC) {
    // Read-only EDIT controls ask for STATIC colors. Answer with the
    // page background so they blend in.
    HDC dc = (HDC)wparam;
    SetTextColor(dc, TEXT_COLOR);
    SetBkMode(dc, TRANSPARENT);
    return (LRESULT)app.page_brush;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void on_create(HWND hwnd, HINSTANCE instance) {
  app.main_window = hwnd;
  app.ui_font = build_ui_font();
  nav_create(hwnd);

  // Pages start hidden. WS_EX_CONTROLPARENT lets Tab move between the
  // nav buttons and a page's controls. Real sizes come with WM_SIZE.
  for (Page page = 0; page < PAGE_COUNT; page++) {
    app.pages[page] = CreateWindowExW(
        WS_EX_CONTROLPARENT, PAGES[page]->class_name, nullptr,
        WS_CHILD | WS_CLIPCHILDREN | PAGES[page]->style, 0, scale(NAV_HEIGHT),
        100, 100, hwnd, nullptr, instance, nullptr);
    ensure(app.pages[page] != nullptr, L"Page window creation failed.");
  }
  nav_set_active_page(PAGE_SELECT);

  // Focus rectangles are a session-wide setting that turns on after the
  // first Tab or Alt press. The underline already marks the active page,
  // so hide them.
  SendMessageW(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}

static void on_size(int32_t width, int32_t height) {
  nav_layout();
  int32_t nav_h = scale(NAV_HEIGHT);
  int32_t page_h = height > nav_h ? height - nav_h : 0;
  for (Page page = 0; page < PAGE_COUNT; page++) {
    MoveWindow(app.pages[page], 0, nav_h, width, page_h, TRUE);
    // Content is centered, so the whole page repaints, not only the
    // newly exposed part.
    InvalidateRect(app.pages[page], nullptr, TRUE);
  }
}

// main_frame_size returns the window size that gives the fixed client size
// at dpi.
static SIZE main_frame_size(uint32_t dpi) {
  RECT frame = {.right = CLIENT_WIDTH * (int32_t)dpi / 96,
                .bottom = CLIENT_HEIGHT * (int32_t)dpi / 96};
  AdjustWindowRectExForDpi(&frame, MAIN_WINDOW_STYLE, FALSE, 0, dpi);
  return (SIZE){rect_width(frame), rect_height(frame)};
}

static void on_dpi_changed(HWND hwnd, uint32_t dpi, const RECT *suggested) {
  app.dpi = dpi;

  // Resizing to the suggested rect sends WM_SIZE, which lays out the nav
  // strip and pages at the new DPI.
  SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
               suggested->right - suggested->left,
               suggested->bottom - suggested->top,
               SWP_NOZORDER | SWP_NOACTIVATE);

  HFONT old_font = app.ui_font;
  app.ui_font = build_ui_font();
  nav_update_for_dpi();
  for (Page page = 0; page < PAGE_COUNT; page++) {
    if (PAGES[page]->update_for_dpi)
      PAGES[page]->update_for_dpi();
  }
  DeleteObject(old_font); // every control has switched to the new one
  // After the resize above, so the popup centers on the new frame.
  qr_update_for_dpi();
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  switch (msg) {
  case WM_CREATE:
    on_create(hwnd, ((const CREATESTRUCTW *)lparam)->hInstance);
    return 0;

  case WM_SIZE:
    on_size(LOWORD(lparam), HIWORD(lparam));
    return 0;

  case WM_DPICHANGED:
    on_dpi_changed(hwnd, LOWORD(wparam), (const RECT *)lparam);
    return 0;

  case WM_COMMAND:
    if (LOWORD(wparam) == ID_APP_EXIT) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      return 0;
    }
    if (nav_on_command(LOWORD(wparam)))
      return 0;
    break;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    nav_paint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_DESTROY:
    // Clear a pending clipboard copy while its owner window still exists.
    clip_shutdown();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void create_brushes(void) {
  app.nav_bar_brush = CreateSolidBrush(RGB(0xEF, 0xEF, 0xEF));
  app.page_brush = CreateSolidBrush(RGB(0xF7, 0xF7, 0xF7));
  app.card_fill_brush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
  app.card_border_brush = CreateSolidBrush(RGB(0xD0, 0xD0, 0xD0));
  app.underline_brush = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
  // Near-black keeps the palette soft and still gives scanners enough contrast.
  app.qr_module_brush = CreateSolidBrush(RGB(0x10, 0x10, 0x10));
}

// delete_shared_objects runs after every window is destroyed, so nothing
// can still be drawing with these.
static void delete_shared_objects(void) {
  DestroyAcceleratorTable(app.accelerators);
  DeleteObject(app.ui_font);
  DeleteObject(app.nav_bar_brush);
  DeleteObject(app.page_brush);
  DeleteObject(app.card_fill_brush);
  DeleteObject(app.card_border_brush);
  DeleteObject(app.underline_brush);
  DeleteObject(app.qr_module_brush);
}

static void register_classes(HINSTANCE instance) {
  HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW);
  WNDCLASSW main_class = {
      .lpfnWndProc = main_proc,
      .hInstance = instance,
      .hIcon = LoadIconW(instance, MAKEINTRESOURCEW(ICON_RESOURCE_ID)),
      .hCursor = arrow,
      .hbrBackground = app.nav_bar_brush,
      .lpszClassName = MAIN_CLASS_NAME,
  };
  ensure(RegisterClassW(&main_class) != 0,
         L"Main window class registration failed.");

  for (Page page = 0; page < PAGE_COUNT; page++) {
    WNDCLASSW page_class = {
        .lpfnWndProc = PAGES[page]->proc,
        .hInstance = instance,
        .hCursor = arrow,
        .hbrBackground = app.page_brush,
        .lpszClassName = PAGES[page]->class_name,
    };
    ensure(RegisterClassW(&page_class) != 0,
           L"Page window class registration failed.");
  }
}

// is_escape_down reports whether msg is an Escape key press. It is caught
// in the message loop because the key goes to whichever control has focus.
static bool is_escape_down(const MSG *msg) {
  return msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE;
}

// already_running reports whether another keyvok is running in this logon
// session. The mutex handle stays open for the life of the process, and
// Windows closes it at exit. A mutex that cannot be created, such as one
// held by another user in the same session, does not count.
static bool already_running(void) {
  HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\KeyvokSingleInstance");
  return mutex && GetLastError() == ERROR_ALREADY_EXISTS;
}

// activate_running_instance brings the running keyvok to the front, or
// its open message box if it has one. Windows allows it because this
// process was just started by the person. Does nothing if the window is
// not found, such as while the other instance is starting or closing.
static void activate_running_instance(void) {
  HWND existing = FindWindowW(MAIN_CLASS_NAME, nullptr);
  if (!existing)
    return;
  if (IsIconic(existing))
    ShowWindow(existing, SW_RESTORE);
  SetForegroundWindow(GetLastActivePopup(existing));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  // A second launch is almost always meant to reach the open window.
  if (already_running()) {
    activate_running_instance();
    return 0;
  }

#ifdef KEYVOK_DEBUG
  // Report leaked allocations on stderr at exit.
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif

  // Per-monitor DPI awareness is declared in app.manifest.
  app.dpi = GetDpiForSystem();

  INITCOMMONCONTROLSEX icc = {.dwSize = sizeof icc,
                              .dwICC = ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  wchar_t db_path[MAX_PATH];
  if (!db_default_path(db_path, MAX_PATH) || !db_open(db_path)) {
    show_error(
        L"Keyvok could not open its password database and cannot continue.");
    return 1;
  }

  create_brushes();
  register_classes(instance);

  SIZE frame = main_frame_size(app.dpi);

  // WS_CLIPCHILDREN stops the nav strip's repaint from drawing over the
  // nav buttons.
  HWND hwnd = CreateWindowExW(0, MAIN_CLASS_NAME, L"Keyvok",
                              MAIN_WINDOW_STYLE | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, frame.cx, frame.cy,
                              nullptr, nullptr, instance, nullptr);
  ensure(hwnd != nullptr, L"Window creation failed.");

  // CW_USEDEFAULT can place the window on a monitor whose scale differs
  // from the system DPI it was built for, and no WM_DPICHANGED follows
  // creation, so lay it out again at its own DPI before it is shown.
  uint32_t window_dpi = GetDpiForWindow(hwnd);
  if (window_dpi != app.dpi) {
    RECT placed;
    GetWindowRect(hwnd, &placed);
    frame = main_frame_size(window_dpi);
    RECT target = {placed.left, placed.top, placed.left + frame.cx,
                   placed.top + frame.cy};
    on_dpi_changed(hwnd, window_dpi, &target);
  }

  ACCEL quit = {.fVirt = FCONTROL | FVIRTKEY, .key = 'Q', .cmd = ID_APP_EXIT};
  app.accelerators = CreateAcceleratorTableW(&quit, 1);
  ensure(app.accelerators != nullptr, L"Accelerator table creation failed.");

  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  // Focus set during WM_CREATE does not reliably stick before the window
  // is shown, so set it again.
  nav_focus_page_default(app.active_page);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (TranslateAcceleratorW(hwnd, app.accelerators, &msg))
      continue;

    // Escape closes the QR popup, which never takes focus, or else
    // clears a pending clipboard copy early.
    if (is_escape_down(&msg)) {
      if (qr_close_if_open())
        continue;
      if (clip_has_pending()) {
        clip_flush_pending();
        continue;
      }
    }

    if (nav_handle_key(&msg))
      continue;

    // Tab and Shift+Tab between the nav buttons and within a page's
    // WS_EX_CONTROLPARENT content. A message it consumes must not be
    // dispatched again.
    if (IsDialogMessageW(hwnd, &msg))
      continue;

    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  delete_shared_objects();
  db_close();
  return 0;
}
