# Multi-Class Navmesh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bake N navmeshes at different agent radii ("classes") over the same map so each entity navigates the mesh eroded for its body size — making `moveAlongSurface` keep the body tangent to walls with zero runtime radius math.

**Architecture:** `NavMeshConfigComponent` gains a fixed-cap `Classes[]` array; `NavMeshSystem` holds one published `NavMesh` per class slot and builds them all from one triangle soup; obstacles fan out to every class mesh via logical ids; new `NavServices` `…ForClass` queries are selected per-entity via a shared `NavClassComponent`. `NavMesh` itself is unchanged (still single-class).

**Tech Stack:** C++23, Recast/Detour (`third_party/recastnavigation`), glm, project ECS (X-macro registration), nlohmann::json, ImGui.

**Build/test preset (project memory):** `msvc-win64-vs2026-community` ONLY (enterprise NOT installed). Test binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/<target>.exe`.

**Commit identity (project memory):** author every commit `Nuno Silva <nuno.levezinho@live.com.pt>` via `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`.

**Reload note:** reshaping `NavMeshConfigComponent` + adding `NavClassComponent` changes the ECS struct set → rebuild `ecs.dll`, `editor`, `game` and **restart the editor** for manual verification. `GAME_API_VERSION` unchanged (no `Game.h` change).

**Branch:** continue on `feat/navmesh-constrained-movement` (stacked on the verified base feature). Builds are slow (minutes) — let them finish.

**Key implementation decision (deviation from spec §1, less churn):** `NavMeshConfigComponent` KEEPS its top-level `AgentRadius/AgentHeight/AgentMaxClimb` fields. They serve two roles: (a) the per-build "active class" values that `NavMesh::Build` reads unchanged, and (b) the implicit class-0 / legacy default when `ClassCount==0`. The new `Classes[]` array is the authoring source; `ConfigForClass(cfg,i)` copies `cfg` and overwrites those three fields from `Classes[i]`. This keeps `NavMesh::Build` untouched and makes legacy migration automatic (a world.json with no `Classes` key → `ClassCount==0` → the flat fields already are class 0).

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/common/include/ECS.h` | `kMaxNavClasses`, `NavClassConfig`, `NavMeshConfigComponent.Classes[]`+`ClassCount`, `NavClassComponent` + X-macro | 1, 2 |
| `src/common/include/NavClass.h` (new) | Pure helpers `ConfigForClass`, `NavLiveClassCount`, `ResolveNavClass` | 1, 2 |
| `src/common/include/ComponentSerialization.h` | `NavClassConfig` + `NavMeshConfigComponent.Classes` (de)serialize; `NavClassComponent` | 1, 2 |
| `src/common/include/ECSCommands.h` | `NavClassComponent` Apply/Remove/Copy | 2 |
| `src/engine/src/utilities/WorldManager.cpp` | `NavClassComponent` save/load | 2 |
| `src/engine/src/navigation/NavMeshSystem.{h,cpp}` | N slots, per-class build/publish/`Current(classId)`, obstacle fan-out, disk-bake short-circuit | 3, 4 |
| `src/common/include/NavServices.h` | append `…ForClass` fn pointers | 5 |
| `src/engine/src/navigation/NavServicesImpl.cpp` | `…ForClass` forwarders; old fns → class 0 | 5 |
| `src/game/src/NavAgentSystem.h` | resolve class → `FindPathForClass` | 6 |
| `src/game/src/game.cpp` | `KinematicMovementSystem` → `MoveAlongSurfaceForClass` | 6 |
| `src/editor/src/panels/NavigationPanel.cpp` | class-list editor | 7 |
| `src/editor/src/panels/inspector/NavClassEditor.{h,cpp}` (new) + registration + CMake | per-entity class pick | 7 |
| `tests/test_navmesh.cpp`, `tests/test_worldserial.cpp` | helpers / multi-class / fan-out / forwarder / migration | 1-5 |

---

## Task 1: Config reshape + `ConfigForClass` helper + serialization/migration

**Files:**
- Modify: `src/common/include/ECS.h` (`NavMeshConfigComponent` ~line 284)
- Create: `src/common/include/NavClass.h`
- Modify: `src/common/include/ComponentSerialization.h` (`NavMeshConfigComponent` json ~line 306)
- Test: `tests/test_navmesh.cpp` (ConfigForClass), `tests/test_worldserial.cpp` (json migration)

- [ ] **Step 1: Write the failing ConfigForClass test in `tests/test_navmesh.cpp`**

Add after the last test function (before `int main()`), and include the new header at the top of the file (add `#include "NavClass.h"` near the other includes):

```cpp
// ---------- Multi-class config: T33 ----------
static void T33_config_for_class_selects_agent_params() {
    NavMeshConfigComponent cfg{};
    cfg.AgentRadius = 0.5f;                 // top-level = legacy / class-0 default
    cfg.ClassCount  = 2;
    cfg.Classes[0]  = NavClassConfig{ /*Radius*/ 0.3f, /*Height*/ 1.8f, /*Climb*/ 0.4f };
    cfg.Classes[1]  = NavClassConfig{ /*Radius*/ 1.5f, /*Height*/ 2.4f, /*Climb*/ 0.5f };

    EXPECT(ConfigForClass(cfg, 0).AgentRadius == 0.3f);
    EXPECT(ConfigForClass(cfg, 1).AgentRadius == 1.5f);
    EXPECT(ConfigForClass(cfg, 1).AgentHeight == 2.4f);
    // Out-of-range classId → unchanged cfg (top-level params, the class-0 default).
    EXPECT(ConfigForClass(cfg, 7).AgentRadius == 0.5f);
    // ClassCount==0 → unchanged cfg (legacy single class).
    NavMeshConfigComponent legacy{};
    legacy.AgentRadius = 0.5f;
    EXPECT(ConfigForClass(legacy, 0).AgentRadius == 0.5f);
    EXPECT(NavLiveClassCount(legacy) == 1);
    EXPECT(NavLiveClassCount(cfg) == 2);
}
```
Register in `main()` after the last `T..();`:
```cpp
    T33_config_for_class_selects_agent_params();
```

- [ ] **Step 2: Run to verify it fails (compile error)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `NavClassConfig` / `ConfigForClass` / `NavLiveClassCount` undeclared, and `Classes`/`ClassCount` not members.

- [ ] **Step 3: Reshape `NavMeshConfigComponent` in `ECS.h`**

Replace the existing `struct NavMeshConfigComponent { ... };` (~lines 284-293) with:

```cpp
inline constexpr int kMaxNavClasses = 8;

// One agent-radius class. Fixed-cap + trivially-copyable (no heap) so it rides
// in the ECS snapshot like NavAgentComponent::CachedPath[].
struct NavClassConfig {
    float AgentRadius   = 0.5f;   // agent capsule radius (m) — drives Recast erosion
    float AgentHeight   = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb = 0.4f;   // step-up height (m)
};

struct NavMeshConfigComponent {
    float CellSize       = 0.3f;   // voxel XZ size (m)
    float CellHeight     = 0.2f;   // voxel Y size (m)
    // Top-level agent params double as (a) the per-build "active class" values
    // NavMesh::Build reads and (b) the implicit class-0 default when ClassCount==0.
    float AgentRadius    = 0.5f;   // agent capsule radius (m)
    float AgentHeight    = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb  = 0.4f;   // step-up height (m)
    float AgentMaxSlope  = 45.0f;  // max walkable slope (degrees) — shared across classes
    float TileSize       = 32.0f;  // tile XZ size (voxels per tile edge) — shared
    int   MaxObstacles   = 128;    // dtTileCache pre-alloc — shared

    // Per-radius classes (multi-class bakes). ClassCount==0 → single legacy class
    // built from the top-level agent params above. ClassId is the index here.
    NavClassConfig Classes[kMaxNavClasses]{};
    uint8_t        ClassCount = 0;
};
```

- [ ] **Step 4: Create `src/common/include/NavClass.h` with the pure helpers**

```cpp
#pragma once

#include <cstdint>

#include "ECS.h"   // NavMeshConfigComponent, NavClassConfig, NavClassComponent, ECS, EntityId

// Number of class meshes to build: at least 1 (ClassCount==0 → legacy single class).
inline uint8_t NavLiveClassCount(const NavMeshConfigComponent& cfg) {
    return cfg.ClassCount > 0 ? cfg.ClassCount : uint8_t{1};
}

// Single-class config for NavMesh::Build: a copy of `cfg` with its agent params
// overwritten from class `classId`. classId >= ClassCount → returns `cfg`
// unchanged (its top-level agent params act as the class-0 / legacy default).
inline NavMeshConfigComponent ConfigForClass(const NavMeshConfigComponent& cfg, uint8_t classId) {
    NavMeshConfigComponent out = cfg;
    if (classId < cfg.ClassCount) {
        out.AgentRadius   = cfg.Classes[classId].AgentRadius;
        out.AgentHeight   = cfg.Classes[classId].AgentHeight;
        out.AgentMaxClimb = cfg.Classes[classId].AgentMaxClimb;
    }
    return out;
}

// Resolve an entity's nav class: its NavClassComponent.ClassId, clamped to
// [0, classCount). Missing component or out-of-range → 0. (NavClassComponent is
// added in Task 2; this header compiles once that struct exists.)
inline uint8_t ResolveNavClass(const ECS& world, EntityId e, uint8_t classCount) {
    const auto* nc = world.GetComponent<NavClassComponent>(e);
    if (!nc || classCount == 0) return 0;
    return (nc->ClassId < classCount) ? nc->ClassId : uint8_t{0};
}
```

NOTE: `ResolveNavClass` references `NavClassComponent`, which Task 2 adds. To keep Task 1 compiling on its own, add a minimal forward declaration is NOT enough (it dereferences `nc->ClassId` and calls the templated `GetComponent`). So in Task 1, temporarily omit `ResolveNavClass` from `NavClass.h` (ship only `NavLiveClassCount` + `ConfigForClass`), and add `ResolveNavClass` in Task 2 after `NavClassComponent` exists. (The Task 1 test only uses the first two.)

- [ ] **Step 5: Run to verify the ConfigForClass test passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.`

- [ ] **Step 6: Write the failing serialization/migration test in `tests/test_worldserial.cpp`**

Add after the existing `T02_daynight_roundtrip` (or any existing test), and register it in `main()` (mirror how other tests are registered — find the `T02_...();` call list and add the new calls):

```cpp
static void T_navcfg_multiclass_roundtrip() {
    NavMeshConfigComponent in;
    in.AgentRadius = 0.5f;
    in.ClassCount  = 2;
    in.Classes[0]  = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    in.Classes[1]  = NavClassConfig{ 1.5f, 2.4f, 0.5f };

    const nlohmann::json j = in;
    const auto out = j.get<NavMeshConfigComponent>();

    EXPECT(out.ClassCount == 2);
    EXPECT(near(out.Classes[0].AgentRadius, 0.3f));
    EXPECT(near(out.Classes[1].AgentRadius, 1.5f));
    EXPECT(near(out.Classes[1].AgentHeight, 2.4f));
}

static void T_navcfg_legacy_migration() {
    // A legacy world.json nav config: flat agent params, NO "Classes" key.
    nlohmann::json j = {
        {"CellSize", 0.3f}, {"CellHeight", 0.2f},
        {"AgentRadius", 0.7f}, {"AgentHeight", 1.9f}, {"AgentMaxClimb", 0.45f},
        {"AgentMaxSlope", 45.0f}, {"TileSize", 32.0f}, {"MaxObstacles", 128}
    };
    const auto out = j.get<NavMeshConfigComponent>();
    EXPECT(out.ClassCount == 0);                 // no classes → legacy single class
    EXPECT(near(out.AgentRadius, 0.7f));         // flat field preserved (= class 0)
}
```
Register both in `main()`:
```cpp
    T_navcfg_multiclass_roundtrip();
    T_navcfg_legacy_migration();
```

- [ ] **Step 7: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_worldserial`
Expected: FAIL to compile (NavClassConfig array not serialized — `Classes`/`ClassCount` members exist after Step 3, but the test asserts round-trip the serializer doesn't yet emit).

- [ ] **Step 8: Update `NavMeshConfigComponent` (de)serialization in `ComponentSerialization.h`**

Add a `NavClassConfig` json pair, and extend the `NavMeshConfigComponent` pair to (de)serialize the `Classes` array. Replace the existing `to_json`/`from_json` for `NavMeshConfigComponent` (~lines 306-327) with:

```cpp
inline void to_json(nlohmann::json& j, const NavClassConfig& c) {
    j = nlohmann::json{
        {"AgentRadius",   c.AgentRadius},
        {"AgentHeight",   c.AgentHeight},
        {"AgentMaxClimb", c.AgentMaxClimb}
    };
}
inline void from_json(const nlohmann::json& j, NavClassConfig& c) {
    if (j.contains("AgentRadius"))   j.at("AgentRadius").get_to(c.AgentRadius);
    if (j.contains("AgentHeight"))   j.at("AgentHeight").get_to(c.AgentHeight);
    if (j.contains("AgentMaxClimb")) j.at("AgentMaxClimb").get_to(c.AgentMaxClimb);
}

inline void to_json(nlohmann::json& j, const NavMeshConfigComponent& t) {
    j = nlohmann::json{
        {"CellSize",      t.CellSize},
        {"CellHeight",    t.CellHeight},
        {"AgentRadius",   t.AgentRadius},
        {"AgentHeight",   t.AgentHeight},
        {"AgentMaxClimb", t.AgentMaxClimb},
        {"AgentMaxSlope", t.AgentMaxSlope},
        {"TileSize",      t.TileSize},
        {"MaxObstacles",  t.MaxObstacles}
    };
    // Class list — array length is the source of truth for ClassCount on load.
    nlohmann::json classes = nlohmann::json::array();
    for (uint8_t i = 0; i < t.ClassCount && i < kMaxNavClasses; ++i) classes.push_back(t.Classes[i]);
    j["Classes"] = std::move(classes);
}
inline void from_json(const nlohmann::json& j, NavMeshConfigComponent& t) {
    if (j.contains("CellSize"))      j.at("CellSize").get_to(t.CellSize);
    if (j.contains("CellHeight"))    j.at("CellHeight").get_to(t.CellHeight);
    if (j.contains("AgentRadius"))   j.at("AgentRadius").get_to(t.AgentRadius);
    if (j.contains("AgentHeight"))   j.at("AgentHeight").get_to(t.AgentHeight);
    if (j.contains("AgentMaxClimb")) j.at("AgentMaxClimb").get_to(t.AgentMaxClimb);
    if (j.contains("AgentMaxSlope")) j.at("AgentMaxSlope").get_to(t.AgentMaxSlope);
    if (j.contains("TileSize"))      j.at("TileSize").get_to(t.TileSize);
    if (j.contains("MaxObstacles"))  j.at("MaxObstacles").get_to(t.MaxObstacles);
    // Class list (absent in legacy files → ClassCount stays 0 = single legacy class).
    t.ClassCount = 0;
    if (j.contains("Classes") && j.at("Classes").is_array()) {
        const auto& arr = j.at("Classes");
        const size_t n = std::min<size_t>(arr.size(), kMaxNavClasses);
        for (size_t i = 0; i < n; ++i) t.Classes[i] = arr.at(i).get<NavClassConfig>();
        t.ClassCount = static_cast<uint8_t>(n);
    }
}
```
(Ensure `<algorithm>` is included for `std::min`; `ComponentSerialization.h` already pulls in `<nlohmann/json.hpp>`. If `<algorithm>` isn't present, add it.)

- [ ] **Step 9: Run both test suites to verify pass**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh --target test_worldserial && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe`
Expected: `All navmesh tests passed.` and the worldserial suite's pass line.

- [ ] **Step 10: Commit**

```bash
git add src/common/include/ECS.h src/common/include/NavClass.h src/common/include/ComponentSerialization.h tests/test_navmesh.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): NavClassConfig + multi-class config + ConfigForClass helper"
```

---

## Task 2: `NavClassComponent` (per-entity class) + plumbing + `ResolveNavClass`

**Files:**
- Modify: `src/common/include/ECS.h` (struct near `NavConstrainedComponent` + X-macro)
- Modify: `src/common/include/NavClass.h` (add `ResolveNavClass`)
- Modify: `src/common/include/ECSCommands.h` (Apply ~line 344 region, Remove ~390, Copy ~263)
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp` (save + load)
- Test: `tests/test_navmesh.cpp` (`ResolveNavClass`), `tests/test_worldserial.cpp` (json round-trip)

- [ ] **Step 1: Write the failing `ResolveNavClass` test in `tests/test_navmesh.cpp`**

```cpp
// ---------- Per-entity nav class: T34 ----------
static void T34_resolve_nav_class() {
    ECS w;
    const EntityId none = w.CreateEntity();           // no NavClassComponent
    const EntityId c1   = w.CreateEntity();
    w.AddComponent(c1, NavClassComponent{ /*ClassId*/ 1 });
    const EntityId cBad = w.CreateEntity();
    w.AddComponent(cBad, NavClassComponent{ /*ClassId*/ 5 });

    EXPECT(ResolveNavClass(w, none, 2) == 0);   // absent → 0
    EXPECT(ResolveNavClass(w, c1,   2) == 1);   // in range → its id
    EXPECT(ResolveNavClass(w, cBad, 2) == 0);   // >= count → 0
    EXPECT(ResolveNavClass(w, c1,   0) == 0);   // count 0 → 0
}
```
Register in `main()`: `T34_resolve_nav_class();`

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL — `NavClassComponent` undeclared, `ResolveNavClass` undeclared.

- [ ] **Step 3: Declare `NavClassComponent` + register in X-macro (`ECS.h`)**

Add the struct immediately after `NavConstrainedComponent` (added by the base feature, near the other nav components):

```cpp
// Per-entity nav class selector: which class mesh (index into
// NavMeshConfigComponent::Classes) this entity navigates / is constrained to.
// Carried by NavAgents and by the player (alongside NavConstrainedComponent).
// Absent → class 0. Out-of-range ClassId → class 0 (NavMeshSystem clamps + warns).
struct NavClassComponent { uint8_t ClassId = 0; };
```

Add to `ECS_FOR_EACH_REGISTERED_COMPONENT` after `X(NavConstrainedComponent)` (moving the trailing backslash):
```cpp
    X(NavConstrainedComponent) \
    X(NavClassComponent)
```

- [ ] **Step 4: Add `ResolveNavClass` to `NavClass.h`**

Append the `ResolveNavClass` function shown in Task 1 Step 4 (it was deferred to here because it needs `NavClassComponent`). Add it at the end of `NavClass.h`:

```cpp
inline uint8_t ResolveNavClass(const ECS& world, EntityId e, uint8_t classCount) {
    const auto* nc = world.GetComponent<NavClassComponent>(e);
    if (!nc || classCount == 0) return 0;
    return (nc->ClassId < classCount) ? nc->ClassId : uint8_t{0};
}
```

- [ ] **Step 5: Run to verify the `ResolveNavClass` test passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.`

- [ ] **Step 6: Add command dispatch in `ECSCommands.h`**

In `ApplyComponentCommand`, after the `NavConstrainedComponent` branch (a data component now — use `Get<>()`):
```cpp
        } else if (componentData.Type == std::type_index(typeid(NavClassComponent))) {
            if (auto* nc = componentData.Get<NavClassComponent>()) {
                world.AddComponent(entity, *nc);
            }
```
In `RemoveComponentByType`, after the `NavConstrainedComponent` branch:
```cpp
        } else if (typeIndex == std::type_index(typeid(NavClassComponent))) {
            world.RemoveComponent<NavClassComponent>(entity);
```
In `CopyEntityComponents` (the duplication block, ~line 263 where `SunMarker`/`NavConstrainedComponent` are copied), add a value-copy:
```cpp
        if (auto* c = world.GetComponent<NavClassComponent>(src)) world.AddComponent(dst, *c);
```

- [ ] **Step 7: Add (de)serialization in `ComponentSerialization.h`**

After the `NavConstrainedComponent` json pair (added by the base feature):
```cpp
inline void to_json(nlohmann::json& j, const NavClassComponent& t) {
    j = nlohmann::json{ {"ClassId", t.ClassId} };
}
inline void from_json(const nlohmann::json& j, NavClassComponent& t) {
    if (j.contains("ClassId")) t.ClassId = static_cast<uint8_t>(j.at("ClassId").get<int>());
}
```

- [ ] **Step 8: Save/load in `WorldManager.cpp`**

In `SaveWorldSnapshot`, after the `NavConstrainedComponent` save block:
```cpp
        if (world->HasComponent<NavClassComponent>(entity)) {
            jEntity["NavClassComponent"] = *(world->GetComponent<NavClassComponent>(entity));
        }
```
In `LoadWorldSnapshot`, after the `NavConstrainedComponent` load line:
```cpp
            if (jEntity.contains("NavClassComponent"))
                world->AddComponent(createdEntity, jEntity["NavClassComponent"].get<NavClassComponent>());
```

- [ ] **Step 9: Write the failing json round-trip test in `tests/test_worldserial.cpp`**

```cpp
static void T_navclass_roundtrip() {
    NavClassComponent in; in.ClassId = 3;
    const nlohmann::json j = in;
    const auto out = j.get<NavClassComponent>();
    EXPECT(out.ClassId == 3);
}
```
Register in `main()`: `T_navclass_roundtrip();`

- [ ] **Step 10: Build both suites + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh --target test_worldserial && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe`
Expected: both suites pass (`All navmesh tests passed.` + worldserial pass line).

- [ ] **Step 11: Commit**

```bash
git add src/common/include/ECS.h src/common/include/NavClass.h src/common/include/ECSCommands.h src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_navmesh.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavClassComponent + ResolveNavClass + plumbing"
```

---

## Task 3: `NavMeshSystem` — N class slots (build/publish/Current(classId))

Obstacles temporarily operate on slot 0 here (kept compiling + existing obstacle tests green); Task 4 fans them out.

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h`
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp`
- Test: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write the failing multi-class build test in `tests/test_navmesh.cpp`**

```cpp
// ---------- Multi-class navmesh: T35 ----------
static void T35_multiclass_build_distinct_erosion() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));  // 10x10 floor
    NavMeshConfigComponent cfg = DefaultCfg();
    cfg.ClassCount = 2;
    cfg.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };  // small radius
    cfg.Classes[1] = NavClassConfig{ 2.0f, 1.8f, 0.4f };  // large radius → erodes more
    NavMeshSystem::Instance().Rebuild(w, cfg);

    auto small = NavMeshSystem::Instance().Current(0);
    auto large = NavMeshSystem::Instance().Current(1);
    EXPECT(small != nullptr);
    EXPECT(large != nullptr);
    if (!small || !large) return;
    // Larger agent radius erodes the walkable floor more → smaller walkable area.
    EXPECT(large->GetStats().PolyCount <= small->GetStats().PolyCount);
    // Out-of-range class slot → null.
    EXPECT(NavMeshSystem::Instance().Current(7) == nullptr);
}
```
Register in `main()`: `T35_multiclass_build_distinct_erosion();`

NOTE the assertion is `<=` not `<` to be robust to coarse cell sizes; if after implementation the implementer observes `PolyCount(large) < PolyCount(small)` reliably for this geometry, tighten to `<` with a comment. If they are equal (erosion difference smaller than one cell), increase `Classes[1].AgentRadius` until divergence appears and document the value — do not weaken the test to hide a non-functioning multi-class build.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `Current(int)` takes no argument / no overload (current `Current()` is nullary).

- [ ] **Step 3: Update `NavMeshSystem.h` for N slots**

Add `#include <array>` at the top. Replace the single `m_Current` member + `Current()` declaration:

Change the public accessor:
```cpp
    // Published mesh for a class slot. Out-of-range classId → nullptr.
    std::shared_ptr<const NavMesh> Current(uint8_t classId = 0) const;
```
Replace the private member `std::shared_ptr<const NavMesh> m_Current;` with:
```cpp
    std::array<std::shared_ptr<const NavMesh>, kMaxNavClasses> m_Classes{};
    uint8_t m_ClassCount = 0;   // live class slots after the last Rebuild
```
Change `PublishNavMesh` to take a slot index:
```cpp
    void PublishNavMesh(uint8_t classId, std::shared_ptr<const NavMesh> mesh);
```

- [ ] **Step 4: Update `NavMeshSystem.cpp` build/publish/Current**

Add `#include "NavClass.h"` near the top. Rewrite the relevant functions:

`Rebuild` — build the soup once, loop the live classes:
```cpp
void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg)
{
    const NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world);
    const uint8_t liveCount = NavLiveClassCount(cfg);

    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh");
        m_EntityToObstacle.clear();
        for (uint8_t i = 0; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});
        m_ClassCount = 0;
        return;
    }

    bool anyBuilt = false;
    for (uint8_t i = 0; i < liveCount; ++i) {
        auto fresh = NavMesh::Build(soup, ConfigForClass(cfg, i));
        if (!fresh) {
            SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null for class %u", (unsigned)i);
            continue;
        }
        PublishNavMesh(i, std::shared_ptr<const NavMesh>(std::move(fresh)));
        anyBuilt = true;
    }
    // Clear any slots beyond the live count (e.g. ClassCount was lowered).
    for (uint8_t i = liveCount; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});

    if (!anyBuilt) {
        SM_WARN("NavMeshSystem::Rebuild: no class meshes built; keeping previous");
        return;
    }
    m_EntityToObstacle.clear();
    m_ClassCount = liveCount;

    // Disk auto-bake (single-mesh format) only for the single-class case.
    // Multi-class disk bake is a deferred follow-up (see spec non-goals).
    if (liveCount == 1 && !m_LastWorldPath.empty()) {
        auto cur = Current(0);
        if (cur) {
            const std::string bakePath = DeriveBakePath(m_LastWorldPath);
            const uint64_t worldMtime = GetFileMtimeEpoch(m_LastWorldPath);
            if (!cur->SaveToFile(bakePath, worldMtime)) {
                SM_WARN("NavMeshSystem::Rebuild: failed to write disk bake to '%s'", bakePath.c_str());
            }
        }
    }
}
```

`Current`:
```cpp
std::shared_ptr<const NavMesh> NavMeshSystem::Current(uint8_t classId) const {
    if (classId >= kMaxNavClasses) return {};
    return std::atomic_load(&m_Classes[classId]);
}
```

`PublishNavMesh`:
```cpp
void NavMeshSystem::PublishNavMesh(uint8_t classId, std::shared_ptr<const NavMesh> mesh) {
    if (classId >= kMaxNavClasses) return;
    std::atomic_store(&m_Classes[classId], std::move(mesh));
    m_NavVersion.fetch_add(1, std::memory_order_relaxed);
}
```

`Tick` and the obstacle methods (`AddCylinderObstacle`, `AddBoxObstacle`, `RemoveObstacle`) currently use `m_Current` — point them at slot 0 for now (Task 4 fans out). E.g.:
```cpp
void NavMeshSystem::Tick(float dt) {
    auto cur = Current(0);
    if (!cur) return;
    const_cast<NavMesh*>(cur.get())->Tick(dt);
}
```
and the same `Current(0)` substitution in `AddCylinderObstacle`/`AddBoxObstacle`/`RemoveObstacle`/`SaveCurrentToDisk` (anywhere that read `std::atomic_load(&m_Current)`).

`TryLoadFromDisk`: replace `PublishNavMesh(shared)` with `PublishNavMesh(0, shared)` and set `m_ClassCount = 1` on success (it loads the single-mesh class-0 bake). It already only serves the single-class case.

- [ ] **Step 5: Build + run the whole navmesh suite**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` — including the prior single-class tests T01-T34 (which use `Current()` defaulting to slot 0) and the new T35.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): NavMeshSystem builds + publishes N class meshes"
```

---

## Task 4: Obstacle fan-out across all class meshes (logical ids)

**Files:**
- Modify: `src/engine/src/navigation/NavMeshSystem.h` (logical-id map)
- Modify: `src/engine/src/navigation/NavMeshSystem.cpp` (Add/Remove/Tick fan-out)
- Test: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write the failing fan-out test in `tests/test_navmesh.cpp`**

```cpp
// ---------- Multi-class obstacle fan-out: T36 ----------
static void T36_obstacle_blocks_all_classes() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshConfigComponent cfg = DefaultCfg();
    cfg.ClassCount = 2;
    cfg.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    cfg.Classes[1] = NavClassConfig{ 0.5f, 1.8f, 0.4f };
    NavMeshSystem::Instance().Rebuild(w, cfg);

    // Straight path on both classes before the obstacle.
    EXPECT(NavMeshSystem::Instance().Current(0)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() == 2);
    EXPECT(NavMeshSystem::Instance().Current(1)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() == 2);

    // Add a cylinder obstacle on the path (fans out to all class meshes).
    SpawnCylinderObstacle(w, glm::vec3(0,0,0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    // Both class meshes now route around it.
    EXPECT(NavMeshSystem::Instance().Current(0)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() > 2);
    EXPECT(NavMeshSystem::Instance().Current(1)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() > 2);
}
```
Register in `main()`: `T36_obstacle_blocks_all_classes();`

NOTE `DrainTileCache` currently ticks only via `NavMeshSystem::Tick` (slot 0 today). After this task `Tick` must drive every class's tilecache (see Step 3), or class 1 won't carve. If the obstacle radius is too small to split the path for the larger class, bump `Classes[1].AgentRadius` / obstacle radius until both classes reroute, and document — do not weaken to `>= 2`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: T36 FAILS — class 1 path stays straight (size 2) because the obstacle only carves slot 0 and `Tick` only updates slot 0.

- [ ] **Step 3: Add the logical-id obstacle map + fan-out in `NavMeshSystem.{h,cpp}`**

In `NavMeshSystem.h`, replace the `EntityId → ObstacleHandle` direct mapping's backing with a logical-id layer. Add members:
```cpp
    // Logical obstacle id → per-class NavMesh-level obstacle refs (0 = unused slot).
    std::unordered_map<uint32_t, std::array<uint32_t, kMaxNavClasses>> m_Obstacles;
    uint32_t m_NextObstacleId = 1;   // 0 reserved as "none"
```
(`m_EntityToObstacle` stays `EntityId → ObstacleHandle`, where the handle is now the logical id.)

In `NavMeshSystem.cpp`, rewrite the obstacle methods to fan out across `[0, m_ClassCount)`:
```cpp
NavMeshSystem::ObstacleHandle NavMeshSystem::AddCylinderObstacle(
    const glm::vec3& pos, float radius, float height)
{
    std::array<uint32_t, kMaxNavClasses> refs{};
    bool any = false;
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (!cur) continue;
        refs[i] = const_cast<NavMesh*>(cur.get())->AddCylinderObstacle(pos, radius, height);
        any = any || (refs[i] != 0);
    }
    if (!any) return 0;
    const uint32_t id = m_NextObstacleId++;
    m_Obstacles[id] = refs;
    return id;
}

NavMeshSystem::ObstacleHandle NavMeshSystem::AddBoxObstacle(
    const glm::vec3& bmin, const glm::vec3& bmax)
{
    std::array<uint32_t, kMaxNavClasses> refs{};
    bool any = false;
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (!cur) continue;
        refs[i] = const_cast<NavMesh*>(cur.get())->AddBoxObstacle(bmin, bmax);
        any = any || (refs[i] != 0);
    }
    if (!any) return 0;
    const uint32_t id = m_NextObstacleId++;
    m_Obstacles[id] = refs;
    return id;
}

void NavMeshSystem::RemoveObstacle(ObstacleHandle id) {
    if (id == 0) return;
    auto it = m_Obstacles.find(id);
    if (it == m_Obstacles.end()) return;
    for (uint8_t i = 0; i < kMaxNavClasses; ++i) {
        const uint32_t ref = it->second[i];
        if (ref == 0) continue;
        auto cur = Current(i);
        if (cur) const_cast<NavMesh*>(cur.get())->RemoveObstacle(ref);
    }
    m_Obstacles.erase(it);
}

void NavMeshSystem::Tick(float dt) {
    for (uint8_t i = 0; i < m_ClassCount; ++i) {
        auto cur = Current(i);
        if (cur) const_cast<NavMesh*>(cur.get())->Tick(dt);
    }
}
```
Also clear `m_Obstacles` everywhere `m_EntityToObstacle.clear()` is called (Rebuild empty-soup path, Rebuild success path, TryLoadFromDisk) — the per-class refs are invalid against freshly-built tilecaches. Add `m_Obstacles.clear();` next to each `m_EntityToObstacle.clear();`.

- [ ] **Step 4: Build + run the suite**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (T36 passes; existing single-class obstacle tests T09-T13 still pass — they run with `ClassCount==0`→`m_ClassCount==1`, so fan-out covers slot 0 exactly as before).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): fan obstacles out to all class meshes via logical ids"
```

---

## Task 5: `NavServices` `…ForClass` queries

**Files:**
- Modify: `src/common/include/NavServices.h` (append fields)
- Modify: `src/engine/src/navigation/NavServicesImpl.cpp` (forwarders + wiring; old → class 0)
- Test: `tests/test_navmesh.cpp`

- [ ] **Step 1: Write the failing tests in `tests/test_navmesh.cpp`**

```cpp
// ---------- Class-aware NavServices: T37-T38 ----------
static void T37_navservices_forclass_queries() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshConfigComponent cfg = DefaultCfg();
    cfg.ClassCount = 2;
    cfg.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    cfg.Classes[1] = NavClassConfig{ 0.5f, 1.8f, 0.4f };
    NavMeshSystem::Instance().Rebuild(w, cfg);

    const NavServices* svc = TestNavServices();
    EXPECT(svc->HasMeshForClass != nullptr);
    EXPECT(svc->HasMeshForClass(0));
    EXPECT(svc->HasMeshForClass(1));
    EXPECT(!svc->HasMeshForClass(7));               // out-of-range slot

    std::vector<glm::vec3> path;
    svc->FindPathForClass(1, glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0), 50.0f, &path);
    EXPECT(path.size() >= 2);

    const glm::vec3 mv = svc->MoveAlongSurfaceForClass(0, glm::vec3(0,0.1f,0), glm::vec3(1.0f,0.1f,0));
    EXPECT(std::fabs(mv.x - 1.0f) < 0.3f);
}

static void T38_navservices_legacy_fns_map_to_class0() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());   // ClassCount 0 → single class 0
    const NavServices* svc = TestNavServices();
    EXPECT(svc->HasMesh());                                // legacy → class 0
    std::vector<glm::vec3> path;
    svc->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0), 50.0f, &path);
    EXPECT(path.size() >= 2);
}
```
Register in `main()`:
```cpp
    T37_navservices_forclass_queries();
    T38_navservices_legacy_fns_map_to_class0();
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `HasMeshForClass`/`FindPathForClass`/`MoveAlongSurfaceForClass` not members of `NavServices`.

- [ ] **Step 3: Append the fields to `NavServices.h`**

At the END of `struct NavServices` (append-only contract — after `MoveAlongSurface` from the base feature):
```cpp
    // ---- Class-aware queries (multi-class navmesh) ----

    // Per-class variants of HasMesh/FindPath/MoveAlongSurface. classId indexes
    // NavMeshConfigComponent::Classes; out-of-range → no mesh / empty / passthrough.
    // The non-class HasMesh/FindPath/MoveAlongSurface above are equivalent to
    // classId == 0. GameThread only.
    bool      (*HasMeshForClass)(uint8_t classId);
    void      (*FindPathForClass)(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                                  float maxSearchRadius, std::vector<glm::vec3>* outPath);
    glm::vec3 (*MoveAlongSurfaceForClass)(uint8_t classId, const glm::vec3& start,
                                          const glm::vec3& desiredEnd);
```
(Ensure `<cstdint>` is included for `uint8_t` — it already is.)

- [ ] **Step 4: Add forwarders + wiring in `NavServicesImpl.cpp`; route old fns to class 0**

Add class-aware forwarders in the anonymous namespace:
```cpp
bool ForwardHasMeshForClass(uint8_t classId) {
    return NavMeshSystem::Instance().Current(classId) != nullptr;
}

void ForwardFindPathForClass(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                             float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    if (!outPath) return;
    outPath->clear();
    auto nm = NavMeshSystem::Instance().Current(classId);
    if (!nm) return;
    const auto path = nm->FindPath(start, end, maxSearchRadius);
    outPath->reserve(path.size());
    for (const auto& pt : path) outPath->push_back(pt.Position);
}

glm::vec3 ForwardMoveAlongSurfaceForClass(uint8_t classId, const glm::vec3& start,
                                          const glm::vec3& desiredEnd) {
    auto nm = NavMeshSystem::Instance().Current(classId);
    if (!nm) return desiredEnd;
    return nm->ConstrainMove(start, desiredEnd);
}
```
Make the existing `ForwardHasMesh`/`ForwardFindPath`/`ForwardMoveAlongSurface` delegate to class 0 (keeps behavior; single source of truth). Simplest — change their bodies to call the class-0 variant:
```cpp
bool ForwardHasMesh() { return ForwardHasMeshForClass(0); }
void ForwardFindPath(const glm::vec3& start, const glm::vec3& end,
                     float maxSearchRadius, std::vector<glm::vec3>* outPath) {
    ForwardFindPathForClass(0, start, end, maxSearchRadius, outPath);
}
glm::vec3 ForwardMoveAlongSurface(const glm::vec3& start, const glm::vec3& desiredEnd) {
    return ForwardMoveAlongSurfaceForClass(0, start, desiredEnd);
}
```
(Reorder so the `…ForClass` forwarders are defined before the legacy ones that call them, or add forward declarations.)

Wire the new pointers in `NavServicesImpl::Init`, after `out.MoveAlongSurface = ...`:
```cpp
    out.HasMeshForClass          = &ForwardHasMeshForClass;
    out.FindPathForClass         = &ForwardFindPathForClass;
    out.MoveAlongSurfaceForClass = &ForwardMoveAlongSurfaceForClass;
```

- [ ] **Step 5: Build + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (T37/T38 pass; the base feature's T24/T31/T32 still pass via class-0 delegation).

- [ ] **Step 6: Commit**

```bash
git add src/common/include/NavServices.h src/engine/src/navigation/NavServicesImpl.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): class-aware NavServices queries (…ForClass)"
```

---

## Task 6: Game systems route by class

Build + manual verified (game systems have no unit-test seam). Both systems read the live class count once per Update from the `NavMeshConfigComponent` singleton, then resolve each entity's class.

**Files:**
- Modify: `src/game/src/NavAgentSystem.h`
- Modify: `src/game/src/game.cpp` (`KinematicMovementSystem`)

- [ ] **Step 1: Route `NavAgentSystem` by class**

In `src/game/src/NavAgentSystem.h`, add `#include "NavClass.h"` near the other includes. In `Update`, compute the live class count once (after the existing early-out), and replace the `FindPath` call with the per-class variant.

Add near the top of `Update`, after `const uint32_t navVer = ctx.Nav->NavVersion();`:
```cpp
        // Live class count from the nav config singleton (default 1).
        uint8_t classCount = 1;
        if (const auto* navCfg = ctx.world.GetSingleton<NavMeshConfigComponent>())
            classCount = NavLiveClassCount(*navCfg);
```
Then inside the per-agent repath block, replace:
```cpp
                        ctx.Nav->FindPath(tr.Position, target.Destination,
                                          kFindPathSearchRadius, &m_PathScratch);
```
with:
```cpp
                        const uint8_t navClass = ResolveNavClass(ctx.world, e, classCount);
                        ctx.Nav->FindPathForClass(navClass, tr.Position, target.Destination,
                                                  kFindPathSearchRadius, &m_PathScratch);
```

- [ ] **Step 2: Route `KinematicMovementSystem` by class (`game.cpp`)**

Add `#include "NavClass.h"` near the top of `game.cpp` (with the other includes). In `KinematicMovementSystem::Update`, compute the class count once before the `Each` loop:
```cpp
        uint8_t classCount = 1;
        if (const auto* navCfg = ctx.world.GetSingleton<NavMeshConfigComponent>())
            classCount = NavLiveClassCount(*navCfg);
```
In the nav-constrain block, replace the `MoveAlongSurface` call:
```cpp
                const glm::vec3 clamped = ctx.Nav->MoveAlongSurface(transform->Position, end);
```
with:
```cpp
                const uint8_t navClass = ResolveNavClass(ctx.world, e, classCount);
                const glm::vec3 clamped = ctx.Nav->MoveAlongSurfaceForClass(navClass, transform->Position, end);
```
(Leave the planar-Y reconstruction and AABB layering from the base feature unchanged.)

- [ ] **Step 3: Build the game library**

Run: `cmake --build --preset msvc-win64-vs2026-community --target game`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add src/game/src/NavAgentSystem.h src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(game): route nav queries by entity class"
```

---

## Task 7: Editor — class-list authoring + per-entity class picker

Build + manual verified (ImGui UI).

**Files:**
- Modify: `src/editor/src/panels/NavigationPanel.cpp`
- Create: `src/editor/src/panels/inspector/NavClassEditor.{h,cpp}`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp` (include + register)
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Add the class-list editor to `NavigationPanel.cpp`**

Replace the three per-agent drag lines (the `Agent Radius` / `Agent Height` / `Max Climb` drags, ~lines 106-108) with a class-list editor. Keep `Cell Size`, `Cell Height`, `Max Slope`, `Tile Size`, `Max Obstacles`. Insert after the `Cell Height` drag and before `Max Slope`:

```cpp
    // --- Classes (per-radius bakes) ---
    ImGui::SeparatorText("Classes");
    if (edited.ClassCount == 0) {
        ImGui::TextDisabled("Legacy single class (uses Agent* defaults below).");
    }
    for (uint8_t i = 0; i < edited.ClassCount; ++i) {
        ImGui::PushID(i);
        ImGui::Text("Class %u", (unsigned)i);
        changed |= ImGui::DragFloat("Radius",  &edited.Classes[i].AgentRadius,   0.05f, 0.05f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Height",  &edited.Classes[i].AgentHeight,   0.05f, 0.10f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Climb",   &edited.Classes[i].AgentMaxClimb, 0.05f, 0.00f, 2.0f, "%.2f m");
        ImGui::PopID();
        ImGui::Separator();
    }
    if (edited.ClassCount < kMaxNavClasses && ImGui::Button("Add Class")) {
        // Seed from the top-level defaults (or the previous class).
        edited.Classes[edited.ClassCount] = (edited.ClassCount > 0)
            ? edited.Classes[edited.ClassCount - 1]
            : NavClassConfig{ edited.AgentRadius, edited.AgentHeight, edited.AgentMaxClimb };
        edited.ClassCount++;
        changed = true;
    }
    ImGui::SameLine();
    if (edited.ClassCount > 0 && ImGui::Button("Remove Last Class")) {
        edited.ClassCount--;
        changed = true;
    }
```
Keep the remaining global drags (Max Slope / Tile Size / Max Obstacles) and the existing `if (changed) PushSingletonEdit(...)`. Leave the existing top-level `Agent Radius/Height/Climb` drags in place too (they remain the legacy/class-0 default and the build-active scratch) — OR move them under a collapsing "Legacy defaults" header for clarity. Simplest acceptable: keep them as-is above the Classes section. (If you keep them, label the section "Agent defaults (class 0 / legacy)".)

- [ ] **Step 2: Create `NavClassEditor.h`**

```cpp
#pragma once
#include "IComponentEditor.h"
class NavClassEditor final : public IComponentEditor {
public:
    const char* Label() const override { return "NavMesh Class Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavClassComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```

- [ ] **Step 3: Create `NavClassEditor.cpp`**

Mirror a data-component editor that uses `EditState<T>` (like `NavMeshSourceEditor`) so the ClassId edit is committed through the command ring:
```cpp
#include "NavClassEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "EditState.h"
#include "lib.h"

namespace { EditState<NavClassComponent> g_St; }

void NavClassEditor::AddDefault(const EditorContext& ctx, EntityId e) {
    ECSCommand addCmd = ECSCommand::AddComponent(e, NavClassComponent{});
    if (!ctx.App->ECSCommandRing.Push(addCmd))
        SM_WARN("ECS command queue full! Add component command dropped.");
}
void NavClassEditor::Remove(const EditorContext& ctx, EntityId e) {
    ECSCommand removeCmd = ECSCommand::RemoveComponent<NavClassComponent>(e);
    if (!ctx.App->ECSCommandRing.Push(removeCmd))
        SM_WARN("ECS command queue full! Remove component command dropped.");
}
void NavClassEditor::DrawEditor(const EditorContext& ctx, EntityId e) {
    const auto* c = g_St.Begin(ctx, e);
    if (!c) return;
    int classId = static_cast<int>(g_St.edit.ClassId);
    if (ImGui::DragInt("Class Id", &classId, 0.1f, 0, kMaxNavClasses - 1)) {
        g_St.edit.ClassId = static_cast<uint8_t>(classId);
        g_St.modified = true;
    }
    ImGui::TextDisabled("Index into NavMeshConfig classes; out-of-range falls back to 0.");
    g_St.Commit(ctx, e);
}
```
IMPORTANT: verify the real `EditState<T>` API (`Begin`/`edit`/`modified`/`Commit`) against an existing editor that uses it (e.g. `NavMeshSourceEditor.cpp` uses a member `m_St`). Match that exact usage — if `EditState` must be a per-instance member rather than a namespace global, make it a member like the other editors (`EditState<NavClassComponent> m_St;` in the header). Prefer the member form to match the codebase.

- [ ] **Step 4: Register in `EcsInspectorPanel.cpp` + add to CMake**

Include near the other inspector includes:
```cpp
#include "inspector/NavClassEditor.h"
```
Register after the `NavConstrainedEditor` push (or after `NavTargetEditor`):
```cpp
    m_Editors.push_back(std::make_unique<NavClassEditor>());
```
Add to `src/editor/CMakeLists.txt` next to the other inspector sources:
```cmake
    src/panels/inspector/NavClassEditor.cpp
```

- [ ] **Step 5: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds + links clean.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/panels/NavigationPanel.cpp src/editor/src/panels/inspector/NavClassEditor.h src/editor/src/panels/inspector/NavClassEditor.cpp src/editor/src/panels/EcsInspectorPanel.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): nav class-list authoring + per-entity NavClass editor"
```

---

## Task 8: End-to-end manual verification

No code. Confirms multi-class behavior in the running editor.

- [ ] **Step 1: Full rebuild + restart editor**

Run: `cmake --build --preset msvc-win64-vs2026-community` (builds `ecs`, `engine`, `game`, `editor`). Then **fully restart `editor.exe`** (ECS struct set changed).

- [ ] **Step 2: Author two classes** in the Navigation panel: Class 0 (player radius, e.g. 0.5) and Class 1 (boss radius, e.g. 1.2). Rebuild NavMesh.

- [ ] **Step 3: Two constrained entities** — give the player `NavConstrained` + `NavMesh Class Component` ClassId 0; give a larger test entity `NavConstrained` + `NavMesh Class` ClassId 1 (+ a `NavAgent`/`NavTarget` if you want it to path). Save World.

- [ ] **Step 4: Play mode** — verify the Class 1 entity keeps **farther** from walls/obstacles than the Class 0 entity (its body no longer pokes through), and neither body penetrates the geometry the way it did pre-change. Confirm a same-size monster on Class 0 behaves like the player.

- [ ] **Step 5: Legacy + persistence** — load an existing world.json with no classes → still works (single class 0). Reload after saving → `NavClassComponent` + the class list persist. Optionally `runtime.exe` honors per-class constraint.

- [ ] **Step 6 (if a code fix was needed): commit it.**

```bash
git add -A
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "fix(nav): <describe manual-verification fix>"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** §1 class model (Task 1, with the documented keep-flat-fields deviation); §2 NavClassComponent + ResolveNavClass (Task 2); §3 N meshes (Task 3); §4 obstacle fan-out (Task 4); §5 …ForClass queries (Task 5); §6 editor (Task 7); §7 error handling (clamps/warns across Tasks 2-5); §8 migration + disk-bake short-circuit (Tasks 1 + 3); testing (Tasks 1-5 unit, Task 8 manual). Game-system routing (Task 6). All mapped.
- **Deviation:** kept `NavMeshConfigComponent` flat agent fields (vs spec "move out") so `NavMesh::Build` is untouched and migration is automatic — documented in the header + Task 1.
- **Type consistency:** `ConfigForClass`, `NavLiveClassCount`, `ResolveNavClass`, `NavClassConfig{AgentRadius,AgentHeight,AgentMaxClimb}`, `NavClassComponent{ClassId}`, `Current(uint8_t)`, `PublishNavMesh(uint8_t, …)`, `…ForClass` signatures are identical across all tasks that reference them.
- **Placeholders:** none — complete code + exact commands/expected output per step.
- **Calibration risk flagged** (T35 PolyCount divergence, T36 fan-out reroute) with explicit instructions to investigate real values and document, not to weaken assertions.
