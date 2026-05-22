# Stripped Runtime Player + Legacy Deletion (Separation Part B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repurpose the `runtime` target into a stripped `runtime.exe` (links Engine, boots `Application` with no overlay → no ImGui) and delete the entire legacy single-threaded runtime/overlay path.

**Architecture:** Phase B1 replaces the legacy `runtime` main + CMake with a thin player that mirrors the editor's `main` minus ImGui/alloc-tracker, and drops `src/overlay` from the root CMake — `runtime.exe` builds while the now-unreferenced legacy files still sit on disk. Phase B2 `git rm`s the full dead set (overlay dir, legacy runtime sources, orphaned `*_old`/legacy common headers, `build.bat`), after which a bare all-targets build goes green (the legacy `input.h` KEY_* conflict is gone).

**Tech Stack:** C++23, CMake (preset `msvc-win64-vs2026-community`), the `Engine` shared lib (runtime core), GLFW/NVRHI.

**Spec:** `docs/superpowers/specs/2026-05-22-stripped-runtime-design.md`

**Branch:** `stripped-runtime` (off `main`, already checked out). Do NOT merge or offer to merge.

**Global rules:** No `GAME_API_VERSION` bump. `editor`/`Engine`/`ecs`/`game` are NOT modified. This is a structural repurpose + deletion — verification is build + behavioral parity, NOT new unit tests. Commit as `Nuno Silva <nuno.levezinho@live.com.pt>`; never stage `.claude/`. Benign "LF will be replaced by CRLF" warnings are expected.

---

## Phase B1 — Repurpose the runtime target into the stripped player

### Task 1: New stripped `runtime` main + CMake + drop overlay from root

**Files:**
- Replace: `src/runtime/src/main.cpp` (overwrite the legacy single-threaded main entirely)
- Rewrite: `src/runtime/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root — remove the `add_subdirectory(src/overlay)` line)

- [ ] **Step 1: Overwrite `src/runtime/src/main.cpp`**

Replace the ENTIRE contents of `src/runtime/src/main.cpp` with the stripped player (the editor's `main.cpp` minus `alloc.h`/`DumpAllocations`, minus the `ImGuiOverlay` include + overlay factory; usage/error text says runtime; keeps `ParseCli` + its own `platform_debug_break`):

```cpp
#include <algorithm>
#include <GLFW/glfw3.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <filesystem>
#include <windows.h>

#include "lib.h"
#include "Application.h"
#include "utilities/SettingsManager.h"

// Per-module SM_ASSERT backend (platform_debug_break is declared non-exported in lib.h,
// so each executable provides its own definition; mirrors editor.exe / Engine.dll).
void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    char buffer[2048] = {};
    sprintf_s(buffer,
            "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
            (expr ? expr : "<none>"),
            (file ? file : "<unknown>"),
            line,
            (message ? message : "<no message>"));

    MessageBoxA(nullptr, buffer, "Assertion Failed", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);

    DEBUG_BREAK();
}

namespace {
    void PrintUsage() {
        std::printf("Usage: runtime.exe [--backend=<api>] [--help]\n"
                    "\n"
                    "  --backend=<api>   Override the persisted renderer backend for this run\n"
                    "                    only. Does NOT modify editor_settings.json.\n"
                    "                    Valid values: vulkan, vk, directx12, dx12.\n"
                    "                    (directx11/dx11 is reserved but the backend is not\n"
                    "                    implemented yet.)\n"
                    "  --help, -h        Print this help and exit.\n");
    }

    struct CliResult {
        std::optional<RendererAPI> override_;
        bool parseOk = true;
        bool helpRequested = false;
    };

    CliResult ParseCli(int argc, char** argv) {
        CliResult r;
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                r.helpRequested = true;
                continue;
            }
            constexpr std::string_view kBackend = "--backend=";
            if (arg.substr(0, kBackend.size()) == kBackend) {
                const std::string_view value = arg.substr(kBackend.size());
                const RendererAPI parsed = SettingsManager::ParseBackend(value);
                if (parsed == RendererAPI::Invalid) {
                    std::printf("Error: invalid --backend value '%.*s'.\n\n",
                                static_cast<int>(value.size()), value.data());
                    r.parseOk = false;
                    return r;
                }
                r.override_ = parsed; // last occurrence wins
                continue;
            }
            std::printf("Error: unknown argument '%.*s'.\n\n",
                        static_cast<int>(arg.size()), arg.data());
            r.parseOk = false;
            return r;
        }
        return r;
    }
}

int main(int argc, char** argv) {
    SM_TRACE("Working Directory: %s", std::filesystem::current_path().string().c_str());

    const CliResult cli = ParseCli(argc, argv);
    if (!cli.parseOk) {
        PrintUsage();
        return 1;
    }
    if (cli.helpRequested) {
        PrintUsage();
        return 0;
    }

    Application app;
    if (!app.Init(cli.override_)) {   // no overlay factory -> no ImGui
        SM_ERROR("Application initialization failed!");
        MessageBoxA(nullptr,
                    "Renderer initialization failed.\n\n"
                    "Edit editor_settings.json next to runtime.exe, or relaunch with\n"
                    "  --backend=vulkan\n"
                    "  --backend=directx12\n",
                    "Runtime — startup failure",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return -1;
    }

    app.Run();

    SM_TRACE("Shutdown complete.");
    return 0;
}
```

- [ ] **Step 2: Rewrite `src/runtime/CMakeLists.txt`**

Replace its ENTIRE contents with:

```cmake
add_executable(runtime
    src/main.cpp
)

target_include_directories(runtime PRIVATE
    src
)

target_compile_definitions(runtime PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN
)

target_link_libraries(runtime PRIVATE
    Engine
    ecs
    CommonHeaders
    GameHeaders
    glm::glm
    glfw
    nvrhi
    nvrhi_d3d12
    nvrhi_d3d11
    nvrhi_vk
    d3dcompiler
    dxgi
    dxcompiler
    freetype
    Tracy::TracyClient
)
if (TARGET Vulkan-Headers)
    target_link_libraries(runtime PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(runtime PRIVATE Vulkan::Headers)
endif()

set_target_properties(runtime PROPERTIES
    OUTPUT_NAME runtime
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}"
)

if(MSVC)
    target_compile_options(runtime PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Build the hot-reloadable Game.dll before the player runs.
add_dependencies(runtime game)

# Ship assets next to runtime.exe so a player-only build is self-sufficient.
add_custom_command(TARGET runtime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)
```

- [ ] **Step 3: Drop `src/overlay` from the root CMake**

In the root `CMakeLists.txt`, find and DELETE the `add_subdirectory(src/overlay)` line (and any adjacent comment that introduces it, e.g. a line like `# Legacy ImGui overlay DLL`). Leave `add_subdirectory(src/runtime)` in place. Do not change anything else (RUNTIME_DIR, VS_STARTUP_PROJECT editor, the other add_subdirectory lines, ordering).

- [ ] **Step 4: Configure + build the `runtime` target**

Run: `cmake --build --preset msvc-win64-vs2026-community --target runtime`
(The first build reconfigures because CMakeLists changed.)
Expected: clean compile/link → `runtime.exe` in the build's `bin/Debug/`. The legacy `src/runtime/src/*` files are still on disk but unreferenced (the `runtime` source list is only `main.cpp`); `imgui_overlay` is no longer built (removed from the root). If the build fails on a genuine error in the new `main.cpp` (e.g. an unresolved `MessageBoxA`/`sprintf_s`), the `<windows.h>`/`<cstdio>` includes above cover them — fix any real mismatch against the actual `Application.h`/`SettingsManager.h` API and rebuild.

- [ ] **Step 5: Commit**

```bash
git add src/runtime/src/main.cpp src/runtime/CMakeLists.txt CMakeLists.txt
git commit -m "Repurpose runtime target into stripped runtime.exe (Engine core, no ImGui)"
```

---

## Phase B2 — Delete the legacy single-threaded path

### Task 2: Remove all dead legacy files + verify a clean all-targets build

**Files:** deletions only (+ verification).

- [ ] **Step 1: Confirm the orphaned headers have no live includer**

Before deleting, grep to confirm each orphaned header is referenced ONLY by legacy/now-deleted files (not by `Engine`, `editor`, `ecs`, or `game`'s built sources). Run (Grep tool, or rg):
- Search `input_old.h` — expect matches only in `src/common/include/ui.h`, `src/common/include/render.h` (both being deleted) and the old legacy main (being replaced). NOT in any `src/engine/**`, `src/editor/**`, `src/ecs/**`, or `src/game/src/Game.cpp`.
- Search `ui.h` / `render.h` / `sound.h` — expect matches only in `src/game/include/game_old.h` + the legacy `src/runtime/src/*` (all being deleted). NOT in engine/editor/ecs or `Game.cpp`.
- Search `game_old.h` — expect matches only in the legacy `src/runtime/src/*` (being deleted).
- Search `imgui_overlay` — expect matches only in `src/runtime/src/*` (being deleted), `src/overlay/**` (being deleted), and docs/README. NOT in engine/editor.

If any live (engine/editor/ecs/`Game.cpp`) includer is found for a file slated for deletion, STOP and report — the spec's assumption is wrong and the deletion set needs revisiting. (Per the analysis none should appear.)

- [ ] **Step 2: Delete the legacy set with `git rm`**

```bash
# Overlay target (whole dir)
git rm -r src/overlay

# Legacy runtime sources (everything under src/runtime/src EXCEPT the new main.cpp)
git rm src/runtime/src/renderer.h src/runtime/src/renderer.cpp \
       src/runtime/src/renderer_common.h \
       src/runtime/src/renderer_dx11.h src/runtime/src/renderer_dx11.cpp \
       src/runtime/src/renderer_dx12.h src/runtime/src/renderer_dx12.cpp \
       src/runtime/src/renderer_vulkan.h src/runtime/src/renderer_vulkan.cpp \
       src/runtime/src/image.h src/runtime/src/image.cpp \
       src/runtime/src/font.h src/runtime/src/font.cpp \
       src/runtime/src/VertexPacked.h \
       src/runtime/src/platform.h src/runtime/src/windows_platform.cpp

# Orphaned legacy common headers
git rm src/common/include/input_old.h src/common/include/ui.h \
       src/common/include/render.h src/common/include/sound.h

# Orphaned legacy game files (game builds only src/Game.cpp; these are uncompiled)
git rm src/game/include/game_old.h \
       src/game/src/game_old.cpp src/game/src/game_in_level_old.cpp \
       src/game/src/game_editor_old.cpp src/game/src/game_main_menu_old.cpp

# Legacy clang single-cpp game.dll build script
git rm build.bat
```

Note: the exact legacy filename set under `src/runtime/src/` is from the analysis; before running, list the dir (`git ls-files src/runtime/src`) and adjust the `git rm` list to the ACTUAL files present (delete every file under `src/runtime/src/` except `main.cpp`). Likewise verify the `*_old.cpp` names under `src/game/src/` with `git ls-files "src/game/src/*_old.cpp"` and rm exactly those.

- [ ] **Step 3: Verify a bare all-targets build (the headline check)**

Run: `cmake --build --preset msvc-win64-vs2026-community`
Expected: **the WHOLE build succeeds** — `ecs`, `Engine`, `game`, `runtime`, `editor`, `test_ecs`, `test_alloc` all build clean. This previously FAILED on the legacy `runtime`'s `input.h` KEY_* redefinition; with the legacy path gone it now passes. If anything fails to find a deleted header, that file was a live consumer the grep missed — report it.

- [ ] **Step 4: Run the test suites**

Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → expect `All ECS tests passed.`
Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe` → expect `All allocator tests passed.`

- [ ] **Step 5: Smoke (report honestly)**

If you can run the GUI: launch `./out/build/msvc-win64-vs2026-community/bin/Debug/runtime.exe` — a window opens, the scene renders (models from `assets/`), input responds, and there is NO ImGui anywhere; clean exit. If headless, report that the all-targets build + tests passed and the `runtime.exe` GUI smoke is pending the user. Do NOT claim runtime success you didn't observe.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "Delete legacy single-threaded runtime + imgui_overlay + orphaned *_old code"
```

---

## Self-Review

**Spec coverage:**
- Stripped `runtime.exe` mirroring editor main minus ImGui/alloc-tracker, `app.Init(cli.override_)` no overlay, own `platform_debug_break`, `--backend` CLI + SettingsManager → Task 1 Step 1. ✓
- Runtime CMake `OUTPUT_NAME runtime`, links Engine, assets POST_BUILD, no DLL copies, `add_dependencies(runtime game)` → Task 1 Step 2. ✓
- Drop `add_subdirectory(src/overlay)` from root → Task 1 Step 3. ✓
- B1 builds `runtime.exe` green with legacy still on disk → Task 1 Step 4. ✓
- Full legacy deletion set (overlay dir, all legacy `src/runtime/src/*`, orphaned `input_old.h`/`ui.h`/`render.h`/`sound.h`, `game_old.h` + 4 `*_old.cpp`, `build.bat`) → Task 2 Step 2. ✓
- Pre-delete grep safety → Task 2 Step 1. ✓
- Bare all-targets build green (KEY_* break cleared) + tests → Task 2 Steps 3-4. ✓
- No GAME_API_VERSION bump; editor/Engine/ecs/game unchanged → global rules (no task touches them). ✓
- GUI smoke pending user → Task 2 Step 5. ✓

**Placeholder scan:** Task 1 has the full `main.cpp` + full CMakeLists. Task 2's "adjust the rm list to the actual files present" is a deliberate safety step (the analysis enumerated the files; the implementer confirms against `git ls-files` before deleting), not a vague placeholder — the deletion set is explicitly listed with a verification command.

**Type/consistency:** `OUTPUT_NAME runtime` is consistent across the CMake (Step 2) and the spec/usage text. `app.Init(cli.override_)` matches `Application::Init`'s single-arg default (overlay factory defaults to `{}`). `SettingsManager::ParseBackend` / `RendererAPI` / `SM_TRACE`/`SM_ERROR`/`DEBUG_BREAK` all match the editor main's usage (carried verbatim). The new `main.cpp` is the only file `runtime` compiles, consistent with Step 2's source list.
