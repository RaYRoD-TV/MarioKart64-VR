# SpaghettiKart VR - notes

Technical notes for the VR build. The main README covers normal play.

## What it does

VR runs through OpenXR. The scene is drawn twice per frame, once per eye, with real head tracking, so
you sit in the kart and look around. At startup the exe checks OpenXR for a connected headset and turns
VR on if it finds one; with no headset (or no OpenXR runtime) it runs as the normal flat game. Force
either way with --vr or --novr.

VR uses the OpenGL renderer, because OpenXR is bound to the GL context here. When VR turns on it switches
the renderer to OpenGL automatically (the same backend you can pick by hand in Settings). Built against
OpenXR 1.0, so it works on runtimes like Virtual Desktop's VDXR as well as SteamVR and the Oculus runtime.

## Running it

Start your VR runtime first (Quest Link, Air Link, Virtual Desktop, or SteamVR) if you want VR. Run
Spaghettify.exe (or play_vr.bat). On first launch, pick your Mario Kart 64 US ROM so it can build
mk64.o2r; there's no game data in the exe. You play with the usual gamepad or keyboard; head tracking
drives the view. Motion controllers aren't wired up yet.

## Building

Same as a normal SpaghettiKart build (Visual Studio + CMake). The only extra dependency is the OpenXR
loader, which vcpkg pulls in automatically (openxr-loader, listed in vcpkg.json). Run build_vr.bat, or:

    cmake -S . -B build/x64 -G "Visual Studio 18 2026" -A x64
    cmake --build build/x64 --config Release --target Spaghettify --parallel

The OpenXR loader is linked statically, so there are no extra runtime DLLs to ship beyond what
SpaghettiKart already needs.

## Where the VR code lives

- src/port/vr/vr.cpp, vr.h - the OpenXR work: session setup, per-eye view/projection matrices, stereo,
  head tracking, the head-locked HUD/menu panel, frame submit, and the startup headset check. Plain C
  interface so it can be called from the C++ renderer and the C game code.
- src/port/Engine.cpp - the per-frame VR loop (in RunCommands): begin frame, render each eye, submit;
  and forcing the OpenGL backend when VR is on.
- src/port/Game.cpp - the --vr / --novr flags and the startup headset auto-detect.
- src/engine/sky/SkyDomeVR.cpp - the 3D sky dome that replaces the flat 2D skybox inside each eye, so
  the sky stays put when you turn your head instead of sliding around.
- src/vr_pause_menu.c - the in-game VR menu (pause, then R1), drawn with the game's own font and boxes
  so it lands on the head-locked panel like the rest of the HUD.
- libultraship/src/fast/interpreter.cpp, interpreter.h - small, contained renderer hooks: the per-eye
  projection substitution in GfxSpMatrix, the aspect-ratio bypass in AdjXForAspectRatio, the HUD-plane
  remap for screen-space rectangles, and RunVrEye (interpret the display list into an externally bound
  eye target). All gated so the flat path is unchanged when VR is off.

## VR settings

View mode (Third Person, First Person, Theater, Diorama), per-mode world scale, camera distance and eye
height, stereo depth, the HUD plane's size and distance, and the menu panel's size, distance and opacity
are all live-tunable. The quickest way is the in-game VR menu: pause the race, then press R1. The desktop
Esc menu has the full slider set under Enhancements > VR.

## Still rough

Beta. Stereo, head tracking, the four view modes, the world-anchored sky and the head-locked HUD are in.
Comfort features (cockpit frame, horizon lock, turn vignette) are not built yet. Motion controllers
aren't wired up; you play with a gamepad or keyboard.
