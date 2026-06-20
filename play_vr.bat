@echo off
REM Launch SpaghettiKart in VR. Start your VR runtime first (Quest Link, Air Link, Virtual Desktop,
REM or SteamVR). With no headset, drop the --vr and it runs flat. --console opens a log window.
cd /d "%~dp0build\x64\Release"
start "" Spaghettify.exe --vr
