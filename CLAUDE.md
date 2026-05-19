# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

CMake presets (out-of-source in `out/build/<preset>/`):

```
cmake --preset clang-win64-vs2026-community     # or msvc-/enterprise variants
cmake --build out/build/clang-win64-vs2026-community
```

Older flow (`README.md` quick-start) uses `cmake --preset debug` writing to `build/debug/`. That preset does not exist in `CMakePresets.json`; use the `clang-win64-vs2026-*` / `msvc-win64-vs2026-*` presets above. Binaries land in `<binaryDir>/bin/<Config>/`. A CMake post-build step copies `assets/` next to the executable.

Two executable targets are built:

- `editor` — current dev target (`VS_STARTUP_PROJECT` in root `CMakeLists.txt`). Multi-threaded, NVRHI-based, ImGui editor, ECS, .NET plugin host. **Most work happens here.**
- `runtime` (output `main.exe`) — older single-threaded host with DLL hot-reload. Still builds. Architecture described in `README.md` is the *runtime* path; large parts (game-as-DLL, `build.bat`) are stale relative to the current `editor` codebase.

`build.bat` rebuilds the legacy `game.dll` for the `runtime` hot-reload flow. It is **not** how the editor builds `game`: in the current `src/game/CMakeLists.txt`, `game` is a `SHARED` library (`Game.dll`) that the editor loads at runtime via `GameLibrary`. Don't run `build.bat` expecting it to affect the editor.

`test_ecs` is the ECS unit-test target. Run it with:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected output: `All ECS tests passed.`

## Hot-reloading the game library

`game` is built as a SHARED library (`Game.dll`) and is loaded at runtime by the editor via `GameLibrary` (`src/editor/src/threading/GameLibrary.{h,cpp}`). A `filewatch::FileWatch` on `Game.dll` triggers a reload between ticks when the file changes on disk.

Typical iteration:

1. Edit `src/game/src/Game.cpp`.
2. `cmake --build --preset msvc-win64-vs2026-enterprise --target game`
3. Editor reloads automatically within ~1 second. Console logs the new timestamped DLL filename and the API version that loaded.

State preservation: cross-tick game state lives in `GameState` (`src/game/include/Game.h`) which is editor-owned. The DLL holds only a pointer. File-static globals in `Game.cpp` are per-tick scratch only and reset on reload — intentional.

Rules:

- **Change `.cpp` only** → hot-reload works. No editor restart.
- **Change `Game.h` (struct layout, new export, etc.)** → bump `GAME_API_VERSION`. Rebuild **both** game and editor. Restart editor (the running `editor.exe` still has the old `GameState` layout linked in).
- **Change `ECS.h` (new component type, etc.)** → rebuild `ecs.dll`, editor, and game. Restart editor.

ECS code lives in `ecs.dll` (`src/ecs/`). All `ComponentArray<T>` template instantiations are explicit, driven by the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro in `ECS.h`. Adding a new component type:

1. Declare the struct in `ECS.h`.
2. Add `X(NewType)` to `ECS_FOR_EACH_REGISTERED_COMPONENT`.
3. Add handling to `ECSCommandProcessor::ApplyComponentCommand` and `RemoveComponentByType` in `ApplicationContext.h`.
4. (Optional) Add inspector UI in `ImGuiRenderer.cpp`.

## Architecture

### Targets and their relationships

```
editor.exe  ─links→ ecs (SHARED) + nvrhi (DX12/DX11/VK) + imgui + ImGuizmo + assimp + freetype + nethost
             ─loads at runtime→ Game.dll (via GameLibrary + filewatch hot-reload)
runtime.exe ─links→ nvrhi + freetype + tracy   (loads game.dll at runtime via build.bat)
```

`runtime` and `editor` are independent; they do not share `main.cpp`.

### Editor threading model

The editor is a three-thread design coordinated through `ApplicationContext` (`src/common/include/ApplicationContext.h`). `src/editor/src/core/Application.cpp` owns the context and spawns:

- **PlatformThread** (main thread) — owns the GLFW window, pumps input, pushes `InputEvent`s into `ApplicationContext::InputRing` (SPSC, 256). All GLFW calls must stay on this thread.
- **GameThread** (`src/editor/src/threading/GameThread.cpp`) — fixed-step simulation (default 60 Hz). Each tick: drains `ECSCommandRing` → calls `game::GameUpdate` (links the static `game` lib) → builds a deep-copy ECS snapshot → publishes via `Seqlock<SimulationSnapshot>` + `std::atomic_store` on `shared_ptr<const ECS>`. Also runs a single worker thread for assimp/obj model loads, posting results back via internal queues.
- **RenderThread** (`src/editor/src/threading/RenderThread.cpp`) — owns `Renderer` and the NVRHI backend. Each frame: **load the ECS `shared_ptr` BEFORE the `SimulationSnapshot`** (see `docs/ECS_Threading_Architecture.md` issue 1) to avoid a dangling raw pointer. Drains `RendererCommandRing` to upload meshes/materials requested by the game.

Communication is lock-free: `Seqlock<T>` (`src/common/include/Seqlock.h`, requires trivially-copyable T) for state snapshots, `SpscRing<T,N>` (`src/common/include/SpscRing.h`, power-of-two N) for command/event streams. The ECS itself crosses threads via `std::atomic_store/load` on `shared_ptr<const ECS>` — GameThread owns the master, RenderThread reads immutable snapshots.

Expected latency from ImGui edit → visible change is 1–2 frames.

### ECS command pattern (RenderThread → GameThread)

ImGui edits in the editor cannot mutate ECS directly. They push an `ECSCommand` (`src/common/include/ECSCommands.h` + `ApplicationContext.h`) into `ECSCommandRing` (SPSC, 128). GameThread drains it via `ECSCommandProcessor::ProcessCommands` *before* running game logic.

**When adding a new component type**, also register it in both branches of `ECSCommandProcessor` in `ApplicationContext.h`:
- `ApplyComponentCommand` — `AddComponent`/`ModifyComponent` dispatch
- `RemoveComponentByType` — `RemoveComponent` dispatch

Forgetting this is silent (commands queue successfully, world stays unchanged).

### Renderer

`src/editor/src/rendering/Renderer.{h,cpp}` is the front-end; it owns an NVRHI device through a `RendererBackend` (DX12 or Vulkan, selected by `RendererAPI` passed to the `RenderThread` constructor). Render passes are pluggable (`IRenderPass`): `PrimitiveRenderPass`, `MeshRenderPass`, `UiRenderPass`, plus the ImGui pass via `ImGuiRenderer`. Mesh/material GPU resources are owned by `MeshSystem` / `MaterialSystem` and addressed by handle (`MeshHandle`, `MaterialHandle`). Shaders are compiled via DXC (`ShaderCompiler.cpp`, depends on `third_party/dxc-prebuilt/bin/x64/dxcompiler.dll`, copied to output by CMake).

### .NET plugins

`DotNetPluginManager` / `DotNetPluginHost` (`src/editor/src/plugins/`) bootstrap a .NET runtime via `nethost.lib` (vendored in `third_party/dotnet/`). Plugins live in `assets/plugins/` and are loaded on GameThread.

### Game library

`src/game/src/Game.cpp` exports `GameUpdate(GameState*)` and `GAME_API_VERSION`. The editor loads `Game.dll` at runtime via `GameLibrary` (`src/editor/src/threading/GameLibrary.{h,cpp}`) and hot-reloads it when the file changes on disk (see "Hot-reloading the game library" above). `GameUpdate` reads input from `g_GameState->PlatformInput` (the SPSC input ring), mutates `GameState` (including a `WorldManager` ECS façade), and posts `RendererCommand`s for new meshes/materials. `GameState` is editor-owned; the DLL holds only a pointer to it.

## Conventions

- C++23, `CMAKE_CXX_STANDARD 23`, exceptions+RTTI enabled.
- Windows-only (`NOMINMAX`, `WIN32_LEAN_AND_MEAN` defined globally).
- GLM forced to depth `[0,1]`, right-handed, `GLM_ENABLE_EXPERIMENTAL`. Match in any new target.
- Asserts via `SM_ASSERT` (`src/common/include/lib.h`) → `platform_debug_break` shows a MessageBox and `DEBUG_BREAK()`s.
- Logging: `SM_TRACE` / `SM_WARN` / `SM_ERROR` (colored). Use them, not `printf`/`std::cout`.
- Tracy is built in (`TracyClient.cpp` compiled into each exe) but `TRACY_ENABLE` is currently commented out in editor's CMakeLists; enabling it requires the define.
- The editor target lists every `.cpp` explicitly in `src/editor/CMakeLists.txt` (no globbing). New source files must be added there manually.

## Pointers to deeper docs

- `README.md` — full code tour of the **runtime** (`main.exe`) architecture and the legacy DLL hot-reload mechanism. Read with the caveat that the active dev target is now `editor`.
- `docs/ECS_Threading_Architecture.md` — detailed walkthrough of the editor's snapshot/command threading model, including subtle ordering rules and how to register new component types.
