# Captures the README screenshots, run by `make screenshots`. Launches the
# keyvok.exe in -Dir, which opens the fake-account keyvok.db beside it, and
# saves select.png and create.png to the same folder.
param([Parameter(Mandatory)][string]$Dir)
$ErrorActionPreference = 'Stop'

# Keyvok allows one running copy, so with another open, the demo copy
# would exit at once.
if (Get-Process keyvok -ErrorAction SilentlyContinue) {
  throw 'Keyvok is running. Close it and run again.'
}

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
}
"@

$WM_SETTEXT = 0x000C
$WM_COMMAND = 0x0111
$WM_CHANGEUISTATE = 0x0127
$BM_CLICK = 0x00F5
$HIDE_ACCEL = [IntPtr]0x20001          # MAKEWPARAM(UIS_SET, UISF_HIDEACCEL)
$PW_RENDERFULLCONTENT = 2
$DWMWA_EXTENDED_FRAME_BOUNDS = 9
$NAV_BASE = 1000                       # ID_NAV_BASE in src/nav.c
$PAGE_SELECT = 0
$PAGE_CREATE = 1

# Per-monitor DPI awareness, so window sizes are in physical pixels.
[W]::SetProcessDpiAwarenessContext([IntPtr]-4) | Out-Null

$app = Start-Process -FilePath (Join-Path $Dir 'keyvok.exe') -WorkingDirectory $Dir -PassThru
try {
  $h = [IntPtr]::Zero
  for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 200
    $app.Refresh()
    $h = $app.MainWindowHandle
  }
  if ($h -eq [IntPtr]::Zero) { throw 'Keyvok window did not appear.' }
  [W]::SetForegroundWindow($h) | Out-Null

  function Show-Page($page) {
    [W]::SendMessage($h, $WM_COMMAND, [IntPtr]($NAV_BASE + $page), [IntPtr]::Zero) | Out-Null
  }

  # Saves the window without its drop shadow. The pointer is moved away so
  # no row shows a hover highlight, and access-key underlines are hidden,
  # since keyboard activity in the session would otherwise show them.
  function Save-Window($name) {
    $r = New-Object W+RECT
    [W]::GetWindowRect($h, [ref]$r) | Out-Null
    [W]::SetCursorPos($r.R + 200, $r.B + 200) | Out-Null
    [W]::SendMessage($h, $WM_CHANGEUISTATE, $HIDE_ACCEL, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800

    $frame = New-Object W+RECT
    [W]::DwmGetWindowAttribute($h, $DWMWA_EXTENDED_FRAME_BOUNDS, [ref]$frame, 16) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [W]::PrintWindow($h, $dc, $PW_RENDERFULLCONTENT) | Out-Null
    $g.ReleaseHdc($dc)
    $g.Dispose()
    $crop = New-Object System.Drawing.Rectangle ($frame.L - $r.L), ($frame.T - $r.T),
      ($frame.R - $frame.L), ($frame.B - $frame.T)
    $out = $bmp.Clone($crop, $bmp.PixelFormat)
    $bmp.Dispose()
    $out.Save((Join-Path $Dir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose()
  }

  Show-Page $PAGE_SELECT
  Save-Window 'select.png'

  # A new label and a freshly generated password, not one from the list.
  Show-Page $PAGE_CREATE
  $create = [W]::FindWindowEx($h, [IntPtr]::Zero, 'KeyvokCreatePage', $null)
  $account = [W]::FindWindowEx($create, [IntPtr]::Zero, 'Edit', $null)
  [W]::SendMessage($account, $WM_SETTEXT, [IntPtr]::Zero, 'Pharmacy | jane@example.com') | Out-Null
  $generate = [W]::FindWindowEx($create, [IntPtr]::Zero, 'Button', 'Generate Strong Password')
  [W]::SendMessage($generate, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Save-Window 'create.png'
} finally {
  if (-not $app.HasExited) { Stop-Process -Id $app.Id }
}
