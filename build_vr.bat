@echo off
REM Build SpaghettiKart with VR support (Windows, Visual Studio + CMake).
REM Run this from a normal command prompt in the repo root.

setlocal
cd /d "%~dp0"

set GEN=Visual Studio 18 2026
set CFG=Release

echo Configuring (vcpkg will build dependencies on first run, including openxr-loader)...
echo Full output -^> build_vr.log
cmake -S . -B build/x64 -G "%GEN%" -A x64 > build_vr.log 2>&1
if errorlevel 1 (
  echo.
  echo Configure failed. Tail of build_vr.log:
  findstr /I /C:"error" /C:"failed" build_vr.log
  echo If "%GEN%" is not installed, change GEN above to your Visual Studio, e.g. "Visual Studio 17 2022".
  echo Full log: build_vr.log
  exit /b 1
)

echo.
echo Building Spaghettify (%CFG%)... appending to build_vr.log
cmake --build build/x64 --config %CFG% --target Spaghettify --parallel >> build_vr.log 2>&1
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Error lines:
  findstr /I /C:"error C" /C:"error LNK" /C:"fatal error" build_vr.log
  echo Full log: build_vr.log
  exit /b 1
)

echo.
echo Done. Executable: build\x64\%CFG%\Spaghettify.exe
echo First run: launch it and pick your Mario Kart 64 US ROM to generate mk64.o2r.
echo Start your VR runtime (Quest Link / Air Link / Virtual Desktop / SteamVR) first for VR;
echo with no headset it runs as the normal flat game. Force with --vr or --novr.
endlocal
