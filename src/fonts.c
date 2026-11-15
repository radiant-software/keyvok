#include "app.h"

static constexpr int32_t FONT_HEIGHT =
    -16; // character height in pixels at 96 DPI

static int CALLBACK note_found(const LOGFONTW *, const TEXTMETRICW *, DWORD,
                               LPARAM found) {
  *(bool *)found = true;
  return 0; // one match is enough
}

// font_installed reports whether a font family named face is installed.
// GDI's font mapper silently substitutes for a missing face, so asking
// for it is no test.
static bool font_installed(const wchar_t *face) {
  LOGFONTW query = {.lfCharSet = DEFAULT_CHARSET};
  wcscpy_s(query.lfFaceName, LF_FACESIZE, face);
  bool found = false;
  HDC dc = GetDC(nullptr);
  if (dc) {
    EnumFontFamiliesExW(dc, &query, note_found, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
  }
  return found;
}

// pick_face returns preferred when it is installed, else fallback. The
// answer is cached in *choice, since fonts are rebuilt on every DPI change.
static const wchar_t *pick_face(const wchar_t **choice,
                                const wchar_t *preferred,
                                const wchar_t *fallback) {
  if (!*choice)
    *choice = font_installed(preferred) ? preferred : fallback;
  return *choice;
}

// pitch_and_family steers GDI's substitute, should the face still be
// missing, toward the same kind of font.
static HFONT build_font(const wchar_t *face, BYTE pitch_and_family) {
  LOGFONTW font = {.lfHeight = scale(FONT_HEIGHT),
                   .lfWeight = FW_NORMAL,
                   .lfPitchAndFamily = pitch_and_family};
  wcscpy_s(font.lfFaceName, LF_FACESIZE, face);
  return CreateFontIndirectW(&font);
}

// Windows 11's Segoe UI Variable installs one family per optical size and
// weight; Text is the one meant for body text at this size. Windows 10 has
// only classic Segoe UI.
HFONT build_ui_font(void) {
  static const wchar_t *face;
  return build_font(pick_face(&face, L"Segoe UI Variable Text", L"Segoe UI"),
                    VARIABLE_PITCH | FF_SWISS);
}

// Cascadia Mono comes with Windows 11, but not with every edition.
HFONT build_mono_font(void) {
  static const wchar_t *face;
  return build_font(pick_face(&face, L"Cascadia Mono", L"Consolas"),
                    FIXED_PITCH | FF_MODERN);
}
