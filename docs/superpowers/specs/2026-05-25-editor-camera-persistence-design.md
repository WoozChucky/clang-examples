# Editor Camera Persistence — Design

**Date:** 2026-05-25
**Status:** Approved (design)
**Extends:** the `editor_preferences.json` feature (`docs/superpowers/specs/2026-05-25-settings-reorg-design.md`), on branch `feat/settings-reorg` before merge.

## Problem

`editor_preferences.json` now persists the RenderStats panel toggles, but not the
editor fly-camera pose. Reopening the editor always resets the viewpoint to the default
(`{0,5,10}`, looking -Z). The camera pose should persist across runs in the same
`editor_preferences.json`.

## Constraint that shapes the design

The camera pose is **continuous** (the user flies it every frame) and **editor-owned**
(`ImGuiRenderer::m_EditorCamera`), unlike the RenderStats toggles, which are discrete
and live in engine globals reachable anywhere. Both share one file. Therefore **any**
write of `editor_preferences.json` must include both blocks, or a toggle-only write
would clobber the camera block (and vice-versa).

**Decision (user-chosen):** keep one file (`editor_preferences.json`) and **centralize
the save** in `ImGuiRenderer`, which is the one place with access to both the globals
(via the `ENGINE_API` getters) and `m_EditorCamera`. The camera is saved on editor exit
(definitive final pose) and incidentally whenever a toggle changes; never per-frame.

## Goal

Persist the editor camera pose (`Position`, `Yaw`, `Pitch`, `FlySpeed`) in
`editor_preferences.json`, restored at editor startup and saved at editor shutdown (plus
on toggle changes). `FlySpeed` is included because it is wheel-tuned during a session.

## Non-Goals

- No second file; no change to the engine-settings tier.
- `Fov`/`Near`/`Far` are not persisted (not user-tuned).
- No per-frame camera save.

## Components

### 1. `EditorCameraState` — new tiny header `src/editor/src/rendering/EditorCameraState.h`

A glm-only POD so consumers (and the unit test) don't pull the full `EditorCamera` /
`CameraView`:

```cpp
#pragma once
#include <glm/vec3.hpp>

// Persistable editor fly-camera pose. Defaults mirror EditorCamera's defaults so a
// default-constructed state == the camera's default pose.
struct EditorCameraState {
    glm::vec3 Position{0.0f, 5.0f, 10.0f};
    float     Yaw      = 0.0f;
    float     Pitch    = 0.0f;
    float     FlySpeed = 7.5f;
};
```

### 2. `EditorCamera` (`src/editor/src/rendering/EditorCamera.{h,cpp}`)

Add state accessors (the class already has private `m_Position/m_Yaw/m_Pitch/m_FlySpeed`
and test-only getters; keep those):

```cpp
#include "EditorCameraState.h"
// ...
EditorCameraState GetState() const;          // { m_Position, m_Yaw, m_Pitch, m_FlySpeed }
void SetState(const EditorCameraState& s);   // sanitized restore
```

`SetState` is defensive (a hand-edited/garbage file must not NaN the camera): apply each
field only if finite; clamp `Pitch` to `±kPitchLimit` (the existing `glm::radians(89°)`
constant in `EditorCamera.cpp`); clamp `FlySpeed` to the existing `[0.5, 200]` wheel
range. `Yaw` is unrestricted (wraps naturally).

### 3. `EditorPreferences` (`src/editor/src/EditorPreferences.{h,cpp}`)

Thread the camera through the existing pure mappers and the IO wrappers (the RenderStats
"apply to globals internally" behavior from the settings-reorg feature is unchanged; the
camera is carried as an explicit value because it is not a global):

- Header includes `"EditorCameraState.h"`.
- `PrefsToJson(const CullingSettings&, const DebugDrawSettings&, const ShadowSettings&, const EditorCameraState&)` — adds a `"camera"` block.
- `PrefsFromJson(const json&, CullingSettings&, DebugDrawSettings&, ShadowSettings&, EditorCameraState&)` — applies present camera fields; missing `"camera"` (or any field) leaves the passed-in state unchanged. Never throws.
- `bool Load(const std::string& path, EditorCameraState& camera)` — applies toggles to the globals as before, and fills `camera` from the file (caller seeds it with the current pose so a missing block keeps it).
- `bool Save(const std::string& path, const EditorCameraState& camera)` — serializes the globals + the given camera.

**JSON shape (camera block added):**
```json
{
  "version": 1,
  "culling":   { "frustum": true },
  "debugDraw": { "lightGizmos": false, "cameraFrustum": false, "selectedAABB": false, "wireframe": false, "grid": true },
  "shadows":   { "enabled": true, "bias": 0.0015 },
  "camera":    { "position": [0.0, 5.0, 10.0], "yaw": 0.0, "pitch": 0.0, "flySpeed": 7.5 }
}
```
`position` is a 3-element array (read tolerantly: applied only if it is an array of size 3).

### 4. `ImGuiRenderer` (`src/editor/src/rendering/imgui/ImGuiRenderer.cpp`) — owns load + save

- **`Init`** (replaces the settings-reorg `Load` call): after the editor `ShowGrid = true`
  default,
  ```cpp
  EditorCameraState cs = m_EditorCamera.GetState();
  EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH, cs);
  m_EditorCamera.SetState(cs);
  ```
  Seeding `cs` from the current pose means a missing `"camera"` block leaves the default.
- **`Render`** (the `DrawRenderStatsPanel(...)` call site): the panel now returns whether
  anything changed; save the full prefs (toggles + current camera) on change:
  ```cpp
  if (DrawRenderStatsPanel(&s_ShowRenderStatsPanel))
      EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
  ```
- **`Shutdown`** (top of the method, before ImGui teardown — `m_EditorCamera` still alive):
  ```cpp
  EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
  ```
  This is the definitive capture of the final pose on editor exit.

### 5. `RenderStatsPanel` (`src/editor/src/rendering/imgui/RenderStatsPanel.{h,cpp}`)

The panel no longer self-saves (the save moved to `ImGuiRenderer` so the camera is always
included). Change the signature to report change:
- `bool DrawRenderStatsPanel(bool* open);` (was `void`).
- Early-return paths return `false`; the normal path returns the accumulated `changed`
  (the same `changed |= ImGui::…` logic, including the bias-slider-on-release case).
- Remove the `#include "EditorPreferences.h"` and the `EditorPreferences::Save(...)` call
  from the panel.

## Testing

- **`tests/test_editorprefs.cpp`** (extend): the round-trip test passes a non-default
  `EditorCameraState` through `PrefsToJson`→`PrefsFromJson` and asserts `Position` (per
  component, epsilon), `Yaw`, `Pitch`, `FlySpeed` preserved. The partial-doc test adds:
  a document with no `"camera"` key leaves the passed-in camera state untouched. Add
  `${CMAKE_SOURCE_DIR}/src/editor/src/rendering` to the test's include dirs (for
  `EditorCameraState.h`, pulled in via `EditorPreferences.h`).
- **`tests/test_editorcam.cpp`** (extend): `GetState` returns the current pose;
  `SetState` round-trips a finite state; `SetState` clamps an out-of-range `Pitch` to
  `±kPitchLimit` and an out-of-range `FlySpeed` into `[0.5, 200]`; a non-finite field is
  ignored (pose unchanged). `test_editorcam` already compiles `EditorCamera.cpp`.

## Build impact

- New header `EditorCameraState.h` (no CMake entry needed — header-only).
- `tests/CMakeLists.txt`: `test_editorprefs` gains the `src/editor/src/rendering` include
  dir. `test_editorcam` is unchanged (already builds `EditorCamera.cpp`).
- No `ECS.h` / `GAME_API_VERSION` change. Rebuild `editor`; run `test_editorprefs` +
  `test_editorcam`. `runtime` unaffected (never persists editor prefs).

## Risks / Notes

- **Shutdown reliability:** if the editor is killed without a clean `ImGuiRenderer::Shutdown`,
  the final pose isn't saved — but the incidental on-toggle-change save still captures a
  recent pose. Acceptable for an editor convenience pref. (The plan verifies `Shutdown` is
  on the normal exit path.)
- **Sanitized restore:** `SetState` guards non-finite values and clamps pitch/flySpeed, so
  a corrupt `"camera"` block can never put the camera in an unusable state.
- **No clobber:** because every `Save` goes through `ImGuiRenderer` and serializes both the
  toggles and the camera, neither block can overwrite the other.
