# Collider Gizmos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Toggleable editor wireframe gizmos for every `ColliderComponent` (Box/Sphere/Capsule), colored by static / trigger / dynamic, with state persisted in `editor_preferences.json`.

**Architecture:** Engine-only debug draw mirroring the existing `ShowGrid`/`ShowSelectedAABB` toggles. `DebugRenderPass` iterates `Each<TransformComponent, ColliderComponent>` and appends wireframe geometry inline; capsule gets a new `DebugAppendCapsule` helper (cylinder + 2 sphere caps). No ECS/GAME_API change.

**Tech Stack:** C++23, NVRHI DebugRenderPass (line-list), Dear ImGui editor, nlohmann/json (preferences).

**Spec:** `docs/superpowers/specs/2026-05-27-collider-gizmos-design.md`.

---

## Reference patterns (verified)

- `DebugDraw.h` has `DebugAppendLine`/`DebugAppendBox`/`DebugAppendSphere`/`DebugAppendArrow`/`DebugAppendFrustum`/`DebugAppendGrid`. Vertex format `DebugVertex { vec3 Position; vec4 Color; }`. Sphere helper draws 3 axis-aligned great circles via N segments.
- `RenderStats.h` `DebugDrawSettings { ShowLightGizmos, ShowCameraFrustum, ShowSelectedAABB, Wireframe, ShowGrid };` — all bool defaults false (except Wireframe). Engine-exported via `GetDebugDrawSettings()`.
- `DebugRenderPass.cpp` Render(): early-out gate `if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid) return;` then one `if (s.X) { … }` per toggle, appending into `m_Verts`. Pass already includes `RenderStats.h`, glm, `ECS.h` (for `Each<TransformComponent, LightningComponent>`).
- `RenderStatsPanel.cpp` Debug Draw section: `changed |= ImGui::Checkbox("Grid", &dd.ShowGrid);` etc. Returns `changed`; the call site in `ImGuiRenderer.cpp` triggers `EditorPreferences::Save` on true.
- `EditorPreferences.h` has inline pure `PrefsToJson(culling, debug, shadows, camera)` + `PrefsFromJson(...)` — both list each `DebugDrawSettings` field by its JSON key (`"lightGizmos"`, `"cameraFrustum"`, `"selectedAABB"`, `"wireframe"`, `"grid"`). Missing keys leave defaults untouched.
- `ColliderComponent { ColliderShape Shape; glm::vec3 Size; glm::vec3 Offset; bool IsTrigger; bool IsStatic; uint32_t Layer; Mask; }` and `enum class ColliderShape : uint8_t { Box, Sphere, Capsule }` in `ECS.h` (already imported by `DebugRenderPass.cpp` transitively via Each usage).
- Collision-extent math (mirrored from `src/game/src/Collision.h::ComputeColliderHalfExtents`/`ComputeColliderCenter`) inlined in the new block — engine must not depend on the game header.

> **No ECS.h layout change, no `GAME_API_VERSION` bump.** Engine rebuild only; editor restart NOT required.

---

## File Structure

- `src/engine/src/rendering/RenderStats.h` — add `bool ShowColliders` field.
- `src/engine/src/rendering/DebugDraw.h` — add `DebugAppendCapsule` helper.
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` — extend early-out gate + add `if (s.ShowColliders)` block.
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — one checkbox.
- `src/editor/src/EditorPreferences.h` — `colliders` key in `PrefsToJson` + `PrefsFromJson`.

---

### Task 1: Settings flag + capsule helper

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h`
- Modify: `src/engine/src/rendering/DebugDraw.h`

- [ ] **Step 1: Add ShowColliders to DebugDrawSettings**

In `src/engine/src/rendering/RenderStats.h`, inside `struct DebugDrawSettings`, after `bool ShowGrid = false;`, add:

```cpp
    bool ShowColliders     = false;
```

- [ ] **Step 2: Add DebugAppendCapsule to DebugDraw.h**

In `src/engine/src/rendering/DebugDraw.h`, after the `DebugAppendSphere` function (~line 46), add:

```cpp
// Capsule outline = cylinder body (4 vertical seams + 2 horizontal circles at the cylinder
// caps) + 2 sphere caps at the dome centers. center is the midpoint between the domes;
// halfHeight is half the cylinder length (so total tip-to-tip height = 2*(halfHeight+radius)).
// Cylinder runs along world Y. Wireframe-only -> the sphere/cylinder overlap is invisible.
inline void DebugAppendCapsule(std::vector<DebugVertex>& out, const glm::vec3& center,
                               float radius, float halfHeight,
                               const glm::vec4& color, int segments = 16) {
    if (radius < 1e-4f) radius = 1e-4f;
    if (halfHeight < 0.0f) halfHeight = 0.0f;
    if (segments < 4) segments = 4;
    const float kTwoPi = 6.28318530718f;
    const glm::vec3 topCenter    = center + glm::vec3(0.0f,  halfHeight, 0.0f);
    const glm::vec3 bottomCenter = center + glm::vec3(0.0f, -halfHeight, 0.0f);

    // 2 horizontal circles at the cylinder caps (XZ plane at top/bottom).
    for (int i = 0; i < segments; ++i) {
        const float t0 = kTwoPi * (float(i)     / float(segments));
        const float t1 = kTwoPi * (float(i + 1) / float(segments));
        const glm::vec3 a(std::cos(t0) * radius, 0.0f, std::sin(t0) * radius);
        const glm::vec3 b(std::cos(t1) * radius, 0.0f, std::sin(t1) * radius);
        DebugAppendLine(out, topCenter    + a, topCenter    + b, color);
        DebugAppendLine(out, bottomCenter + a, bottomCenter + b, color);
    }

    // 4 vertical seam lines connecting the cap circles (at +X, -X, +Z, -Z).
    const glm::vec3 seams[4] = {
        { radius, 0.0f, 0.0f }, { -radius, 0.0f, 0.0f },
        { 0.0f, 0.0f,  radius }, { 0.0f, 0.0f, -radius },
    };
    for (const glm::vec3& s : seams) {
        DebugAppendLine(out, bottomCenter + s, topCenter + s, color);
    }

    // Dome caps: 2 full spheres at the dome centers (wireframe overlap is invisible).
    DebugAppendSphere(out, topCenter,    radius, color, segments);
    DebugAppendSphere(out, bottomCenter, radius, color, segments);
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected: builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/engine/src/rendering/DebugDraw.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(debug): ShowColliders toggle + DebugAppendCapsule helper"
```

---

### Task 2: DebugRenderPass — render collider gizmos

**Files:**
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp`

- [ ] **Step 1: Extend the early-out gate**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`, find the line (~92):

```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid)
        return;
```

Change it to:

```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders)
        return;
```

- [ ] **Step 2: Add the ShowColliders block**

In the same file, immediately after the `if (s.ShowSelectedAABB) { … }` block (the one that ends with the `}` before `if (m_Verts.empty())`, around line 149), add:

```cpp
    if (s.ShowColliders) {
        // Mirrors src/game/src/Collision.h math (center = Position + Offset*Scale;
        // extents per shape). Inlined here so the engine debug pass doesn't depend on the
        // game header.
        world->Each<TransformComponent, ColliderComponent>(
            [&](EntityId, const TransformComponent& t, const ColliderComponent& c) {
                const glm::vec3 absScale = glm::abs(t.Scale);
                const glm::vec3 center   = t.Position + c.Offset * t.Scale;

                const glm::vec4 color =
                      c.IsTrigger ? glm::vec4(0.20f, 0.85f, 1.00f, 1.0f)   // cyan: trigger
                    : !c.IsStatic ? glm::vec4(1.00f, 0.55f, 0.10f, 1.0f)   // orange: dynamic
                                  : glm::vec4(1.00f, 0.85f, 0.10f, 1.0f);  // yellow: static blocker

                switch (c.Shape) {
                    case ColliderShape::Box: {
                        const glm::vec3 ext = glm::max(glm::abs(c.Size) * absScale, glm::vec3(0.0001f));
                        DebugAppendBox(m_Verts, center - ext, center + ext, color);
                        break;
                    }
                    case ColliderShape::Sphere: {
                        const float r = std::max({ c.Size.x * absScale.x,
                                                   c.Size.x * absScale.y,
                                                   c.Size.x * absScale.z, 0.0f });
                        DebugAppendSphere(m_Verts, center, r, color);
                        break;
                    }
                    case ColliderShape::Capsule: {
                        const float r  = std::max({ c.Size.x * absScale.x,
                                                    c.Size.x * absScale.z, 0.0f });
                        const float hh = std::max(c.Size.y * absScale.y, 0.0f);
                        DebugAppendCapsule(m_Verts, center, r, hh, color);
                        break;
                    }
                }
            });
    }
```

(`DebugAppendCapsule` is the helper added in Task 1; `ColliderComponent`/`ColliderShape` resolve via the existing `ECS.h` include.)

- [ ] **Step 3: Build to verify**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine editor`
Expected: builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(debug): render collider gizmos (Box/Sphere/Capsule, colored by role)"
```

---

### Task 3: Editor checkbox + persistence

**Files:**
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`
- Modify: `src/editor/src/EditorPreferences.h`

- [ ] **Step 1: Add the Colliders checkbox**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, in the Debug Draw section, after the existing `Grid` checkbox (line ~30):

```cpp
    changed |= ImGui::Checkbox("Grid",           &dd.ShowGrid);
```

add:

```cpp
    changed |= ImGui::Checkbox("Colliders",      &dd.ShowColliders);
```

- [ ] **Step 2: Persist the field — write side**

In `src/editor/src/EditorPreferences.h`, in the `PrefsToJson` function, inside the `"debugDraw"` object literal, after the line `{"grid",          debug.ShowGrid},`, add:

```cpp
            {"colliders",     debug.ShowColliders},
```

- [ ] **Step 3: Persist the field — read side**

In the same file, in `PrefsFromJson`, in the `if (j.contains("debugDraw") && j["debugDraw"].is_object())` block, after the line:

```cpp
        if (d.contains("grid")          && d["grid"].is_boolean())          debug.ShowGrid          = d["grid"].get<bool>();
```

add:

```cpp
        if (d.contains("colliders")     && d["colliders"].is_boolean())     debug.ShowColliders     = d["colliders"].get<bool>();
```

- [ ] **Step 4: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors.

- [ ] **Step 5: Manual smoke (human — no editor restart needed; no ECS.h change)**

- Author an entity with `TransformComponent` + `ColliderComponent` (Box, default Size 1,1,1). Toggle **Render Stats → Debug Draw → Colliders** on → yellow wireframe box at the entity's position.
- Switch Shape to `Sphere` → yellow wireframe sphere with radius `Size.x * max(Scale)`.
- Switch Shape to `Capsule`, set `Size = (0.5, 1.0, 0)` → wireframe capsule, radius 0.5 cylinder seams + half-spheres on top/bottom (total height = 2*(1.0+0.5)=3.0).
- Toggle `Trigger` on → cyan. Toggle `Static` off → orange. Restore → yellow.
- Toggle the panel off → all gizmos disappear (DebugRenderPass early-outs).
- Close + relaunch the editor → toggle state persists (verify `editor_preferences.json` next to `editor.exe` has `"debugDraw": { …, "colliders": <bool> }`).
- Verify on **DX12 and Vulkan** (`editor.exe --backend=vulkan`) — debug pass should behave identically.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/editor/src/EditorPreferences.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): Colliders toggle in Render Stats, persisted in editor_preferences.json"
```

---

## Final verification

- [ ] Existing test still green (no regression): `cmake --build --preset msvc-win64-vs2026-community --target test_editorprefs` then `./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorprefs.exe` → existing tests pass (the new `colliders` key is additive; tests don't assert key absence).
- [ ] Human GUI smoke per Task 3 Step 5 (DX12 + Vulkan).

## Self-review notes (vs. spec)

- Covers spec §Architecture sections 1–4 (flag, capsule helper, render block, editor checkbox + persistence) verbatim.
- No unit test for the render block (spec: "Pure render — no unit test"). The `EditorPreferences` round-trip is exercised by the existing `test_editorprefs` (additive key, non-regression).
- No GAME_API bump (no ECS.h / GameState layout change).
- Colors and extent math match the spec exactly. Capsule helper produces a recognizable capsule (cylinder + 2 dome spheres) — overlap of the sphere with the cylinder is invisible in wireframe.
