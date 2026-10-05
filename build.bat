@echo off
setlocal
cd /d "%~dp0"

if not exist "third_party\ts3client-pluginsdk\include\ts3_functions.h" (
  echo Downloading TeamSpeak 3 plugin SDK...
  git clone --depth 1 https://github.com/teamspeak/ts3client-pluginsdk.git third_party\ts3client-pluginsdk
  if errorlevel 1 exit /b 1
)

set "GCC=%~dp0tools\w64devkit\bin\gcc.exe"
if not exist "%GCC%" set "GCC=gcc"
where "%GCC%" >nul 2>&1
if errorlevel 1 (
  echo gcc not found. Install MinGW-w64 gcc and add it to PATH, or place it at tools\w64devkit\bin\gcc.exe
  exit /b 1
)

if not exist build mkdir build

"%GCC%" -shared -static -O2 -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8 ^
  -o build\music_pm_hotkeys.dll src\plugin.c ^
  -I third_party\ts3client-pluginsdk\include ^
  -luser32 -lgdi32
if errorlevel 1 exit /b 1

echo Built build\music_pm_hotkeys.dll
exit /b 0
