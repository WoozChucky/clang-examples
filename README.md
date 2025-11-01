# clang-examples

A tiny, hot-reloadable game/runtime sandbox for Windows featuring:

- A host runtime executable (DX11/DX12 via NVRHI) that loads a "game" DLL and an optional ImGui overlay DLL at runtime.
- A minimal platform layer (window/input/audio/file-watching) with a simple bump-allocator memory model.
- An immediate-mode UI kit that records draw commands into a renderer-agnostic command buffer.
- A renderer that consumes those commands and draws 3D and UI using NVRHI.
- Tracy CPU instrumentation and GPU timers.

This document is a code tour, an architecture overview, and a quick developer guide.

---

## Quick start (Windows)

Prerequisites
- Visual Studio 2022 (or Build Tools) with the Windows 10/11 SDK
- CMake 3.20+
- A DX11/DX12-capable GPU

Optional
- Vulkan SDK (the build links Vulkan::Headers for NVRHI; headers are vendored in third_party/NVRHI, so you typically don’t need the full SDK unless you enable the Vulkan backend)
- Ninja (for faster builds)

Build and run (CMake Presets)

```
cmake --preset debug
cmake --build --preset debug

rem Run the app (binary is placed under build/<config>/bin/<Config>)
.\nbuild\debug\bin\Debug\main.exe
```

Notes
- First run copies the `assets/` folder to the runtime output dir via a CMake post-build step.
- Press F5 in the app to toggle VSync.
- The game and overlay are DLLs that can be hot-reloaded while the runtime keeps running.

One-command fast rebuild of just the game DLL (for hot reload)

```
rem From repo root (uses clang if available)
build.bat
```

This compiles `src/game/src/game.cpp` into `build\debug-clang\bin\Debug\game.dll` with a unique PDB name to avoid lock contention.

---

## Repository layout

Top-level
- `CMakeLists.txt` – root CMake; adds third_party, src/game, src/overlay, src/runtime
- `CMakePresets.json` – build presets (Debug/RelWithDebInfo, clang toolset, etc.)
- `build.bat` – convenience batch to rebuild only the game DLL using clang
- `assets/` – fonts, sounds, shaders, textures used by sample scenes and UI

Source
- `src/common/include/` – cross-layer headers and simple facilities
  - `lib.h` – logging, asserts, math helpers, array, macros, and allocation sizes
  - `render.h` – renderer-agnostic interfaces and immediate-mode UI draw command buffer
  - `input.h` – input model (keys, mouse, event helpers)
  - `sound.h` – sound types and ring buffer for the platform audio backend
  - `ui.h` – a small immediate-mode UI kit that writes into `RenderData`
- `src/runtime/` – the host executable and platform + renderer backends
  - `src/main.cpp` – process bootstrap, hot-reload loop, frame timing, VSync toggle
  - `src/windows_platform.cpp` – Win32 window/input, XAudio2 audio, file watching
  - `src/renderer*.{h,cpp}` – renderer front-end and backends (DX11, DX12; Vulkan stub)
  - `src/font.cpp` – FreeType font loading into a simple atlas consumed by the UI pass
  - `src/image.cpp` – texture loading helpers
- `src/game/` – the hot-reloaded game DLL
  - `src/game.cpp` – game entry points (update/resize/version/assert handler), scenes
  - `src/game_*` – state-specific update functions (main menu, in-level, editor)
- `src/overlay/` – optional ImGui-based debug overlay as a separately hot-reloadable DLL

Third-party (vendored)
- `third_party/NVRHI` – Rendering Hardware Interface used for DX11/DX12/Vulkan
- `third_party/imgui` – Dear ImGui backends and core
- `third_party/glm` – math
- `third_party/freetype` – font rasterizer
- `third_party/tracy` – Tracy profiler client

---

## High-level architecture

Layers (top to bottom)

1) Game DLL (`src/game`)
- Exports a strict API consumed by the runtime: `game_update`, `game_resize`, `game_get_api_version` and an optional `game_set_platform_debug_break` to receive the host’s assert handler.
- Reads input and writes to `RenderData` and `SoundState` using the helpers defined in `render.h` and `sound.h`.
- Uses `ui.h` immediate-mode helpers to push UI commands (rects, text, etc.).

2) Runtime EXE (`src/runtime`)
- Owns the process, window, device, and the main loop.
- Maintains `PlatformContext` with pointers to the live `Input`, `RenderData`, and `SoundState` blocks that are passed to the game DLL.
- Watches for game/overlay DLL file changes; hot-reloads them behind timestamped filenames to avoid lock issues.
- Calls the renderer to consume the current frame’s `RenderData`.

3) Renderer (`src/runtime/src/renderer*.cpp`)
- Front-end (`renderer.h/.cpp`) builds simple pipelines and dispatches to a backend.
- Backend is selected at compile time (current default: DX12 via `#define RENDER_API directx12`).
- Uses NVRHI to set up:
  - A simple 3D pass (textured cube using `assets/block_atlas.png`)
  - A procedural primitives pass (e.g., grid on Y=0 plane)
  - A UI pass using instanced quads and a structured buffer for per-instance data
- Loads fonts with FreeType and builds an atlas; UI text rendering samples that R8 atlas.
- Measures GPU time using NVRHI timer queries; exposes ms to the game.

4) Platform (`windows_platform.cpp`)
- Win32 message pump, key/mouse mapping, dragging/moving window behavior.
- Audio with XAudio2 (simple source-voice pool); per-frame `platform_update_audio` consumes `SoundState` commands.
- File watchers using Win32 change notifications for DLL hot-reload.
- Focus/minimized flags and per-frame `FrameStats` book-keeping.

Data flow
- Runtime owns memory arenas (`BumpAllocator` persistent + transient) and allocates `Input`, `RenderData`, `SoundState`, `GameState`, `UIState` once at startup.
- Each frame:
  1. Platform processes window messages and updates `Input` and `FrameStats`.
  2. Runtime triggers hot-reload if DLLs changed (debounced).
  3. Runtime calls `game_update(...)` from the currently loaded game DLL.
  4. Game writes draw commands into `RenderData` (UI + 3D primitives) and sound commands into `SoundState`.
  5. Renderer consumes `RenderData` and presents the frame.
  6. Audio backend plays/updates sounds.
  7. Transient allocator is reset.

---

## The hot-reload mechanism

Files
- `src/runtime/src/main.cpp` – entry point and reload functions `reload_game_dll` and `reload_ui_dll`.
- `src/runtime/src/windows_platform.cpp` – `platform_watch_file`, `platform_file_changed`.

How it works
- The game/overlay DLLs are built as `game.dll` and `imgui_overlay.dll` into the runtime output folder.
- On change, the runtime creates a timestamped copy (e.g., `runtime/game_load_1700000000000.dll`) to avoid replacing a loaded module and to avoid file locks.
- The timestamped copy is loaded, symbols are looked up and validated:
  - `game_get_api_version()` must match `GAME_API_VERSION` defined in headers.
  - Required entry points (`game_update`/`game_resize`) must exist.
  - Optional callback `game_set_platform_debug_break` is passed to route asserts through the host.
- The old module is freed after the new one loads successfully.

Overlay
- Works the same way; provides `overlay_setup/overlay_render/overlay_shutdown` and an input hook `overlay_handle_wndproc`.

---

## Renderer overview

Front-end (API in `renderer.h`)
- `renderer_init(width, height, hwnd, allocator)` – creates backend, device, swapchain, command list; builds pipelines, loads fonts/atlas.
- `render(dt, renderData, transient, uiOverlay)` – records + submits commands for:
  - Primitive pass (Y=0 grid),
  - 3D cube pass (textured, simple packed-vertex format),
  - UI pass (instanced quads and text). Then calls the overlay render hook, and presents.
- `renderer_resize(width, height)` – rebuilds swapchain-dependent pipelines.
- `renderer_toggle_vsync()` – flips a backend flag read during `Present`.

Backends
- DX12 – `renderer_dx12.*`
  - Creates graphics/compute/copy queues, DXGI flip-discard swapchain, frame fence per backbuffer, and NVRHI device.
  - `renderer_present` calls `IDXGISwapChain::Present(syncInterval, flags)` with `syncInterval = vsyncEnabled ? 1 : 0`. If the window is occluded, `Present` can return `DXGI_STATUS_OCCLUDED` immediately.
- DX11 – `renderer_dx11.*`
  - Similar structure in a simpler form (single backbuffer, immediate context).

UI pass
- A structured buffer holds `UIInstance`s (transform, color, UV rect, flags). Quads are instanced; text uses the font atlas (R8) and straight alpha blending.

Fonts
- `src/runtime/src/font.cpp` uses FreeType to bake ASCII glyphs into an atlas; `renderer_font_load()` registers additional fonts; UI selects fonts by `fontIndex`.

GPU timings
- `GpuTimer` wraps NVRHI timer queries. We sample the previous frame’s timer to avoid blocking.

---

## Immediate-mode UI (game-side)

Header: `src/common/include/ui.h`

Pattern
- Call `ui_im_begin_frame(ui, renderData, input)` once per frame.
- Build UI using widgets (`ui_begin_panel`, `ui_button`, `ui_label`, `ui_input_text`, `ui_progress_bar`, ...).
- Call `ui_im_end_frame(ui)`; the renderer consumes `renderData->uiRects` + `renderData->uiTexts`.

Layout
- Panels can be docked (Top/Bottom/Left/Right/Fill) into the parent available rect; padding is supported.
- A simple vertical layout cursor advances within a panel as widgets are added.

Text
- The UI uses precomputed ASCII glyph counts and the font atlas to minimize per-frame CPU work.

---

## Platform layer (Windows)

File: `src/runtime/src/windows_platform.cpp`

- Win32 window creation and message pump (`platform_create_window`, `platform_update_window`).
- Focus/minimized states, DPI-aware resize handling.
- Input mapping from Win32 VKs to engine `KeyCodeID`s.
- Audio via XAudio2 (source-voice pool, fade-in/out helpers; `platform_update_audio`).
- File watching implemented with directory change notifications + a debounced worker thread.
- Assertion handler that shows a MessageBox and breaks into the debugger.

Frame stats
- `FrameStats` (in `platform.h`) is filled from `main.cpp::get_delta_time()` and used by overlays and diagnostics.

---

## Controls and features

- F5 – Toggle VSync
- 1 / 2 / 3 – Toggle Diagnostics / HUD / Editor panels (handled in the game DLL)
- TAB – Play a sound (example XAudio2 usage)

On-screen
- A 3D textured cube and a procedural ground grid (primitives pass)
- Simple UI (buttons, labels, text input) rendered via the UI pass
- Optional ImGui overlay (if the overlay DLL is present and loaded)

---

## Development workflow

- Build and run the runtime once from CMake.
- Rebuild only the game DLL as you iterate (using your IDE or `build.bat`).
- The runtime watches the DLL, copies it to a timestamped filename, validates API version, and swaps it in.
- Edit the overlay DLL similarly if you’re working on UI tooling.

Debugging tips
- `SM_ASSERT` routes to the platform’s `platform_debug_break`, which shows a dialog and breaks in the debugger.
- Tracy zones are sprinkled through the frame (`ZoneScoped`, `FrameMark`); run the Tracy server to capture.
- GPU times are sampled and exposed via `render()` return value; the game stores and displays them.

---

## Build system notes

- Binaries are placed under `build/<preset>/bin/<Config>/` by `CMakeLists.txt` (`RUNTIME_DIR`).
- The runtime executable is named `main.exe` (target `runtime`).
- The game DLL is named `game.dll` (target `game`).
- The overlay DLL is named `imgui_overlay.dll` (target `imgui_overlay`).

---

## Troubleshooting

- Black window or immediate exit
  - Check the console logs (colored `SM_*` macros). Asset copy step runs after build; ensure `assets/` is present next to `main.exe`.
- Present returns `DXGI_STATUS_OCCLUDED`
  - This is expected when minimized or fully occluded. Add a small sleep in that code path to avoid pegging a CPU core (see VSync notes above).
- Hot reload doesn’t trigger
  - The file watcher tracks `game.dll` and `imgui_overlay.dll` in the output dir. Ensure your IDE rebuilds to the same location or copy the DLLs there.
- D3D12 device creation fails
  - The example currently asserts for missing DX12 Ultimate features in `ValidateDX12UltimateCapabilities`. Relax or guard those checks if you’re targeting broader hardware.

---

## Next steps and ideas

- Add DXGI waitable swapchain for consistent frame pacing and background throttling.
- Expose a simple renderer plugin API for adding new passes from the game DLL.
- Expand the UI kit (sliders, checkboxes, lists) and add input text selection/caret/clipboard.
- Implement Vulkan backend (files are stubbed).
- Add unit tests for hot-reload path and the UI layout helpers.

---

## License

This repository includes third-party components under their respective licenses (see `third_party/*`). The project code in this repository is provided as-is for learning and experimentation.

