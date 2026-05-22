# CPU Frustum Culling — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Skip CPU-side submission of meshes whose world-space AABB is fully outside the camera
frustum, in `MeshRenderPass` on the RenderThread. Because the work lives in the `Engine`
shared lib, **both `editor.exe` and `runtime.exe`** benefit. The cull frustum is the *same*
view-projection the pass already uses to render, so nothing visible on screen is ever
dropped — the only change is fewer draw/instance calls for off-screen entities.

Two supporting deliverables:
- **Per-frame render stats** (total / drawn / culled mesh entities, instances, batches),
  tracked in an Engine-side struct so the stripped runtime accumulates them too.
- **A cull on/off toggle** (default on), surfaced from a **dedicated editor ImGui panel**
  (`Render Stats`), for A/B comparison and to rule culling out if a mesh wrongly vanishes.

## Background (verified)

- **Collection loop** — `src/engine/src/rendering/passes/MeshRenderPass.cpp:381-391`:
  ```cpp
  world->Each<TransformComponent, MeshComponent>(
      [&](EntityId e, const TransformComponent&, const MeshComponent& meshComp) {
          if (!meshComp.Visible) return;
          const auto* materialComp = world->GetComponent<MaterialComponent>(e);
          uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;
          entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
      });
  ```
  The cull check inserts here, right after the existing `Visible` check. The lambda already
  receives the `TransformComponent` (currently unnamed) — name it and use it.
- **World matrix** — built only in the *instance* loop, `MeshRenderPass.cpp:453-459`:
  `M = T * Rz * Ry * Rx * S` (translate · rotZ · rotY · rotX · scale, Euler radians from
  `TransformComponent::Rotation`). Culling needs this matrix at collection time → extract a
  file-local `BuildWorldMatrix(const TransformComponent&)` and call it in **both** places so
  the cull transform and the render transform can never diverge.
- **Camera / VP** — `MeshRenderPass.cpp:357-362` reads `WorldCameraComponent` singleton
  (`View`, `Projection`, `Position`) and already computes `perFrame.VP = P * V`. Reuse that
  exact `VP` for frustum extraction. Projection is `glm::perspectiveRH_ZO` (depth **[0,1]**,
  right-handed) — confirmed `CLAUDE.md:110`, computed on GameThread in
  `src/game/src/game.cpp` `FreeLookCameraSystem`, published via the ECS snapshot/singleton.
- **Mesh bounds already exist** — `src/engine/src/rendering/MeshSystem.h:49-54`:
  ```cpp
  struct BoundingBox { glm::vec3 min{0}; glm::vec3 max{0}; bool valid = false; };
  BoundingBox GetMeshBounds(uint32_t meshId) const;
  ```
  Computed from vertex positions at upload (`MeshSystem.cpp:86-94`), local/model space.
  `GetMeshBounds` is a vector index + copy; out-of-range returns `valid=false`. The pass
  already reaches `MeshSystem` via `m_Renderer->GetMeshSystem()` (`MeshRenderPass.cpp:419`).
- **No frustum math exists** anywhere in the repo (grep: `frustum|plane|extractPlanes` →
  only the `MeshSystem` AABB). This is greenfield, pure GLM.
- **No render stats exist** today — the editor `Memory` panel
  (`src/editor/src/rendering/imgui/MemoryPanel.cpp`) shows only memory/pool stats. No draw
  / triangle / cull counters.
- **Panel pattern** — `MemoryPanel` exposes `void DrawMemoryPanel(bool* open, const ECS* world);`
  (`MemoryPanel.h`), called from `ImGuiRenderer.cpp:481-482` behind a `static bool` toggle;
  it `#include`s its Engine stat header the same way it includes `"StagingBufferPool.h"`
  (the editor links `Engine` with PUBLIC includes). A new panel mirrors this exactly.
- **Cross-DLL singletons** — an `inline`/header function-local `static` duplicates per module
  across the Engine.dll/editor.exe boundary (the staging-pool bug). Any process-wide state
  shared between the mesh pass (Engine.dll) and the panel (editor.exe) must be **defined in
  one Engine.dll TU and exported `ENGINE_API`**.

## Scope

**In scope:** a header-only `Frustum.h` (plane extraction + AABB transform + visibility
test); a `RenderStats.{h,cpp}` Engine-exported stats + culling-settings singleton; the
`MeshRenderPass.cpp` edits (extract `BuildWorldMatrix`, frustum extract, cull gate, stats);
a new editor `RenderStatsPanel.{h,cpp}` + its wiring in `ImGuiRenderer.cpp`; a `test_frustum`
unit-test target; the `CMakeLists.txt` edits (engine, editor, tests).

**Out of scope / unchanged:** no new ECS component, no `GameState`/export change → **no
`GAME_API_VERSION` bump, no ecs.dll rebuild, no forced editor restart** beyond the normal
Engine/editor rebuild. No hierarchical / BVH / spatial-partition culling. No frozen-frustum
debug mode. `runtime` gets no panel (no ImGui) but does accumulate the stats.

## Design

### `src/engine/src/rendering/Frustum.h` (new, header-only, pure GLM)

All functions `inline`, no Engine link required → unit-testable in isolation.

```cpp
#pragma once
#include <glm/glm.hpp>

struct Frustum { glm::vec4 Planes[6]; }; // each (n.x, n.y, n.z, d), inward unit normal

// Gribb-Hartmann from a GLM (column-major) view-proj, clip = VP * v, depth-[0,1] (ZO).
// rowI(m) = vec4(m[0][I], m[1][I], m[2][I], m[3][I]) (0-based). Six planes, then normalized:
//   left   = row3 + row0      right = row3 - row0
//   bottom = row3 + row1      top   = row3 - row1
//   near   = row2             far   = row3 - row2
// The ZO-specific bit is near = row2 (NOT row3 + row2, which is the OpenGL [-1,1] form).
inline Frustum ExtractFrustum(const glm::mat4& viewProj);

// Local AABB -> world AABB via center+extents: world center = M * center;
// world extent_k = sum_j |M3[j][k]| * localExtent_j (abs of upper-left 3x3). No 8-corner loop.
inline void TransformAABB(const glm::mat4& m, glm::vec3 localMin, glm::vec3 localMax,
                          glm::vec3& outMin, glm::vec3& outMax);

// p-vertex test against all 6 planes: AABB is culled iff its positive vertex is behind
// any plane. Returns true if (possibly) visible, false if definitely outside.
inline bool IsAABBVisible(const Frustum& f, glm::vec3 worldMin, glm::vec3 worldMax);
```

The **ZO plane signs are the #1 correctness risk** (wrong signs → cull everything or
nothing). `near = row2` (not `row3 + row2` as in the OpenGL [-1,1] form) is the ZO-specific
detail. Locked down by `test_frustum`.

### `src/engine/src/rendering/RenderStats.{h,cpp}` (new, Engine-exported singleton)

```cpp
// RenderStats.h
#pragma once
#include <cstdint>
#include "Engine.h"

struct RenderStats {
    uint32_t MeshEntitiesTotal  = 0; // entities with Visible==true considered this frame
    uint32_t MeshEntitiesDrawn  = 0; // entries actually submitted (Total - Culled)
    uint32_t MeshEntitiesCulled = 0; // rejected by the frustum test
    uint32_t InstancesDrawn     = 0; // sum of per-batch instance counts emitted
    uint32_t BatchesDrawn       = 0; // draw batches (runs) issued
};
struct CullingSettings { bool Enabled = true; };

// Single instances DEFINED in RenderStats.cpp (Engine.dll), exported so the mesh pass
// (Engine.dll) and the editor panel (editor.exe) share ONE copy each. Header-inline would
// give every module its own static (the staging-pool bug).
ENGINE_API RenderStats&     GetRenderStats();
ENGINE_API CullingSettings& GetCullingSettings();
```
```cpp
// RenderStats.cpp
#include "RenderStats.h"
RenderStats&     GetRenderStats()     { static RenderStats s;     return s; }
CullingSettings& GetCullingSettings() { static CullingSettings s; return s; }
```

**Threading:** both globals are touched only on the RenderThread. The mesh pass *writes*
`RenderStats` and *reads* `CullingSettings.Enabled`; the ImGui overlay (same thread, later in
the same frame) *reads* `RenderStats` and *writes* `CullingSettings.Enabled` (next frame's
pass sees it). Sequential, single-thread → **plain globals, no atomics, no mutex**. (Matches
the staging-pool reasoning: shared instance via Engine.dll export, access discipline by
thread, not by lock.)

Add `RenderStats.cpp` to `src/engine/CMakeLists.txt`. `Frustum.h` is header-only (no source
entry needed; may be listed for IDE visibility).

### `MeshRenderPass.cpp` edits

1. **Extract** a file-local helper near the top of the TU:
   ```cpp
   static glm::mat4 BuildWorldMatrix(const TransformComponent& t) {
       glm::mat4 T  = glm::translate(glm::mat4(1.0f), t.Position);
       glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), t.Rotation.x, {1,0,0});
       glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), t.Rotation.y, {0,1,0});
       glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), t.Rotation.z, {0,0,1});
       glm::mat4 S  = glm::scale(glm::mat4(1.0f), t.Scale);
       return T * Rz * Ry * Rx * S;
   }
   ```
   Replace the inline block at `:453-459` with `glm::mat4 M = BuildWorldMatrix(*transform);`
   (behavior identical).
2. **Once per frame**, after `perFrame.VP` is computed (`:362`):
   ```cpp
   const Frustum fr = ExtractFrustum(perFrame.VP);
   const bool cullEnabled = GetCullingSettings().Enabled;
   auto* meshSystem = m_Renderer->GetMeshSystem();
   uint32_t culledCount = 0;
   ```
3. **Cull gate** in the collection lambda (`:381-391`), naming the transform param:
   ```cpp
   [&](EntityId e, const TransformComponent& transform, const MeshComponent& meshComp) {
       if (!meshComp.Visible) return;
       if (cullEnabled) {
           const auto bounds = meshSystem->GetMeshBounds(meshComp.MeshId);
           if (bounds.valid) {                       // invalid/unloaded bounds -> never cull
               glm::vec3 wMin, wMax;
               TransformAABB(BuildWorldMatrix(transform), bounds.min, bounds.max, wMin, wMax);
               if (!IsAABBVisible(fr, wMin, wMax)) { ++culledCount; return; }
           }
       }
       const auto* materialComp = world->GetComponent<MaterialComponent>(e);
       uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;
       entries[entryCount++] = BatchEntry{ meshComp.MeshId, materialId, e };
   });
   ```
4. **Stats**: accumulate `instancesDrawn += instanceOut` and a batch counter inside the
   render-run loop, then write the global **once** near the end of `Render()`:
   ```cpp
   RenderStats& rs = GetRenderStats();
   rs.MeshEntitiesDrawn  = entryCount;
   rs.MeshEntitiesCulled = culledCount;
   rs.MeshEntitiesTotal  = entryCount + culledCount;
   rs.InstancesDrawn     = instancesDrawn;
   rs.BatchesDrawn       = batchesDrawn;
   ```
   Writing once at the end avoids the panel reading a half-built frame. `Total = Drawn +
   Culled`; `Visible==false` entities are not candidates and are not counted.

`BatchEntry` is unchanged — bounds are fetched at collection time, not stored. (If profiling
later shows the per-entity `GetMeshBounds` lookup is hot, caching the world AABB in
`BatchEntry` is a future optimization; YAGNI now.)

### `src/editor/src/rendering/imgui/RenderStatsPanel.{h,cpp}` (new) + wiring

Mirror `MemoryPanel`:
```cpp
// RenderStatsPanel.h
#pragma once
void DrawRenderStatsPanel(bool* open); // open may be null (always draw) or a toggle bool
```
```cpp
// RenderStatsPanel.cpp
#include "RenderStatsPanel.h"
#include <imgui.h>
#include "RenderStats.h"      // resolved via the editor's Engine PUBLIC includes
void DrawRenderStatsPanel(bool* open) {
    if (open && !*open) return;
    if (!ImGui::Begin("Render Stats", open)) { ImGui::End(); return; }
    ImGui::Checkbox("Frustum culling", &GetCullingSettings().Enabled);
    const RenderStats& s = GetRenderStats();
    ImGui::Text("Mesh entities: %u", s.MeshEntitiesTotal);
    ImGui::Text("  drawn:  %u", s.MeshEntitiesDrawn);
    ImGui::Text("  culled: %u", s.MeshEntitiesCulled);
    ImGui::Text("Instances: %u", s.InstancesDrawn);
    ImGui::Text("Batches:   %u", s.BatchesDrawn);
    ImGui::End();
}
```
Wire into `ImGuiRenderer.cpp` next to the Memory panel (`:481-482`):
```cpp
static bool s_ShowRenderStatsPanel = true;
DrawRenderStatsPanel(&s_ShowRenderStatsPanel);
```
Add `#include "RenderStatsPanel.h"` near the `MemoryPanel.h` include (`:22`), and add
`RenderStatsPanel.cpp` to `src/editor/CMakeLists.txt`.

### `tests/test_frustum.cpp` (new) + `test_frustum` target

Pure GLM, header-only `Frustum.h` → links only `glm` + `CommonHeaders`-style include of the
engine rendering dir (no Engine.dll link). Build a known
`glm::perspectiveRH_ZO(radians(60), 1.0, 0.1, 100.0)` and a simple view (camera at origin
looking down -Z, RH). Cases:
- A point/small AABB directly ahead (e.g. center `(0,0,-10)`) → **visible**.
- AABBs fully past each of the 6 planes (left/right/top/bottom of view, behind near at
  `+Z`, beyond far) → **culled**.
- AABB straddling a plane (partly inside) → **visible** (conservative).
- AABB fully inside → **visible**.
- `TransformAABB` with a translate+rotate+scale matrix → world min/max enclose the rotated
  box; a box translated off-screen by the matrix → **culled**; same box translated into view
  → **visible** (exercises the matrix path, not just identity).

Mirror the existing `tests/CMakeLists.txt` target style (as `test_ecs`/`test_alloc`) and the
self-checking `All ... tests passed.` convention. Add the include dir for
`src/engine/src/rendering`.

## Data flow (per frame, RenderThread)

1. Pass reads `WorldCameraComponent` → `V`, `P`; computes `perFrame.VP = P*V` (existing).
2. `Frustum fr = ExtractFrustum(perFrame.VP)`; `cullEnabled = GetCullingSettings().Enabled`.
3. Collection loop: for each `Visible` mesh entity, if `cullEnabled` and bounds valid,
   `TransformAABB(BuildWorldMatrix(transform), …)` then `IsAABBVisible(fr,…)`; reject →
   `++culled`, else push `BatchEntry`.
4. Batch + instance loops unchanged (now build M via `BuildWorldMatrix`); accumulate
   instance/batch counts.
5. Write `GetRenderStats()` once at end.
6. Later same frame, ImGui overlay (editor only) reads `GetRenderStats()` and the toggle
   checkbox writes `GetCullingSettings().Enabled` for the next frame.

Invariant: the frustum culled against == the frustum rendered with → a mesh on screen is
never culled.

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump; `ecs`/`game`
unchanged.
- `test_frustum` → `All frustum tests passed.` (the core correctness gate).
- `test_ecs` → `All ECS tests passed.`; `test_alloc` → `All allocator tests passed.`
  (regression — neither is touched, but both must stay green).
- `editor` + `runtime` build clean; `dumpbin /exports Engine.dll | findstr RenderStats`
  shows the two exported getters (single-instance check).
- **GUI smoke (user):** in the editor, open the `Render Stats` panel; pan the camera so
  models leave the view → `culled` rises, `drawn` falls, and **on-screen meshes are
  unchanged** (nothing visible pops out). Toggle `Frustum culling` off → `culled` drops to
  0, `drawn` jumps to total, scene identical. `runtime.exe` renders the scene identically
  to before (culling silently active, no panel).

## Risks

- **ZO plane signs wrong** → over/under-cull. Mitigated by `test_frustum` (explicit
  per-plane cases) and the cull==render-frustum invariant (worst case is *extra* culling of
  off-screen geometry, never on-screen).
- **Cross-DLL stats divergence** (panel reads 0) → exactly the staging-pool bug; mitigated
  by defining the singletons in `RenderStats.cpp` and exporting `ENGINE_API`, verified by
  `dumpbin`.
- **Cull/render transform divergence** → mitigated by the shared `BuildWorldMatrix` helper
  used by both the cull test and the instance loop.
- **Per-entity `GetMeshBounds` each frame** → cheap (vector index + copy); flagged as a
  future cache only if profiled hot. No change now.
- **Unloaded/invalid bounds** → treated as visible (never culled), so a not-yet-uploaded
  mesh can't wrongly disappear.
