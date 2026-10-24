# Cemu for UWP / Xbox Series

This fork builds Cemu as a UWP app for Xbox Series consoles in Developer Mode, with a Direct3D 12 renderer. It is tuned for the Xbox Series S (5 GB app memory limit, Zen 2 CPU), with Breath of the Wild as the main test case.

Code layout:
- `src/uwp/`: UWP host, app manifest and the ImGui frontend (`UwpFrontend.cpp`).
- `src/Cafe/HW/Latte/Renderer/D3D12/`: Direct3D 12 renderer.

## Building

Requirements:
- Visual Studio 2022 with the C++ desktop and UWP workloads.
- Windows SDK 10.0.26100.
- vcpkg dependencies for the `x64-uwp-static` triplet. They are fetched on the first configure, which takes a few hours.

Steps:
1. Run `tools\uwp\configure.bat` to configure `out\uwp` (Ninja, Release, `ENABLE_UWP=ON`).
2. Run `tools\uwp\build.bat` to build `bin\uwp\CemuUwp.exe`. Extra arguments go to `cmake --build`.

`tools\uwp\env.bat` finds Visual Studio with `vswhere`. To use another install, set `VCVARS` to its `vcvars64.bat`. The scripts call `vcvars64.bat store`: without it the exe links the desktop `VCRUNTIME140.dll` and closes at launch on the console. `dumpbin /dependents bin\uwp\CemuUwp.exe` must show `VCRUNTIME140_APP.dll`.

DXC must be 1.7 or newer, from `Windows Kits\10\bin\<sdk>\x64`. The copy in `Redist\D3D` (10.0.19041) has no `IDxcCompiler3`, which gives a black screen. CMake and the packaging script refuse that copy.

## Packaging and installing

- **Package:** `tools\uwp\package.ps1` builds `out\package\Cemu_<version>_x64.msix`. It signs it with a self-signed test certificate whose subject matches the manifest publisher, and copies the VCLibs dependency next to it. The version comes from `src\uwp\Package.appxmanifest`. Bump the version before each install so the console accepts it as an upgrade.
- **Install:** `XBOX_HOST=<console ip> tools/uwp/deploy.sh` (bash) uploads the newest package through the Xbox Device Portal, upgrades in place (keeping settings, caches and saves) and launches it. `--clean` uninstalls first, which wipes the app data. Device Portal authentication must be off.
- **Copy caches:** `tools/uwp/cache.sh export|import <titleId> <dir> [host]` copies a game's shader, DXIL and pipeline caches between consoles.

## What the D3D12 renderer adds

- **Async shader and pipeline compilation:** with "Async shader compile" enabled, a draw whose pipeline is not ready is skipped while the pipeline builds on background threads. Draws that cannot be skipped are never skipped: full-screen passes without depth, stream-out, and active occlusion queries.
- **Pipeline recipe cache:** pipelines are serialized to `shaderCache/transferable/{title}_d3d12recipes.bin` and prewarmed at launch, within a memory budget. Identical pipelines under different keys are shared through a content fingerprint.
- **DXIL cache:** `shaderCache/precompiled/{title}_dxil.bin` stores compiled shaders. HLSL is pruned of unused helpers before DXC runs; `tools/tests/hlsl_pruner/run.bat` checks that the output DXIL stays byte-identical.
- **Memory pressure handling:** cold pipelines and idle shader bytecode are released under Hard and Critical pressure. Large guest memory areas are committed on first access.
- **Threaded draw recording** (Settings > Graphics, off by default): the Latte thread (the emulated GPU command processor) captures each draw's state, and a second thread builds descriptor tables and records the D3D12 command lists. Operations that need recording state either go through the queue or take exclusive access.
- **Diagnostics:** every 5 seconds `log.txt` gets frame pacing, per-stage CPU cost, GPU time, recorder load and memory lines. Frames over 100 ms are logged with what the render thread was doing.

## Graphic packs

D3D12 loads OpenGL-filtered packs that only patch game code. Pixel shader replacements written in GLSL are converted to HLSL. For BOTW, the FPS++ pack has a "Series S recommended" preset: 30 FPS limit and VSync every second refresh.
