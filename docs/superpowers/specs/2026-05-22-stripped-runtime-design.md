# Stripped Runtime Player + Legacy Deletion (Editor/Runtime Separation — Part B) — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (Part A merged).

## Goal

Complete the editor/runtime separation. Part A put the ImGui-free runtime core in the `Engine`
shared lib and made the editor `Engine core + an injected ImGui overlay`. Part B:

1. **Repurpose the `runtime` target** into a **stripped player exe** (`runtime.exe`) that links
   `Engine`, boots `Application` with **no overlay** (no ImGui), and otherwise mirrors the
   editor's `main` (CLI `--backend` override + `SettingsManager` reuse).
2. **Delete the legacy single-threaded path** entirely — the old `runtime` sources, the
   `imgui_overlay` target (`src/overlay/`), the orphaned legacy headers (`*_old.h`, the legacy
   `ui.h`/`render.h`/`sound.h`), the four uncompiled `*_old.cpp` in `src/game`, and `build.bat`.
   This also clears the pre-broken `input.h` KEY_* redefinition, so a bare all-targets
   `cmake --build` goes green again.

End state: `editor.exe` (Engine core + ImGui tooling) and `runtime.exe` (Engine core, no ImGui,
ship/play build) share one core. No legacy relics remain. The editor is untouched.

## Background (verified)

- The `runtime` target currently builds `main.exe` from a **single-threaded** legacy
  `src/runtime/src/main.cpp` using a legacy non-NVRHI renderer + a hot-reloaded
  `imgui_overlay.dll`. The **entire `src/runtime/src/` tree is legacy/deletable**; the new player
  needs only a fresh `main.cpp`.
- `imgui_overlay` (`src/overlay/`, a C-API hot-reload DLL) is referenced **only** by the legacy
  runtime (and docs). The editor's `ImGuiOverlay` is an unrelated C++ class in
  `src/editor/src/rendering/imgui/` — same name, different mechanism; it does **not** use
  `src/overlay`.
- The `input.h` KEY_* break: `src/common/include/input.h` (engine, `enum KeyCode : uint16_t`)
  and `src/common/include/input_old.h` (legacy, `enum KeyCodeID`) both define `KEY_A`/`KEY_SPACE`/
  etc. with different values. They only collide in the legacy runtime TU (which transitively
  includes both via `platform.h`/`renderer.h` and `ui.h`/`render.h`). Deleting the legacy path
  removes the only TU that sees both → conflict gone. `input_old.h` is reached only via `ui.h`/
  `render.h`/legacy main; `ui.h`/`render.h`/`sound.h` only via `game_old.h` + legacy runtime; the
  engine includes none of them.
- `Application::Init(std::optional<RendererAPI> backendOverride = {}, OverlayFactory overlayFactory = {})`
  (Engine). Passing no factory → empty `std::function` → `RenderThread` never calls `SetOverlay`
  → no overlay → zero ImGui. Confirmed.
- `RUNTIME_DIR = ${CMAKE_BINARY_DIR}/bin/$<CONFIG>`. Engine's POST_BUILD already copies
  `nethost.dll`/`hostfxr.dll`/`dxcompiler.dll` there; `Engine.dll`/`Game.dll`/`ecs.dll` land there
  by their own output dirs. So the stripped runtime needs **no DLL-copy** of its own. The editor
  copies `assets/` there; the runtime needs its own `assets` copy to be self-sufficient when built
  without the editor.
- `platform_debug_break` is declared **non-exported** in `src/common/include/lib.h` and used by
  `SM_ASSERT`. Each module needs its own definition (Engine has `PlatformDebug.cpp`; editor has one
  in `main.cpp`). The stripped runtime exe needs **its own** definition.

## Scope

**In scope:** a new `src/runtime/src/main.cpp`; a rewritten `src/runtime/CMakeLists.txt`
(`OUTPUT_NAME runtime`); deletion of the full legacy set; the one root-CMake edit (drop
`add_subdirectory(src/overlay)`).

**Out of scope / unchanged:** `editor`, `Engine`, `ecs`, `game`, `GAME_API_VERSION`, the engine's
`input.h`. No new engine features. The editor remains `VS_STARTUP_PROJECT`.

## Design

### New `src/runtime/src/main.cpp` (the stripped player)

Mirror `src/editor/src/main.cpp` MINUS the editor-only pieces:

- **Keep:** includes `<optional> <string_view> <filesystem> "lib.h" "Application.h"
  "utilities/SettingsManager.h"`; the `PrintUsage`/`ParseCli` block (`--help`/`-h`,
  `--backend=<vulkan|vk|directx12|dx12>` via `SettingsManager::ParseBackend`, last-wins, reject
  `Invalid`) with usage text naming `runtime.exe`; `main()` does
  `Application app; if (!app.Init(cli.override_)) { /* error MessageBox + return -1 */ } app.Run();`.
  **No second `Init` argument** → no overlay factory → no ImGui.
- **Own `platform_debug_break`** definition (copy the editor's: `sprintf_s` a message →
  `MessageBoxA` → `DEBUG_BREAK()`). Required because it's non-exported and the runtime's TUs use
  `SM_ASSERT`.
- **Drop:** `#include "alloc.h"`, the `std::atexit([]{ DumpAllocations(); })` line,
  `#include "rendering/imgui/ImGuiOverlay.h"`, and the overlay factory lambda.

The player thus boots the same 3-thread Engine core: PlatformThread (window+input), GameThread
(loads `assets/models`, runs `Game.dll`, ECS), RenderThread (NVRHI render of gameplay passes) —
with no editor UI.

### Rewritten `src/runtime/CMakeLists.txt`

```cmake
add_executable(runtime src/main.cpp)

target_include_directories(runtime PRIVATE src)

target_compile_definitions(runtime PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN)

target_link_libraries(runtime PRIVATE
    Engine ecs CommonHeaders GameHeaders glm::glm glfw
    nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk d3dcompiler dxgi dxcompiler
    freetype Tracy::TracyClient)
if (TARGET Vulkan-Headers)
    target_link_libraries(runtime PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(runtime PRIVATE Vulkan::Headers)
endif()

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME runtime
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}")

if(MSVC)
    target_compile_options(runtime PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

add_dependencies(runtime game)
add_custom_command(TARGET runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets)
```

(The third-party libs are largely transitive via `Engine` PUBLIC; listing the ones the exe links
directly mirrors the editor and is harmless. No nethost/dxc copy — Engine handles it. The
`runtime` target stays in the default build, NOT `EXCLUDE_FROM_ALL`.)

### Root `CMakeLists.txt`

Remove the `add_subdirectory(src/overlay)` line (+ its comment). Keep `add_subdirectory(src/runtime)`
(the dir persists, rewritten). `VS_STARTUP_PROJECT editor`, `RUNTIME_DIR`, and the rest unchanged.
Resulting subdir order: `third_party → common → ecs → engine → game → runtime → editor → tests`.

### Full legacy deletion set

- **Directory:** `src/overlay/` (entire — `CMakeLists.txt`, `include/imgui_overlay.h`,
  `src/imgui_overlay.cpp`, `src/imgui_nvrhi.cpp`, `src/registered_font.cpp`) → removes the
  `imgui_overlay` target.
- **Legacy runtime sources** (all of `src/runtime/src/` except the new `main.cpp`): old `main.cpp`
  (replaced), `renderer.{h,cpp}`, `renderer_common.h`, `renderer_dx11.{h,cpp}`,
  `renderer_dx12.{h,cpp}`, `renderer_vulkan.{h,cpp}`, `image.{h,cpp}`, `font.{h,cpp}`,
  `VertexPacked.h`, `platform.h`, `windows_platform.cpp`.
- **Orphaned legacy common headers:** `src/common/include/input_old.h`, `ui.h`, `render.h`,
  `sound.h`.
- **Orphaned legacy game files:** `src/game/include/game_old.h`; `src/game/src/game_old.cpp`,
  `game_in_level_old.cpp`, `game_editor_old.cpp`, `game_main_menu_old.cpp` (none are in the `game`
  build today).
- **`build.bat`** (root) — legacy clang single-cpp `game.dll` build; nothing references it.

Before deleting each orphaned header, the implementer greps to confirm no live (non-legacy)
includer remains; the analysis shows none, but the grep is the safety check.

## Phasing (builds green at each step)

- **B1 — repurpose runtime.** Replace `src/runtime/src/main.cpp` with the stripped player; rewrite
  `src/runtime/CMakeLists.txt` (`OUTPUT_NAME runtime`, links Engine, assets copy); remove
  `add_subdirectory(src/overlay)` from root CMake. The `runtime` source list now references only
  the new `main.cpp`, so the legacy `src/runtime/src/*` files are unreferenced (still on disk) and
  `imgui_overlay` is no longer built. Build the `runtime` target → `runtime.exe` links clean.
- **B2 — delete the legacy.** `git rm` the full deletion set (overlay dir, legacy runtime sources,
  orphaned common/game headers + `*_old.cpp`, build.bat). Then a **bare all-targets
  `cmake --build --preset msvc-win64-vs2026-community`** must succeed (the KEY_* conflict is gone,
  no dangling references) and `test_ecs`/`test_alloc` stay green.

## Build / verification

No `GAME_API_VERSION` bump; `Engine`/`ecs`/`game`/`editor` unchanged. Build preset
`msvc-win64-vs2026-community`. Verification (build + parity — no new unit-testable logic):
- `runtime` builds → `runtime.exe` in `RUNTIME_DIR`.
- **Bare all-targets `cmake --build`** succeeds (previously failed on the legacy `runtime`'s
  `input.h` KEY_* conflict).
- `test_ecs` → `All ECS tests passed.`; `test_alloc` → `All allocator tests passed.`
- `editor` still builds + runs unchanged.
- `dumpbin /exports` is irrelevant here (no new lib); instead confirm `runtime.exe` links no
  ImGui (it doesn't include or link imgui/ImGuizmo).
- **GUI smoke (user):** launch `runtime.exe` — window opens, scene renders (models from
  `assets/`), responds to input, **no ImGui** anywhere; clean exit.

## Risks

- **An orphaned-header deletion is premature** (some live TU still includes it) → compile error,
  caught immediately by the B2 all-targets build; mitigated by the pre-delete grep. The analysis
  shows `ui.h`/`render.h`/`sound.h`/`input_old.h`/`game_old.h` are legacy-only.
- **Assets missing for a player-only build** → handled by the runtime's own `assets` copy POST_BUILD.
- **`platform_debug_break` omitted** → link error (unresolved external) in the runtime exe, caught
  at B1 build; mitigated by including the definition in the new `main.cpp`.
- **Legacy `game/src/*_old.cpp` were silently not built** — confirm the `game` target's source list
  doesn't reference them before deleting (it builds only `src/Game.cpp`), so deletion is a no-op for
  the `game` build.
