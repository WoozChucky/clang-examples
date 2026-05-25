# Editor Camera Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist the editor fly-camera pose (Position/Yaw/Pitch/FlySpeed) in `editor_preferences.json`, restored at editor startup and saved at editor shutdown (plus on toggle changes).

**Architecture:** Add an `EditorCameraState` POD + sanitized `GetState`/`SetState` on `EditorCamera`. Thread the camera through the existing `EditorPreferences` mappers + `Load`/`Save`. Centralize the save in `ImGuiRenderer` (the one place with both the RenderStats globals and `m_EditorCamera`): load in `Init`, save on RenderStats-panel change and on `Shutdown`. `RenderStatsPanel` returns `bool changed` instead of self-saving, so every write includes the camera.

**Tech Stack:** C++23, nlohmann/json, Dear ImGui editor overlay, GLM. Pure mappers + camera math unit-tested via `test_editorprefs` and `test_editorcam`.

**Spec:** `docs/superpowers/specs/2026-05-25-editor-camera-persistence-design.md`

---

## Build & test reference (every task)

Build preset: **`msvc-win64-vs2026-community`** (enterprise preset is NOT installed — never use it). Binaries in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

```powershell
cmake --preset msvc-win64-vs2026-community                                   # reconfigure (after CMakeLists changes)
cmake --build --preset msvc-win64-vs2026-community --target <target>          # build
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe                   # run a test
```

**Commit identity (MANDATORY):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"`. Never the work email. Never `git add .claude/`.

No `ECS.h` / `GAME_API_VERSION` change. `runtime` is unaffected (never persists editor prefs); do not build it.

## File map

- `src/editor/src/rendering/EditorCameraState.h` — **new.** glm-only POD `EditorCameraState`. (Task 1)
- `src/editor/src/rendering/EditorCamera.{h,cpp}` — `GetState`/`SetState`. (Task 1)
- `tests/test_editorcam.cpp` — state accessor + clamp tests. (Task 1)
- `src/editor/src/EditorPreferences.{h,cpp}` — thread camera through mappers + `Load`/`Save`. (Task 2)
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — load in `Init`, save in `Render` + `Shutdown`. (Task 2)
- `src/editor/src/rendering/imgui/RenderStatsPanel.{h,cpp}` — return `bool changed`, drop self-save. (Task 2)
- `tests/test_editorprefs.cpp` + `tests/CMakeLists.txt` — camera round-trip + include dir. (Task 2)

---

## Task 1: `EditorCameraState` + `EditorCamera::GetState`/`SetState` (TDD)

**Files:**
- Create: `src/editor/src/rendering/EditorCameraState.h`
- Modify: `src/editor/src/rendering/EditorCamera.h`, `src/editor/src/rendering/EditorCamera.cpp`
- Modify: `tests/test_editorcam.cpp`

`GetState`/`SetState` are pure-ish camera logic, testable in the existing `test_editorcam` (which already compiles `EditorCamera.cpp`). This task is additive — no callers change, so the editor still builds.

- [ ] **Step 1: Write the failing test (add `T09` to `tests/test_editorcam.cpp`)**

Add this function before `main()`:

```cpp
static void T09_get_set_state_roundtrip_and_clamp()
{
    EditorCamera c;
    // GetState reflects defaults (pos {0,5,10}, flySpeed 7.5).
    const EditorCameraState d = c.GetState();
    EXPECT(std::abs(d.Position.y - 5.0f) < 1e-4f);
    EXPECT(std::abs(d.FlySpeed - 7.5f) < 1e-4f);

    // SetState round-trips a finite, in-range state.
    EditorCameraState s{};
    s.Position = glm::vec3(1.0f, 2.0f, 3.0f);
    s.Yaw = 0.5f; s.Pitch = 0.3f; s.FlySpeed = 12.0f;
    c.SetState(s);
    const EditorCameraState r = c.GetState();
    EXPECT(std::abs(r.Position.x - 1.0f) < 1e-4f && std::abs(r.Position.y - 2.0f) < 1e-4f && std::abs(r.Position.z - 3.0f) < 1e-4f);
    EXPECT(std::abs(r.Yaw - 0.5f) < 1e-4f);
    EXPECT(std::abs(r.Pitch - 0.3f) < 1e-4f);
    EXPECT(std::abs(r.FlySpeed - 12.0f) < 1e-4f);

    // Pitch clamps to +/-89deg; FlySpeed clamps into [0.5, 200].
    EditorCameraState over{};
    over.Pitch = glm::radians(200.0f);
    over.FlySpeed = 9999.0f;
    c.SetState(over);
    const EditorCameraState rc = c.GetState();
    EXPECT(rc.Pitch <= glm::radians(89.0f) + 1e-4f);
    EXPECT(rc.FlySpeed <= 200.0f + 1e-4f);

    // Non-finite fields are ignored (previous good values kept).
    EditorCamera c2;
    c2.SetState(s); // known-good pose
    EditorCameraState bad{};
    bad.Position = glm::vec3(std::nanf(""), 0.0f, 0.0f);
    bad.Yaw = std::nanf(""); bad.Pitch = std::nanf(""); bad.FlySpeed = std::nanf("");
    c2.SetState(bad);
    const EditorCameraState rk = c2.GetState();
    EXPECT(std::abs(rk.Position.x - 1.0f) < 1e-4f); // unchanged from s
    EXPECT(std::abs(rk.Yaw - 0.5f) < 1e-4f);
    EXPECT(std::abs(rk.FlySpeed - 12.0f) < 1e-4f);
}
```

In `main()`, add after `T08_tocameraview_view_looks_at_target();`:

```cpp
    T09_get_set_state_roundtrip_and_clamp();
```

- [ ] **Step 2: Build to verify FAIL (state API missing)**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_editorcam
```
Expected: FAIL — `'EditorCameraState': undeclared identifier` / `'GetState' is not a member of 'EditorCamera'`.

- [ ] **Step 3: Create `src/editor/src/rendering/EditorCameraState.h`**

```cpp
#pragma once

#include <glm/vec3.hpp>

// Persistable editor fly-camera pose. Defaults mirror EditorCamera's defaults so a
// default-constructed state equals the camera's default pose.
struct EditorCameraState {
    glm::vec3 Position{0.0f, 5.0f, 10.0f};
    float     Yaw      = 0.0f;
    float     Pitch    = 0.0f;
    float     FlySpeed = 7.5f;
};
```

- [ ] **Step 4: Add the accessors to `EditorCamera.h`**

Add the include after the existing `#include "CameraView.h"` (line 4):

```cpp
#include "EditorCameraState.h"
```

In the `public:` section of `class EditorCamera`, after `void OrbitAround(...)` (line 32), add:

```cpp
    // Snapshot / restore the persistable pose. SetState is defensive: non-finite fields
    // are ignored, Pitch is clamped to +/-89deg, FlySpeed to the [0.5, 200] wheel range.
    EditorCameraState GetState() const;
    void SetState(const EditorCameraState& s);
```

- [ ] **Step 5: Implement them in `EditorCamera.cpp`**

`EditorCamera.cpp` already has the anonymous-namespace `constexpr float kPitchLimit = glm::radians(89.0f);` and includes `<algorithm>` (`std::clamp`) and `<cmath>`. Append these definitions at the end of the file (inside the file's translation unit; they are member functions so no namespace wrapper needed):

```cpp
EditorCameraState EditorCamera::GetState() const
{
    return EditorCameraState{ m_Position, m_Yaw, m_Pitch, m_FlySpeed };
}

void EditorCamera::SetState(const EditorCameraState& s)
{
    if (std::isfinite(s.Position.x) && std::isfinite(s.Position.y) && std::isfinite(s.Position.z))
        m_Position = s.Position;
    if (std::isfinite(s.Yaw))   m_Yaw   = s.Yaw;
    if (std::isfinite(s.Pitch)) m_Pitch = std::clamp(s.Pitch, -kPitchLimit, kPitchLimit);
    if (std::isfinite(s.FlySpeed) && s.FlySpeed > 0.0f)
        m_FlySpeed = std::clamp(s.FlySpeed, 0.5f, 200.0f);
}
```

(If `<cmath>` is not already included in `EditorCamera.cpp`, add `#include <cmath>` near the top; `std::isfinite` lives there. Verify before assuming.)

- [ ] **Step 6: Build + run the test → PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_editorcam
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
```
Expected: `All editor camera tests passed.`

- [ ] **Step 7: Build `editor` (confirm the additive change compiles in context)**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 8: Commit**

```powershell
git add src/editor/src/rendering/EditorCameraState.h src/editor/src/rendering/EditorCamera.h src/editor/src/rendering/EditorCamera.cpp tests/test_editorcam.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): EditorCamera GetState/SetState + EditorCameraState"
```

---

## Task 2: Thread camera through EditorPreferences + centralize the save (TDD + cutover)

**Files:**
- Modify: `src/editor/src/EditorPreferences.h`, `src/editor/src/EditorPreferences.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.h`, `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`
- Modify: `tests/test_editorprefs.cpp`, `tests/CMakeLists.txt`

This is an atomic cutover: the mapper/`Load`/`Save` signatures gain a camera parameter, so every caller must change in the same commit or the editor won't build. TDD the pure mappers first (header-only via `test_editorprefs`), then wire the `.cpp` + callers, then build the editor once.

- [ ] **Step 1: Update `tests/test_editorprefs.cpp` to exercise the camera**

Add `#include <glm/glm.hpp>` after the existing includes (for `glm::vec3` in the test body). Then:

Replace `T00_roundtrip` with:

```cpp
static void T00_roundtrip()
{
    CullingSettings   culling;  culling.Enabled = false;          // default true
    DebugDrawSettings debug;    debug.ShowLightGizmos = true; debug.ShowCameraFrustum = true;
                                debug.ShowSelectedAABB = true; debug.Wireframe = true; debug.ShowGrid = true; // defaults all false
    ShadowSettings    shadows;  shadows.Enabled = false; shadows.Bias = 0.0042f; // defaults true / 0.0015
    EditorCameraState cam;      cam.Position = glm::vec3(1.5f, -2.5f, 3.5f); cam.Yaw = 0.7f; cam.Pitch = -0.4f; cam.FlySpeed = 21.0f;

    const nlohmann::json j = EditorPreferences::PrefsToJson(culling, debug, shadows, cam);

    CullingSettings   c2;  DebugDrawSettings d2;  ShadowSettings s2;  EditorCameraState cam2; // fresh defaults
    EditorPreferences::PrefsFromJson(j, c2, d2, s2, cam2);

    EXPECT(c2.Enabled == culling.Enabled);
    EXPECT(d2.ShowLightGizmos   == debug.ShowLightGizmos);
    EXPECT(d2.ShowCameraFrustum == debug.ShowCameraFrustum);
    EXPECT(d2.ShowSelectedAABB  == debug.ShowSelectedAABB);
    EXPECT(d2.Wireframe         == debug.Wireframe);
    EXPECT(d2.ShowGrid          == debug.ShowGrid);
    EXPECT(s2.Enabled == shadows.Enabled);
    EXPECT(std::fabs(s2.Bias - shadows.Bias) < 1e-6f);
    EXPECT(std::fabs(cam2.Position.x - cam.Position.x) < 1e-5f);
    EXPECT(std::fabs(cam2.Position.y - cam.Position.y) < 1e-5f);
    EXPECT(std::fabs(cam2.Position.z - cam.Position.z) < 1e-5f);
    EXPECT(std::fabs(cam2.Yaw - cam.Yaw) < 1e-5f);
    EXPECT(std::fabs(cam2.Pitch - cam.Pitch) < 1e-5f);
    EXPECT(std::fabs(cam2.FlySpeed - cam.FlySpeed) < 1e-5f);
}
```

Replace `T01_missing_keys_leave_defaults` with (adds a camera-absent assertion):

```cpp
static void T01_missing_keys_leave_defaults()
{
    // Partial/old document: only debugDraw.grid present (no shadows, no camera).
    nlohmann::json j;
    j["debugDraw"]["grid"] = false;

    CullingSettings   culling;            // default Enabled=true
    DebugDrawSettings debug;              // defaults all false
    debug.Wireframe = true;              // sentinel: absent key must leave this true
    ShadowSettings    shadows;            // default Enabled=true, Bias=0.0015
    EditorCameraState cam;                // sentinel pose: absent "camera" must leave it
    cam.Position = glm::vec3(9.0f, 9.0f, 9.0f); cam.FlySpeed = 50.0f;

    EditorPreferences::PrefsFromJson(j, culling, debug, shadows, cam);

    EXPECT(culling.Enabled == true);      // absent -> default kept
    EXPECT(debug.ShowGrid == false);      // present -> applied
    EXPECT(debug.Wireframe == true);      // absent -> sentinel kept
    EXPECT(shadows.Enabled == true);      // absent -> default kept
    EXPECT(std::fabs(shadows.Bias - 0.0015f) < 1e-6f); // absent -> default kept
    EXPECT(std::fabs(cam.Position.x - 9.0f) < 1e-5f);   // absent camera -> sentinel kept
    EXPECT(std::fabs(cam.FlySpeed - 50.0f) < 1e-5f);    // absent camera -> sentinel kept
}
```

- [ ] **Step 2: Add the rendering include dir to `test_editorprefs` in `tests/CMakeLists.txt`**

In the `test_editorprefs` target's `target_include_directories`, add the rendering dir so `EditorCameraState.h` (pulled in via `EditorPreferences.h`) resolves:

```cmake
target_include_directories(test_editorprefs PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/include
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)
```

- [ ] **Step 3: Configure + build to verify FAIL (signature mismatch / type unknown)**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs
```
Expected: FAIL — `'EditorCameraState': undeclared identifier` and/or too-many-arguments to `PrefsToJson`/`PrefsFromJson`.

- [ ] **Step 4: Update `src/editor/src/EditorPreferences.h` (include + 4-arg mappers + 2-arg IO)**

Add the include after `#include "RenderStats.h"` (line 8):

```cpp
#include "EditorCameraState.h"
```

Change `PrefsToJson` to take the camera and emit a `"camera"` block (replace the existing function):

```cpp
inline nlohmann::json PrefsToJson(const CullingSettings& culling,
                                  const DebugDrawSettings& debug,
                                  const ShadowSettings& shadows,
                                  const EditorCameraState& camera) {
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
        {"camera", {
            {"position", { camera.Position.x, camera.Position.y, camera.Position.z }},
            {"yaw",      camera.Yaw},
            {"pitch",    camera.Pitch},
            {"flySpeed", camera.FlySpeed},
        }},
    };
}
```

Change `PrefsFromJson` to take the camera and read the `"camera"` block (replace the existing function — keep the existing culling/debugDraw/shadows reads, append the camera read):

```cpp
inline void PrefsFromJson(const nlohmann::json& j,
                          CullingSettings& culling,
                          DebugDrawSettings& debug,
                          ShadowSettings& shadows,
                          EditorCameraState& camera) {
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
    if (j.contains("camera") && j["camera"].is_object()) {
        const auto& cam = j["camera"];
        if (cam.contains("position") && cam["position"].is_array() && cam["position"].size() == 3) {
            const auto& p = cam["position"];
            if (p[0].is_number() && p[1].is_number() && p[2].is_number())
                camera.Position = glm::vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
        }
        if (cam.contains("yaw")      && cam["yaw"].is_number())      camera.Yaw      = cam["yaw"].get<float>();
        if (cam.contains("pitch")    && cam["pitch"].is_number())    camera.Pitch    = cam["pitch"].get<float>();
        if (cam.contains("flySpeed") && cam["flySpeed"].is_number()) camera.FlySpeed = cam["flySpeed"].get<float>();
    }
}
```

Change the `Load`/`Save` declarations to take the camera:

```cpp
// Read editor_preferences.json at `path`: applies toggles to the live RenderStats
// globals and fills `camera` from the file. `camera` should be seeded with the current
// pose so a missing "camera" block leaves it. Missing file -> true. Parse error -> false.
bool Load(const std::string& path, EditorCameraState& camera);

// Serialize the live RenderStats globals + the given camera to `path`. I/O failure -> false.
bool Save(const std::string& path, const EditorCameraState& camera);
```

- [ ] **Step 5: Build + run `test_editorprefs` → PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe
```
Expected: `All editor preferences tests passed.` (The test exercises only the pure mappers; the new `Load`/`Save` decls don't affect it.)

- [ ] **Step 6: Update `src/editor/src/EditorPreferences.cpp` (`Load`/`Save` take the camera)**

Replace the two function definitions:

```cpp
bool Load(const std::string& path, EditorCameraState& camera) {
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
    PrefsFromJson(j, GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings(), camera);
    SM_TRACE("EditorPreferences: loaded '%s'", path.c_str());
    return true;
}

bool Save(const std::string& path, const EditorCameraState& camera) {
    const json j = PrefsToJson(GetCullingSettings(), GetDebugDrawSettings(), GetShadowSettings(), camera);
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
```

- [ ] **Step 7: Wire load/save in `ImGuiRenderer.cpp`**

(a) In `ImGuiRenderer::Init`, replace the existing prefs-load block (the comment + `EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH);` line) with:

```cpp
    // Restore persisted RenderStats toggles + editor camera pose. Runs after the editor
    // defaults above so a saved file wins; first run (no file) keeps the defaults.
    EditorCameraState cs = m_EditorCamera.GetState();
    EditorPreferences::Load(EditorPreferences::DEFAULT_PREFERENCES_PATH, cs);
    m_EditorCamera.SetState(cs);
```

(b) Find the panel call site `DrawRenderStatsPanel(&s_ShowRenderStatsPanel);` and replace it with a save-on-change:

```cpp
        if (DrawRenderStatsPanel(&s_ShowRenderStatsPanel))
            EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
```

(c) At the very top of `ImGuiRenderer::Shutdown()` (before `m_ImGuiNvrhi.reset();`), add the definitive save:

```cpp
    // Persist final editor preferences (RenderStats toggles + camera pose) on exit.
    EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
```

`ImGuiRenderer.cpp` already includes `EditorCamera.h` (which now pulls `EditorCameraState.h`) and `EditorPreferences.h`, so no new includes are needed. Before relying on the `Shutdown` save, confirm `ImGuiRenderer::Shutdown()` is on the normal editor-exit teardown path (grep for where it's called — it should be the full teardown, distinct from `ShutdownNvrhiOnly` used for backend swaps). If `Shutdown` turns out NOT to run on exit, report DONE_WITH_CONCERNS noting the on-change save still captures a recent pose.

- [ ] **Step 8: `RenderStatsPanel` returns `bool changed`, drop self-save**

In `src/editor/src/rendering/imgui/RenderStatsPanel.h`, change the declaration + comment:

```cpp
#pragma once

// Draws the "Render Stats" debug window: per-frame mesh draw/cull counters and the
// debug toggles. `open` may be null (always draw) or point to a toggle bool.
// Returns true if any toggle/slider changed this frame (so the caller can persist).
bool DrawRenderStatsPanel(bool* open);
```

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, replace the entire file with (drops the `EditorPreferences.h` include + the `Save` call; returns `changed`):

```cpp
#include "RenderStatsPanel.h"

#include <imgui.h>

#include "RenderStats.h" // resolved via the editor's Engine PUBLIC include dirs (same as StagingBufferPool.h in MemoryPanel)

bool DrawRenderStatsPanel(bool* open)
{
    if (open && !*open) return false;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return false; }

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
    // The slider mutates Bias live every drag frame; report the change only once the
    // drag ends (IsItemDeactivatedAfterEdit) so the caller doesn't rewrite every frame.
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();

    ImGui::End();
    return changed;
}
```

- [ ] **Step 9: Build `editor` + run both tests**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs
cmake --build --preset msvc-win64-vs2026-community --target test_editorcam
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
```
Expected: `editor` builds clean; both tests print their `All … passed.` lines.

- [ ] **Step 10: Commit**

```powershell
git add src/editor/src/EditorPreferences.h src/editor/src/EditorPreferences.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/imgui/RenderStatsPanel.h src/editor/src/rendering/imgui/RenderStatsPanel.cpp tests/test_editorprefs.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): persist editor camera pose in editor_preferences.json"
```

---

## Done criteria

- `test_editorprefs` and `test_editorcam` print their `All … passed.` lines; `editor` builds clean.
- User GUI smoke: move the editor camera, change a RenderStats toggle (writes the file), and/or close the editor → reopen → camera viewpoint AND toggles are restored. A corrupt/edited `"camera"` block never leaves the camera unusable (SetState sanitizes).
