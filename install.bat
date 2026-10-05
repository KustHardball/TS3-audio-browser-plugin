@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo Requesting administrator rights to copy the plugin into TeamSpeak...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)

set "DLL=%~dp0dist\music_pm_hotkeys.dll"
if exist "%~dp0build\music_pm_hotkeys.dll" set "DLL=%~dp0build\music_pm_hotkeys.dll"
if not exist "%DLL%" (
  echo Plugin DLL not found.
  pause
  exit /b 1
)

set "DEST=C:\Program Files\TeamSpeak 3 Client\plugins"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%DLL%" "%DEST%\music_pm_hotkeys.dll"
if errorlevel 1 (
  echo Copy failed.
  pause
  exit /b 1
)

echo.
echo Installed:
echo   %DEST%\music_pm_hotkeys.dll
echo.
echo Restart TeamSpeak 3. In Tools - Plugins, enable the Music PM plugin if it is off.
echo.
pause
exit /b 0
