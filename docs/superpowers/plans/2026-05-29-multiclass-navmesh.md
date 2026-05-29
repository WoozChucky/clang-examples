# Multi-Class Navmesh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bake N navmeshes at different agent radii ("classes") over the same map so each entity navigates the mesh eroded for its body size — making `moveAlongSurface` keep the body tangent to walls with zero runtime radius math.

**Architecture:** `NavMeshConfigComponent` drops its flat per-agent fields and gains a fixed-cap `Classes[]` array (`NavClassConfig`, single source of truth, `ClassCount >= 1`). `NavMesh::Build` takes the chosen `NavClassConfig`. `NavMeshSystem` holds one published `NavMesh` per class slot, built from one triangle soup; obstacles fan out to every class mesh via logical ids; new `NavServices` `…ForClass` queries are selected per-entity via a shared `NavClassComponent`. `NavMesh` is otherwise unchanged (still single-class).

**Tech Stack:** C++23, Recast/Detour (`third_party/recastnavigation`), glm, project ECS (X-macro registration), nlohmann::json, ImGui.

**Data-model decision (Design Y — confirmed with user):** the per-agent fields (`AgentRadius/AgentHeight/AgentMaxClimb`) are **removed** from `NavMeshConfigComponent`; they live only in `NavClassConfig`. `NavMesh::Build` reads them from the passed class. **No backward-compat / migration code** (engine is early-dev; breaking the world.json schema is acceptable) — a config with no `Classes` key loads as one default class. The build-output `world.json` is hand-updated to the new schema (Task 8).

**Build/test preset (project memory):** `msvc-win64-vs2026-community` ONLY (enterprise NOT installed). Test binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/<target>.exe`.

**Commit identity (project memory):** author every commit `Nuno Silva <nuno.levezinho@live.com.pt>` via `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`.

**Reload note:** reshaping `NavMeshConfigComponent` + adding `NavClassComponent` changes the ECS struct set → rebuild `ecs.dll`, `editor`, `game` and **restart the editor** for manual verification. `GAME_API_VERSION` unchanged.

**Branch:** continue on `feat/navmesh-constrained-movement` (stacked on the verified base feature). Builds are slow (minutes) — let them finish.

---

## File Structure

| File | Responsibility | Task |
|------|----------------|------|
| `src/common/include/ECS.h` | `kMaxNavClasses`, `NavClassConfig`, reshape `NavMeshConfigComponent`, `NavClassComponent` + X-macro | 1, 2 |
| `src/engine/src/navigation/NavMesh.{h,cpp}` | `Build` takes a `NavClassConfig` | 1 |
| `src/common/include/NavClass.h` (new) | Pure helpers `NavLiveClassCount`, `ResolveNavClass` | 1, 2 |
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
| build-output `world.json` | hand-update to `Classes` schema (not committed) | 8 |
| `tests/test_navmesh.cpp`, `tests/test_worldserial.cpp` | helpers / multi-class / fan-out / forwarder / serialization | 1-5 |

---

## Task 1: Config reshape (drop flat fields) + `NavMesh::Build(cls)` + serialization

**Files:**
- Modify: `src/common/include/ECS.h` (`NavMeshConfigComponent` ~line 284)
- Modify: `src/engine/src/navigation/NavMesh.h` (`Build` decl ~line 51) + `NavMesh.cpp` (`Build` body ~lines 117-200)
- Create: `src/common/include/NavClass.h`
- Modify: `src/common/include/ComponentSerialization.h` (`NavMeshConfigComponent` json ~line 306)
- Test: `tests/test_navmesh.cpp` (NavLiveClassCount), `tests/test_worldserial.cpp` (serialization)

- [ ] **Step 1: Write the failing `NavLiveClassCount` test in `tests/test_navmesh.cpp`**

Add `#include "NavClass.h"` near the top includes. Add after the last test fn (before `int main()`):
```cpp
// ---------- Multi-class config: T33 ----------
static void T33_live_class_count_clamps_to_one() {
    NavMeshConfigComponent def{};                 // default → 1 class
    EXPECT(NavLiveClassCount(def) == 1);
    NavMeshConfigComponent two{}; two.ClassCount = 2;
    EXPECT(NavLiveClassCount(two) == 2);
    NavMeshConfigComponent zero{}; zero.ClassCount = 0;   // malformed → clamp to 1
    EXPECT(NavLiveClassCount(zero) == 1);
}
```
Register in `main()`: `T33_live_class_count_clamps_to_one();`

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL to compile — `NavClass.h` not found / `NavLiveClassCount` undeclared / `NavClassConfig`/`Classes`/`ClassCount` not members.

- [ ] **Step 3: Reshape `NavMeshConfigComponent` in `ECS.h`**

Replace the existing `struct NavMeshConfigComponent { ... };` (~lines 284-293) with:
```cpp
inline constexpr int kMaxNavClasses = 8;

// One agent-radius class. Fixed-cap + trivially-copyable (no heap) so it rides
// in the ECS snapshot like NavAgentComponent::CachedPath[]. Single source of
// truth for agent footprint — NavMesh::Build reads these directly.
struct NavClassConfig {
    float AgentRadius   = 0.5f;   // agent capsule radius (m) — drives Recast erosion
    float AgentHeight   = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb = 0.4f;   // step-up height (m)
};

struct NavMeshConfigComponent {
    float CellSize      = 0.3f;   // voxel XZ size (m)        — shared across classes
    float CellHeight    = 0.2f;   // voxel Y size (m)         — shared
    float AgentMaxSlope = 45.0f;  // max walkable slope (deg) — shared
    float TileSize      = 32.0f;  // tile XZ size (voxels)    — shared
    int   MaxObstacles  = 128;    // dtTileCache pre-alloc    — shared

    // Per-radius classes. Invariant: ClassCount >= 1 (a default config has one
    // class). ClassId on an entity indexes here.
    NavClassConfig Classes[kMaxNavClasses]{};
    uint8_t        ClassCount = 1;
};
```

- [ ] **Step 4: Change `NavMesh::Build` to take a `NavClassConfig` (`NavMesh.h` + `.cpp`)**

In `NavMesh.h`: add a forward decl near the existing `struct NavMeshConfigComponent;` (~line 22):
```cpp
struct NavClassConfig;                // from ECS.h
```
Change the `Build` declaration (~line 51-52) to:
```cpp
    static std::unique_ptr<NavMesh> Build(const NavMeshTriangleSoup& soup,
                                          const NavMeshConfigComponent& cfg,
                                          const NavClassConfig& cls);
```

In `NavMesh.cpp`, change the `Build` definition signature to match, and replace the seven agent-param reads (`cfg.Agent*` → `cls.Agent*`). The shared params (`cfg.CellSize`, `cfg.CellHeight`, `cfg.AgentMaxSlope`, `cfg.TileSize`, `cfg.MaxObstacles`) stay on `cfg`. Specifically:
```cpp
std::unique_ptr<NavMesh> NavMesh::Build(const NavMeshTriangleSoup& soup,
                                        const NavMeshConfigComponent& cfg,
                                        const NavClassConfig& cls)
```
Then within the body change:
- `rcc.walkableHeight = (int)std::ceil(cfg.AgentHeight / cfg.CellHeight);` → `cls.AgentHeight`
- `rcc.walkableClimb  = (int)std::floor(cfg.AgentMaxClimb / cfg.CellHeight);` → `cls.AgentMaxClimb`
- `rcc.walkableRadius = (int)std::ceil(cfg.AgentRadius / cfg.CellSize);` → `cls.AgentRadius`
- the `bmax` line `soup.AabbMax.y + cfg.AgentHeight + 1.0f` → `cls.AgentHeight`
- `tcParams.walkableHeight = cfg.AgentHeight;` → `cls.AgentHeight`
- `tcParams.walkableRadius = cfg.AgentRadius;` → `cls.AgentRadius`
- `tcParams.walkableClimb  = cfg.AgentMaxClimb;` → `cls.AgentMaxClimb`
Leave everything else (`rcc.cs/ch`, `walkableSlopeAngle = cfg.AgentMaxSlope`, `tileSize = cfg.TileSize`, `tcParams.maxObstacles = cfg.MaxObstacles`, etc.) reading from `cfg`.

- [ ] **Step 5: Update the single `Build` caller in `NavMeshSystem.cpp` (Task 3 generalizes it)**

In `NavMeshSystem::Rebuild`, the one `NavMesh::Build(soup, cfg)` call becomes:
```cpp
    auto fresh = NavMesh::Build(soup, cfg, cfg.Classes[0]);
```
(`ClassCount >= 1`, so `Classes[0]` is always valid. Task 3 replaces this with the per-class loop.)

- [ ] **Step 6: Create `src/common/include/NavClass.h`**

```cpp
#pragma once

#include <cstdint>

#include "ECS.h"   // NavMeshConfigComponent

// Number of class meshes to build: ClassCount clamped to at least 1.
inline uint8_t NavLiveClassCount(const NavMeshConfigComponent& cfg) {
    return cfg.ClassCount > 0 ? cfg.ClassCount : uint8_t{1};
}

// ResolveNavClass is added in Task 2 (needs NavClassComponent).
```

- [ ] **Step 7: Run to verify the NavLiveClassCount test passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (existing single-class tests still pass: `DefaultCfg()` has `ClassCount=1`, `Classes[0]` defaulted, so `Rebuild` builds the same mesh as before).

- [ ] **Step 8: Write the failing serialization tests in `tests/test_worldserial.cpp`**

```cpp
static void T_navcfg_multiclass_roundtrip() {
    NavMeshConfigComponent in;
    in.ClassCount = 2;
    in.Classes[0] = NavClassConfig{ 0.3f, 1.8f, 0.4f };
    in.Classes[1] = NavClassConfig{ 1.5f, 2.4f, 0.5f };
    const nlohmann::json j = in;
    const auto out = j.get<NavMeshConfigComponent>();
    EXPECT(out.ClassCount == 2);
    EXPECT(near(out.Classes[0].AgentRadius, 0.3f));
    EXPECT(near(out.Classes[1].AgentRadius, 1.5f));
    EXPECT(near(out.Classes[1].AgentHeight, 2.4f));
}

static void T_navcfg_no_classes_defaults_to_one() {
    // A config json with shared params but NO "Classes" key (old/hand-written).
    nlohmann::json j = {
        {"CellSize", 0.3f}, {"CellHeight", 0.2f},
        {"AgentMaxSlope", 45.0f}, {"TileSize", 32.0f}, {"MaxObstacles", 128}
    };
    const auto out = j.get<NavMeshConfigComponent>();
    EXPECT(out.ClassCount == 1);                       // seeded one default class
    EXPECT(near(out.Classes[0].AgentRadius, 0.5f));    // NavClassConfig default
}
```
Register both in `main()`:
```cpp
    T_navcfg_multiclass_roundtrip();
    T_navcfg_no_classes_defaults_to_one();
```

- [ ] **Step 9: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_worldserial`
Expected: FAIL — serializer doesn't emit/read `Classes` yet (`out.ClassCount` mismatches).

- [ ] **Step 10: Update `NavMeshConfigComponent` (de)serialization in `ComponentSerialization.h`**

Ensure `<algorithm>` is included. Replace the existing `to_json`/`from_json` for `NavMeshConfigComponent` (~lines 306-327) with:
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
        {"AgentMaxSlope", t.AgentMaxSlope},
        {"TileSize",      t.TileSize},
        {"MaxObstacles",  t.MaxObstacles}
    };
    nlohmann::json classes = nlohmann::json::array();
    for (uint8_t i = 0; i < t.ClassCount && i < kMaxNavClasses; ++i) classes.push_back(t.Classes[i]);
    j["Classes"] = std::move(classes);
}
inline void from_json(const nlohmann::json& j, NavMeshConfigComponent& t) {
    if (j.contains("CellSize"))      j.at("CellSize").get_to(t.CellSize);
    if (j.contains("CellHeight"))    j.at("CellHeight").get_to(t.CellHeight);
    if (j.contains("AgentMaxSlope")) j.at("AgentMaxSlope").get_to(t.AgentMaxSlope);
    if (j.contains("TileSize"))      j.at("TileSize").get_to(t.TileSize);
    if (j.contains("MaxObstacles"))  j.at("MaxObstacles").get_to(t.MaxObstacles);

    // Classes (absent/empty → one default class; invariant ClassCount >= 1).
    if (j.contains("Classes") && j.at("Classes").is_array() && !j.at("Classes").empty()) {
        const auto& arr = j.at("Classes");
        const size_t n = std::min<size_t>(arr.size(), kMaxNavClasses);
        for (size_t i = 0; i < n; ++i) t.Classes[i] = arr.at(i).get<NavClassConfig>();
        t.ClassCount = static_cast<uint8_t>(n);
    } else {
        t.Classes[0] = NavClassConfig{};
        t.ClassCount = 1;
    }
}
```

- [ ] **Step 11: Run both suites**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh --target test_worldserial && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe`
Expected: `All navmesh tests passed.` + worldserial pass line.

- [ ] **Step 12: Commit**

```bash
git add src/common/include/ECS.h src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp src/engine/src/navigation/NavMeshSystem.cpp src/common/include/NavClass.h src/common/include/ComponentSerialization.h tests/test_navmesh.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): NavClassConfig classes + NavMesh::Build(cls) + serialization"
```

---

## Task 2: `NavClassComponent` (per-entity class) + plumbing + `ResolveNavClass`

**Files:**
- Modify: `src/common/include/ECS.h` (struct near `NavConstrainedComponent` + X-macro)
- Modify: `src/common/include/NavClass.h` (add `ResolveNavClass`)
- Modify: `src/common/include/ECSCommands.h` (Apply, Remove, Copy)
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp` (save + load)
- Test: `tests/test_navmesh.cpp` (`ResolveNavClass`), `tests/test_worldserial.cpp` (json round-trip)

- [ ] **Step 1: Write the failing `ResolveNavClass` test in `tests/test_navmesh.cpp`**

```cpp
// ---------- Per-entity nav class: T34 ----------
static void T34_resolve_nav_class() {
    ECS w;
    const EntityId none = w.CreateEntity();
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
Expected: FAIL — `NavClassComponent` / `ResolveNavClass` undeclared.

- [ ] **Step 3: Declare `NavClassComponent` + register in X-macro (`ECS.h`)**

Add after `NavConstrainedComponent` (base feature):
```cpp
// Per-entity nav class selector: which class mesh (index into
// NavMeshConfigComponent::Classes) this entity navigates / is constrained to.
// Carried by NavAgents and by the player (alongside NavConstrainedComponent).
// Absent → class 0. Out-of-range ClassId → class 0.
struct NavClassComponent { uint8_t ClassId = 0; };
```
Add to `ECS_FOR_EACH_REGISTERED_COMPONENT` after `X(NavConstrainedComponent)` (move the trailing backslash):
```cpp
    X(NavConstrainedComponent) \
    X(NavClassComponent)
```

- [ ] **Step 4: Add `ResolveNavClass` to `NavClass.h`**

Append:
```cpp
// Resolve an entity's nav class: its NavClassComponent.ClassId clamped to
// [0, classCount). Missing component or out-of-range → 0.
inline uint8_t ResolveNavClass(const ECS& world, EntityId e, uint8_t classCount) {
    const auto* nc = world.GetComponent<NavClassComponent>(e);
    if (!nc || classCount == 0) return 0;
    return (nc->ClassId < classCount) ? nc->ClassId : uint8_t{0};
}
```

- [ ] **Step 5: Run to verify the test passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.`

- [ ] **Step 6: Add command dispatch in `ECSCommands.h`**

In `ApplyComponentCommand`, after the `NavConstrainedComponent` branch (data component → `Get<>()`):
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
In `CopyEntityComponents` (where `SunMarker`/`NavConstrainedComponent` are copied):
```cpp
        if (auto* c = world.GetComponent<NavClassComponent>(src)) world.AddComponent(dst, *c);
```

- [ ] **Step 7: (De)serialization in `ComponentSerialization.h`**

After the `NavConstrainedComponent` json pair:
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
Expected: both suites pass.

- [ ] **Step 11: Commit**

```bash
git add src/common/include/ECS.h src/common/include/NavClass.h src/common/include/ECSCommands.h src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_navmesh.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavClassComponent + ResolveNavClass + plumbing"
```

---

## Task 3: `NavMeshSystem` — N class slots

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
    EXPECT(large->GetStats().PolyCount <= small->GetStats().PolyCount);   // more erosion
    EXPECT(NavMeshSystem::Instance().Current(7) == nullptr);              // out-of-range slot
}
```
Register in `main()`: `T35_multiclass_build_distinct_erosion();`

NOTE `<=` is intentionally robust to coarse cells; if `PolyCount(large) < PolyCount(small)` holds reliably for this geometry after implementation, tighten to `<` with a comment. If equal (erosion difference < one cell), raise `Classes[1].AgentRadius` until divergence appears and document the value — do NOT weaken the test to mask a non-functioning multi-class build.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh`
Expected: FAIL — `Current(int)` no matching overload (current `Current()` is nullary).

- [ ] **Step 3: Update `NavMeshSystem.h`**

Add `#include <array>`. Change the accessor:
```cpp
    // Published mesh for a class slot. Out-of-range classId → nullptr.
    std::shared_ptr<const NavMesh> Current(uint8_t classId = 0) const;
```
Replace `std::shared_ptr<const NavMesh> m_Current;` with:
```cpp
    std::array<std::shared_ptr<const NavMesh>, kMaxNavClasses> m_Classes{};
    uint8_t m_ClassCount = 0;   // live class slots after the last Rebuild
```
Change `PublishNavMesh`:
```cpp
    void PublishNavMesh(uint8_t classId, std::shared_ptr<const NavMesh> mesh);
```

- [ ] **Step 4: Update `NavMeshSystem.cpp`**

Add `#include "NavClass.h"`. Rewrite `Rebuild` to loop classes:
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
        auto fresh = NavMesh::Build(soup, cfg, cfg.Classes[i]);
        if (!fresh) {
            SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null for class %u", (unsigned)i);
            continue;
        }
        PublishNavMesh(i, std::shared_ptr<const NavMesh>(std::move(fresh)));
        anyBuilt = true;
    }
    for (uint8_t i = liveCount; i < kMaxNavClasses; ++i) PublishNavMesh(i, {});   // clear stale slots

    if (!anyBuilt) {
        SM_WARN("NavMeshSystem::Rebuild: no class meshes built; keeping previous");
        return;
    }
    m_EntityToObstacle.clear();
    m_ClassCount = liveCount;

    // Disk auto-bake (single-mesh format) only for the single-class case.
    // Multi-class disk bake is the deferred follow-up (see spec non-goals).
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
`Current` / `PublishNavMesh`:
```cpp
std::shared_ptr<const NavMesh> NavMeshSystem::Current(uint8_t classId) const {
    if (classId >= kMaxNavClasses) return {};
    return std::atomic_load(&m_Classes[classId]);
}

void NavMeshSystem::PublishNavMesh(uint8_t classId, std::shared_ptr<const NavMesh> mesh) {
    if (classId >= kMaxNavClasses) return;
    std::atomic_store(&m_Classes[classId], std::move(mesh));
    m_NavVersion.fetch_add(1, std::memory_order_relaxed);
}
```
`Tick` + obstacle methods (`AddCylinderObstacle`/`AddBoxObstacle`/`RemoveObstacle`/`SaveCurrentToDisk`): replace every `std::atomic_load(&m_Current)` with `Current(0)` (slot 0 for now; Task 4 fans out). e.g.:
```cpp
void NavMeshSystem::Tick(float dt) {
    auto cur = Current(0);
    if (!cur) return;
    const_cast<NavMesh*>(cur.get())->Tick(dt);
}
```
`TryLoadFromDisk`: replace `PublishNavMesh(shared)` → `PublishNavMesh(0, shared)` and set `m_ClassCount = 1` on success.

- [ ] **Step 5: Build + run the navmesh suite**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (prior tests use `Current()`→slot 0; new T35 passes).

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

    EXPECT(NavMeshSystem::Instance().Current(0)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() == 2);
    EXPECT(NavMeshSystem::Instance().Current(1)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() == 2);

    SpawnCylinderObstacle(w, glm::vec3(0,0,0), 1.5f, 2.0f);
    NavObstacleSyncSystem sync;
    RunSync(w, sync);
    DrainTileCache(NavMeshSystem::Instance());

    EXPECT(NavMeshSystem::Instance().Current(0)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() > 2);
    EXPECT(NavMeshSystem::Instance().Current(1)->FindPath(glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0)).size() > 2);
}
```
Register in `main()`: `T36_obstacle_blocks_all_classes();`

NOTE this only passes once `Tick` drives every class's tilecache (Step 3) AND Add fans out. If the obstacle doesn't split the larger class's path, bump radii and document — do not weaken to `>= 2`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: T36 FAILS — class 1 stays straight (obstacle only carves slot 0, `Tick` only updates slot 0).

- [ ] **Step 3: Logical-id obstacle map + fan-out in `NavMeshSystem.{h,cpp}`**

In `NavMeshSystem.h` add members:
```cpp
    // Logical obstacle id → per-class NavMesh-level obstacle refs (0 = unused slot).
    std::unordered_map<uint32_t, std::array<uint32_t, kMaxNavClasses>> m_Obstacles;
    uint32_t m_NextObstacleId = 1;   // 0 reserved as "none"
```
In `NavMeshSystem.cpp` rewrite the obstacle methods to fan out across `[0, m_ClassCount)`:
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
Add `m_Obstacles.clear();` next to every `m_EntityToObstacle.clear();` (Rebuild empty-soup, Rebuild success, TryLoadFromDisk) — per-class refs are invalid against fresh tilecaches.

- [ ] **Step 4: Build + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (T36 passes; single-class obstacle tests T09-T13 pass — `m_ClassCount==1` → fan-out covers slot 0 exactly as before).

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
    EXPECT(!svc->HasMeshForClass(7));

    std::vector<glm::vec3> path;
    svc->FindPathForClass(1, glm::vec3(-4,0.5f,0), glm::vec3(4,0.5f,0), 50.0f, &path);
    EXPECT(path.size() >= 2);

    const glm::vec3 mv = svc->MoveAlongSurfaceForClass(0, glm::vec3(0,0.1f,0), glm::vec3(1.0f,0.1f,0));
    EXPECT(std::fabs(mv.x - 1.0f) < 0.3f);
}

static void T38_navservices_legacy_fns_map_to_class0() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg());   // single class 0
    const NavServices* svc = TestNavServices();
    EXPECT(svc->HasMesh());
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
Expected: FAIL — `…ForClass` not members of `NavServices`.

- [ ] **Step 3: Append fields to `NavServices.h`**

At the END of `struct NavServices` (append-only — after `MoveAlongSurface`):
```cpp
    // ---- Class-aware queries (multi-class navmesh) ----

    // Per-class variants of HasMesh/FindPath/MoveAlongSurface. classId indexes
    // NavMeshConfigComponent::Classes; out-of-range → no mesh / empty / passthrough.
    // The non-class HasMesh/FindPath/MoveAlongSurface above equal classId == 0.
    // GameThread only.
    bool      (*HasMeshForClass)(uint8_t classId);
    void      (*FindPathForClass)(uint8_t classId, const glm::vec3& start, const glm::vec3& end,
                                  float maxSearchRadius, std::vector<glm::vec3>* outPath);
    glm::vec3 (*MoveAlongSurfaceForClass)(uint8_t classId, const glm::vec3& start,
                                          const glm::vec3& desiredEnd);
```

- [ ] **Step 4: Forwarders + wiring in `NavServicesImpl.cpp`; old fns → class 0**

Add class-aware forwarders (place them BEFORE the legacy ones, or forward-declare):
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
Make the existing forwarders delegate to class 0:
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
Wire in `NavServicesImpl::Init`, after `out.MoveAlongSurface = ...`:
```cpp
    out.HasMeshForClass          = &ForwardHasMeshForClass;
    out.FindPathForClass         = &ForwardFindPathForClass;
    out.MoveAlongSurfaceForClass = &ForwardMoveAlongSurfaceForClass;
```

- [ ] **Step 5: Build + run**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_navmesh && ./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe`
Expected: `All navmesh tests passed.` (T37/T38 pass; base-feature T24/T31/T32 pass via class-0 delegation).

- [ ] **Step 6: Commit**

```bash
git add src/common/include/NavServices.h src/engine/src/navigation/NavServicesImpl.cpp tests/test_navmesh.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(nav): class-aware NavServices queries (…ForClass)"
```

---

## Task 6: Game systems route by class

Build + manual verified (no unit-test seam). Each system reads the live class count once per Update from the `NavMeshConfigComponent` singleton.

**Files:**
- Modify: `src/game/src/NavAgentSystem.h`
- Modify: `src/game/src/game.cpp` (`KinematicMovementSystem`)

- [ ] **Step 1: Route `NavAgentSystem` by class**

In `NavAgentSystem.h` add `#include "NavClass.h"`. After `const uint32_t navVer = ctx.Nav->NavVersion();` add:
```cpp
        uint8_t classCount = 1;
        if (const auto* navCfg = ctx.world.GetSingleton<NavMeshConfigComponent>())
            classCount = NavLiveClassCount(*navCfg);
```
Replace the `FindPath` call in the repath block:
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

Add `#include "NavClass.h"` near the top of `game.cpp`. In `KinematicMovementSystem::Update`, before the `Each` loop:
```cpp
        uint8_t classCount = 1;
        if (const auto* navCfg = ctx.world.GetSingleton<NavMeshConfigComponent>())
            classCount = NavLiveClassCount(*navCfg);
```
Replace:
```cpp
                const glm::vec3 clamped = ctx.Nav->MoveAlongSurface(transform->Position, end);
```
with:
```cpp
                const uint8_t navClass = ResolveNavClass(ctx.world, e, classCount);
                const glm::vec3 clamped = ctx.Nav->MoveAlongSurfaceForClass(navClass, transform->Position, end);
```
(Leave the planar-Y reconstruction + AABB layering unchanged.)

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
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Replace the per-agent drags with a class-list editor in `NavigationPanel.cpp`**

Remove the three drags `Agent Radius` / `Agent Height` / `Max Climb` (~lines 106-108; those fields no longer exist on `NavMeshConfigComponent` and won't compile). Keep `Cell Size`, `Cell Height`, `Max Slope`, `Tile Size`, `Max Obstacles`. Insert the class list after `Cell Height` and before `Max Slope`:
```cpp
    // --- Classes (per-radius bakes) ---
    ImGui::SeparatorText("Classes");
    for (uint8_t i = 0; i < edited.ClassCount; ++i) {
        ImGui::PushID(i);
        ImGui::Text("Class %u", (unsigned)i);
        changed |= ImGui::DragFloat("Radius", &edited.Classes[i].AgentRadius,   0.05f, 0.05f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Height", &edited.Classes[i].AgentHeight,   0.05f, 0.10f, 5.0f, "%.2f m");
        changed |= ImGui::DragFloat("Climb",  &edited.Classes[i].AgentMaxClimb, 0.05f, 0.00f, 2.0f, "%.2f m");
        ImGui::PopID();
        ImGui::Separator();
    }
    if (edited.ClassCount < kMaxNavClasses && ImGui::Button("Add Class")) {
        edited.Classes[edited.ClassCount] = edited.Classes[edited.ClassCount - 1];  // seed from previous
        edited.ClassCount++;
        changed = true;
    }
    ImGui::SameLine();
    if (edited.ClassCount > 1 && ImGui::Button("Remove Last Class")) {   // invariant: keep >= 1
        edited.ClassCount--;
        changed = true;
    }
```
Keep the existing `if (changed) PushSingletonEdit(ctx, world, edited, "nav config");`.

- [ ] **Step 2: Create `NavClassEditor.h`**

```cpp
#pragma once
#include "IComponentEditor.h"
#include "EditState.h"
class NavClassEditor final : public IComponentEditor {
    EditState<NavClassComponent> m_St;
public:
    const char* Label() const override { return "NavMesh Class Component"; }
    bool Has(const ECS& s, EntityId e) const override { return s.HasComponent<NavClassComponent>(e); }
    void AddDefault(const EditorContext& ctx, EntityId e) override;
    void Remove(const EditorContext& ctx, EntityId e) override;
    void DrawEditor(const EditorContext& ctx, EntityId e) override;
};
```
(Verify `EditState<T>` member-form usage against `NavMeshSourceEditor.h` — it uses `EditState<NavMeshSourceComponent> m_St;`. Match that exactly.)

- [ ] **Step 3: Create `NavClassEditor.cpp`**

```cpp
#include "NavClassEditor.h"
#include "EditorContext.h"
#include <imgui.h>
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "lib.h"

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
    const auto* c = m_St.Begin(ctx, e);
    if (!c) return;
    int classId = static_cast<int>(m_St.edit.ClassId);
    if (ImGui::DragInt("Class Id", &classId, 0.1f, 0, kMaxNavClasses - 1)) {
        m_St.edit.ClassId = static_cast<uint8_t>(classId);
        m_St.modified = true;
    }
    ImGui::TextDisabled("Index into NavMeshConfig classes; out-of-range falls back to 0.");
    m_St.Commit(ctx, e);
}
```
(Match the exact `EditState` API — `Begin`/`edit`/`modified`/`Commit` — against `NavMeshSourceEditor.cpp`.)

- [ ] **Step 4: Register + CMake**

In `EcsInspectorPanel.cpp`, include near the other inspector includes:
```cpp
#include "inspector/NavClassEditor.h"
```
Register after the `NavConstrainedEditor` push:
```cpp
    m_Editors.push_back(std::make_unique<NavClassEditor>());
```
Add to `src/editor/CMakeLists.txt` with the other inspector sources:
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

## Task 8: world.json schema fix + end-to-end manual verification

**Files:**
- Modify (NOT committed — runtime data): the `world.json` next to the editor binary actually run. The user identified `build/relwithdebinfo/bin/RelWithDebInfo/world.json`; also check `out/build/msvc-win64-vs2026-community/bin/Debug/world.json` if that's the one the built editor loads.

- [ ] **Step 1: Full rebuild + restart editor**

Run: `cmake --build --preset msvc-win64-vs2026-community` (builds `ecs`, `engine`, `game`, `editor`). Then **fully restart `editor.exe`** (ECS struct set changed).

- [ ] **Step 2: Hand-update `world.json` to the new schema**

Open the `world.json` the editor loads. In its top-level `"Environment"."NavMeshConfig"` block:
- Remove the obsolete flat keys `"AgentRadius"`, `"AgentHeight"`, `"AgentMaxClimb"` (now ignored).
- Add a `"Classes"` array, e.g. two classes for testing:
```json
"NavMeshConfig": {
    "CellSize": 0.3, "CellHeight": 0.2, "AgentMaxSlope": 45.0,
    "TileSize": 32.0, "MaxObstacles": 128,
    "Classes": [
        { "AgentRadius": 0.5, "AgentHeight": 1.8, "AgentMaxClimb": 0.4 },
        { "AgentRadius": 1.2, "AgentHeight": 2.2, "AgentMaxClimb": 0.4 }
    ]
}
```
- Optionally add `"NavClassComponent": { "ClassId": 0 }` to the player entity (the one with `NavConstrainedComponent`).

(If you'd rather not hand-edit: loading without `Classes` seeds one default class, and you can author classes live in the Navigation panel, then Save World — which writes the new schema. Either path is fine.)

- [ ] **Step 3: Verify in Play mode**

- Author/confirm two classes in the Navigation panel; Rebuild NavMesh.
- Player (`NavConstrained` + `NavClass` 0) and a larger test entity (`NavConstrained` + `NavClass` 1): the Class 1 entity keeps **farther** from walls/obstacles than the Class 0 entity, and neither body pokes through geometry the way the single-class build did. A same-size monster on Class 0 matches the player.
- Removing `NavClass` → defaults to class 0. A config with one class behaves exactly like the base feature.

- [ ] **Step 4: Persistence**

Save World → reload/restart: `NavClassComponent` + the class list persist (new schema round-trips). Optionally `runtime.exe` honors per-class constraint.

- [ ] **Step 5 (if a code fix was needed): commit it.**

```bash
git add -A
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "fix(nav): <describe manual-verification fix>"
```

---

## Self-Review (completed during authoring)

- **Spec coverage:** §1 class model + Build(cls) + serialization no-migration (Task 1); §2 NavClassComponent + ResolveNavClass (Task 2); §3 N meshes (Task 3); §4 obstacle fan-out (Task 4); §5 …ForClass (Task 5); §6 editor (Task 7); §7 error handling (clamps/warns across Tasks 1-5); §8 disk-bake short-circuit + world.json fix (Tasks 3 + 8); testing (Tasks 1-5 unit, Task 8 manual). Game routing (Task 6). All mapped.
- **Design Y consistency:** flat agent fields removed everywhere; `NavMesh::Build(soup, cfg, cls)`; no `ConfigForClass`; `ClassCount >= 1` invariant; from_json seeds one default class when `Classes` absent.
- **Type consistency:** `NavLiveClassCount`, `ResolveNavClass`, `NavClassConfig{AgentRadius,AgentHeight,AgentMaxClimb}`, `NavClassComponent{ClassId}`, `Build(soup,cfg,cls)`, `Current(uint8_t)`, `PublishNavMesh(uint8_t,…)`, `…ForClass` signatures identical across all referencing tasks.
- **Placeholders:** none — complete code + exact commands/expected output per step.
- **Calibration risk flagged** (T35 PolyCount divergence, T36 fan-out reroute) with explicit instructions to investigate real values and document, not weaken assertions.
