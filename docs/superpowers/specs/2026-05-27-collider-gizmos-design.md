# Collider Gizmos (Editor Debug Draw) — Design

**Date:** 2026-05-27
**Status:** Approved (design); pending spec review
**Scope:** Engine debug-draw + editor toggle + preference persistence. No ECS/GAME_API change.

## Problem

Authoring a `ColliderComponent` (Phase-1 v1 collision) is blind: there's no way to see where a collider sits in the world, what shape it is, or whether it's a trigger vs. static blocker. Sizing and placing colliders means guessing and re-checking by playing the game and bumping into walls.

## Goal

A toggleable editor gizmo that draws every `ColliderComponent` as a wireframe of its **authored shape** (Box / Sphere / Capsule), colored by role (static blocker / trigger / dynamic). Mirrors the existing `DebugDrawSettings` toggles (`ShowGrid`, `ShowSelectedAABB`, `ShowLightGizmos`, `ShowCameraFrustum`). Toggle state persists in `editor_preferences.json`.

## Why authored-shape, not the collision-AABB

`Collision.h` reduces every shape to a world-axis AABB for the v1 kinematic resolver. The user might expect the gizmo to show that AABB, but:

- The **`Selected AABB`** debug toggle already draws an AABB (for the selected entity).
- Authoring intent is the *shape* — a sphere collider should look like a sphere, not a cube — so the user can size and place it confidently.
- The AABB approximation is an internal collision detail. If the user wants to inspect it, the existing toggle covers it.

So the gizmo draws the **authored outline** (true sphere wireframe, true capsule outline, Box-AABB-for-Box). One-trick collision (rotation ignored) means the Box happens to be world-aligned; that's correct, not a regression.

## Architecture

Strictly an engine-side debug-draw block + a settings flag + an editor checkbox. No new ECS components, no new system, no GAME_API bump.

### 1. Settings flag

`src/engine/src/rendering/RenderStats.h` — add to `DebugDrawSettings`:

```cpp
bool ShowColliders = false;
```

`RenderStats.cpp` unchanged (the struct's exported accessor `GetDebugDrawSettings()` already exists). Render-thread-touched, like the sibling flags.

### 2. New helper

`src/engine/src/rendering/DebugDraw.h` — add a capsule helper alongside `DebugAppendBox`/`DebugAppendSphere`:

```cpp
inline void DebugAppendCapsule(std::vector<DebugVertex>& out,
                               const glm::vec3& center,
                               float radius, float halfHeight,
                               const glm::vec4& color, int segments = 16);
```

Implementation = cylinder body (4 vertical lines from the cap-cylinder junctions + 2 horizontal circles at top/bottom of the cylinder) + 2 full `DebugAppendSphere`s at the dome centers (above/below the cylinder by `halfHeight`). Overlap of the sphere with the cylinder body is invisible in wireframe — cheapest visually-clear option. `segments = 16` matches `DebugAppendSphere` defaults' aesthetic.

### 3. Render block

`src/engine/src/rendering/passes/DebugRenderPass.cpp` — new toggle block alongside the existing ones. Pseudocode:

```cpp
if (s.ShowColliders) {
    world->Each<TransformComponent, ColliderComponent>([&](EntityId,
        const TransformComponent& t, const ColliderComponent& c) {
        const glm::vec3 center = t.Position + c.Offset * t.Scale;     // matches Collision.h
        const glm::vec3 absScale = glm::abs(t.Scale);

        const glm::vec4 color =
              c.IsTrigger  ? glm::vec4(0.20f, 0.85f, 1.00f, 1.0f)      // cyan: passable
            : !c.IsStatic  ? glm::vec4(1.00f, 0.55f, 0.10f, 1.0f)      // orange: dynamic
                           : glm::vec4(1.00f, 0.85f, 0.10f, 1.0f);     // yellow: static blocker

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

Extent math mirrors `Collision.h::ComputeColliderHalfExtents` so the gizmo is **visually consistent with the actual collision shape**. Center math mirrors `ComputeColliderCenter`. The same `if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid) return;` early-out at the top of the pass must be extended to include `&& !s.ShowColliders` so the new toggle doesn't short-circuit.

### 4. Editor toggle + persistence

`src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — one new checkbox in the Debug Draw section:

```cpp
changed |= ImGui::Checkbox("Colliders", &dd.ShowColliders);
```

`src/editor/src/EditorPreferences.{h,cpp}` — extend the persisted DebugDraw block to include `ShowColliders` (mirrors the existing `ShowGrid`/`ShowSelectedAABB` save/load). The centralized save in `ImGuiRenderer.cpp` (on panel `changed`) already covers it.

## Coloring rationale

| State (`IsStatic`, `IsTrigger`) | Color | Why |
|---|---|---|
| static + not trigger | **yellow** | Walls / world blockers — high-attention authoring target. |
| trigger (either static or not) | **cyan** | Water/passable — distinct, "soft" color. |
| dynamic + not trigger | **orange** | Currently passes through (v1 dynamic-vs-dynamic = no block). Signals "will block later." |

Selected-entity highlight is **out of scope**: the existing `Selected AABB` toggle already singles out the selected entity. The colliders gizmo is for whole-scene situational awareness.

## Testing

Pure render — no unit test. Manual smoke checklist:

- Author a Box collider on an entity, toggle on → see a yellow wireframe box at `Position + Offset` of size `Size * Scale`.
- Switch to Sphere → wireframe sphere with the same color rule, radius from `Size.x * max(Scale)`.
- Switch to Capsule → wireframe (cylinder + sphere caps); resizing `Size.x` (radius) and `Size.y` (half-height) visibly works.
- Toggle `IsTrigger` → cyan. Toggle `IsStatic` off → orange. Restore → yellow.
- Toggle `ShowColliders` off in the panel → all wireframes disappear (no draws).
- Close + relaunch the editor → toggle state persists.
- DX12 + Vulkan: no rendering-backend asymmetry expected (debug pass already works on both).

## Touch list

- `src/engine/src/rendering/RenderStats.h` — `DebugDrawSettings::ShowColliders`.
- `src/engine/src/rendering/DebugDraw.h` — `DebugAppendCapsule`.
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` — new `if (s.ShowColliders)` block + the early-out gate extension.
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — one checkbox.
- `src/editor/src/EditorPreferences.{h,cpp}` — persist the field.

No changes to ECS.h, ECSCommands.h, ComponentSerialization.h, WorldManager, game.cpp, game.h. No `GAME_API_VERSION` bump.

## Non-goals (YAGNI)

- No OBB / rotated visualization (collision ignores rotation; gizmo follows suit).
- No selected-collider highlight (covered by existing Selected-AABB toggle).
- No per-layer color coding (would need a UI; future if collision layers grow).
- No depth-occluded vs always-on-top distinction; the existing debug pass already chooses.
- No runtime persistence (`runtime.exe` never reads editor_preferences.json — by design).
