# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

CMake presets (out-of-source in `out/build/<preset>/`):

```
cmake --preset clang-win64-vs2026-community     # or msvc-/enterprise variants
cmake --build out/build/clang-win64-vs2026-community
```

Use the `clang-win64-vs2026-*` / `msvc-win64-vs2026-*` presets above. Binaries land in `<binaryDir>/bin/<Config>/`. A CMake post-build step copies `assets/` next to the executable.

Two executable targets are built, both layered on the shared `Engine` runtime core (`Engine.dll`, see Architecture below):

- `editor` — current dev target (`VS_STARTUP_PROJECT` in root `CMakeLists.txt`). The `Engine` core **plus an ImGui tooling layer** (panels/inspector/gizmos/Memory panel), attached by injecting an `ImGuiOverlay` (which implements the engine's `IOverlay` interface) into the renderer. **Most work happens here.**
- `runtime` (output `runtime.exe`) — the stripped player/ship build: links `Engine` and boots `Application` with **no overlay → no ImGui**. `src/runtime/src/main.cpp` is a thin `main` mirroring the editor's minus the ImGui/alloc-tracker bits (parses `--backend=`, reuses `SettingsManager`).

Both exes link `ecs.dll` and load `Game.dll` at runtime via `GameLibrary` (which lives in `Engine`). `Game.dll` is built by CMake (target `game`) and hot-reloaded; there is no `build.bat`.

`test_ecs` (ECS) and `test_alloc` (allocator) are the unit-test targets. Run e.g.:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected output: `All ECS tests passed.` (and `All allocator tests passed.` for `test_alloc`).

## Hot-reloading the game library

`game` is built as a SHARED library (`Game.dll`) and is loaded at runtime via `GameLibrary` (`src/engine/src/threading/GameLibrary.{h,cpp}`, part of the `Engine` core, so both `editor` and `runtime` hot-reload it). A `filewatch::FileWatch` on `Game.dll` triggers a reload between ticks when the file changes on disk.

Typical iteration:

1. Edit `src/game/src/Game.cpp`.
2. `cmake --build --preset msvc-win64-vs2026-community --target game`
3. Editor reloads automatically within ~1 second. Console logs the new timestamped DLL filename and the API version that loaded.

State preservation: cross-tick game state lives in `GameState` (`src/game/include/Game.h`) which is editor-owned. The DLL holds only a pointer. File-static globals in `Game.cpp` are per-tick scratch only and reset on reload — intentional.

Rules:

- **Change `.cpp` only** → hot-reload works. No editor restart.
- **Change `Game.h` (struct layout, new export, etc.)** → bump `GAME_API_VERSION`. Rebuild **both** game and editor. Restart editor (the running `editor.exe` still has the old `GameState` layout linked in).
- **Change `ECS.h` (new component type, etc.)** → rebuild `ecs.dll`, editor, and game. Restart editor.

ECS code lives in `ecs.dll` (`src/ecs/`). All `ComponentArray<T>` template instantiations are explicit, driven by the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro in `ECS.h`. Adding a new component type:

1. Declare the struct in `ECS.h`.
2. Add `X(NewType)` to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
3. Add handling to `ECSCommandProcessor::ApplyComponentCommand` and `RemoveComponentByType` in `src/common/include/ECSCommands.h`.
4. (Optional) Add inspector UI in `ImGuiRenderer.cpp`.

## Architecture

### Targets and their relationships

The reusable runtime core lives in **`Engine`** (`Engine.dll`, source under `src/engine/`): the 3-thread model, the NVRHI `Renderer` + gameplay render passes, the GPU resource systems, `GameLibrary` hot-reload, the .NET plugin host, and the allocator toolkit. `Engine` is **ImGui-free**. Both executables consume it; `editor` adds the ImGui overlay layer on top.

```
editor.exe  ─links→ Engine (core) + ecs + imgui + ImGuizmo
            ─loads at runtime→ Game.dll (via GameLibrary + filewatch hot-reload)
runtime.exe ─links→ Engine (core) + ecs
            ─loads at runtime→ Game.dll (via GameLibrary + filewatch hot-reload)
Engine.dll  ─links→ ecs + nvrhi (DX12/DX11/VK) + freetype + assimp + nethost
            ─loads at runtime→ Game.dll
```

`runtime` and `editor` are independent thin `main.cpp`s over the same `Engine` core; they do not share a `main.cpp`.

### Threading model (Engine)

The runtime is a three-thread design coordinated through `ApplicationContext` (`src/common/include/ApplicationContext.h`). `src/engine/src/core/Application.cpp` owns the context and spawns:

- **PlatformThread** (main thread) — owns the GLFW window, pumps input, pushes `InputEvent`s into `ApplicationContext::InputRing` (SPSC, 256). All GLFW calls must stay on this thread.
- **GameThread** (`src/engine/src/threading/GameThread.cpp`) — fixed-step simulation (default 60 Hz). Each tick: drains `ECSCommandRing` → calls `game::GameUpdate` (the hot-reloaded `Game.dll`) → builds a deep-copy ECS snapshot → publishes via `Seqlock<SimulationSnapshot>` + `std::atomic_store` on `shared_ptr<const ECS>`. Also runs a single worker thread for assimp/obj model loads, posting results back via internal queues.
- **RenderThread** (`src/engine/src/threading/RenderThread.cpp`) — owns `Renderer` and the NVRHI backend. Each frame: **load the ECS `shared_ptr` BEFORE the `SimulationSnapshot`** (see `docs/ECS_Threading_Architecture.md` issue 1) to avoid a dangling raw pointer. Drains `RendererCommandRing` to upload meshes/materials requested by the game. The optional ImGui overlay (editor only) is injected here as an `IOverlay`.

Communication is lock-free: `Seqlock<T>` (`src/common/include/Seqlock.h`, requires trivially-copyable T) for state snapshots, `SpscRing<T,N>` (`src/common/include/SpscRing.h`, power-of-two N) for command/event streams. The ECS itself crosses threads via `std::atomic_store/load` on `shared_ptr<const ECS>` — GameThread owns the master, RenderThread reads immutable snapshots.

Expected latency from ImGui edit → visible change is 1–2 frames.

### ECS command pattern (RenderThread → GameThread)

ImGui edits in the editor cannot mutate ECS directly. They push an `ECSCommand` (`src/common/include/ECSCommands.h`) into `ECSCommandRing` (SPSC, 128, declared in `ApplicationContext.h`). GameThread drains it via `ECSCommandProcessor::ProcessCommands` *before* running game logic.

**When adding a new component type**, also register it in both branches of `ECSCommandProcessor` in `src/common/include/ECSCommands.h`:
- `ApplyComponentCommand` — `AddComponent`/`ModifyComponent` dispatch
- `RemoveComponentByType` — `RemoveComponent` dispatch

Forgetting this is silent (commands queue successfully, world stays unchanged).

### Renderer (Engine)

`src/engine/src/rendering/Renderer.{h,cpp}` is the front-end; it owns an NVRHI device through a `RendererBackend` (DX12 or Vulkan, selected by `RendererAPI` passed to the `RenderThread` constructor). The pipeline is **deferred** (opaque geometry → G-buffer, then full-screen lighting) and built from pluggable `IRenderPass`es. The active pass order (see `Renderer::Init` / `Renderer::InitForSwap`) is:
- `ShadowDepthPass` — directional shadow depth map (ortho frustum fit to the visible-mesh AABB).
- `GBufferFillPass` — opaque geometry into a G-buffer (albedo / world-normal / world-position MRT + depth).
- `LightingRenderPass` — full-screen deferred lighting: directional + point lights, 3x3 PCF shadows, exponential distance fog; reads the G-buffer.
- `SkyRenderPass` — full-screen procedural sky: day/night gradient + sun/moon discs (far-plane depth test).
- `OutlineRenderPass` — selection silhouette.
- `DebugRenderPass` — debug line gizmos + the editor ground grid (camera-snapped lines via `DebugAppendGrid`, gated by the `ShowGrid` debug flag the editor sets on startup; runtime leaves it off).
- `UiRenderPass` — UI text/quads (instanced, FreeType R8 atlas).

ImGui is **not** a built-in pass — tooling UI is layered in via an injected `IOverlay` (the editor's `ImGuiOverlay`/`ImGuiRenderer`, which live in `src/editor/src/rendering/imgui/`); `runtime` injects no overlay. Mesh/material GPU resources are owned by `MeshSystem` / `MaterialSystem` and addressed by handle (`MeshHandle`, `MaterialHandle`). Atmosphere is authored per-scene as ECS **singleton components** — `FogComponent` and `SkyComponent` (declared in `src/common/include/ECS.h`), seeded by the game and read off the ECS snapshot by `Renderer`/`LightingRenderPass`/`SkyRenderPass` (default-constructed if unset). They are persisted in `world.json`'s top-level `"Environment"` block alongside `DayNightConfigComponent` ((de)serialized in `src/common/include/ComponentSerialization.h`), and edited in the editor's **Atmosphere** panel (`DayNightPanel.cpp`) via the ECS command ring. `ComputeFog()` (`Fog.{h,cpp}`) maps sun elevation → fog color/density; sky shading lives in `SkyRenderPass`'s shader. The day/night cycle is driven by `DayNightSystem` in `src/game/src/game.cpp`, which moves the sun light and writes the `AtmosphereStateComponent` singleton (consumed by `LightingRenderPass`); tunables live in `DayNightConfigComponent`. Shaders are compiled via DXC (`ShaderCompiler.cpp`, depends on `third_party/dxc-prebuilt/bin/x64/dxcompiler.dll`, copied to output by CMake).

### .NET plugins (Engine)

`DotNetPluginManager` / `DotNetPluginHost` (`src/engine/src/plugins/`) bootstrap a .NET runtime via `nethost.lib` (vendored in `third_party/dotnet/`). Plugins live in `assets/plugins/` and are loaded on GameThread.

### Game library

`src/game/src/Game.cpp` exports `GameUpdate(GameState*)` and `GAME_API_VERSION`. The host loads `Game.dll` at runtime via `GameLibrary` (`src/engine/src/threading/GameLibrary.{h,cpp}`) and hot-reloads it when the file changes on disk (see "Hot-reloading the game library" above). `GameUpdate` reads input from `g_GameState->PlatformInput` (the SPSC input ring), mutates `GameState` (including a `WorldManager` ECS façade), and posts `RendererCommand`s for new meshes/materials. `GameState` is owned by the host (`Engine`); the DLL holds only a pointer to it.

## Conventions

- C++23, `CMAKE_CXX_STANDARD 23`, exceptions+RTTI enabled.
- Windows-only (`NOMINMAX`, `WIN32_LEAN_AND_MEAN` defined globally).
- GLM forced to depth `[0,1]`, right-handed, `GLM_ENABLE_EXPERIMENTAL`. Match in any new target.
- Asserts via `SM_ASSERT` (`src/common/include/lib.h`) → `platform_debug_break` shows a MessageBox and `DEBUG_BREAK()`s.
- Logging: `SM_TRACE` / `SM_WARN` / `SM_ERROR` (colored). Use them, not `printf`/`std::cout`.
- Tracy is built in (`TracyClient` linked into `Engine` and the exes) but `TRACY_ENABLE` is currently commented out in editor's CMakeLists; enabling it requires the define.
- Targets list every `.cpp` explicitly in their `CMakeLists.txt` (no globbing). New source files must be added there manually: engine sources go in `src/engine/CMakeLists.txt`, ImGui editor-layer sources in `src/editor/CMakeLists.txt`.

## Pointers to deeper docs

- `README.md` — full code tour of the current architecture: the shared `Engine` core and the two thin executables (`editor` with the ImGui overlay, `runtime` as the stripped player) layered on it, plus the `GameLibrary` hot-reload mechanism.
- `docs/ECS_Threading_Architecture.md` — detailed walkthrough of the snapshot/command threading model, including subtle ordering rules and how to register new component types.
