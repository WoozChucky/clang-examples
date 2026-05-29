# Navmesh-Constrained Movement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in entities (the player) have their per-tick WASD/intent movement clamped to the walkable navmesh via Detour `moveAlongSurface` (wall-slide), layered before the existing AABB collider resolve.

**Architecture:** A new engine-side `NavMesh::ConstrainMove` wraps `dtNavMeshQuery::moveAlongSurface` (+ off-mesh recovery). It is exposed to `Game.dll` through an appended `NavServices.MoveAlongSurface` function pointer. A fieldless `NavConstrainedComponent` marker gates the behavior; `KinematicMovementSystem` (the sole mover) runs the navmesh clamp before `ResolveKinematicMove` when the marker is present and a mesh exists.

**Tech Stack:** C++23, Recast/Detour (`third_party/recastnavigation`), glm, the project ECS (X-macro registration), nlohmann::json (serialization), ImGui (inspector).

**Build/test preset (per project memory):** `msvc-win64-vs2026-community` (enterprise is NOT installed). Test binaries land in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

**Reload note:** Task 1 adds a registered ECS component → struct-set change. Per `CLAUDE.md` this requires rebuilding `ecs.dll`, `editor`, and `game`, then **restarting the editor** for manual verification. `GAME_API_VERSION` does NOT change (the marker lives in `ECS.h`, not `Game.h`).

**Commit identity (per project memory):** commit as `Nuno Silva <nuno.levezinho@live.com.pt>` — e.g. `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/common/include/ECS.h` | `NavConstrainedComponent` struct + X-macro entry | 1 |
| `src/common/include/ECSCommands.h` | Add/Remove command dispatch for the marker | 1 |
| `src/common/include/ComponentSerialization.h` | `to_json`/`from_json` for the marker | 1 |
| `src/engine/src/utilities/WorldManager.cpp` | Save/load the marker per entity | 1 |
| `src/engine/src/navigation/NavMesh.{h,cpp}` | `ConstrainMove` (moveAlongSurface + recovery) | 2 |
| `tests/test_navmesh.cpp` | `ConstrainMove` + `NavServices.MoveAlongSurface` cases | 2, 3 |
| `src/common/include/NavServices.h` | Appended `MoveAlongSurface` field | 3 |
| `src/engine/src/navigation/NavServicesImpl.cpp` | Wire `MoveAlongSurface` forwarder | 3 |
| `src/game/src/game.cpp` | `KinematicMovementSystem` navmesh-clamp step | 4 |
| `src/editor/src/panels/inspector/NavConstrainedEditor.{h,cpp}` | Inspector add/remove/draw (marker) | 5 |
| `src/editor/src/panels/EcsInspectorPanel.cpp` | Register the editor | 5 |
| `src/editor/CMakeLists.txt` | Add the editor `.cpp` | 5 |

---

## Task 1: `NavConstrainedComponent` marker (ECS plumbing)

Foundational — everything else references the type. No behavior yet; verified by build.

**Files:**
- Modify: `src/common/include/ECS.h` (struct after `NavTargetComponent` ~line 343; X-macro entry ~line 381)
- Modify: `src/common/include/ECSCommands.h` (Apply dispatch ~line 340; Remove dispatch ~line 386)
- Modify: `src/common/include/ComponentSerialization.h` (after `SunMarker` json, ~line 116)
- Modify: `src/engine/src/utilities/WorldManager.cpp` (save ~line 78; load after the `NavTargetComponent` contains-check)

- [ ] **Step 1: Declare the marker struct in `ECS.h`**

Insert immediately after the `NavTargetComponent` struct (after its closing `};`, ~line 343):

```cpp
// Opt-in marker: KinematicMovementSystem clamps this entity's per-tick move to
// the walkable navmesh (wall-slide via NavServices::MoveAlongSurface) before the
// AABB collider resolve. No fields. Added to directly-controlled movers (the
// player). Without it, movement is not navmesh-constrained (today's behavior).
struct NavConstrainedComponent {};
```

- [ ] **Step 2: Register it in the X-macro**

In `ECS_FOR_EACH_REGISTERED_COMPONENT`, add a line after `X(NavTargetComponent)` (which is currently the last entry, ~line 381). Remember the trailing backslash moves to the new last line:

```cpp
    X(NavTargetComponent) \
    X(NavConstrainedComponent)
```

- [ ] **Step 3: Add command dispatch in `ECSCommands.h`**

In `ApplyComponentCommand`, add after the `NavTargetComponent` branch (~line 340-342):

```cpp
        } else if (componentData.Type == std::type_index(typeid(NavConstrainedComponent))) {
            world.AddComponent(entity, NavConstrainedComponent{});
```

In `RemoveComponentByType`, add after the `NavTargetComponent` branch (~line 386):

```cpp
        } else if (typeIndex == std::type_index(typeid(NavConstrainedComponent))) {
            world.RemoveComponent<NavConstrainedComponent>(entity);
```

- [ ] **Step 4: Add json (de)serializers in `ComponentSerialization.h`**

Mirror the `SunMarker` pattern (it's a fieldless marker). Add after the `SunMarker` lines (~line 116):

```cpp
inline void to_json(nlohmann::json& j, const NavConstrainedComponent&) { j = nlohmann::json::object(); }
inline void from_json(const nlohmann::json&, NavConstrainedComponent&) {}
```

- [ ] **Step 5: Save the marker in `WorldManager::SaveWorldSnapshot`**

In `WorldManager.cpp`, add after the `NavTargetComponent` save block (~line 78, just before `j["Entities"].push_back(jEntity);`):

```cpp
        if (world->HasComponent<NavConstrainedComponent>(entity)) {
            jEntity["NavConstrainedComponent"] = *(world->GetComponent<NavConstrainedComponent>(entity));
        }
```

- [ ] **Step 6: Load the marker in `WorldManager::LoadWorldSnapshot`**

In the same file, in the entity-load loop, add after the `NavTargetComponent` `contains` check (mirror the `SunMarker` load at ~line 133-134 — presence-only):

```cpp
            if (jEntity.contains("NavConstrainedComponent"))
                world->AddComponent(createdEntity, NavConstrainedComponent{});
```

- [ ] **Step 7: Build ecs + engine to verify it compiles**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: builds clean (this target links `ecs` + `engine`, so it forces the new explicit `ComponentArray<NavConstrainedComponent>` template instantiation through the X-macro). No link errors about missing instantiations.

- [ ] **Step 8: Commit**

```bash
git add src/common/include/ECS.h src/common/include/ECSCommands.h src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavConstrainedComponent marker + commands/serialization"
```

---

## Task 2: `NavMesh::ConstrainMove` (engine, wall-slide + recovery)

TDD with real assertions over a built navmesh (matches the existing `test_navmesh.cpp` integration style — `moveAlongSurface` is Detour-internal, not pure-mathable).

**Files:**
- Modify: `src/engine/src/navigation/NavMesh.h` (declare, after `ClosestPoint` ~line 68)
- Modify: `src/engine/src/navigation/NavMesh.cpp` (implement, after `ClosestPoint` ~line 380)
- Test: `tests/test_navmesh.cpp` (new T28/T29/T30 + register in `main`)

- [ ] **Step 1: Write the failing tests in `tests/test_navmesh.cpp`**

Add these three functions after `T27_geometry_mesh_cache_miss_skips_entity` (~line 630), before `int main()`. They reuse the existing `SpawnNavBox` + `DefaultCfg` helpers. The floor is a 10×10 box (half-extents 5,0.1,5) with its top at y=0; after Recast erosion the walkable square is roughly `|x|,|z| <= ~4.5`.

```cpp
// ---------- Navmesh-constrained movement: T28-T30 ----------

static void T28_constrain_open_move_reaches_target() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    // Move 1m across open floor — result should land ~at the target, on the mesh.
    const glm::vec3 result = nm->ConstrainMove(glm::vec3(0, 0.1f, 0), glm::vec3(1.0f, 0.1f, 0));
    EXPECT(std::fabs(result.x - 1.0f) < 0.3f);   // advanced ~1m in +X
    EXPECT(std::fabs(result.z - 0.0f) < 0.3f);   // no lateral drift
}

static void T29_constrain_into_boundary_clamps_on_mesh() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    // Push from a valid on-mesh point far past the +X boundary. moveAlongSurface
    // must clamp/slide along the edge: result stays inside the floor bounds and
    // does NOT reach the requested x=20.
    const glm::vec3 result = nm->ConstrainMove(glm::vec3(3.0f, 0.1f, 0), glm::vec3(20.0f, 0.1f, 0));
    EXPECT(result.x < 6.0f);            // clamped near the eroded edge, not at 20
    EXPECT(std::fabs(result.x) <= 5.5f); // still within the floor extents (on mesh)
    EXPECT(std::fabs(result.z) <= 5.5f);
}

static void T30_constrain_offmesh_recovers_toward_mesh() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    // Start ~5.5m off the +X edge (beyond the 2m near-extent, inside the 16m
    // recovery extent). ConstrainMove should return the nearest poly point —
    // i.e. pulled back TOWARD the mesh (closer to origin than the start).
    const glm::vec3 start(10.0f, 0.1f, 0);
    const glm::vec3 result = nm->ConstrainMove(start, glm::vec3(11.0f, 0.1f, 0));
    EXPECT(glm::length(result) < glm::length(start)); // moved toward the mesh
    EXPECT(result.x < start.x);
}
```

Register them in `main()` after the `T27_...();` call (~line 659):

```cpp
    T28_constrain_open_move_reaches_target();
    T29_constrain_into_boundary_clamps_on_mesh();
    T30_constrain_offmesh_recovers_toward_mesh();
```

- [ ] **Step 2: Run the tests to verify they fail (compile error — method undefined)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `'ConstrainMove': is not a member of 'NavMesh'`.

- [ ] **Step 3: Declare `ConstrainMove` in `NavMesh.h`**

Add after the `ClosestPoint` declaration (~line 68):

```cpp
    // Constrain a desired move to the navmesh surface (wall-slide via Detour
    // moveAlongSurface). If `start` has a poly within near extents, returns the
    // surface-constrained end position (slides along boundaries, never leaves the
    // mesh). If off-mesh, searches wider recovery extents and returns the nearest
    // poly point (pull back onto mesh) with a rate-limited SM_WARN; if even that
    // misses, returns `start` unchanged (no movement). No query / no mesh →
    // returns `desiredEnd` (unconstrained). GameThread only.
    glm::vec3 ConstrainMove(const glm::vec3& start, const glm::vec3& desiredEnd) const;
```

- [ ] **Step 4: Implement `ConstrainMove` in `NavMesh.cpp`**

Add after the `ClosestPoint` definition (~line 380). Note: the spec named `ClosestPoint` for recovery, but `ClosestPoint`'s extents (2,4,2) equal the near-extents, so it could never recover a point the near-lookup already missed. We use a dedicated **wider** recovery `findNearestPoly` (16,16,16) instead — same intent, actually distinct behavior.

```cpp
glm::vec3 NavMesh::ConstrainMove(const glm::vec3& start, const glm::vec3& desiredEnd) const
{
    SM_ASSERT(std::this_thread::get_id() == NavQueryOwnerThread(),
              "NavMesh::ConstrainMove called from non-owner thread; dtNavMeshQuery is not thread-safe");
    if (!m_Query || !m_NavMesh) return desiredEnd;  // no query → unconstrained

    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    const float s[3] = { start.x, start.y, start.z };
    const float e[3] = { desiredEnd.x, desiredEnd.y, desiredEnd.z };

    // Normal path: small near-extents start-poly lookup, then slide along surface.
    const float nearExt[3] = { 2.0f, 4.0f, 2.0f };
    dtPolyRef startRef = 0;
    float startNearest[3];
    m_Query->findNearestPoly(s, nearExt, &filter, &startRef, startNearest);

    if (startRef) {
        float resultPos[3];
        dtPolyRef visited[16];
        int nvisited = 0;
        if (dtStatusFailed(m_Query->moveAlongSurface(startRef, startNearest, e, &filter,
                                                     resultPos, visited, &nvisited, 16))) {
            return start;  // query failed → no movement this tick
        }
        return glm::vec3(resultPos[0], resultPos[1], resultPos[2]);
    }

    // Recovery: wider search to pull a displaced entity back toward the mesh.
    const float recoverExt[3] = { 16.0f, 16.0f, 16.0f };
    dtPolyRef recoverRef = 0;
    float recoverNearest[3];
    m_Query->findNearestPoly(s, recoverExt, &filter, &recoverRef, recoverNearest);
    if (recoverRef) {
        // Rate-limit: this fires per tick while off-mesh; log ~once/2s at 60Hz.
        static int s_offMeshLog = 0;
        if ((s_offMeshLog++ % 120) == 0) {
            SM_WARN("NavMesh::ConstrainMove: start off-mesh, recovering toward nearest poly");
        }
        return glm::vec3(recoverNearest[0], recoverNearest[1], recoverNearest[2]);
    }
    return start;  // fully off-mesh (beyond recovery extents) → freeze this tick
}
```

`moveAlongSurface`, `findNearestPoly`, `dtQueryFilter`, `dtPolyRef`, `dtStatusFailed` are already available via the `<DetourNavMeshQuery.h>` / `<DetourNavMesh.h>` includes at the top of `NavMesh.cpp`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): NavMesh::ConstrainMove (wall-slide + off-mesh recovery)"
```

---

## Task 3: `NavServices.MoveAlongSurface` (Game.dll bridge)

Append-only field + forwarder, with tests via the existing `TestNavServices()` harness.

**Files:**
- Modify: `src/common/include/NavServices.h` (append field at end of struct, ~line 49)
- Modify: `src/engine/src/navigation/NavServicesImpl.cpp` (forwarder + wire in `Init`)
- Test: `tests/test_navmesh.cpp` (new T31/T32 + register in `main`)

- [ ] **Step 1: Write the failing tests in `tests/test_navmesh.cpp`**

Add after `T30_...` (before `int main()`):

```cpp
static void T31_navservices_movealongsurface_forwards() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());

    const NavServices* svc = TestNavServices();
    EXPECT(svc->MoveAlongSurface != nullptr);
    // Open move advances toward the target.
    const glm::vec3 open = svc->MoveAlongSurface(glm::vec3(0, 0.1f, 0), glm::vec3(1.0f, 0.1f, 0));
    EXPECT(std::fabs(open.x - 1.0f) < 0.3f);
    // Into-boundary clamps on mesh (does not reach x=20).
    const glm::vec3 clamped = svc->MoveAlongSurface(glm::vec3(3.0f, 0.1f, 0), glm::vec3(20.0f, 0.1f, 0));
    EXPECT(clamped.x < 6.0f);
}

static void T32_navservices_movealongsurface_no_mesh_returns_desired() {
    ECS empty;
    NavMeshSystem::Instance().Rebuild(empty, DefaultCfg());  // empty soup → null current
    const NavServices* svc = TestNavServices();
    EXPECT(!svc->HasMesh());
    const glm::vec3 desiredEnd(7.0f, 1.0f, -2.0f);
    const glm::vec3 out = svc->MoveAlongSurface(glm::vec3(0, 0, 0), desiredEnd);
    EXPECT(std::fabs(out.x - desiredEnd.x) < 1e-4f);  // unchanged: no mesh = no constraint
    EXPECT(std::fabs(out.y - desiredEnd.y) < 1e-4f);
    EXPECT(std::fabs(out.z - desiredEnd.z) < 1e-4f);
}
```

Register in `main()` after the `T30_...();` line:

```cpp
    T31_navservices_movealongsurface_forwards();
    T32_navservices_movealongsurface_no_mesh_returns_desired();
```

- [ ] **Step 2: Run to verify failure (field undefined)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `'MoveAlongSurface': is not a member of 'NavServices'`.

- [ ] **Step 3: Append the field to `NavServices.h`**

Add at the **end** of the `struct NavServices` (after `UntrackEntity`, ~line 49 — append-only to preserve `Game.dll` binary compat):

```cpp
    // ---- Surface constraint (navmesh-constrained movement) ----

    // Constrain a move to the navmesh surface (wall-slide via dtNavMeshQuery::
    // moveAlongSurface). Off-mesh start → pulls toward nearest poly (recovery).
    // No mesh → returns desiredEnd unchanged. GameThread only.
    glm::vec3 (*MoveAlongSurface)(const glm::vec3& start, const glm::vec3& desiredEnd);
```

- [ ] **Step 4: Add the forwarder + wire it in `NavServicesImpl.cpp`**

Add a forwarder in the anonymous namespace, after `ForwardUntrackEntity` (~line 49):

```cpp
glm::vec3 ForwardMoveAlongSurface(const glm::vec3& start, const glm::vec3& desiredEnd) {
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) return desiredEnd;   // no mesh → unconstrained
    return nm->ConstrainMove(start, desiredEnd);
}
```

Wire it in `NavServicesImpl::Init`, after the `out.UntrackEntity = ...` line (~line 64):

```cpp
    out.MoveAlongSurface       = &ForwardMoveAlongSurface;
```

- [ ] **Step 5: Run to verify pass**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/common/include/NavServices.h src/engine/src/navigation/NavServicesImpl.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): expose MoveAlongSurface via NavServices bridge"
```

---

## Task 4: `KinematicMovementSystem` navmesh-clamp step (game)

Layered constraint: navmesh clamp first, then the existing AABB resolve. Verified by build + manual (the system is defined inline in `game.cpp`, not header-exposed, so it has no unit-test seam — matching the other game systems).

**Files:**
- Modify: `src/game/src/game.cpp` (`KinematicMovementSystem::Update`, lines ~262-288)

- [ ] **Step 1: Insert the navmesh-clamp step**

Replace the body of the `Each` lambda in `KinematicMovementSystem::Update` (current lines ~263-287) with the version below. The only addition is the `NavConstrainedComponent` block between reading `desired` and the AABB resolve; everything else is unchanged.

```cpp
        ctx.world.Each<TransformComponent, MoveIntentComponent>([&](EntityId e) {
            const auto* intent = ctx.world.GetComponent<MoveIntentComponent>(e);
            if (!intent) return;
            const glm::vec3 desired = intent->DesiredDelta;
            if (desired.x == 0.0f && desired.y == 0.0f && desired.z == 0.0f) return;

            const auto* transform = ctx.world.GetComponent<TransformComponent>(e);
            if (!transform) return;

            // Navmesh constraint (opt-in via NavConstrainedComponent): clamp the
            // desired move to the walkable surface (wall-slide) BEFORE the AABB
            // resolve. Skipped when no marker / no nav table / no mesh built.
            glm::vec3 navDesired = desired;
            if (ctx.world.HasComponent<NavConstrainedComponent>(e)
                && ctx.Nav && ctx.Nav->HasMesh()) {
                const glm::vec3 end     = transform->Position + desired;
                const glm::vec3 clamped = ctx.Nav->MoveAlongSurface(transform->Position, end);
                navDesired = clamped - transform->Position;
            }

            glm::vec3 applied = navDesired;
            if (const auto* collider = ctx.world.GetComponent<ColliderComponent>(e)) {
                applied = ResolveKinematicMove(ctx.world, e, *transform, *collider, navDesired).AppliedDelta;
            }

            if (applied.x != 0.0f || applied.y != 0.0f || applied.z != 0.0f) {
                ctx.world.Modify<TransformComponent>(e, [&](TransformComponent& t){
                    t.Position += applied;
                });
            }

            // Clear-after-consume: zero the intent so a stale value doesn't re-fire next tick
            // if the producer didn't run (e.g. gating turned a movement system off).
            ctx.world.Modify<MoveIntentComponent>(e, [](MoveIntentComponent& m){
                m.DesiredDelta = glm::vec3(0.0f);
            });
        });
```

- [ ] **Step 2: Build the game library**

Run: `cmake --build --preset msvc-win64-vs2026-community --target game`
Expected: builds clean. (`NavConstrainedComponent`, `ctx.Nav`, and `MoveAlongSurface` are all visible to `Game.dll` via `ECS.h` / `Systems.h` / `NavServices.h`.)

- [ ] **Step 3: Commit**

```bash
git add src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): navmesh-constrain player move before AABB resolve"
```

---

## Task 5: Inspector editor for the marker (editor)

Lets the user add/remove `NavConstrainedComponent` on an entity (e.g. the player) in the editor; persists through the WorldManager round-trip from Task 1. Mirrors `SunMarkerEditor` (fieldless marker). No automated test (ImGui UI) — verified by build + manual.

**Files:**
- Create: `src/editor/src/panels/inspector/NavConstrainedEditor.h`
- Create: `src/editor/src/panels/inspector/NavConstrainedEditor.cpp`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp` (include + register, ~lines 27 / 45)
- Modify: `src/editor/CMakeLists.txt` (add the new `.cpp`)

- [ ] **Step 1: Create `NavConstrainedEditor.h`**

```cpp
#pragma once
#include "IComponentEditor.h"
class NavConstrainedEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "NavMesh Constrained"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavConstrainedComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

- [ ] **Step 2: Create `NavConstrainedEditor.cpp`**

```cpp
#include "NavConstrainedEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

void NavConstrainedEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavConstrainedComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd)) {
        SM_WARN("ECS command queue full! Add component command dropped.");
    }
}
void NavConstrainedEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavConstrainedComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd)) {
        SM_WARN("ECS command queue full! Remove component command dropped.");
    }
}
void NavConstrainedEditor::DrawEditor(const EditorContext&, EntityId) {
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Movement constrained to navmesh");
    ImGui::TextDisabled("Wall-slides along walkable edges; off-mesh recovers to nearest poly.");
}
```

- [ ] **Step 3: Register the editor in `EcsInspectorPanel.cpp`**

Add the include next to the other nav editor includes (~line 27):

```cpp
#include "inspector/NavConstrainedEditor.h"
```

Add the registration after the `NavTargetEditor` push (~line 45):

```cpp
    m_Editors.push_back(std::make_unique<NavConstrainedEditor>());
```

- [ ] **Step 4: Add the source file to `src/editor/CMakeLists.txt`**

Find the line listing `src/panels/inspector/NavTargetEditor.cpp` (the inspector editor sources are listed explicitly — no globbing) and add alongside it:

```cmake
    src/panels/inspector/NavConstrainedEditor.cpp
```

- [ ] **Step 5: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds + links clean; `NavConstrainedEditor.cpp` appears in the build output.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/panels/inspector/NavConstrainedEditor.h src/editor/src/panels/inspector/NavConstrainedEditor.cpp src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): NavConstrained inspector editor"
```

---

## Task 6: End-to-end manual verification

No code. Confirms the feature works in the running editor and persists. (The player entity is data-driven in the build-output `world.json`, not checked in — so the marker is attached at runtime via the inspector, then saved.)

- [ ] **Step 1: Full rebuild + restart**

Run: `cmake --build --preset msvc-win64-vs2026-community` (builds `ecs`, `engine`, `game`, `editor`). Then **fully restart `editor.exe`** (Task 1 changed the ECS component set — a running editor still has the old layout linked in).

- [ ] **Step 2: Bake a navmesh** for the current scene (Navigation panel → build/bake), so a walkable mesh exists.

- [ ] **Step 3: Select the player entity** in the ECS inspector, add the **NavMesh Constrained** component (the new editor section), and **Save World**.

- [ ] **Step 4: Enter Play mode and verify** (per project memory, game input only routes in Play mode with the viewport hovered and no gizmo active):
  - Walk the player into a baked wall / off the walkable edge → it **slides along the boundary** and never leaves the mesh (vs. the old free-movement-through-navmesh-edges).
  - Remove the marker (inspector) → free movement returns (AABB-only, today's behavior).
  - With no navmesh baked → behaves as today (constraint skipped, no errors).

- [ ] **Step 5: Verify persistence + runtime** — reload the world (or restart): the player still has the marker (round-trips through `world.json`). Optionally launch `runtime.exe` on the same world — the constraint is engine-side, so the player is navmesh-bound there too.

- [ ] **Step 6 (if any code fix was needed during verification): commit**

```bash
git add -A
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "fix(nav): <describe the manual-verification fix>"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** boundary=wall-slide (Task 2 `moveAlongSurface`); scope=opt-in marker (Task 1 + 4 gate); collision combo=layered nav-then-AABB (Task 4 order); off-mesh=recover toward nearest poly (Task 2 recovery branch); NavServices append (Task 3); inspector + persistence (Tasks 1, 5); tests (Tasks 2, 3); build/reload note (header). All spec sections mapped.
- **Deviation noted:** spec said recovery via `ClosestPoint`; implemented as a wider-extent `findNearestPoly` (16m) because `ClosestPoint`'s 2m extents equal the near-lookup and could not actually recover. Same intent, correct behavior — documented inline in Task 2 Step 4.
- **Type consistency:** `ConstrainMove(start, desiredEnd)` and `MoveAlongSurface(start, desiredEnd)` signatures match across Tasks 2/3/4; `NavConstrainedComponent` spelled identically across all tasks.
- **Placeholders:** none — every code step shows complete code; every run step shows the command + expected output.
