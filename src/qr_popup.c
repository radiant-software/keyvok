// A popup showing a password as a QR code for a phone to scan.
//
// The encoded code lives in a fixed global buffer, never heap memory, and
// is wiped when the popup is destroyed, whatever the cause. Modules are
// drawn straight from it with FillRect: a bitmap would put another copy in
// GDI memory that the app cannot wipe.

#include "app.h"

#include <qrcodegen.h>
#include <shellscalingapi.h>
#ifdef KEYVOK_DEBUG
#include <stdio.h>
#endif

#include "clipboard.h"

static const wchar_t CLASS_NAME[] = L"KeyvokQrPopup";

static constexpr uint32_t AUTO_DISMISS_MS =
    CLIPBOARD_CLEAR_DELAY_MS; // same exposure as a clipboard copy
static constexpr UINT_PTR DISMISS_TIMER_ID = 1;

// Codes are drawn at a constant overall size, so short passwords do not
// make small codes. MIN_MODULE_PX keeps the modules of large codes big
// enough to scan off a screen; those popups grow past the target size.
static constexpr int32_t TARGET_CONTENT_PX = 320;
static constexpr int32_t MIN_MODULE_PX = 8;
static constexpr int32_t QUIET_ZONE_MODULES =
    4; // required blank border, in modules

// Sized for the largest code (version 40); only the part for the current
// code's size is meaningful.
static uint8_t code[qrcodegen_BUFFER_LEN_MAX];
static HWND popup;
static bool class_registered;
static QrDismissFn *on_dismiss;
static uint32_t popup_dpi; // the DPI the open popup is laid out and painted at

static constexpr DWORD POPUP_STYLE = WS_POPUP | WS_VISIBLE;
// WS_EX_TOOLWINDOW keeps it out of Alt+Tab and the taskbar.
static constexpr DWORD POPUP_EX_STYLE = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

// module_px returns the module size at dpi for a code of size modules.
// The popup uses its monitor's DPI, not app.dpi: it is a separate top-level
// window, so it stays on its monitor when the main window moves to another.
static int32_t module_px(int32_t size, uint32_t dpi) {
  return max_i32(MIN_MODULE_PX,
                 TARGET_CONTENT_PX / (size + QUIET_ZONE_MODULES * 2)) *
         (int32_t)dpi / 96;
}

// popup_rect returns the popup's frame for the current code at dpi,
// centered on the main window.
static RECT popup_rect(uint32_t dpi) {
  int32_t size = qrcodegen_getSize(code);
  int32_t content_px = (size + QUIET_ZONE_MODULES * 2) * module_px(size, dpi);
  RECT frame = {.right = content_px, .bottom = content_px};
  AdjustWindowRectExForDpi(&frame, POPUP_STYLE, FALSE, POPUP_EX_STYLE, dpi);
  int32_t width = rect_width(frame), height = rect_height(frame);

  RECT main;
  GetWindowRect(app.main_window, &main);
  int32_t x = main.left + (rect_width(main) - width) / 2;
  int32_t y = main.top + (rect_height(main) - height) / 2;
  return (RECT){x, y, x + width, y + height};
}

static void move_popup(const RECT *r) {
  SetWindowPos(popup, nullptr, r->left, r->top, rect_width(*r), rect_height(*r),
               SWP_NOZORDER | SWP_NOACTIVATE);
}

// monitor_dpi returns the effective DPI of the monitor showing most of
// hwnd. It asks the monitor rather than the window, so it is current even
// before the window has been told of a scale change.
static uint32_t monitor_dpi(HWND hwnd) {
  UINT x, y;
  HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  return SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))
             ? x
             : app.dpi;
}

// place_popup sizes the open popup for its monitor's DPI, centers it on
// the main window and repaints it. Sizing for the main window's monitor
// comes first, since that is where it is centered; the center can still
// fall on a monitor at another scale when the main window straddles two.
static void place_popup(const char *why) {
  popup_dpi = monitor_dpi(app.main_window);
  RECT r = popup_rect(popup_dpi);
  move_popup(&r);
  uint32_t dpi = monitor_dpi(popup);
  if (dpi != popup_dpi) {
    popup_dpi = dpi;
    r = popup_rect(dpi);
    move_popup(&r);
  }
  InvalidateRect(popup, nullptr, TRUE);
#ifdef KEYVOK_DEBUG
  fprintf(stderr, "keyvok qr: placed at %u DPI (%s)\n", popup_dpi, why);
#else
  (void)why;
#endif
}

bool qr_close_if_open(void) {
  if (!popup)
    return false;
  DestroyWindow(popup);
  return true;
}

// paint fills the background and draws the dark modules. Light modules
// and the quiet zone are left as background.
static void paint(HWND hwnd, HDC hdc) {
  RECT client;
  GetClientRect(hwnd, &client);
  FillRect(hdc, &client, app.card_fill_brush);

  int32_t size = qrcodegen_getSize(code);
  int32_t px = module_px(size, popup_dpi);
  int32_t origin = QUIET_ZONE_MODULES * px;
  for (int32_t y = 0; y < size; y++) {
    for (int32_t x = 0; x < size; x++) {
      if (!qrcodegen_getModule(code, x, y))
        continue;
      RECT module = {origin + x * px, origin + y * px, origin + (x + 1) * px,
                     origin + (y + 1) * px};
      FillRect(hdc, &module, app.qr_module_brush);
    }
  }
}

static LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam) {
  switch (msg) {
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    paint(hwnd, hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }

  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
    DestroyWindow(hwnd);
    return 0;

  case WM_KEYDOWN:
    if (wparam == VK_ESCAPE)
      DestroyWindow(hwnd);
    return 0;

  // Sent when the popup's monitor changes scale. Sized from the code
  // rather than the suggested rect, which can round differently.
  case WM_DPICHANGED:
    place_popup("WM_DPICHANGED");
    return 0;

  case WM_TIMER:
    if (wparam == DISMISS_TIMER_ID)
      DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY: {
    KillTimer(hwnd, DISMISS_TIMER_ID);
    SecureZeroMemory(code, sizeof code);
    popup = nullptr;
    QrDismissFn *callback = on_dismiss;
    on_dismiss = nullptr;
    if (callback)
      callback();
    return 0;
  }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// open_popup shows the popup for the code in code, or resizes and
// repaints it if it is already open. False means the dismiss timer could
// not be armed and the popup was closed.
static bool open_popup(void) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!class_registered) {
    WNDCLASSW cls = {
        .lpfnWndProc = popup_proc,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .hbrBackground = app.card_fill_brush,
        .lpszClassName = CLASS_NAME,
    };
    ensure(RegisterClassW(&cls) != 0,
           L"QR popup window class registration failed.");
    class_registered = true;
  }

  if (!popup) {
    popup_dpi = monitor_dpi(app.main_window);
    RECT r = popup_rect(popup_dpi);
    // The main window owns it, so Windows destroys it with the main
    // window.
    popup = CreateWindowExW(POPUP_EX_STYLE, CLASS_NAME, L"Keyvok", POPUP_STYLE,
                            r.left, r.top, rect_width(r), rect_height(r),
                            app.main_window, nullptr, instance, nullptr);
    ensure(popup != nullptr, L"QR popup window creation failed.");
    // Keeps the code out of screenshots and screen sharing on Windows 10
    // 2004 and later; no effect before that.
    SetWindowDisplayAffinity(popup, WDA_EXCLUDEFROMCAPTURE);
  }
  place_popup("shown");

  // Shown without activation: the click that led here is still being
  // handled by the ListView, which would take focus back anyway.
  ShowWindow(popup, SW_SHOWNA);

  // Re-arming restarts the timer, so a new code gets its full time.
  // Without it the code would stay on screen until clicked away.
  if (!SetTimer(popup, DISMISS_TIMER_ID, AUTO_DISMISS_MS, nullptr)) {
    qr_close_if_open();
    return false;
  }
  return true;
}

QrResult qr_show(const Secret *password, QrDismissFn *dismiss) {
  if (!password->text) {
    qr_close_if_open();
    return QR_FAILED;
  }

  // The encoder only clears the bytes the new code uses. Clear the whole
  // buffer so a shorter code does not leave part of the previous one.
  SecureZeroMemory(code, sizeof code);
  uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
  bool encoded = qrcodegen_encodeText(
      password->text, temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
      qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
  SecureZeroMemory(temp, sizeof temp);
  if (!encoded) {
    // Do not leave an earlier code on screen.
    qr_close_if_open();
    SecureZeroMemory(code, sizeof code);
    return QR_TOO_LONG;
  }

  if (!open_popup())
    return QR_FAILED;
  // Set only once the popup is up, so it does not run for a failed show.
  on_dismiss = dismiss; // replaces any earlier callback
  return QR_SHOWN;
}

void qr_update_for_dpi(void) {
  if (popup)
    place_popup("main window DPI changed");
}
