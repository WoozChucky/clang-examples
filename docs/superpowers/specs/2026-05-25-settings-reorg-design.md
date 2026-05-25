# Settings Reorganization — Design

**Date:** 2026-05-25
**Status:** Approved (design)

## Problem

The project's persisted configuration has grown into distinct concerns that aren't
cleanly separated:

- **Scene data** already lives in `world.json` (entities + the `Environment` block). Good.
- **Engine configuration** (renderer backend, window size, vsync) is persisted by
  `SettingsManager` into a file named **`editor_settings.json`**. The name is a
  misnomer: this is engine-level config, loaded by `Application::Init` for **both**
  `editor.exe` and `runtime.exe` (`src/engine/src/core/Application.cpp:10`). It should
  be named `engine_settings.json`.
- **Editor view/debug preferences** — the RenderStats panel toggles (frustum culling,
  debug-draw gizmos, wireframe, grid, shadow enable/bias) — are **not persisted at
  all**. They reset to defaults every run. These are editor-only and belong in their
  own file.

## Goal

Establish a clean three-tier persistence model:

1. `world.json` — scene (entities + `Environment`). *(unchanged)*
2. `engine_settings.json` — engine config (backend / window / vsync), shared by editor
   and runtime. *(renamed from `editor_settings.json`)*
3. `editor_preferences.json` — editor-only RenderStats panel state, persisted across
   runs. *(new)*

## Non-Goals

- No change to `world.json` / scene persistence.
- No new engine-settings fields; Part A is a pure rename + text fixups.
- The runtime never reads or writes `editor_preferences.json` (it has no editor panel).
- No `ECS.h` / `GAME_API_VERSION` change.

## Part A — rename `editor_settings.json` → `engine_settings.json` (clean)

A clean rename: the engine reads/writes `engine_settings.json`; any pre-existing
`editor_settings.json` is ignored (the user re-saves the backend once via the editor's
Apply button). No migration code.

Changes:
- `src/engine/src/utilities/SettingsManager.h`: `constexpr auto DEFAULT_SETTINGS_PATH
  = "engine_settings.json";` and update the doc comment (currently "Settings persist in
  editor_settings.json …").
- Text references to update (no logic):
  - `src/editor/src/main.cpp` — `--help` text mentioning `editor_settings.json`
    (two spots) → `engine_settings.json`.
  - `src/runtime/src/main.cpp` — same `--help` text (two spots).
  - `src/editor/src/rendering/imgui/MainMenuBar.cpp` — the save-error string
    `"Failed to save editor_settings.json"` → `engine_settings.json`.

Load (`Application::Init`) and save (`MainMenuBar` Apply) already route through
`DEFAULT_SETTINGS_PATH`, so changing the constant repoints both automatically.

## Part B — `editor_preferences.json` (new, editor-only)

Persist the RenderStats panel state across runs. The state lives in three engine
structs, each a single `ENGINE_API` instance shared editor↔engine
(`src/engine/src/rendering/RenderStats.h`), touched only on the RenderThread:

- `CullingSettings { bool Enabled = true; }` → `GetCullingSettings()`
- `DebugDrawSettings { bool ShowLightGizmos=false; ShowCameraFrustum=false;
  ShowSelectedAABB=false; Wireframe=false; ShowGrid=false; }` → `GetDebugDrawSettings()`
- `ShadowSettings { bool Enabled=true; float Bias=0.0015f; }` → `GetShadowSettings()`

### Module: `src/editor/src/EditorPreferences.{h,cpp}` (editor-side)

Editor-side keeps the tier clean: the engine core stays unaware of editor preferences.
The module calls the `ENGINE_API` getters (the editor links `Engine.dll`).

**Header (`EditorPreferences.h`):**
- `constexpr auto DEFAULT_PREFERENCES_PATH = "editor_preferences.json";`
- `constexpr uint32_t PREFERENCES_VERSION = 1;`
- **Pure inline mappers** (no globals → unit-testable without Engine link):
  ```cpp
  inline nlohmann::json PrefsToJson(const CullingSettings& culling,
                                    const DebugDrawSettings& debug,
                                    const ShadowSettings& shadows);
  // Applies present keys onto the out-params; missing keys leave them unchanged
  // (so caller-supplied defaults survive a partial/old file). Never throws.
  inline void PrefsFromJson(const nlohmann::json& j,
                            CullingSettings& culling,
                            DebugDrawSettings& debug,
                            ShadowSettings& shadows);
  ```
- Declarations `bool Load(const std::string& path);` / `bool Save(const std::string& path);`.

**Source (`EditorPreferences.cpp`):**
- `Load(path)`: open file (missing → `SM_TRACE`, return true, defaults stay); parse
  (error → `SM_WARN`, return false, leave globals untouched); `PrefsFromJson(j,
  GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings())` applied to the
  live globals. Mirrors `SettingsManager::Load` hardening.
- `Save(path)`: `PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(),
  GetShadowSettings())` → write `j.dump(4)`; I/O failure → `SM_WARN`, return false.
  Mirrors `SettingsManager::Save`.

### JSON schema

```json
{
  "version": 1,
  "culling":   { "frustum": true },
  "debugDraw": { "lightGizmos": false, "cameraFrustum": false, "selectedAABB": false, "wireframe": false, "grid": true },
  "shadows":   { "enabled": true, "bias": 0.0015 }
}
```

(`bias` default is `0.0015`, matching `ShadowSettings::Bias`.)

### Load at startup

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` `Init` (RenderThread), the
existing `GetDebugDrawSettings().ShowGrid = true;` line stays as the **editor default**,
followed immediately by `EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH);`.
Ordering: set the editor default (grid on) first, then `Load` overrides every field from
the file when present. First run (no file) → grid on + engine defaults; later runs →
saved values applied.

### Save on change

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, capture the ImGui return
values of the culling / debug-draw / shadow widgets into a `bool changed` (e.g.
`changed |= ImGui::Checkbox("Grid", &dd.ShowGrid);`), and at the end:
`if (changed) EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH);`.
ImGui widgets return true only on the frame the value changes, so the file is written
on edits only — never per-frame.

## Testing

New `tests/test_editorprefs.cpp` target (mirrors `test_worldserial` / `test_debugdraw`:
plain `main()`, prints `All … passed.`), exercising the **pure** mappers (no globals,
no Engine link — links `nlohmann_json` + includes `RenderStats.h` for the structs and
`EditorPreferences.h` for the mappers):

1. **Round-trip:** set non-default values in all three structs, `PrefsToJson` then
   `PrefsFromJson` into fresh structs, assert every field preserved (bools exact; `Bias`
   within epsilon).
2. **Backward/partial-compat:** `PrefsFromJson` on a document missing `"shadows"` (or
   missing the whole body) leaves the corresponding out-param structs at their
   caller-supplied defaults and does not throw.

## Build impact

- `src/editor/CMakeLists.txt`: add `src/EditorPreferences.cpp` to the editor sources;
  ensure the editor target links `nlohmann_json::nlohmann_json` (add if not already
  present — `RenderStatsPanel`/`ImGuiRenderer` will include `EditorPreferences.h` which
  pulls `<nlohmann/json.hpp>`).
- `tests/CMakeLists.txt`: add `test_editorprefs` (sources `test_editorprefs.cpp`; links
  `CommonHeaders` + `glm::glm` + `nlohmann_json::nlohmann_json`; include dirs
  `src/engine/src/rendering` for `RenderStats.h` and `src/editor/src` for
  `EditorPreferences.h`).
- No `ECS.h` / `GAME_API_VERSION` change. Rebuild `editor` + run `test_editorprefs`.
  `runtime` source-unaffected and never touches `editor_preferences.json`.

## Risks / Notes

- **Header include of `RenderStats.h` in the test** is declarations only (the
  `ENGINE_API` getters are not called by the pure mappers), so no Engine link is needed
  — same pattern as `test_worldserial` including `ECS.h`.
- **Save-on-change frequency** is bounded by ImGui change events (click/drag frames),
  not frames — negligible I/O.
- **Thread safety:** `EditorPreferences::Load` (in `ImGuiRenderer::Init`) and `Save`
  (in `RenderStatsPanel`, drawn by the overlay) both run on the RenderThread, the same
  thread that owns the RenderStats globals — no cross-thread access.
- **Clean rename caveat:** an existing `editor_settings.json` is silently ignored after
  the rename; the user re-saves the backend once. Acceptable for a single-dev project.
