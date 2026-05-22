# clang-examples

A small, hot-reloadable game engine sandbox for Windows featuring:

- A reusable runtime core (`Engine.dll`) with a three-thread model (platform/game/render), an NVRHI renderer (DX12/Vulkan), an ECS, a .NET plugin host, and an allocator toolkit.
- Two thin executables layered on that core: `editor.exe` (the core plus an ImGui tooling layer) and `runtime.exe` (the stripped player build, no ImGui).
- A hot-reloadable game library (`Game.dll`) loaded at runtime via `GameLibrary` + file watching.
- Lock-free thread communication (`SpscRing` rings, `Seqlock` snapshots) and `shared_ptr<const ECS>` snapshots.
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
rem Configure + build (msvc-win64-vs2026-community is the working preset;
rem clang-win64-vs2026-* variants also exist). Out-of-source under out/build/<preset>/.
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community

rem Run the editor (binaries land under out/build/<preset>/bin/<Config>/)
.\out\build\msvc-win64-vs2026-community\bin\Debug\editor.exe

rem Or run the stripped player build (no ImGui)
.\out\build\msvc-win64-vs2026-community\bin\Debug\runtime.exe
```

Notes
- A CMake post-build step copies the `assets/` folder next to the executable.
- `Game.dll` is built by CMake (target `game`) and can be hot-reloaded while the host keeps running.
- Pass `--backend=vulkan` or `--backend=directx12` to override the persisted renderer backend for one run; `--help` prints usage.

Fast iteration on game code (hot reload)

```
rem Rebuild just the game library; the running editor/runtime reloads it within ~1s.
cmake --build --preset msvc-win64-vs2026-community --target game
```

This rebuilds `Game.dll` (target `game`). The host's `GameLibrary` file-watch copies it to a timestamped filename, validates the API version, and swaps it in between ticks. There is no `build.bat`.

---

## Repository layout

Top-level
- `CMakeLists.txt` – root CMake; adds third_party, src/common, src/ecs, src/engine, src/game, src/runtime, src/editor, tests
- `CMakePresets.json` – build presets (`msvc-win64-vs2026-*`, `clang-win64-vs2026-*`)
- `assets/` – fonts, sounds, shaders, textures, plugins copied next to the executable at build time

Source
- `src/common/include/` – cross-layer headers and lock-free primitives
  - `lib.h` – logging, asserts, math helpers, array, macros, and allocation sizes
  - `ApplicationContext.h` – shared context + ring/snapshot wiring + `ECSCommandProcessor`
  - `SpscRing.h` / `Seqlock.h` – lock-free SPSC ring and seqlock snapshot primitives
  - `ECS.h` / `ECSCommands.h` – ECS types and the editor→game command pattern
  - `input.h` – input model (keys, mouse, event helpers)
  - `sound.h` – sound types/ring kept as a future-audio reference (not part of any build)
- `src/engine/` – the reusable runtime core (`Engine.dll`); ImGui-free
  - `src/core/Application.cpp` – owns `ApplicationContext`, spawns the three threads, takes an optional overlay factory
  - `src/threading/` – `PlatformThread`, `GameThread`, `RenderThread`, and `GameLibrary` (Game.dll hot-reload)
  - `src/rendering/` – `Renderer`, `RendererBackend` (DX12/Vulkan), passes (`Primitive`/`Mesh`/`Ui`), `MeshSystem`/`MaterialSystem`, `ShaderCompiler`
  - `src/plugins/` – `DotNetPluginManager` / `DotNetPluginHost` (.NET plugin host)
  - `src/memory/` (+ `AllocatorRegistry.cpp`) – Arena/Pool allocator toolkit and registry
  - `include/IOverlay.h` – the interface a tooling overlay implements to hook into the renderer
- `src/editor/` – the dev executable: `Engine` core plus the ImGui tooling layer
  - `src/main.cpp` – thin `main`; injects an `ImGuiOverlay` factory into `Application::Init`
  - `src/rendering/imgui/` – `ImGuiOverlay`, `ImGuiRenderer` (panels/inspector/gizmos), `MemoryPanel`, `MeshPreviewRenderer`, `imgui_nvrhi`
  - `src/alloc.h` – allocation-tracker hooks (editor-only)
- `src/runtime/` – the stripped player executable (`runtime.exe`)
  - `src/main.cpp` – thin `main` mirroring the editor's, but boots `Application` with no overlay → no ImGui
- `src/ecs/` – the ECS shared library (`ecs.dll`)
- `src/game/` – the hot-reloaded game library (`Game.dll`)
  - `src/Game.cpp` – exports `GameUpdate(GameState*)` and `GAME_API_VERSION`
- `tests/` – `test_ecs` and `test_alloc` unit-test targets

Third-party (vendored)
- `third_party/NVRHI` – Rendering Hardware Interface used for DX12/DX11/Vulkan
- `third_party/imgui` + `ImGuizmo` – Dear ImGui core/backends and gizmos (editor layer only)
- `third_party/glm` – math
- `third_party/freetype` – font rasterizer
- `third_party/assimp` – model importer
- `third_party/dotnet` – nethost/hostfxr for the .NET plugin runtime
- `third_party/dxc-prebuilt` – DXC shader compiler (`dxcompiler.dll`)
- `third_party/tracy` – Tracy profiler client

---

## High-level architecture

The reusable runtime core lives in **`Engine.dll`**. Both executables are thin `main.cpp`s that construct an `Application` and call `Run()`; the only difference is whether they inject an ImGui overlay:

- `editor.exe` = `Engine` core + the ImGui tooling layer (passes an `ImGuiOverlay` factory to `Application::Init`).
- `runtime.exe` = `Engine` core with no overlay → no ImGui (a ship/play build).

Both link `ecs.dll` and load `Game.dll` at runtime.

Threads (coordinated by `ApplicationContext`, all inside `Engine`)

1) PlatformThread (main thread)
- Owns the GLFW window and pumps OS input. All GLFW calls stay here.
- Pushes `InputEvent`s into `InputRing` (SPSC) for the game thread.

2) GameThread
- Fixed-step simulation (default 60 Hz). Each tick: drains `ECSCommandRing` (editor edits), calls `game::GameUpdate` from the currently loaded `Game.dll`, then publishes a deep-copy ECS snapshot via `Seqlock<SimulationSnapshot>` + `std::atomic_store` on `shared_ptr<const ECS>`.
- Runs a worker thread for assimp/obj model loads and hosts the .NET plugins.

3) RenderThread
- Owns `Renderer` and the NVRHI backend (DX12 or Vulkan, chosen by `RendererAPI`).
- Each frame loads the ECS `shared_ptr` then the `SimulationSnapshot`, drains `RendererCommandRing` to upload meshes/materials, runs the gameplay passes, and (editor only) the injected ImGui overlay.

Game library (`src/game`)
- Exports `GameUpdate(GameState*)` and `GAME_API_VERSION`.
- Reads input from `g_GameState->PlatformInput` (the SPSC input ring), mutates `GameState` (including a `WorldManager` ECS façade), and posts `RendererCommand`s for new meshes/materials.
- `GameState` is owned by the host (`Engine`); the DLL holds only a pointer to it, so state survives hot reloads.

Renderer (`src/engine/src/rendering`)
- Front-end (`Renderer.{h,cpp}`) owns an NVRHI device through a `RendererBackend` and runs pluggable `IRenderPass`es:
  - A primitives pass (e.g., a ground grid on the Y=0 plane)
  - A mesh pass (GPU meshes/materials addressed by `MeshHandle` / `MaterialHandle`)
  - A UI text pass using the FreeType-baked R8 glyph atlas
- ImGui is not a built-in pass; tooling UI is layered in via an injected `IOverlay`.
- Shaders are compiled via DXC (`ShaderCompiler`).

---

## The hot-reload mechanism

Files
- `src/engine/src/threading/GameLibrary.{h,cpp}` – loads, validates, watches, and swaps `Game.dll`.
- `src/common/include/FileWatch.h` – `filewatch::FileWatch` used to detect on-disk changes.

How it works
- `Game.dll` is built by CMake (target `game`) into the shared output folder.
- A `filewatch::FileWatch` on `Game.dll` flags a pending reload when the file changes.
- Between game-thread ticks, `GameLibrary` copies the DLL to a timestamped filename (to avoid replacing a loaded module / file locks), loads it, and validates symbols:
  - `GAME_API_VERSION` must match the value the host was built against.
  - The `GameUpdate` entry point must exist.
- The old module is freed after the new one loads successfully. Cross-tick state in `GameState` is host-owned, so it survives the swap.

There is no separate overlay DLL: the ImGui tooling layer is compiled into `editor.exe` and injected as an `IOverlay`, not hot-reloaded.

---

## Renderer overview

Front-end (`src/engine/src/rendering/Renderer.{h,cpp}`)
- Owns an NVRHI device through a `RendererBackend` selected by `RendererAPI` (DX12 or Vulkan), creates the swapchain and command lists, builds pipelines, and loads fonts/atlas.
- Each frame runs the pluggable `IRenderPass`es and, if one is attached, the `IOverlay` (the editor's ImGui overlay), then presents.

Render passes (`src/engine/src/rendering/passes/`)
- `PrimitiveRenderPass` – procedural primitives (e.g., a ground grid on the Y=0 plane).
- `MeshRenderPass` – GPU meshes/materials owned by `MeshSystem` / `MaterialSystem`, addressed by `MeshHandle` / `MaterialHandle`.
- `UiRenderPass` – UI text using instanced quads sampling the FreeType R8 glyph atlas with straight alpha blending.

Backends (`src/engine/src/rendering/backends/`)
- `RendererBackendDX12` – DX12 queues, DXGI flip-discard swapchain, per-backbuffer fence, NVRHI device. `Present` can return `DXGI_STATUS_OCCLUDED` when the window is occluded.
- `RendererBackendVulkan` – Vulkan backend.

Shaders
- Compiled at build/load time via DXC (`ShaderCompiler`, uses `third_party/dxc-prebuilt/bin/x64/dxcompiler.dll`, copied next to the executable by CMake).

---

## ECS and the editor command pattern

- The ECS lives in `ecs.dll` (`src/ecs/`). Component types are registered through the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro in `ECS.h`.
- The editor cannot mutate the ECS directly across threads. ImGui edits push an `ECSCommand` (`src/common/include/ECSCommands.h`) into `ECSCommandRing` (SPSC); the GameThread drains it via `ECSCommandProcessor::ProcessCommands` before running game logic.
- Adding a new component type also requires registering it in both branches of `ECSCommandProcessor` in `ApplicationContext.h` (`ApplyComponentCommand` and `RemoveComponentByType`). See `docs/ECS_Threading_Architecture.md`.

---

## Platform layer (Windows)

- `PlatformThread` (`src/engine/src/threading/PlatformThread.cpp`) owns the GLFW window, pumps OS input, and pushes `InputEvent`s into the SPSC `InputRing`. All GLFW calls stay on this thread.
- Input model and key codes are defined in `src/common/include/input.h`.
- `SM_ASSERT` routes to a per-module `platform_debug_break` (defined in each exe and in `Engine`) that shows a MessageBox and breaks into the debugger.
- `sound.h` is kept as a future-audio reference and is not wired into any build (there is no XAudio2 backend at present).

---

## Controls and features

Controls are defined by the loaded `Game.dll`; consult `src/game/src/Game.cpp` for the current bindings.

On-screen
- A procedural ground grid (primitives pass) and any meshes the game requests (mesh pass).
- UI text via the UI pass.
- In `editor.exe`, the ImGui tooling layer (panels, inspector, gizmos, Memory panel). `runtime.exe` renders no ImGui.

---

## Development workflow

- Build and run `editor.exe` once from CMake.
- Iterate on game code by rebuilding just `Game.dll` (`--target game`); the host's `GameLibrary` watches the file, copies it to a timestamped filename, validates `GAME_API_VERSION`, and swaps it in between ticks.
- Changing `Game.h` (struct layout / new export) requires bumping `GAME_API_VERSION` and restarting the editor; changing `ECS.h` requires rebuilding `ecs.dll`, the host, and the game, then restarting. See `CLAUDE.md` for the full rules.
- For UI tooling work, edit `src/editor/src/rendering/imgui/` and rebuild `editor` (the overlay is compiled into the exe, not hot-reloaded).

Debugging tips
- `SM_ASSERT` shows a dialog and breaks in the debugger.
- Tracy zones are present (`ZoneScoped`, `FrameMark`); `TracyClient` is linked but `TRACY_ENABLE` is currently commented out — enable the define to capture.

---

## Build system notes

- Out-of-source builds live under `out/build/<preset>/`; binaries land in `bin/<Config>/` (`RUNTIME_DIR`).
- `Engine` is a shared library (`Engine.dll`); `ecs` is `ecs.dll`; `game` is `Game.dll` (loaded at runtime, not linked).
- Executable targets: `editor` (`editor.exe`) and `runtime` (`runtime.exe`).
- Unit-test targets: `test_ecs` (`All ECS tests passed.`) and `test_alloc` (`All allocator tests passed.`).

---

## Troubleshooting

- Black window or immediate exit
  - Check the console logs (colored `SM_*` macros). The asset-copy post-build step must have run; ensure `assets/` is present next to the executable. A renderer init failure shows a dialog suggesting `--backend=vulkan` / `--backend=directx12`.
- Present returns `DXGI_STATUS_OCCLUDED`
  - Expected when minimized or fully occluded.
- Hot reload doesn’t trigger
  - The file watcher tracks `Game.dll` in the output dir. Ensure your build writes `Game.dll` to the same location the running host loaded from.
- D3D12 device creation fails
  - Try `--backend=vulkan`, or relax the DX12 capability checks if you’re targeting broader hardware.

---

## Next steps and ideas

- Add a DXGI waitable swapchain for consistent frame pacing and background throttling.
- Expose a renderer plugin API for adding new passes from the game side.
- Flesh out the Vulkan backend.
- Wire up an audio backend (see `sound.h`).

---

## License

This repository includes third-party components under their respective licenses (see `third_party/*`). The project code in this repository is provided as-is for learning and experimentation.

