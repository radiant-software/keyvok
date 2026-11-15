// The save-file, open-file and folder-picker dialogs, through the COM
// IFileDialog API.

#define COBJMACROS
#include "app.h"

#include <shobjidl.h>

// The This PC folder, where every dialog starts.
static const wchar_t THIS_PC[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

static const COMDLG_FILTERSPEC CSV_TYPES[] = {
    {L"CSV Files", L"*.csv"},
    {L"All Files", L"*.*"},
};

// show starts dialog in This PC, shows it, and sets *path to the chosen
// file or folder. False if the person cancels or anything fails.
static bool show(IFileDialog *dialog, PWSTR *path) {
  // If This PC cannot be opened, the dialog uses its own default folder.
  IShellItem *start = nullptr;
  if (SUCCEEDED(SHCreateItemFromParsingName(THIS_PC, nullptr, &IID_IShellItem,
                                            (void **)&start))) {
    IFileDialog_SetDefaultFolder(dialog, start);
    IShellItem_Release(start);
  }

  IShellItem *result = nullptr;
  bool ok =
      SUCCEEDED(IFileDialog_Show(dialog, app.main_window)) &&
      SUCCEEDED(IFileDialog_GetResult(dialog, &result)) &&
      SUCCEEDED(IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, path));
  if (result)
    IShellItem_Release(result);
  return ok;
}

bool save_file_dialog(const wchar_t *default_name, PWSTR *path) {
  *path = nullptr;
  bool initialized = SUCCEEDED(CoInitialize(nullptr));
  bool ok = false;

  IFileSaveDialog *dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(&CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                 &IID_IFileSaveDialog, (void **)&dialog))) {
    IFileSaveDialog_SetOptions(dialog, FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                           FOS_OVERWRITEPROMPT);
    IFileSaveDialog_SetFileTypes(dialog, (UINT)_countof(CSV_TYPES), CSV_TYPES);
    IFileSaveDialog_SetFileTypeIndex(dialog, 1); // 1-based
    IFileSaveDialog_SetDefaultExtension(dialog, L"csv");
    IFileSaveDialog_SetFileName(dialog, default_name);
    ok = show((IFileDialog *)dialog, path);
    IFileSaveDialog_Release(dialog);
  }

  if (initialized)
    CoUninitialize();
  return ok;
}

bool open_file_dialog(PWSTR *path) {
  *path = nullptr;
  bool initialized = SUCCEEDED(CoInitialize(nullptr));
  bool ok = false;

  IFileOpenDialog *dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 &IID_IFileOpenDialog, (void **)&dialog))) {
    IFileOpenDialog_SetOptions(dialog, FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                           FOS_FILEMUSTEXIST);
    IFileOpenDialog_SetFileTypes(dialog, (UINT)_countof(CSV_TYPES), CSV_TYPES);
    IFileOpenDialog_SetFileTypeIndex(dialog, 1); // 1-based
    ok = show((IFileDialog *)dialog, path);
    IFileOpenDialog_Release(dialog);
  }

  if (initialized)
    CoUninitialize();
  return ok;
}

bool pick_folder_dialog(PWSTR *path) {
  *path = nullptr;
  bool initialized = SUCCEEDED(CoInitialize(nullptr));
  bool ok = false;

  IFileOpenDialog *dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                 &IID_IFileOpenDialog, (void **)&dialog))) {
    IFileOpenDialog_SetOptions(dialog, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                           FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dialog, L"Select Destination Folder");
    ok = show((IFileDialog *)dialog, path);
    IFileOpenDialog_Release(dialog);
  }

  if (initialized)
    CoUninitialize();
  return ok;
}
