@echo off
REM Incremental rebuild of the VR build (no reconfigure). Use after editing source.
REM Kills any running Spaghettify.exe first (a running game locks the exe and the link fails with
REM LNK1104), builds, and writes the full output to build_vr.log in the repo root.

setlocal
cd /d "%~dp0"
set CFG=Release

taskkill /F /IM Spaghettify.exe >nul 2>&1

echo Building (incremental, %CFG%)... full output -> build_vr.log
cmake --build build/x64 --config %CFG% --target Spaghettify --parallel > build_vr.log 2>&1

if errorlevel 1 (
  echo.
  echo BUILD FAILED. Error lines:
  findstr /I /C:"error C" /C:"error LNK" /C:"fatal error" build_vr.log
  echo.
  echo Full log: build_vr.log
  exit /b 1
)

echo Build OK -^> build\x64\%CFG%\Spaghettify.exe   ^(full log: build_vr.log^)
endlocal
