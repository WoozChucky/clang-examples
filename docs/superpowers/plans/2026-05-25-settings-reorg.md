# Settings Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a clean three-tier persistence model: `world.json` (scene), `engine_settings.json` (engine config, renamed from `editor_settings.json`), and a new `editor_preferences.json` (editor-only RenderStats toggles, persisted across runs).

**Architecture:** Part A renames the engine-settings file (one constant + text fixups). Part B adds an editor-side `EditorPreferences` module with pure JSON↔struct mappers (unit-tested) plus `Load`/`Save` wrappers over the `ENGINE_API` RenderStats getters; the editor loads prefs at startup (`ImGuiRenderer::Init`) and saves on-change (`RenderStatsPanel`).

**Tech Stack:** C++23, nlohmann/json, Dear ImGui editor overlay, the engine's `SettingsManager` pattern. Pure mappers unit-tested via a new `test_editorprefs` target.

**Spec:** `docs/superpowers/specs/2026-05-25-settings-reorg-design.md`

---

## Build & test reference (every task)

Build preset: **`msvc-win64-vs2026-community`** (enterprise preset is NOT installed — never use it). Binaries in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

```powershell
cmake --preset msvc-win64-vs2026-community                                   # reconfigure (after CMakeLists changes)
cmake --build --preset msvc-win64-vs2026-community --target <target>          # build
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe                   # run a test
```

**Commit identity (MANDATORY):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"`. Never the work email. Never `git add .claude/` — stage only files you changed.

No `ECS.h` / `GAME_API_VERSION` change. Per project notes the `runtime` target is pre-broken/legacy — do NOT gate any task on building `runtime`; the `runtime/main.cpp` text edit in Task 3 is a string-literal change that cannot break a build, and `editor` building green covers the functional `SettingsManager` change.

## File map

- `src/editor/src/EditorPreferences.h` — **new.** Path constant + pure inline `PrefsToJson`/`PrefsFromJson` mappers + `Load`/`Save` decls. (Task 1)
- `src/editor/src/EditorPreferences.cpp` — **new.** `Load`/`Save` over the RenderStats getters. (Task 1)
- `tests/test_editorprefs.cpp` — **new.** Pure-mapper round-trip + partial-doc tests. (Task 1)
- `src/editor/CMakeLists.txt` — add the source + `nlohmann_json` link. (Task 1)
- `tests/CMakeLists.txt` — add the `test_editorprefs` target. (Task 1)
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — load prefs at startup. (Task 2)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — save prefs on change. (Task 2)
- `src/engine/src/utilities/SettingsManager.h` — rename the path constant + comment. (Task 3)
- `src/editor/src/main.cpp`, `src/runtime/src/main.cpp`, `src/editor/src/rendering/imgui/MainMenuBar.cpp` — text references. (Task 3)

---

## Task 1: `EditorPreferences` module + pure-mapper tests (TDD)

**Files:**
- Create: `src/editor/src/EditorPreferences.h`, `src/editor/src/EditorPreferences.cpp`
- Create: `tests/test_editorprefs.cpp`
- Modify: `src/editor/CMakeLists.txt`, `tests/CMakeLists.txt`

The pure mappers (`PrefsToJson`/`PrefsFromJson`) are header-only and testable with no Engine link (the test includes `RenderStats.h` only for the plain structs; the `ENGINE_API` getters are never called by the mappers). TDD the mappers first, then add the `Load`/`Save` wrappers + editor build wiring.

- [ ] **Step 1: Write the failing test `tests/test_editorprefs.cpp`**

```cpp
#include <cstdio>
#include <cmath>
#include <nlohmann/json.hpp>

#include "EditorPreferences.h" // pure mappers + RenderStats structs (via RenderStats.h)

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_roundtrip()
{
    CullingSettings   culling;  culling.Enabled = false;          // default true
    DebugDrawSettings debug;    debug.ShowLightGizmos = true; debug.ShowCameraFrustum = true;
                                debug.ShowSelectedAABB = true; debug.Wireframe = true; debug.ShowGrid = true; // defaults all false
    ShadowSettings    shadows;  shadows.Enabled = false; shadows.Bias = 0.0042f; // defaults true / 0.0015

    const nlohmann::json j = EditorPreferences::PrefsToJson(culling, debug, shadows);

    CullingSettings   c2;  DebugDrawSettings d2;  ShadowSettings s2; // fresh defaults
    EditorPreferences::PrefsFromJson(j, c2, d2, s2);

    EXPECT(c2.Enabled == culling.Enabled);
    EXPECT(d2.ShowLightGizmos   == debug.ShowLightGizmos);
    EXPECT(d2.ShowCameraFrustum == debug.ShowCameraFrustum);
    EXPECT(d2.ShowSelectedAABB  == debug.ShowSelectedAABB);
    EXPECT(d2.Wireframe         == debug.Wireframe);
    EXPECT(d2.ShowGrid          == debug.ShowGrid);
    EXPECT(s2.Enabled == shadows.Enabled);
    EXPECT(std::fabs(s2.Bias - shadows.Bias) < 1e-6f);
}

static void T01_missing_keys_leave_defaults()
{
    // Partial/old document: only debugDraw.grid present.
    nlohmann::json j;
    j["debugDraw"]["grid"] = false;

    CullingSettings   culling;            // default Enabled=true
    DebugDrawSettings debug;              // defaults all false
    debug.Wireframe = true;              // sentinel: absent key must leave this true
    ShadowSettings    shadows;            // default Enabled=true, Bias=0.0015

    EditorPreferences::PrefsFromJson(j, culling, debug, shadows);

    EXPECT(culling.Enabled == true);      // absent -> default kept
    EXPECT(debug.ShowGrid == false);      // present -> applied
    EXPECT(debug.Wireframe == true);      // absent -> sentinel kept
    EXPECT(shadows.Enabled == true);      // absent -> default kept
    EXPECT(std::fabs(shadows.Bias - 0.0015f) < 1e-6f); // absent -> default kept
}

int main()
{
    T00_roundtrip();
    T01_missing_keys_leave_defaults();
    if (g_Failures == 0) { std::printf("All editor preferences tests passed.\n"); return 0; }
    std::printf("%d editor preferences test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the `test_editorprefs` target to `tests/CMakeLists.txt`**

Append at the end of `tests/CMakeLists.txt`:

```cmake
add_executable(test_editorprefs
    test_editorprefs.cpp
)

target_link_libraries(test_editorprefs PRIVATE
    glm::glm
    nlohmann_json::nlohmann_json
)

target_include_directories(test_editorprefs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src
)

target_compile_definitions(test_editorprefs PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_editorprefs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build to verify FAIL (header missing)**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs
```
Expected: FAIL — `Cannot open include file: 'EditorPreferences.h'`.

- [ ] **Step 4: Create `src/editor/src/EditorPreferences.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "RenderStats.h" // CullingSettings, DebugDrawSettings, ShadowSettings (+ ENGINE_API getters)

// Editor-only persistence of the RenderStats panel toggles. Saved next to the
// executable as editor_preferences.json; the runtime never touches it.
namespace EditorPreferences {

constexpr auto     DEFAULT_PREFERENCES_PATH = "editor_preferences.json";
constexpr uint32_t PREFERENCES_VERSION      = 1;

// Pure: serialize the three RenderStats settings structs to the preferences JSON shape.
// No globals touched -> unit-testable without an Engine link.
inline nlohmann::json PrefsToJson(const CullingSettings& culling,
                                  const DebugDrawSettings& debug,
                                  const ShadowSettings& shadows) {
    return nlohmann::json{
        {"version", PREFERENCES_VERSION},
        {"culling", {
            {"frustum", culling.Enabled},
        }},
        {"debugDraw", {
            {"lightGizmos",   debug.ShowLightGizmos},
            {"cameraFrustum", debug.ShowCameraFrustum},
            {"selectedAABB",  debug.ShowSelectedAABB},
            {"wireframe",     debug.Wireframe},
            {"grid",          debug.ShowGrid},
        }},
        {"shadows", {
            {"enabled", shadows.Enabled},
            {"bias",    shadows.Bias},
        }},
    };
}

// Pure: apply present keys from `j` onto the out-params. Missing/wrong-typed keys leave
// the corresponding field unchanged (caller-supplied defaults survive a partial/old
// file). Never throws (checked contains/type tests, no `.at()`).
inline void PrefsFromJson(const nlohmann::json& j,
                          CullingSettings& culling,
                          DebugDrawSettings& debug,
                          ShadowSettings& shadows) {
    if (j.contains("culling") && j["culling"].is_object()) {
        const auto& c = j["culling"];
        if (c.contains("frustum") && c["frustum"].is_boolean()) culling.Enabled = c["frustum"].get<bool>();
    }
    if (j.contains("debugDraw") && j["debugDraw"].is_object()) {
        const auto& d = j["debugDraw"];
        if (d.contains("lightGizmos")   && d["lightGizmos"].is_boolean())   debug.ShowLightGizmos   = d["lightGizmos"].get<bool>();
        if (d.contains("cameraFrustum") && d["cameraFrustum"].is_boolean()) debug.ShowCameraFrustum = d["cameraFrustum"].get<bool>();
        if (d.contains("selectedAABB")  && d["selectedAABB"].is_boolean())  debug.ShowSelectedAABB  = d["selectedAABB"].get<bool>();
        if (d.contains("wireframe")     && d["wireframe"].is_boolean())     debug.Wireframe         = d["wireframe"].get<bool>();
        if (d.contains("grid")          && d["grid"].is_boolean())          debug.ShowGrid          = d["grid"].get<bool>();
    }
    if (j.contains("shadows") && j["shadows"].is_object()) {
        const auto& s = j["shadows"];
        if (s.contains("enabled") && s["enabled"].is_boolean()) shadows.Enabled = s["enabled"].get<bool>();
        if (s.contains("bias")    && s["bias"].is_number())     shadows.Bias    = s["bias"].get<float>();
    }
}

// Read editor_preferences.json at `path` and apply it onto the live RenderStats globals.
// Missing file -> true (defaults stay). Parse error -> false (WARN; globals untouched).
bool Load(const std::string& path);

// Serialize the live RenderStats globals to `path`. I/O failure -> false (WARN).
bool Save(const std::string& path);

} // namespace EditorPreferences
```

- [ ] **Step 5: Build + run the test → PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe
```
Expected: `All editor preferences tests passed.`

- [ ] **Step 6: Create `src/editor/src/EditorPreferences.cpp`**

```cpp
#include "EditorPreferences.h"

#include <fstream>

#include "lib.h" // SM_WARN / SM_TRACE

using json = nlohmann::json;

namespace EditorPreferences {

bool Load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        SM_TRACE("EditorPreferences: '%s' not found; using defaults", path.c_str());
        return true;
    }
    json j;
    try {
        ifs >> j;
    } catch (const std::exception& ex) {
        SM_WARN("EditorPreferences: failed to parse '%s': %s", path.c_str(), ex.what());
        return false;
    }
    PrefsFromJson(j, GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings());
    SM_TRACE("EditorPreferences: loaded '%s'", path.c_str());
    return true;
}

bool Save(const std::string& path) {
    const json j = PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        SM_WARN("EditorPreferences: could not open '%s' for writing", path.c_str());
        return false;
    }
    ofs << j.dump(4);
    if (!ofs.good()) {
        SM_WARN("EditorPreferences: write to '%s' failed", path.c_str());
        return false;
    }
    return true;
}

} // namespace EditorPreferences
```

- [ ] **Step 7: Wire the new source + `nlohmann_json` into the editor target**

In `src/editor/CMakeLists.txt`:

(a) Add the source to the `add_executable(editor ...)` list (e.g. right after `src/main.cpp`):
```cmake
    src/EditorPreferences.cpp
```

(b) Add `nlohmann_json::nlohmann_json` to `target_link_libraries(editor PRIVATE ...)` (e.g. after the `glm::glm` line):
```cmake
    nlohmann_json::nlohmann_json
```

- [ ] **Step 8: Build `editor` to verify the module compiles + links**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds clean (the editor now compiles `EditorPreferences.cpp`, which links the `ENGINE_API` getters from `Engine.dll`).

- [ ] **Step 9: Commit**

```powershell
git add src/editor/src/EditorPreferences.h src/editor/src/EditorPreferences.cpp tests/test_editorprefs.cpp tests/CMakeLists.txt src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): EditorPreferences module (RenderStats prefs IO) + tests"
```

---

## Task 2: Wire load at startup + save on change

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (load at startup)
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (save on change)

After Task 1 the module exists but nothing calls `Load`/`Save`. Wire it: load once at editor startup (after setting the editor default grid-on), and save whenever a RenderStats toggle changes.

- [ ] **Step 1: Load prefs at editor startup in `ImGuiRenderer.cpp`**

Add the include near the other editor includes at the top of the file (the `src` dir is on the editor include path, so the bare name resolves):
```cpp
#include "EditorPreferences.h"
```

In `ImGuiRenderer::Init`, find the existing editor grid default (added in the grid feature):
```cpp
    // Ground grid is an editor authoring aid — on by default in the editor, never in
    // the runtime (separate process, no overlay, leaves the flag at its default false).
    GetDebugDrawSettings().ShowGrid = true;
```
Immediately AFTER that line, add:
```cpp
    // Restore persisted RenderStats toggles. Runs after the editor defaults above so a
    // saved file wins; first run (no file) keeps the defaults (e.g. grid on).
    EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH);
```

- [ ] **Step 2: Save prefs on change in `RenderStatsPanel.cpp`**

Replace the whole body of `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` with (adds the `EditorPreferences.h` include, captures `changed` on every toggle/slider, and saves when changed):

```cpp
#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)
#include "EditorPreferences.h" // persist the toggles across runs

void DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return; }

    bool changed = false;
    changed |= ImGui::Checkbox("Frustum culling", &GetCullingSettings().Enabled);
    ImGui::Separator();

    const RenderStats& s = GetRenderStats();
    ImGui::Text("Mesh entities: %u", s.MeshEntitiesTotal);
    ImGui::Text("  drawn:  %u", s.MeshEntitiesDrawn);
    ImGui::Text("  culled: %u", s.MeshEntitiesCulled);
    ImGui::Text("Instances: %u", s.InstancesDrawn);
    ImGui::Text("Batches:   %u", s.BatchesDrawn);

    ImGui::Separator();
    ImGui::TextDisabled("Debug Draw");
    DebugDrawSettings& dd = GetDebugDrawSettings();
    changed |= ImGui::Checkbox("Light gizmos",   &dd.ShowLightGizmos);
    changed |= ImGui::Checkbox("Camera frustum", &dd.ShowCameraFrustum);
    changed |= ImGui::Checkbox("Selected AABB",  &dd.ShowSelectedAABB);
    changed |= ImGui::Checkbox("Wireframe",      &dd.Wireframe);
    changed |= ImGui::Checkbox("Grid",           &dd.ShowGrid);

    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    changed |= ImGui::Checkbox("Shadows", &sh.Enabled);
    changed |= ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");

    // ImGui widgets return true only on the frame the value changes, so this writes
    // editor_preferences.json on edits only — never per-frame.
    if (changed)
        EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH);

    ImGui::End();
}
```

- [ ] **Step 3: Build `editor` to verify it compiles**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 4: Commit**

```powershell
git add src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/imgui/RenderStatsPanel.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): load RenderStats prefs at startup, save on change"
```

---

## Task 3: Rename `editor_settings.json` → `engine_settings.json`

**Files:**
- Modify: `src/engine/src/utilities/SettingsManager.h` (constant + comment)
- Modify: `src/editor/src/main.cpp` (two help/error strings)
- Modify: `src/runtime/src/main.cpp` (two help/error strings)
- Modify: `src/editor/src/rendering/imgui/MainMenuBar.cpp` (one error string)

Clean rename: only the path constant is functional; the rest are user-facing strings. `Application::Init` (load) and `MainMenuBar` Apply (save) both route through `DEFAULT_SETTINGS_PATH`, so repointing the constant repoints both.

- [ ] **Step 1: Rename the constant + comment in `SettingsManager.h`**

In `src/engine/src/utilities/SettingsManager.h`, change the comment and constant:
```cpp
    // Settings persist in engine_settings.json next to the executable.
    // Unknown keys are ignored on load so the file format can evolve.
    constexpr auto     DEFAULT_SETTINGS_PATH = "engine_settings.json";
    constexpr uint32_t SETTINGS_VERSION      = 1;
```
(Only the comment's filename and the string literal change; `SETTINGS_VERSION` stays.)

- [ ] **Step 2: Update the two strings in `src/editor/src/main.cpp`**

Change the `--help` line:
```cpp
                    "                    only. Does NOT modify engine_settings.json.\n"
```
and the MessageBox line:
```cpp
                    "Edit engine_settings.json next to editor.exe, or relaunch with\n"
```

- [ ] **Step 3: Update the two strings in `src/runtime/src/main.cpp`**

Change the `--help` line:
```cpp
                    "                    only. Does NOT modify engine_settings.json.\n"
```
and the MessageBox line:
```cpp
                    "Edit engine_settings.json next to runtime.exe, or relaunch with\n"
```

- [ ] **Step 4: Update the error string in `MainMenuBar.cpp`**

In `src/editor/src/rendering/imgui/MainMenuBar.cpp`, change:
```cpp
                    m_SettingsSaveError = "Failed to save engine_settings.json";
```

- [ ] **Step 5: Build `editor` + confirm no stale references remain**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds clean. Then grep the repo under `src/` for `editor_settings` — there must be ZERO matches (all references repointed). Do NOT build `runtime` (pre-broken/legacy per project notes); its change is a string literal that cannot affect compilation.

- [ ] **Step 6: Commit**

```powershell
git add src/engine/src/utilities/SettingsManager.h src/editor/src/main.cpp src/runtime/src/main.cpp src/editor/src/rendering/imgui/MainMenuBar.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(settings): rename editor_settings.json -> engine_settings.json"
```

---

## Done criteria

- `test_editorprefs` prints `All editor preferences tests passed.`; `editor` builds clean.
- Zero `editor_settings` references remain under `src/` (all → `engine_settings`).
- User GUI smoke: toggle RenderStats panel options (grid / gizmos / wireframe / shadows / culling / bias) → `editor_preferences.json` appears next to `editor.exe`; restart the editor → toggles restored. Engine settings still load/save (backend Apply still works, now writing `engine_settings.json`).
