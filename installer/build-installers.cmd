@echo off
rem Signs build\release\keyvok.exe, then builds and signs the installer
rem (Inno signs the setup exe and its uninstaller with the same command).
rem Run "make" in WSL first. Output: build\installer\Keyvok-Setup-x64.exe

setlocal
rem Run from the repo root, whatever the current directory.
cd /d "%~dp0.."

set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
rem Same Windows SDK version as SDK_VERSION in the Makefile.
set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
set "SIGN=$q%SIGNTOOL%$q sign /n $qRadiant Software LLC$q /tr http://timestamp.digicert.com /td sha256 /fd sha256"

echo --- Signing keyvok.exe ---
"%SIGNTOOL%" sign /n "Radiant Software LLC" /tr http://timestamp.digicert.com /td sha256 /fd sha256 "build\release\keyvok.exe" || goto :fail

echo.
echo --- Building installer ---
"%ISCC%" "/Skeyvok=%SIGN% $f" installer\keyvok.iss || goto :fail

echo.
echo --- Verifying signatures ---
"%SIGNTOOL%" verify /pa "build\release\keyvok.exe" || goto :fail
"%SIGNTOOL%" verify /pa "build\installer\Keyvok-Setup-x64.exe" || goto :fail

echo.
echo Done: build\installer\Keyvok-Setup-x64.exe
exit /b 0

:fail
echo.
echo FAILED
exit /b 1
