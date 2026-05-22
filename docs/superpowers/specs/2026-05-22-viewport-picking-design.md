# Viewport Mouse-Picking — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Left-click an entity in the editor Viewport panel to select it: the click casts a world-space
ray (from the camera through the clicked pixel) against each mesh entity's world-space AABB and
selects the nearest hit, setting the ECS Inspector's `selectedEntity` so the gizmo and per-
component editors follow. Clicking empty space deselects. Editor-only, render-thread; reuses the
frustum-culling AABBs. No GameThread or ECS-command involvement — selection is editor state.

Bundled DRY (user-requested): extract the duplicated `T*Rz*Ry*Rx*S` transform→matrix formula into
one shared `ModelMatrix(const TransformComponent&)` and adopt it in the mesh pass, the inspector
gizmo, and the new picker.

## Background (verified)

- **AABBs already exist:** `MeshSystem::GetMeshBounds(meshId)` returns local-space `{min,max,valid}`;
  `src/common/include/Frustum.h` has `TransformAABB(model, localMin, localMax, outMin, outMax)`
  (local→world AABB). Picking reuses both.
- **Camera:** `WorldCameraComponent { glm::mat4 View; glm::mat4 Projection; glm::vec3 Position; }`
  (`ECS.h:117`), a singleton in the ECS snapshot. GLM is depth `[0,1]` (ZO), right-handed.
- **Entity iteration:** `ECS::Each<TransformComponent, MeshComponent>(fn) const` (`ECS.h:640`).
  `TransformComponent { Position, Rotation, Scale }`; `MeshComponent { uint32_t MeshId; bool Visible; }`.
- **Viewport rect is already published to the editor context:** `EditorContext` carries
  `ViewportMinX/MinY` (screen-space image top-left), `ViewportW/ViewportH`, plus `World`
  (live snapshot), `WorldSnapshot`, `MeshSys`. The `ImGuiRenderer` Viewport window already records
  `m_ViewportHovered`/`m_ViewportFocused`.
- **Selection lives in the inspector:** `EcsInspectorPanel::selectedEntity` (private member,
  `EcsInspectorPanel.h:14`); the entity list highlights it and the Transform editor draws the
  gizmo (`m_Gizmo`) for it. There is no public way to set it from outside today.
- **The duplicated model matrix:** `MeshRenderPass.cpp` has a file-static
  `BuildWorldMatrix(const TransformComponent&)` (`M = T*Rz*Ry*Rx*S`, used at the cull gate + the
  instance loop); `EcsInspectorPanel.cpp`'s gizmo builds the same `M` inline from `editTransform`.
  (`game.cpp`'s camera builds a *view* matrix — inverse/negated — NOT a model matrix; it is NOT a
  consumer and stays untouched.)
- **Input:** clicks reach the editor via ImGui (`ImGuiInputRing` → `io`); picking reads ImGui
  mouse state, independent of the game input ring. In play mode (cursor-locked) clicks go to the
  game, so picking is naturally edit-mode only.

## Scope

**In scope:** `Picking.h` (pure ray/AABB math) + `test_picking`; `TransformMath.h`
(`ModelMatrix`); adopting `ModelMatrix` in `MeshRenderPass` + the inspector gizmo; a
`ViewportPicker` (editor) that ray-casts entities; `EcsInspectorPanel::SetSelectedEntity`; the
click-detect + wiring in `ImGuiRenderer::Render`; CMake additions.

**Out of scope / non-goals:** triangle-precise or GPU entity-ID-buffer picking (ray-vs-AABB only;
nearest wins); multi-select / box-select; play-mode picking; `game.cpp` camera; `GAME_API_VERSION`
(no `GameState`/ECS-layout change). Reusing `ModelMatrix` in `game.cpp` (it builds a view matrix,
not a model matrix).

## Design

### 1. `src/common/include/TransformMath.h` (new, header-only) — shared model matrix
```cpp
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ECS.h" // TransformComponent

// The single source of truth for an entity's world matrix: T * Rz * Ry * Rx * S
// (translate * rotZ * rotY * rotX * scale), Euler radians from TransformComponent::Rotation.
inline glm::mat4 ModelMatrix(const TransformComponent& t)
{
    glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
    glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, glm::vec3(1.f, 0.f, 0.f));
    glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, glm::vec3(0.f, 1.f, 0.f));
    glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, glm::vec3(0.f, 0.f, 1.f));
    glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
    return T * Rz * Ry * Rx * S;
}
```
**Adopt it (behavior-preserving — identical formula):**
- `MeshRenderPass.cpp`: delete the file-static `BuildWorldMatrix`; `#include "TransformMath.h"`;
  replace both `BuildWorldMatrix(...)` call sites with `ModelMatrix(...)`.
- `EcsInspectorPanel.cpp`: replace the inline gizmo `M = T*Rz*...` block with
  `glm::mat4 M = ModelMatrix(editTransform);` (`#include "TransformMath.h"`).

### 2. `src/common/include/Picking.h` (new, header-only, pure GLM — unit-testable)
```cpp
#pragma once
#include <glm/glm.hpp>

struct Ray { glm::vec3 Origin; glm::vec3 Dir; };

// World-space ray from a click at (mouseX,mouseY) screen coords inside the Viewport image rect
// [vpMinX,vpMinY, vpW x vpH]. ImGui Y is top-down; NDC Y is flipped. Unprojects near (z=0) and
// far (z=1) — depth [0,1] (ZO) — through inverse(proj*view).
inline Ray ScreenPointToRay(float mouseX, float mouseY,
                            float vpMinX, float vpMinY, float vpW, float vpH,
                            const glm::mat4& view, const glm::mat4& proj);

// Slab test. Returns true and the entry distance tHit (clamped >= 0) when the ray hits the AABB.
inline bool RayIntersectsAABB(const Ray& r, glm::vec3 aabbMin, glm::vec3 aabbMax, float& tHit);
```
`ScreenPointToRay`: `ndcX = 2*(mouseX-vpMinX)/vpW - 1`, `ndcY = 1 - 2*(mouseY-vpMinY)/vpH`;
`invVP = inverse(proj*view)`; `near = invVP*vec4(ndcX,ndcY,0,1)` (÷w), `far = invVP*vec4(ndcX,ndcY,1,1)`
(÷w); `Origin = near`, `Dir = normalize(far-near)`. The ZO near/far + Y-flip are the #1
correctness risk → covered by `test_picking`.
`RayIntersectsAABB`: standard slab method; handles `Dir` components near zero; `tHit` = entry t
(if the origin is inside, tHit = 0).

### 3. `tests/test_picking.cpp` (+ `test_picking` target)
Pure GLM, mirrors `test_frustum`'s self-contained style. Cases with a known
`perspectiveRH_ZO(radians(60),1,0.1,100)` + `lookAtRH(origin, -Z, +Y)` and a 1000×1000 viewport
at origin: center click → ray Origin near camera, Dir ≈ (0,0,-1); a click maps to a ray that
`RayIntersectsAABB` hits for a unit box at (0,0,-10) and misses for one at (8,0,-10); a box behind
the camera is not hit (no positive tHit); two boxes along the ray → smaller `tHit` is the nearer.
Also a couple of direct `RayIntersectsAABB` asserts (axis-aligned hit, parallel miss, inside-origin
tHit==0).

### 4. `src/editor/src/rendering/imgui/ViewportPicker.{h,cpp}` (new)
```cpp
#pragma once
#include "ECS.h" // EntityId
struct EditorContext;

// Ray-casts the click (screen coords) against every visible mesh entity's world AABB and returns
// the nearest hit, or INVALID_ENTITY if none. Reads camera/entities/bounds/viewport-rect from ctx.
EntityId PickEntity(const EditorContext& ctx, float mouseX, float mouseY);
```
`.cpp`: read `WorldCameraComponent` from `ctx.World` (fallback: no camera → return
`INVALID_ENTITY`); `Ray r = ScreenPointToRay(mouseX, mouseY, ctx.ViewportMinX, ctx.ViewportMinY,
(float)ctx.ViewportW, (float)ctx.ViewportH, cam.View, cam.Projection)`; iterate
`ctx.WorldSnapshot->Each<TransformComponent, MeshComponent>([&](EntityId e, const TransformComponent& t, const MeshComponent& m){ ... })`:
skip `!m.Visible`; `bounds = ctx.MeshSys->GetMeshBounds(m.MeshId)`; skip `!bounds.valid`;
`TransformAABB(ModelMatrix(t), bounds.min, bounds.max, wMin, wMax)`; `RayIntersectsAABB(r, wMin, wMax, tHit)`
→ track the smallest `tHit`. Return the best entity (or `INVALID_ENTITY`).

### 5. Wiring
- `EcsInspectorPanel.h`: add `public: void SetSelectedEntity(EntityId e) { selectedEntity = e; }`.
- `ImGuiRenderer::Render`, **immediately after the `ctx.Viewport*` fields are assigned** (i.e.
  just after the `"Viewport"` window's `ImGui::End()` + the existing `ctx.ViewportDrawList/MinX/...`
  publish block), add:
  ```cpp
  if (m_ViewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
      && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
      m_EcsInspector.SetSelectedEntity(PickEntity(ctx, io.MousePos.x, io.MousePos.y));
  }
  ```
  Placing it here (not inside the `Begin` block) guarantees `ctx.ViewportMinX/Y/W/H` hold this
  frame's values before `PickEntity` reads them; `m_ViewportHovered` is the member set inside the
  block this frame, and `io` is the in-scope `ImGuiIO&`. The `!ImGuizmo::IsOver()/IsUsing()` guard
  means clicking a gizmo handle manipulates instead of re-picking; `PickEntity` returning
  `INVALID_ENTITY` (empty space) deselects.

## Data flow

Click in Viewport (edit mode) → `ImGuiRenderer` (render thread) builds the ray from the snapshot
camera + viewport rect → `PickEntity` ray-casts entity world-AABBs → nearest entity (or invalid)
→ `m_EcsInspector.SetSelectedEntity(...)` → the inspector highlights it, draws its component
editors, and the gizmo targets it. All synchronous on the render thread; no cross-thread or ECS
command needed (entity *manipulation* still flows through the existing inspector→ECSCommand path).

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump.
- `test_picking` (ray/AABB math), `test_ecs`/`test_alloc`/`test_frustum`/`test_input` all green.
- `editor` + `runtime` build clean. (The `ModelMatrix` adoption in `MeshRenderPass` is
  behavior-preserving — runtime renders unchanged.)
- **GUI smoke (user):** click a model in the Viewport → it becomes selected (Inspector highlights
  it, gizmo appears on it); click another → selection moves; click empty space → deselected
  (gizmo/editor gone); clicking a gizmo handle still drags the gizmo (doesn't re-pick); selection
  via the Inspector list still works; picking respects the camera (orbit, then click still hits
  the right object); `runtime.exe` unaffected.

## Risks

- **ZO unproject / Y-flip wrong** → ray misaligned, clicks miss or pick the wrong entity →
  mitigated by `test_picking` + reusing the proven ZO convention from `Frustum.h`.
- **`ModelMatrix` adoption changes behavior** → mitigated: identical formula, behavior-preserving;
  verified by the existing scene rendering + gizmo still matching.
- **AABB picking imprecision** (picks bounding box, not triangles; overlapping/again-behind cases)
  → accepted per non-goals; nearest-`tHit` tiebreak is reasonable; GPU ID-buffer is the future
  upgrade.
- **Click handler ordering** (reading `ctx.Viewport*` before they're set this frame) → mitigated by
  placing the click handling inside/after the Viewport block where those fields are assigned.
