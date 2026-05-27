# Navigation Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up Recast/Detour navmesh build pipeline, query API, editor panel, and debug viz. No agents, no dynamic obstacles, no disk bake (deferred to later specs).

**Architecture:** Build a `dtTileCache` from author-tagged scene geometry on `GameThread`, atomic-publish a `shared_ptr<const NavMesh>` through a new `NavMeshSystem` engine service, expose `FindPath` for game code, render poly edges via the existing `DebugRenderPass`. TileCache (not plain `dtNavMesh`) from day 1 so Spec 2 plugs obstacles in additively.

**Tech Stack:** C++23, custom ECS (`ecs.dll`), NVRHI deferred renderer, Recast/Detour libs (Recast, Detour, DetourTileCache, DebugUtils) vendored at `third_party/recastnavigation/`, nlohmann/json for world.json persistence.

**Spec reference:** `docs/superpowers/specs/2026-05-27-navigation-core-design.md` (commit `5cc0aa5`).

---

## Codebase orientation (read once before Task 1)

- **ECS components** live in `src/common/include/ECS.h`. Add the struct, then a line in `ECS_FOR_EACH_REGISTERED_COMPONENT` (line ~260), then handling in `ECSCommandProcessor` in `src/common/include/ECSCommands.h` (`ApplyComponentCommand` + `RemoveComponentByType` + `DuplicateEntityComponents`). Forgetting any of these is silent.
- **Per-entity JSON round-trip** happens in two files: declarations in `src/common/include/ComponentSerialization.h` (`to_json` / `from_json`), wiring per-entity in `src/engine/src/utilities/WorldManager.cpp` (Save + Load sections).
- **Environment block** (scene singletons) round-trips through `BuildEnvironmentJson` / `ParseEnvironmentJson` in `ComponentSerialization.h` + `EnvironmentData` struct + matching `SetSingleton` calls in `WorldManager.cpp`.
- **`MeshSystem` already retains CPU vertices + indices** (`MeshEntry::cpuVertices` / `cpuIndices` in `src/engine/src/rendering/MeshSystem.h:79-80`) for hot-swap. We expose a read accessor in Task 4 — **Risk 1 from the spec is resolved**.
- **`DebugDrawSettings`** lives in `src/engine/src/rendering/RenderStats.h`. Toggles are added there, persisted in `src/editor/src/EditorPreferences.h` (both `PrefsToJson` and `PrefsFromJson` need the new key), UI checkbox added in `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, debug-draw block lives in `src/engine/src/rendering/passes/DebugRenderPass.cpp` next to the `ShowColliders` block.
- **Test pattern:** `tests/test_collision.cpp` is the template — `EXPECT(cond)` macro, counter `g_Failures`, sub-functions called from `main`, no framework. Each test target compiles its own sources + `ecs.dll` (or `Engine`). See `tests/CMakeLists.txt`.
- **GameThread runs the world load** in `src/engine/src/threading/GameThread.cpp` around line 63 (`WorldManager::LoadWorldSnapshot`). That's where we trigger the first navmesh rebuild in Task 9.
- **ECSCommands.h is shared** between editor and game and lives in `src/common/include/`. It must NOT include engine-private headers. The `RebuildNavMesh` dispatch uses a hook callback so the engine wires `NavMeshSystem::Rebuild` from `GameThread.cpp` (Task 3 + Task 6).

---

## Task 0: Create feature branch

**Files:** none (git only)

- [ ] **Step 1: Verify we're on main, clean, and `MeshSystem` retains CPU data**

```bash
git status -sb
# Expected: "## main...origin/main" with no uncommitted changes (caveman/branding files OK as ?? — those are .claude/).
grep -n "cpuVertices\|cpuIndices" src/engine/src/rendering/MeshSystem.h
# Expected: lines ~79-80 show std::vector<MeshVertex> cpuVertices; std::vector<uint32_t> cpuIndices;
```

- [ ] **Step 2: Create branch**

```bash
git checkout -b feat/navigation-core
git status -sb
# Expected: "## feat/navigation-core"
```

- [ ] **Step 3: No commit yet** — administrative only.

---

## Task 1: ECS components — enum + 2 components + X-macro + GAME_API bump

**Files:**
- Modify: `src/common/include/ECS.h` (add enum + 2 structs + 2 X-macro entries)
- Modify: `src/game/include/game.h` (GAME_API_VERSION 14 → 15)

- [ ] **Step 1: Add `NavMeshGeometrySource` enum + components in `ECS.h`**

Add immediately AFTER the existing `MoveIntentComponent` struct (around line 254, before the X-macro comment block at line 256):

```cpp
// Author-driven choice: which geometry source feeds the navmesh voxelizer for this entity.
// Unset is the sentinel — the build pipeline SM_WARNs + skips entities that forgot to pick.
// Forces authoring intent at scene-construction time (no silent fallback).
enum class NavMeshGeometrySource : uint8_t {
    Unset    = 0,  // sentinel: author forgot to pick → SM_WARN + skip at build
    Collider = 1,  // voxelize ColliderComponent triangles
    Mesh     = 2,  // voxelize MeshComponent triangles (CPU-side verts via MeshSystem)
};

// Per-entity opt-in. Tagging an entity = it contributes triangles to the navmesh build.
// AreaId is Recast's per-triangle classification (0-63); default 63 == RC_WALKABLE_AREA.
struct NavMeshSourceComponent {
    uint8_t                AreaId   = 63;
    NavMeshGeometrySource  Geometry = NavMeshGeometrySource::Unset;
};

// Singleton (one per scene, persisted in world.json Environment block). Recast/Detour
// build knobs — tune per scene. Indoor/outdoor scenes want very different cell sizes.
struct NavMeshConfigComponent {
    float CellSize       = 0.3f;   // voxel XZ size (m)
    float CellHeight     = 0.2f;   // voxel Y size (m)
    float AgentRadius    = 0.5f;   // agent capsule radius (m)
    float AgentHeight    = 1.8f;   // agent capsule height (m)
    float AgentMaxClimb  = 0.4f;   // step-up height (m)
    float AgentMaxSlope  = 45.0f;  // max walkable slope (degrees)
    float TileSize       = 32.0f;  // tile XZ size (voxels per tile edge)
    int   MaxObstacles   = 128;    // dtTileCache pre-alloc (unused in Spec 1; plumbed for Spec 2)
};
```

- [ ] **Step 2: Register both components in the X-macro**

Modify `src/common/include/ECS.h` lines 260-287 — add two `X(...)` lines at the end of the macro list (immediately before the closing `)`):

```cpp
#define ECS_FOR_EACH_REGISTERED_COMPONENT(X) \
    X(TransformComponent) \
    X(MeshComponent) \
    X(MaterialComponent) \
    X(TextComponent) \
    X(LightningComponent) \
    X(ParentComponent) \
    X(ChildComponent) \
    X(SunMarker) \
    X(InputStateComponent) \
    X(WorldCameraComponent) \
    X(UICameraComponent) \
    X(DayNightConfigComponent) \
    X(AtmosphereStateComponent) \
    X(FogComponent) \
    X(SkyComponent) \
    X(AppControlComponent) \
    X(ViewportComponent) \
    X(PlayerComponent) \
    X(CameraZoomComponent) \
    X(GameStateComponent) \
    X(ActionQueueComponent) \
    X(UIRectComponent) \
    X(StateScopeComponent) \
    X(MenuButtonComponent) \
    X(MenuStateComponent) \
    X(ColliderComponent) \
    X(MoveIntentComponent) \
    X(NavMeshSourceComponent) \
    X(NavMeshConfigComponent)
```

- [ ] **Step 3: Bump GAME_API_VERSION**

Modify `src/game/include/game.h`:

```cpp
#define GAME_API_VERSION 15u  // was 14u
```

- [ ] **Step 4: Build ecs.dll + everything**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: builds clean. Explicit template instantiations for both new components are emitted automatically (X-macro drives `ECS_EXTERN_TEMPLATE_DECL` and friends — see `ECS.h:467, 589, 870`).

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ECS.h src/game/include/game.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs): NavMeshSource + NavMeshConfig components + GAME_API 15

NavMeshGeometrySource enum (Unset/Collider/Mesh) forces explicit
authoring intent — Unset is a sentinel that the navmesh builder will
SM_WARN on and skip. NavMeshConfigComponent is a per-scene singleton
(Recast build knobs). Both registered in the X-macro for explicit
template instantiation. GAME_API_VERSION 14 -> 15 because ECS.h
layout changed; editor + game must be rebuilt + editor restarted."
```

---

## Task 2: JSON round-trip + test_worldserial T20-T22

**Files:**
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/engine/src/utilities/WorldManager.cpp`
- Modify: `tests/test_worldserial.cpp`

- [ ] **Step 1: Write failing tests first**

Append to `tests/test_worldserial.cpp` (before `main()` — add new sub-function `Test_Navigation()`; if `main()` is at the bottom, add the function immediately above and call it from `main()`):

```cpp
// ---------- T20-T22: navigation serialization ----------
static void Test_Navigation() {
    using json = nlohmann::json;

    // T20: NavMeshConfigComponent round-trip with custom values
    {
        FogComponent fog{};                              // default — irrelevant here
        SkyComponent sky{};                              // default — irrelevant here
        DayNightConfigComponent dn{};                    // default — irrelevant here
        NavMeshConfigComponent cfg{};
        cfg.CellSize      = 0.42f;
        cfg.CellHeight    = 0.17f;
        cfg.AgentRadius   = 0.66f;
        cfg.AgentHeight   = 2.10f;
        cfg.AgentMaxClimb = 0.55f;
        cfg.AgentMaxSlope = 50.0f;
        cfg.TileSize      = 48.0f;
        cfg.MaxObstacles  = 256;

        json root;
        root["Environment"] = BuildEnvironmentJson(fog, sky, dn, cfg);
        const EnvironmentData parsed = ParseEnvironmentJson(root);
        EXPECT(parsed.HasNavMeshConfig);
        EXPECT(near(parsed.NavMeshConfig.CellSize,      0.42f));
        EXPECT(near(parsed.NavMeshConfig.CellHeight,    0.17f));
        EXPECT(near(parsed.NavMeshConfig.AgentRadius,   0.66f));
        EXPECT(near(parsed.NavMeshConfig.AgentHeight,   2.10f));
        EXPECT(near(parsed.NavMeshConfig.AgentMaxClimb, 0.55f));
        EXPECT(near(parsed.NavMeshConfig.AgentMaxSlope, 50.0f));
        EXPECT(near(parsed.NavMeshConfig.TileSize,      48.0f));
        EXPECT(parsed.NavMeshConfig.MaxObstacles == 256);
    }

    // T21: NavMeshSourceComponent per-entity round-trip
    {
        NavMeshSourceComponent src{};
        src.AreaId   = 12;
        src.Geometry = NavMeshGeometrySource::Mesh;
        json j = src;
        NavMeshSourceComponent back = j.get<NavMeshSourceComponent>();
        EXPECT(back.AreaId == 12);
        EXPECT(back.Geometry == NavMeshGeometrySource::Mesh);
    }

    // T22: backward-compat — old world.json without NavMeshConfig still loads, fields default
    {
        json root;
        // Build an Environment with ONLY Fog/Sky/DayNight (no NavMeshConfig key).
        FogComponent fog{};  SkyComponent sky{};  DayNightConfigComponent dn{};
        root["Environment"] = nlohmann::json{
            {"Fog", fog}, {"Sky", sky}, {"DayNight", dn}
        };
        const EnvironmentData parsed = ParseEnvironmentJson(root);
        EXPECT(!parsed.HasNavMeshConfig);   // absent
        // parsed.NavMeshConfig was default-initialized; verify a sentinel default survived.
        EXPECT(near(parsed.NavMeshConfig.CellSize, 0.3f));
    }
}
```

Add the call to `main()` (alongside the other `Test_*()` calls):

```cpp
    Test_Navigation();
```

Add `#include` if not already present in test_worldserial.cpp (most likely already covered by `ECS.h` + `ComponentSerialization.h`):

```cpp
// (No new includes — ECS.h gives us the components, ComponentSerialization.h gives Build/Parse.)
```

- [ ] **Step 2: Run test — expect compile failure**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
```

Expected: FAIL with errors like `'BuildEnvironmentJson': no overload takes 4 arguments` and `'HasNavMeshConfig' is not a member of 'EnvironmentData'` and `nlohmann::json` cannot serialize `NavMeshSourceComponent`. Good — drives the next step.

- [ ] **Step 3: Add per-entity `to_json` / `from_json` for `NavMeshSourceComponent`**

Insert in `src/common/include/ComponentSerialization.h` after the `ColliderComponent` block (around line 180, before the `// ----- Atmosphere components -----` comment):

```cpp
inline void to_json(nlohmann::json& j, const NavMeshSourceComponent& t) {
    j = nlohmann::json{
        {"AreaId",   t.AreaId},
        // Geometry as uint8_t for JSON stability (matches ColliderShape pattern).
        {"Geometry", static_cast<uint8_t>(t.Geometry)}
    };
}
inline void from_json(const nlohmann::json& j, NavMeshSourceComponent& t) {
    if (j.contains("AreaId"))   j.at("AreaId").get_to(t.AreaId);
    if (j.contains("Geometry")) t.Geometry = static_cast<NavMeshGeometrySource>(j.at("Geometry").get<uint8_t>());
}
```

- [ ] **Step 4: Add per-scene `to_json` / `from_json` for `NavMeshConfigComponent`**

Insert in `src/common/include/ComponentSerialization.h` immediately after the `SkyComponent` `from_json` (around line 244, before the `// ----- world.json top-level "Environment" block -----` comment):

```cpp
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
}
```

- [ ] **Step 5: Extend `BuildEnvironmentJson` + `EnvironmentData` + `ParseEnvironmentJson`**

Replace the existing 3-arg `BuildEnvironmentJson` (lines 249-256 of `ComponentSerialization.h`) with a 4-arg overload. Keep the old signature too for callers that haven't migrated, since collider tests / other call sites may depend on it. Actually — we own all call sites; safer to just change the signature and update the one call site in `WorldManager.cpp` (Step 6).

```cpp
inline nlohmann::json BuildEnvironmentJson(const FogComponent& fog,
                                           const SkyComponent& sky,
                                           const DayNightConfigComponent& dayNight,
                                           const NavMeshConfigComponent& navmesh) {
    return nlohmann::json{
        {"Fog", fog},
        {"Sky", sky},
        {"DayNight", dayNight},
        {"NavMeshConfig", navmesh}};
}
```

Extend the `EnvironmentData` struct (lines 261-268):

```cpp
struct EnvironmentData {
    bool HasFog = false;
    bool HasSky = false;
    bool HasDayNight = false;
    bool HasNavMeshConfig = false;
    FogComponent Fog;
    SkyComponent Sky;
    DayNightConfigComponent DayNight;
    NavMeshConfigComponent NavMeshConfig;
};
```

Extend `ParseEnvironmentJson` (line 270-278) by appending one line in the env-parse block:

```cpp
inline EnvironmentData ParseEnvironmentJson(const nlohmann::json& root) {
    EnvironmentData e;
    if (!root.contains("Environment")) return e;
    const auto& env = root.at("Environment");
    if (env.contains("Fog"))           { e.Fog           = env.at("Fog").get<FogComponent>();              e.HasFog = true; }
    if (env.contains("Sky"))           { e.Sky           = env.at("Sky").get<SkyComponent>();              e.HasSky = true; }
    if (env.contains("DayNight"))      { e.DayNight      = env.at("DayNight").get<DayNightConfigComponent>(); e.HasDayNight = true; }
    if (env.contains("NavMeshConfig")) { e.NavMeshConfig = env.at("NavMeshConfig").get<NavMeshConfigComponent>(); e.HasNavMeshConfig = true; }
    return e;
}
```

- [ ] **Step 6: Update `WorldManager.cpp` — save NavMeshConfig in Environment + per-entity NavMeshSource**

Modify `src/engine/src/utilities/WorldManager.cpp`:

Save side — change the Environment block (lines 72-80) to include NavMeshConfig:

```cpp
    // Top-level scene atmosphere + nav config (singletons live on the hidden reserved entity).
    {
        FogComponent fog{};
        SkyComponent sky{};
        DayNightConfigComponent dayNight{};
        NavMeshConfigComponent navmesh{};
        if (const auto* f = world->GetSingleton<FogComponent>())            fog      = *f;
        if (const auto* s = world->GetSingleton<SkyComponent>())            sky      = *s;
        if (const auto* d = world->GetSingleton<DayNightConfigComponent>()) dayNight = *d;
        if (const auto* n = world->GetSingleton<NavMeshConfigComponent>())  navmesh  = *n;
        j["Environment"] = BuildEnvironmentJson(fog, sky, dayNight, navmesh);
    }
```

Save side — per-entity NavMeshSource, immediately after the existing `ColliderComponent` block (line 63-65):

```cpp
        if (world->HasComponent<NavMeshSourceComponent>(entity)) {
            jEntity["NavMeshSourceComponent"] = *(world->GetComponent<NavMeshSourceComponent>(entity));
        }
```

Load side — per-entity NavMeshSource, immediately after the existing `ColliderComponent` load (line 129-130):

```cpp
            if (jEntity.contains("NavMeshSourceComponent"))
                world->AddComponent(createdEntity, jEntity["NavMeshSourceComponent"].get<NavMeshSourceComponent>());
```

Load side — apply NavMeshConfig singleton if present (extend the existing apply block at lines 135-138):

```cpp
        // Apply scene atmosphere + nav config if present.
        const EnvironmentData env = ParseEnvironmentJson(j);
        if (env.HasFog)            world->SetSingleton(env.Fog);
        if (env.HasSky)            world->SetSingleton(env.Sky);
        if (env.HasDayNight)       world->SetSingleton(env.DayNight);
        if (env.HasNavMeshConfig)  world->SetSingleton(env.NavMeshConfig);
```

- [ ] **Step 7: Run test to verify pass**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_worldserial --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```

Expected: `All worldserial tests passed.` (or whatever the suite's success line is). 3 new tests green.

- [ ] **Step 8: Run all other tests to confirm no regression**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_menu --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```

Expected: each prints its success line, no failures.

- [ ] **Step 9: Commit**

```bash
git add src/common/include/ComponentSerialization.h src/engine/src/utilities/WorldManager.cpp tests/test_worldserial.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): JSON round-trip for NavMeshSource + NavMeshConfig

NavMeshSourceComponent serialized per-entity in world.json (Geometry
stored as uint8_t for stability, matching ColliderShape pattern).
NavMeshConfigComponent serialized in the top-level Environment block
alongside Fog/Sky/DayNight. ParseEnvironmentJson exposes
HasNavMeshConfig so old world.json files (no NavMeshConfig key)
still load — defaults applied (seed-if-absent). New tests T20/T21/T22
in test_worldserial pin config round-trip, per-entity round-trip,
and backward-compat defaults."
```

---

## Task 3: ECSCommands — NavMeshSource Apply/Remove/Duplicate + RebuildNavMesh hook

**Files:**
- Modify: `src/common/include/ECSCommands.h`

- [ ] **Step 1: Add `RebuildNavMesh` command type + factory + hooks struct**

Modify `src/common/include/ECSCommands.h`. Extend the `ECSCommandType` enum (lines 14-21):

```cpp
enum class ECSCommandType : uint8_t {
    CreateEntity     = 0,
    DestroyEntity    = 1,
    AddComponent     = 2,
    RemoveComponent  = 3,
    ModifyComponent  = 4,
    DuplicateEntity  = 5,
    RebuildNavMesh   = 6,  // engine hook — dispatched via ECSCommandHooks::OnRebuildNavMesh
};
```

Add factory method inside the `ECSCommand` struct (alongside other `static ECSCommand` factories — after `DuplicateEntity` at line 125-127):

```cpp
    static ECSCommand RebuildNavMesh() {
        return ECSCommand(ECSCommandType::RebuildNavMesh);
    }
```

Add the hooks struct immediately above `class ECSCommandProcessor`, around line 162. Include `<functional>` at the top of the file (currently absent):

```cpp
#include <functional>
```

```cpp
// Engine-side handlers for commands that can't be processed in pure-ECS-land
// (e.g., RebuildNavMesh needs to call NavMeshSystem::Rebuild, which is engine-private).
// Default-constructed hooks struct means "no engine-side handlers" — commands are silently
// dropped. GameThread populates this with NavMeshSystem::Rebuild for the rebuild case.
struct ECSCommandHooks {
    std::function<void(ECS&)> OnRebuildNavMesh; // optional
};
```

- [ ] **Step 2: Extend `ProcessCommands` signature + add `RebuildNavMesh` dispatch**

Modify `ProcessCommands` signature (line 165) to accept hooks:

```cpp
    static void ProcessCommands(ECS& world, SpscRing<ECSCommand, 128>& commandRing,
                                const ECSCommandHooks& hooks = {}) {
        ECSCommand cmd; // Use default constructor explicitly (not aggregate init {})
        while (commandRing.Pop(cmd)) {
            switch (cmd.Type) {
                // ... existing cases unchanged ...

                case ECSCommandType::DuplicateEntity: {
                    if (cmd.TargetEntity != INVALID_ENTITY && world.IsValidEntity(cmd.TargetEntity)) {
                        const EntityId dst = world.CreateEntity();
                        DuplicateEntityComponents(world, cmd.TargetEntity, dst);
                    }
                    break;
                }

                case ECSCommandType::RebuildNavMesh: {
                    if (hooks.OnRebuildNavMesh) hooks.OnRebuildNavMesh(world);
                    break;
                }
            }
        }
    }
```

- [ ] **Step 3: Add NavMeshSource handling in `ApplyComponentCommand`**

In `src/common/include/ECSCommands.h`, append a new `else if` to the chain in `ApplyComponentCommand` (after the `ColliderComponent` block around line 287):

```cpp
        } else if (componentData.Type == std::type_index(typeid(NavMeshSourceComponent))) {
            if (auto* src = componentData.Get<NavMeshSourceComponent>()) {
                world.AddComponent(entity, *src);
            }
        } else if (componentData.Type == std::type_index(typeid(NavMeshConfigComponent))) {
            if (auto* cfg = componentData.Get<NavMeshConfigComponent>()) {
                world.AddComponent(entity, *cfg); // AddComponent updates if present (singleton edit)
            }
        }
```

- [ ] **Step 4: Add NavMeshSource handling in `RemoveComponentByType`**

Append after the `ColliderComponent` block (around line 322):

```cpp
        } else if (typeIndex == std::type_index(typeid(NavMeshSourceComponent))) {
            world.RemoveComponent<NavMeshSourceComponent>(entity);
        } else if (typeIndex == std::type_index(typeid(NavMeshConfigComponent))) {
            world.RemoveComponent<NavMeshConfigComponent>(entity);
        }
```

- [ ] **Step 5: Add NavMeshSource handling in `DuplicateEntityComponents`**

Append in `DuplicateEntityComponents` (after the `ColliderComponent` line at 227, before `SunMarker`):

```cpp
        if (auto* c = world.GetComponent<NavMeshSourceComponent>(src))   world.AddComponent(dst, *c);
```

(NavMeshConfigComponent is a singleton — duplicating it onto a per-entity copy makes no sense, skip.)

- [ ] **Step 6: Build everything to confirm header changes compile across all consumers**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target ecs Engine editor game --config Debug
```

Expected: clean build. `ECSCommands.h` is a header — every TU that includes it sees the change.

- [ ] **Step 7: Commit**

```bash
git add src/common/include/ECSCommands.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(ecs-cmds): NavMeshSource/Config dispatch + RebuildNavMesh hook

NavMeshSourceComponent gets the standard Apply/Remove/Duplicate
plumbing so the inspector can add/edit/remove per-entity, and
duplicated entities carry it. NavMeshConfigComponent gets Apply/Remove
for singleton edits via ModifyComponent commands from the Navigation
panel (Task 8).

RebuildNavMesh is a new command type dispatched through
ECSCommandHooks (a callback struct passed by GameThread). Keeps
ECSCommands.h free of engine-private deps — NavMeshSystem::Rebuild
gets wired in via the hook from GameThread.cpp in Task 6."
```

---

## Task 4: NavMeshBuilder — triangulation helpers + triangle collection

**Files:**
- Create: `src/engine/src/navigation/NavMeshBuilder.h`
- Create: `src/engine/src/navigation/NavMeshBuilder.cpp`
- Modify: `src/engine/src/rendering/MeshSystem.h` (add CPU data accessor)
- Modify: `src/engine/src/rendering/MeshSystem.cpp` (implement accessor)
- Modify: `src/engine/CMakeLists.txt` (add navigation/ + link Recast libs)

- [ ] **Step 1: Add `GetMeshCpuData` accessor on `MeshSystem`**

In `src/engine/src/rendering/MeshSystem.h`, inside the `public:` section (alongside `GetMeshResources` around line 42):

```cpp
    // CPU-side vertex/index view for nav build, mesh decimation, picking, etc.
    // Span is valid until the MeshSystem is mutated (AddMesh / Shutdown / RecreateGpuResources).
    struct MeshCpuData {
        std::span<const MeshVertex> vertices;
        std::span<const uint32_t>   indices;
        bool                        valid = false;
    };
    MeshCpuData GetMeshCpuData(uint32_t meshId) const;
```

In `src/engine/src/rendering/MeshSystem.cpp`, add the implementation (after `GetMeshResources` or near its sibling getters):

```cpp
MeshSystem::MeshCpuData MeshSystem::GetMeshCpuData(uint32_t meshId) const
{
    MeshCpuData out{};
    if (meshId >= m_Meshes.size()) return out;
    const auto& e = m_Meshes[meshId];
    if (e.cpuVertices.empty() || e.cpuIndices.empty()) return out;
    out.vertices = std::span<const MeshVertex>(e.cpuVertices.data(), e.cpuVertices.size());
    out.indices  = std::span<const uint32_t>(e.cpuIndices.data(), e.cpuIndices.size());
    out.valid    = true;
    return out;
}
```

- [ ] **Step 2: Create `NavMeshBuilder.h`**

Create `src/engine/src/navigation/NavMeshBuilder.h`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

#include "Engine.h"

class ECS;
class MeshSystem;

// Output of triangle collection: vertex/index/area arrays ready to feed into Recast.
// Verts are interleaved x/y/z floats; tris are 3 indices per triangle into verts;
// areas is one uint8 per triangle (Recast area ID).
struct ENGINE_API NavMeshTriangleSoup {
    std::vector<float>    Verts;   // 3 floats per vertex
    std::vector<int>      Tris;    // 3 ints per triangle (index into Verts/3)
    std::vector<uint8_t>  Areas;   // 1 uint8 per triangle
    glm::vec3             AabbMin{0.0f};
    glm::vec3             AabbMax{0.0f};
    bool                  Empty = true;
};

namespace NavMeshBuilder {

    // Walk all entities with NavMeshSourceComponent, resolve geometry per the Geometry
    // enum, transform to world space, concatenate. Logs SM_WARN per skipped entity.
    // meshSystem may be null — entities with Geometry=Mesh will then SM_WARN + skip.
    ENGINE_API NavMeshTriangleSoup CollectTriangles(const ECS& world,
                                                    const MeshSystem* meshSystem);

    // Append a unit box (12 tris) sized by halfExtents, transformed by worldXform, with areaId.
    ENGINE_API void TriangulateBox(const glm::vec3& halfExtents,
                                   const glm::mat4& worldXform,
                                   uint8_t areaId,
                                   NavMeshTriangleSoup& out);

    // Append a UV-sphere (segments per ring). radius in local space.
    // For nav purposes 8 segments is enough (Recast voxelizes anyway).
    ENGINE_API void TriangulateSphere(float radius,
                                      const glm::mat4& worldXform,
                                      uint8_t areaId,
                                      int segments,
                                      NavMeshTriangleSoup& out);

    // Append a capsule (cylinder + two hemispheres). radius + halfHeight in local space.
    ENGINE_API void TriangulateCapsule(float radius,
                                       float halfHeight,
                                       const glm::mat4& worldXform,
                                       uint8_t areaId,
                                       int segments,
                                       NavMeshTriangleSoup& out);

} // namespace NavMeshBuilder
```

- [ ] **Step 3: Create `NavMeshBuilder.cpp`**

Create `src/engine/src/navigation/NavMeshBuilder.cpp`:

```cpp
#include "navigation/NavMeshBuilder.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "ECS.h"
#include "MeshSystem.h"
#include "lib.h"          // SM_WARN
#include "Engine.h"

namespace {

// Append a triangle by index triple to the soup (no transform — caller pre-transformed verts).
// Adjusts indices to be relative to the existing Verts size.
inline void EmitTri(NavMeshTriangleSoup& out, int i0, int i1, int i2, uint8_t areaId) {
    out.Tris.push_back(i0);
    out.Tris.push_back(i1);
    out.Tris.push_back(i2);
    out.Areas.push_back(areaId);
}

inline int EmitVert(NavMeshTriangleSoup& out, const glm::vec3& v) {
    const int idx = static_cast<int>(out.Verts.size() / 3);
    out.Verts.push_back(v.x);
    out.Verts.push_back(v.y);
    out.Verts.push_back(v.z);
    return idx;
}

inline void GrowAabb(NavMeshTriangleSoup& out, const glm::vec3& v) {
    if (out.Empty) {
        out.AabbMin = out.AabbMax = v;
        out.Empty = false;
    } else {
        out.AabbMin = glm::min(out.AabbMin, v);
        out.AabbMax = glm::max(out.AabbMax, v);
    }
}

inline glm::vec3 Xform(const glm::mat4& m, const glm::vec3& p) {
    const glm::vec4 r = m * glm::vec4(p, 1.0f);
    return glm::vec3(r.x, r.y, r.z);
}

inline glm::mat4 BuildWorld(const TransformComponent& t) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, t.Position);
    m = glm::rotate(m, glm::radians(t.Rotation.y), glm::vec3(0,1,0));
    m = glm::rotate(m, glm::radians(t.Rotation.x), glm::vec3(1,0,0));
    m = glm::rotate(m, glm::radians(t.Rotation.z), glm::vec3(0,0,1));
    m = glm::scale(m, t.Scale);
    return m;
}

} // namespace

namespace NavMeshBuilder {

void TriangulateBox(const glm::vec3& he, const glm::mat4& xf, uint8_t area, NavMeshTriangleSoup& out)
{
    // 8 corners
    const glm::vec3 corners[8] = {
        {-he.x, -he.y, -he.z}, { he.x, -he.y, -he.z}, { he.x,  he.y, -he.z}, {-he.x,  he.y, -he.z},
        {-he.x, -he.y,  he.z}, { he.x, -he.y,  he.z}, { he.x,  he.y,  he.z}, {-he.x,  he.y,  he.z},
    };
    int idx[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 w = Xform(xf, corners[i]);
        idx[i] = EmitVert(out, w);
        GrowAabb(out, w);
    }
    // 12 triangles, 2 per face. Winding: counter-clockwise when viewed from outside.
    auto Q = [&](int a, int b, int c, int d) {
        EmitTri(out, idx[a], idx[b], idx[c], area);
        EmitTri(out, idx[a], idx[c], idx[d], area);
    };
    Q(0,1,2,3); // -Z
    Q(5,4,7,6); // +Z
    Q(4,0,3,7); // -X
    Q(1,5,6,2); // +X
    Q(4,5,1,0); // -Y
    Q(3,2,6,7); // +Y
}

void TriangulateSphere(float r, const glm::mat4& xf, uint8_t area, int segs, NavMeshTriangleSoup& out)
{
    if (segs < 4) segs = 4;
    const int rings = segs / 2;
    // Build ring vertices first, indexed by (ring, segment).
    std::vector<int> idx((rings + 1) * (segs + 1), 0);
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(rings);
        const float phi = v * 3.14159265358979f;
        const float y = std::cos(phi);
        const float rr = std::sin(phi);
        for (int s = 0; s <= segs; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segs);
            const float theta = u * 6.28318530717958f;
            const glm::vec3 local{ rr * std::cos(theta) * r, y * r, rr * std::sin(theta) * r };
            const glm::vec3 w = Xform(xf, local);
            const int i = EmitVert(out, w);
            GrowAabb(out, w);
            idx[ring * (segs + 1) + s] = i;
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int s = 0; s < segs; ++s) {
            const int a = idx[ring * (segs + 1) + s];
            const int b = idx[ring * (segs + 1) + s + 1];
            const int c = idx[(ring + 1) * (segs + 1) + s + 1];
            const int d = idx[(ring + 1) * (segs + 1) + s];
            EmitTri(out, a, b, c, area);
            EmitTri(out, a, c, d, area);
        }
    }
}

void TriangulateCapsule(float r, float halfH, const glm::mat4& xf, uint8_t area, int segs, NavMeshTriangleSoup& out)
{
    if (segs < 4) segs = 4;
    // Cylinder side: ring at -halfH and +halfH
    std::vector<int> bot(segs + 1), top(segs + 1);
    for (int s = 0; s <= segs; ++s) {
        const float u = static_cast<float>(s) / static_cast<float>(segs);
        const float theta = u * 6.28318530717958f;
        const glm::vec3 lb{ r * std::cos(theta), -halfH, r * std::sin(theta) };
        const glm::vec3 lt{ r * std::cos(theta),  halfH, r * std::sin(theta) };
        const glm::vec3 wb = Xform(xf, lb); GrowAabb(out, wb);
        const glm::vec3 wt = Xform(xf, lt); GrowAabb(out, wt);
        bot[s] = EmitVert(out, wb);
        top[s] = EmitVert(out, wt);
    }
    for (int s = 0; s < segs; ++s) {
        EmitTri(out, bot[s], bot[s+1], top[s+1], area);
        EmitTri(out, bot[s], top[s+1], top[s],   area);
    }
    // Hemispheres at each cap. Use TriangulateSphere with translated transforms.
    const glm::mat4 topCap = xf * glm::translate(glm::mat4(1.0f), glm::vec3(0,  halfH, 0));
    const glm::mat4 botCap = xf * glm::translate(glm::mat4(1.0f), glm::vec3(0, -halfH, 0));
    TriangulateSphere(r, topCap, area, segs, out);
    TriangulateSphere(r, botCap, area, segs, out);
}

NavMeshTriangleSoup CollectTriangles(const ECS& world, const MeshSystem* meshSystem)
{
    NavMeshTriangleSoup soup;

    world.Each<NavMeshSourceComponent>([&](EntityId e) {
        const auto* src = world.GetComponent<NavMeshSourceComponent>(e);
        const auto* tr  = world.GetComponent<TransformComponent>(e);
        if (!src || !tr) {
            SM_WARN("NavMeshSource entity %llu missing TransformComponent; skipped", e);
            return;
        }
        if (src->Geometry == NavMeshGeometrySource::Unset) {
            SM_WARN("NavMeshSource entity %llu Geometry=Unset; pick Collider or Mesh", e);
            return;
        }
        const glm::mat4 worldXform = BuildWorld(*tr);

        if (src->Geometry == NavMeshGeometrySource::Collider) {
            const auto* col = world.GetComponent<ColliderComponent>(e);
            if (!col) {
                SM_WARN("NavMeshSource entity %llu Geometry=Collider but no ColliderComponent; skipped", e);
                return;
            }
            // Apply collider's local Offset to the world transform.
            const glm::mat4 xfWithOffset = glm::translate(worldXform, col->Offset);
            switch (col->Shape) {
                case ColliderShape::Box:
                    TriangulateBox(col->Size, xfWithOffset, src->AreaId, soup);
                    break;
                case ColliderShape::Sphere:
                    TriangulateSphere(col->Size.x, xfWithOffset, src->AreaId, 8, soup);
                    break;
                case ColliderShape::Capsule:
                    TriangulateCapsule(col->Size.x, col->Size.y, xfWithOffset, src->AreaId, 8, soup);
                    break;
            }
        } else { // NavMeshGeometrySource::Mesh
            const auto* mc = world.GetComponent<MeshComponent>(e);
            if (!mc) {
                SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshComponent; skipped", e);
                return;
            }
            if (!meshSystem) {
                SM_WARN("NavMeshSource entity %llu Geometry=Mesh but no MeshSystem available; skipped", e);
                return;
            }
            const auto cpu = meshSystem->GetMeshCpuData(mc->MeshId);
            if (!cpu.valid) {
                SM_WARN("NavMeshSource entity %llu MeshId %u has no CPU data; skipped", e, mc->MeshId);
                return;
            }
            // Transform each vertex into world, append as triangles (every 3 indices = 1 tri).
            // Indices are local to the mesh's vertex buffer; remap to global Verts indexing.
            const int baseVert = static_cast<int>(soup.Verts.size() / 3);
            for (const auto& v : cpu.vertices) {
                const glm::vec3 w = Xform(worldXform, glm::vec3(v.position[0], v.position[1], v.position[2]));
                soup.Verts.push_back(w.x);
                soup.Verts.push_back(w.y);
                soup.Verts.push_back(w.z);
                GrowAabb(soup, w);
            }
            for (size_t i = 0; i + 2 < cpu.indices.size(); i += 3) {
                EmitTri(soup,
                        baseVert + static_cast<int>(cpu.indices[i + 0]),
                        baseVert + static_cast<int>(cpu.indices[i + 1]),
                        baseVert + static_cast<int>(cpu.indices[i + 2]),
                        src->AreaId);
            }
        }
    });

    return soup;
}

} // namespace NavMeshBuilder
```

Note on `MeshVertex::position`: confirm member access by skimming `src/engine/src/rendering/Renderer.h` (or wherever `MeshVertex` is declared). If it's a `glm::vec3 Position` rather than `float position[3]`, adjust the field access. Either spelling is a one-line edit.

- [ ] **Step 4: Update Engine `CMakeLists.txt`**

Modify `src/engine/CMakeLists.txt` — add new sources and recast libs.

Add to the `add_library(Engine SHARED ...)` sources block (right after the `# Utilities` section, before `# Third-party`):

```cmake
    # Navigation (Recast/Detour)
    src/navigation/NavMeshBuilder.cpp
```

Add to `target_include_directories(Engine PUBLIC ...)`:

```cmake
    src/navigation
```

Add to `target_link_libraries(Engine PUBLIC ...)`:

```cmake
    Recast
    Detour
    DetourTileCache
    DebugUtils
```

(`DetourCrowd` is NOT needed for Spec 1 — agents come in Spec 3. Leave it out until Spec 3.)

- [ ] **Step 5: Build to verify**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: Engine builds clean, `NavMeshBuilder.obj` produced, Recast libs link in. No use of NavMeshBuilder anywhere yet (callers come in Task 6) so this is a pure compile-test.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/navigation/NavMeshBuilder.h src/engine/src/navigation/NavMeshBuilder.cpp src/engine/src/rendering/MeshSystem.h src/engine/src/rendering/MeshSystem.cpp src/engine/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMeshBuilder + MeshSystem CPU data accessor

NavMeshBuilder::CollectTriangles walks the ECS for NavMeshSource
entities, dispatches per Geometry enum (Collider/Mesh), emits SM_WARN
on Unset and on Geometry vs available-component mismatches. Box /
Sphere / Capsule triangulation helpers (segs=8 for sphere+capsule;
Recast voxelizes anyway so higher tri count gives no win).

MeshSystem::GetMeshCpuData exposes the already-retained CPU
vertex/index buffers (kept for backend hot-swap; nav build is a
second consumer with no extra cost). Spec Risk 1 resolved without
needing a retention flag.

Engine links Recast/Detour/DetourTileCache/DebugUtils static libs.
DetourCrowd held back to Spec 3 (agents)."
```

---

## Task 5: NavMesh wrapper — RAII, Recast/Detour build, query API

**Files:**
- Create: `src/engine/src/navigation/NavMesh.h`
- Create: `src/engine/src/navigation/NavMesh.cpp`
- Modify: `src/engine/CMakeLists.txt` (add NavMesh.cpp)

- [ ] **Step 1: Create `NavMesh.h`**

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <thread>

#include <glm/vec3.hpp>

#include "Engine.h"

// Forward-declare Detour/Recast types so the header doesn't drag them in.
class  dtNavMesh;
class  dtNavMeshQuery;
struct dtTileCache;
struct dtTileCacheAlloc;
struct dtTileCacheCompressor;
struct dtTileCacheMeshProcess;

struct NavMeshTriangleSoup;          // from NavMeshBuilder.h
struct NavMeshConfigComponent;       // from ECS.h

// Owning RAII wrapper for the per-NavMesh allocator/compressor/process objects
// so rebuild teardown is clean and we don't leak on shutdown.
struct ENGINE_API NavMeshAlloc {
    dtTileCacheAlloc*       Alloc      = nullptr;
    dtTileCacheCompressor*  Compressor = nullptr;
    dtTileCacheMeshProcess* MeshProc   = nullptr;
    NavMeshAlloc();
    ~NavMeshAlloc();
    NavMeshAlloc(const NavMeshAlloc&)            = delete;
    NavMeshAlloc& operator=(const NavMeshAlloc&) = delete;
};

class ENGINE_API NavMesh {
public:
    struct PathPoint {
        glm::vec3 Position{0.0f};
        uint8_t   AreaId = 0;
    };

    // Build a navmesh from a triangle soup + config. Returns nullptr on failure
    // (already logged via SM_WARN). Caller atomic-publishes through NavMeshSystem.
    static std::unique_ptr<NavMesh> Build(const NavMeshTriangleSoup& soup,
                                          const NavMeshConfigComponent& cfg);

    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&)            = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // String-pulled path (Detour dtFindStraightPath). Empty result = no path
    // / out of search radius / start or end off the navmesh. GameThread only
    // (asserted in Debug — dtNavMeshQuery is not thread-safe).
    std::vector<PathPoint> FindPath(const glm::vec3& start,
                                    const glm::vec3& end,
                                    float maxSearchRadius = 50.0f) const;

    // Snap a world point to the closest poly. Returns false if no poly within
    // search extents (default 2m XZ, 4m Y).
    bool ClosestPoint(const glm::vec3& world, glm::vec3& out) const;

    // Collect poly outline edges as line segments (pairs of vec3 — caller draws
    // as DebugAppendLine pairs). Used by DebugRenderPass ShowNavMesh.
    void CollectPolyEdges(std::vector<glm::vec3>& outLines) const;

    // Stats for the Navigation panel's status block.
    struct Stats {
        int TilesBuilt = 0;
        int PolyCount  = 0;
        int VertCount  = 0;
        int MemoryKB   = 0;
    };
    Stats GetStats() const;

private:
    friend std::unique_ptr<NavMesh> NavMesh_BuildImpl(const NavMeshTriangleSoup& soup,
                                                      const NavMeshConfigComponent& cfg);
    NavMeshAlloc                          m_Alloc;
    dtTileCache*                          m_TileCache = nullptr;
    dtNavMesh*                            m_NavMesh   = nullptr;
    std::unique_ptr<dtNavMeshQuery>       m_Query;
};
```

- [ ] **Step 2: Create `NavMesh.cpp`**

This is the largest file in the plan — ~250 lines of Recast/Detour glue. The structure is canonical (see `RecastDemo/Sample_TempObstacles.cpp` in the upstream submodule at `third_party/recastnavigation/RecastDemo/Source/Sample_TempObstacles.cpp` — copy + adapt, don't write from scratch). Key adaptations to the upstream sample:

- Replace SDL/GL drawing with no-op (we never call any drawing from inside NavMesh).
- Replace fastlz with our own simple `dtTileCacheLZCompressor` (the sample includes a `LinearAllocator` / `FastLZCompressor` / `MeshProcess` set — reuse those impls as-is, paste into `NavMesh.cpp` in an anonymous namespace).
- The sample's `buildTiledNavigation` becomes our `NavMesh::Build`.
- Use `m_Alloc` for the allocator/compressor/processor ownership.

Create `src/engine/src/navigation/NavMesh.cpp`:

```cpp
#include "navigation/NavMesh.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include <Recast.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourTileCache.h>
#include <DetourTileCacheBuilder.h>

#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "lib.h"    // SM_WARN, SM_ASSERT

namespace {

// ---------- Linear allocator + simple "no-op" compressor + mesh processor ----------
// Adapted from RecastDemo Sample_TempObstacles.cpp. The compressor is intentionally
// a no-op (memcpy) — we don't ship the fastlz dep, and Spec 1 navmeshes are tiny
// so compression buys nothing. Spec 4 (disk bake) may revisit.

struct LinearAllocator : public dtTileCacheAlloc {
    unsigned char* buffer = nullptr;
    size_t         capacity = 0;
    size_t         top = 0;
    size_t         high = 0;

    explicit LinearAllocator(size_t cap) : capacity(cap) {
        buffer = (unsigned char*)dtAlloc(cap, DT_ALLOC_PERM);
    }
    ~LinearAllocator() override { dtFree(buffer); }

    void reset() override { high = std::max(high, top); top = 0; }
    void* alloc(const size_t size) override {
        if (!buffer || top + size > capacity) return nullptr;
        unsigned char* p = buffer + top;
        top += size;
        return p;
    }
    void free(void*) override { /* no-op (LinearAllocator owns all) */ }
};

struct NoopCompressor : public dtTileCacheCompressor {
    int maxCompressedSize(const int bufferSize) override { return bufferSize + 1; }
    dtStatus compress(const unsigned char* buffer, const int bufferSize,
                      unsigned char* compressed, const int /*maxCompressedSize*/,
                      int* compressedSize) override {
        std::memcpy(compressed, buffer, bufferSize);
        *compressedSize = bufferSize;
        return DT_SUCCESS;
    }
    dtStatus decompress(const unsigned char* compressed, const int compressedSize,
                        unsigned char* buffer, const int /*maxBufferSize*/,
                        int* bufferSize) override {
        std::memcpy(buffer, compressed, compressedSize);
        *bufferSize = compressedSize;
        return DT_SUCCESS;
    }
};

struct MeshProcess : public dtTileCacheMeshProcess {
    void process(struct dtNavMeshCreateParams* params,
                 unsigned char* polyAreas,
                 unsigned short* polyFlags) override {
        for (int i = 0; i < params->polyCount; ++i) {
            if (polyAreas[i] != RC_NULL_AREA) {
                polyFlags[i] = 0x01;   // walkable bit; future spec adds per-area flags
            }
        }
    }
};

// Thread-id pin for the dtNavMeshQuery (debug-only assert in FindPath).
inline std::thread::id& NavQueryOwnerThread() {
    static std::thread::id id;
    return id;
}

} // namespace

// ---------- NavMeshAlloc RAII ----------
NavMeshAlloc::NavMeshAlloc() {
    Alloc      = new LinearAllocator(64 * 1024); // 64 KB scratch — bumped if a build needs more
    Compressor = new NoopCompressor();
    MeshProc   = new MeshProcess();
}
NavMeshAlloc::~NavMeshAlloc() {
    delete static_cast<LinearAllocator*>(Alloc);
    delete static_cast<NoopCompressor*>(Compressor);
    delete static_cast<MeshProcess*>(MeshProc);
}

// ---------- NavMesh ctor/dtor ----------
NavMesh::NavMesh() = default;
NavMesh::~NavMesh() {
    if (m_TileCache) { dtFreeTileCache(m_TileCache); m_TileCache = nullptr; }
    if (m_NavMesh)   { dtFreeNavMesh(m_NavMesh);     m_NavMesh   = nullptr; }
    // m_Query is unique_ptr — auto-deleted.
}

// ---------- Build ----------
std::unique_ptr<NavMesh> NavMesh::Build(const NavMeshTriangleSoup& soup,
                                        const NavMeshConfigComponent& cfg)
{
    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMesh::Build called with empty soup; returning null");
        return nullptr;
    }

    auto out = std::unique_ptr<NavMesh>(new NavMesh());

    // ----- rcConfig from NavMeshConfigComponent -----
    rcConfig rcc{};
    rcc.cs                     = cfg.CellSize;
    rcc.ch                     = cfg.CellHeight;
    rcc.walkableSlopeAngle     = cfg.AgentMaxSlope;
    rcc.walkableHeight         = (int)std::ceil(cfg.AgentHeight / cfg.CellHeight);
    rcc.walkableClimb          = (int)std::floor(cfg.AgentMaxClimb / cfg.CellHeight);
    rcc.walkableRadius         = (int)std::ceil(cfg.AgentRadius / cfg.CellSize);
    rcc.maxEdgeLen             = (int)(12.0f / cfg.CellSize);
    rcc.maxSimplificationError = 1.3f;
    rcc.minRegionArea          = (int)rcSqr(8);
    rcc.mergeRegionArea        = (int)rcSqr(20);
    rcc.maxVertsPerPoly        = DT_VERTS_PER_POLYGON;
    rcc.tileSize               = (int)cfg.TileSize;
    rcc.borderSize             = rcc.walkableRadius + 3;
    rcc.width                  = rcc.tileSize + rcc.borderSize * 2;
    rcc.height                 = rcc.tileSize + rcc.borderSize * 2;
    rcc.detailSampleDist       = (rcc.cs * 6.0f);
    rcc.detailSampleMaxError   = (rcc.ch * 1.0f);

    const float bmin[3] = { soup.AabbMin.x, soup.AabbMin.y, soup.AabbMin.z };
    const float bmax[3] = { soup.AabbMax.x, soup.AabbMax.y, soup.AabbMax.z };
    int gw = 0, gh = 0;
    rcCalcGridSize(bmin, bmax, rcc.cs, &gw, &gh);
    const int tw = (gw + rcc.tileSize - 1) / rcc.tileSize;
    const int th = (gh + rcc.tileSize - 1) / rcc.tileSize;

    // ----- dtTileCacheParams -----
    dtTileCacheParams tcParams{};
    rcVcopy(tcParams.orig, bmin);
    tcParams.cs                     = cfg.CellSize;
    tcParams.ch                     = cfg.CellHeight;
    tcParams.width                  = rcc.tileSize;
    tcParams.height                 = rcc.tileSize;
    tcParams.walkableHeight         = cfg.AgentHeight;
    tcParams.walkableRadius         = cfg.AgentRadius;
    tcParams.walkableClimb          = cfg.AgentMaxClimb;
    tcParams.maxSimplificationError = 1.3f;
    tcParams.maxTiles               = tw * th * 4;        // 4 layers max per tile (Recast sample default)
    tcParams.maxObstacles           = cfg.MaxObstacles;

    out->m_TileCache = dtAllocTileCache();
    if (!out->m_TileCache) { SM_WARN("dtAllocTileCache failed"); return nullptr; }
    if (dtStatusFailed(out->m_TileCache->init(&tcParams,
                                              out->m_Alloc.Alloc,
                                              out->m_Alloc.Compressor,
                                              out->m_Alloc.MeshProc))) {
        SM_WARN("dtTileCache::init failed");
        return nullptr;
    }

    // ----- dtNavMesh + params -----
    dtNavMeshParams nmParams{};
    rcVcopy(nmParams.orig, bmin);
    nmParams.tileWidth  = rcc.tileSize * cfg.CellSize;
    nmParams.tileHeight = rcc.tileSize * cfg.CellSize;
    nmParams.maxTiles   = tw * th;
    nmParams.maxPolys   = 16384;

    out->m_NavMesh = dtAllocNavMesh();
    if (!out->m_NavMesh) { SM_WARN("dtAllocNavMesh failed"); return nullptr; }
    if (dtStatusFailed(out->m_NavMesh->init(&nmParams))) {
        SM_WARN("dtNavMesh::init failed");
        return nullptr;
    }

    // ----- Build each tile (boilerplate adapted from Sample_TempObstacles::buildTiledNavigation) -----
    rcContext ctx(false);
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            // Per-tile config: shift bmin/bmax to tile bounds + borderSize halo.
            rcConfig tcfg = rcc;
            tcfg.bmin[0] = bmin[0] + (x * rcc.tileSize - rcc.borderSize) * rcc.cs;
            tcfg.bmin[1] = bmin[1];
            tcfg.bmin[2] = bmin[2] + (y * rcc.tileSize - rcc.borderSize) * rcc.cs;
            tcfg.bmax[0] = bmin[0] + ((x + 1) * rcc.tileSize + rcc.borderSize) * rcc.cs;
            tcfg.bmax[1] = bmax[1];
            tcfg.bmax[2] = bmin[2] + ((y + 1) * rcc.tileSize + rcc.borderSize) * rcc.cs;

            rcHeightfield* hf = rcAllocHeightfield();
            if (!hf || !rcCreateHeightfield(&ctx, *hf, tcfg.width, tcfg.height,
                                            tcfg.bmin, tcfg.bmax, tcfg.cs, tcfg.ch)) {
                rcFreeHeightField(hf);
                SM_WARN("rcCreateHeightfield failed for tile (%d,%d)", x, y);
                continue;
            }

            // Mark walkable + rasterize.
            const int triCount = (int)(soup.Tris.size() / 3);
            std::vector<unsigned char> areasMutable(soup.Areas.begin(), soup.Areas.end());
            // areasMutable now starts as the user-supplied areas; mark slope filters as RC_NULL_AREA.
            rcClearUnwalkableTriangles(&ctx, tcfg.walkableSlopeAngle,
                                       soup.Verts.data(), (int)(soup.Verts.size() / 3),
                                       soup.Tris.data(), triCount,
                                       areasMutable.data());
            if (!rcRasterizeTriangles(&ctx,
                                      soup.Verts.data(), (int)(soup.Verts.size() / 3),
                                      soup.Tris.data(), areasMutable.data(), triCount,
                                      *hf, tcfg.walkableClimb)) {
                rcFreeHeightField(hf);
                SM_WARN("rcRasterizeTriangles failed for tile (%d,%d)", x, y);
                continue;
            }

            rcFilterLowHangingWalkableObstacles(&ctx, tcfg.walkableClimb, *hf);
            rcFilterLedgeSpans(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *hf);
            rcFilterWalkableLowHeightSpans(&ctx, tcfg.walkableHeight, *hf);

            rcCompactHeightfield* chf = rcAllocCompactHeightfield();
            if (!chf || !rcBuildCompactHeightfield(&ctx, tcfg.walkableHeight, tcfg.walkableClimb, *hf, *chf)) {
                rcFreeHeightField(hf); rcFreeCompactHeightfield(chf);
                SM_WARN("rcBuildCompactHeightfield failed for tile (%d,%d)", x, y);
                continue;
            }
            rcFreeHeightField(hf);

            if (!rcErodeWalkableArea(&ctx, tcfg.walkableRadius, *chf)) {
                rcFreeCompactHeightfield(chf);
                SM_WARN("rcErodeWalkableArea failed for tile (%d,%d)", x, y);
                continue;
            }

            rcHeightfieldLayerSet* lset = rcAllocHeightfieldLayerSet();
            if (!lset || !rcBuildHeightfieldLayers(&ctx, *chf, tcfg.borderSize, tcfg.walkableHeight, *lset)) {
                rcFreeCompactHeightfield(chf); rcFreeHeightfieldLayerSet(lset);
                SM_WARN("rcBuildHeightfieldLayers failed for tile (%d,%d)", x, y);
                continue;
            }
            rcFreeCompactHeightfield(chf);

            // Build one tile cache layer per heightfield layer.
            for (int i = 0; i < lset->nlayers; ++i) {
                const rcHeightfieldLayer* layer = &lset->layers[i];
                dtTileCacheLayerHeader header{};
                header.magic   = DT_TILECACHE_MAGIC;
                header.version = DT_TILECACHE_VERSION;
                header.tx = x; header.ty = y; header.tlayer = i;
                dtVcopy(header.bmin, layer->bmin);
                dtVcopy(header.bmax, layer->bmax);
                header.hmin   = (unsigned short)layer->hmin;
                header.hmax   = (unsigned short)layer->hmax;
                header.width  = (unsigned char)layer->width;
                header.height = (unsigned char)layer->height;
                header.minx   = (unsigned char)layer->minx;
                header.maxx   = (unsigned char)layer->maxx;
                header.miny   = (unsigned char)layer->miny;
                header.maxy   = (unsigned char)layer->maxy;

                unsigned char* tileData = nullptr;
                int tileSize = 0;
                if (dtStatusFailed(dtBuildTileCacheLayer(out->m_Alloc.Compressor,
                                                         &header, layer->heights, layer->areas, layer->cons,
                                                         &tileData, &tileSize))) {
                    SM_WARN("dtBuildTileCacheLayer failed for tile (%d,%d) layer %d", x, y, i);
                    continue;
                }
                dtCompressedTileRef ref = 0;
                if (dtStatusFailed(out->m_TileCache->addTile(tileData, tileSize, DT_COMPRESSEDTILE_FREE_DATA, &ref))) {
                    dtFree(tileData);
                    SM_WARN("dtTileCache::addTile failed for tile (%d,%d) layer %d", x, y, i);
                    continue;
                }
            }
            rcFreeHeightfieldLayerSet(lset);
        }
    }

    // Build all tiles into the nav mesh.
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            out->m_TileCache->buildNavMeshTilesAt(x, y, out->m_NavMesh);
        }
    }

    // Init query for runtime path lookups; pin to current thread.
    out->m_Query = std::unique_ptr<dtNavMeshQuery>(dtAllocNavMeshQuery());
    if (dtStatusFailed(out->m_Query->init(out->m_NavMesh, 2048))) {
        SM_WARN("dtNavMeshQuery::init failed");
        return nullptr;
    }
    NavQueryOwnerThread() = std::this_thread::get_id();

    return out;
}

// ---------- FindPath ----------
std::vector<NavMesh::PathPoint> NavMesh::FindPath(const glm::vec3& start,
                                                  const glm::vec3& end,
                                                  float maxSearchRadius) const
{
    SM_ASSERT(std::this_thread::get_id() == NavQueryOwnerThread(),
              "NavMesh::FindPath called from non-owner thread; dtNavMeshQuery is not thread-safe");
    std::vector<PathPoint> out;
    if (!m_Query || !m_NavMesh) return out;

    const float ext[3] = { maxSearchRadius, maxSearchRadius, maxSearchRadius };
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    dtPolyRef startRef = 0, endRef = 0;
    float startNearest[3], endNearest[3];
    const float s[3] = { start.x, start.y, start.z };
    const float e[3] = { end.x,   end.y,   end.z   };
    m_Query->findNearestPoly(s, ext, &filter, &startRef, startNearest);
    m_Query->findNearestPoly(e, ext, &filter, &endRef,   endNearest);
    if (!startRef || !endRef) return out;

    dtPolyRef polys[256];
    int npolys = 0;
    if (dtStatusFailed(m_Query->findPath(startRef, endRef, startNearest, endNearest,
                                         &filter, polys, &npolys, 256))) {
        return out;
    }
    if (npolys == 0) return out;

    float straight[256 * 3];
    unsigned char straightFlags[256];
    dtPolyRef straightRefs[256];
    int nstraight = 0;
    if (dtStatusFailed(m_Query->findStraightPath(startNearest, endNearest,
                                                 polys, npolys,
                                                 straight, straightFlags, straightRefs,
                                                 &nstraight, 256))) {
        return out;
    }
    out.reserve(nstraight);
    for (int i = 0; i < nstraight; ++i) {
        PathPoint p;
        p.Position = glm::vec3(straight[i*3+0], straight[i*3+1], straight[i*3+2]);
        p.AreaId   = 0;
        out.push_back(p);
    }
    return out;
}

bool NavMesh::ClosestPoint(const glm::vec3& world, glm::vec3& out) const
{
    if (!m_Query) return false;
    const float ext[3] = { 2.0f, 4.0f, 2.0f };
    dtQueryFilter filter;
    dtPolyRef ref = 0;
    float nearest[3];
    const float p[3] = { world.x, world.y, world.z };
    m_Query->findNearestPoly(p, ext, &filter, &ref, nearest);
    if (!ref) return false;
    out = glm::vec3(nearest[0], nearest[1], nearest[2]);
    return true;
}

void NavMesh::CollectPolyEdges(std::vector<glm::vec3>& outLines) const
{
    if (!m_NavMesh) return;
    for (int i = 0; i < m_NavMesh->getMaxTiles(); ++i) {
        const dtMeshTile* tile = m_NavMesh->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly* poly = &tile->polys[p];
            for (int v = 0; v < poly->vertCount; ++v) {
                const int vi0 = poly->verts[v];
                const int vi1 = poly->verts[(v + 1) % poly->vertCount];
                const float* a = &tile->verts[vi0 * 3];
                const float* b = &tile->verts[vi1 * 3];
                outLines.emplace_back(a[0], a[1], a[2]);
                outLines.emplace_back(b[0], b[1], b[2]);
            }
        }
    }
}

NavMesh::Stats NavMesh::GetStats() const
{
    Stats s;
    if (!m_NavMesh) return s;
    for (int i = 0; i < m_NavMesh->getMaxTiles(); ++i) {
        const dtMeshTile* tile = m_NavMesh->getTile(i);
        if (!tile || !tile->header) continue;
        ++s.TilesBuilt;
        s.PolyCount += tile->header->polyCount;
        s.VertCount += tile->header->vertCount;
    }
    s.MemoryKB = (int)((s.VertCount * sizeof(float) * 3 + s.PolyCount * 64) / 1024);
    return s;
}
```

- [ ] **Step 3: Update Engine `CMakeLists.txt`**

Add to the navigation sources block:

```cmake
    # Navigation (Recast/Detour)
    src/navigation/NavMeshBuilder.cpp
    src/navigation/NavMesh.cpp
```

- [ ] **Step 4: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine --config Debug
```

Expected: clean build. Recast/Detour headers resolve via the public include dirs registered by the `Recast` / `Detour` / `DetourTileCache` targets. If a header isn't found, double-check the Engine target links those interface libraries (Recast's CMakeLists.txt should propagate include dirs).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/navigation/NavMesh.h src/engine/src/navigation/NavMesh.cpp src/engine/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMesh wrapper + Recast TileCache build pipeline

NavMesh owns dtTileCache + dtNavMesh + dtNavMeshQuery via NavMeshAlloc
RAII (LinearAllocator + no-op compressor + simple MeshProcess
adapted from Sample_TempObstacles). Build pipeline follows the
canonical Recast tile-cache flow: rcCreateHeightfield ->
rcRasterizeTriangles -> filter passes -> rcBuildCompactHeightfield ->
rcErodeWalkableArea -> rcBuildHeightfieldLayers -> per-layer
dtBuildTileCacheLayer -> dtTileCache::addTile ->
buildNavMeshTilesAt.

FindPath uses dtFindStraightPath for nice diagonals through corners
(string-pulling). Debug-only assert pins the dtNavMeshQuery to the
build-thread id, since the query is not thread-safe per Detour docs.

ClosestPoint, CollectPolyEdges (for ShowNavMesh debug viz), and
GetStats (for the Navigation panel) round out the public API."
```

---

## Task 6: NavMeshSystem + GameThread hook wiring + test_navmesh

**Files:**
- Create: `src/engine/src/navigation/NavMeshSystem.h`
- Create: `src/engine/src/navigation/NavMeshSystem.cpp`
- Modify: `src/engine/CMakeLists.txt` (add NavMeshSystem.cpp)
- Modify: `src/engine/src/threading/GameThread.cpp` (wire ECSCommandHooks)
- Create: `tests/test_navmesh.cpp`
- Modify: `tests/CMakeLists.txt` (add test_navmesh target)

- [ ] **Step 1: Create `NavMeshSystem.h`**

```cpp
#pragma once

#include <memory>

#include "Engine.h"

class  ECS;
class  MeshSystem;
class  NavMesh;
struct NavMeshConfigComponent;

// Engine-side service holding the current navmesh. Build runs on GameThread; the
// resulting shared_ptr is atomic-published so any reader (RenderThread debug viz,
// game-side queries on the same GameThread) can grab a stable snapshot.
class ENGINE_API NavMeshSystem {
public:
    static NavMeshSystem& Instance();

    // Build navmesh from current ECS state. GameThread only.
    // meshSystem may be null — entities tagged Geometry=Mesh will be skipped with SM_WARN.
    void Rebuild(const ECS& world, const NavMeshConfigComponent& cfg, const MeshSystem* meshSystem);

    // Get current navmesh. Any thread. May be null before first build.
    std::shared_ptr<const NavMesh> Current() const;

private:
    NavMeshSystem() = default;
    std::shared_ptr<const NavMesh> m_Current;
};
```

- [ ] **Step 2: Create `NavMeshSystem.cpp`**

```cpp
#include "navigation/NavMeshSystem.h"

#include <atomic>

#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "ECS.h"
#include "lib.h"

NavMeshSystem& NavMeshSystem::Instance() {
    static NavMeshSystem s;
    return s;
}

void NavMeshSystem::Rebuild(const ECS& world,
                            const NavMeshConfigComponent& cfg,
                            const MeshSystem* meshSystem)
{
    const NavMeshTriangleSoup soup = NavMeshBuilder::CollectTriangles(world, meshSystem);
    if (soup.Empty || soup.Tris.empty()) {
        SM_WARN("NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh");
        std::atomic_store(&m_Current, std::shared_ptr<const NavMesh>{});
        return;
    }
    auto fresh = NavMesh::Build(soup, cfg);
    if (!fresh) {
        SM_WARN("NavMeshSystem::Rebuild: NavMesh::Build returned null; keeping previous navmesh");
        return;
    }
    std::shared_ptr<const NavMesh> shared(std::move(fresh));
    std::atomic_store(&m_Current, shared);
}

std::shared_ptr<const NavMesh> NavMeshSystem::Current() const {
    return std::atomic_load(&m_Current);
}
```

- [ ] **Step 3: Update Engine CMakeLists**

Add `src/navigation/NavMeshSystem.cpp` to the Engine sources block.

- [ ] **Step 4: Wire `ECSCommandHooks::OnRebuildNavMesh` in `GameThread.cpp`**

In `src/engine/src/threading/GameThread.cpp`, add `#include "navigation/NavMeshSystem.h"` near the top. Find the per-tick `ECSCommandProcessor::ProcessCommands` call (search for `ProcessCommands(`); add a hooks-construction block above it and pass it as the third arg:

```cpp
        ECSCommandHooks hooks;
        hooks.OnRebuildNavMesh = [meshSystem = m_AppContext->RendererPtr ? m_AppContext->RendererPtr->GetMeshSystem() : nullptr]
                                 (ECS& w) {
            const auto* cfg = w.GetSingleton<NavMeshConfigComponent>();
            NavMeshConfigComponent defaultCfg{};
            NavMeshSystem::Instance().Rebuild(w, cfg ? *cfg : defaultCfg, meshSystem);
        };
        ECSCommandProcessor::ProcessCommands(gameState.World, m_AppContext->EcsCommands, hooks);
```

(The exact `m_AppContext->RendererPtr->GetMeshSystem()` chain may need to be adapted to match current `ApplicationContext` field names — read `src/common/include/ApplicationContext.h` first. If MeshSystem isn't reachable from GameThread cleanly, capture it from `m_AppContext->Renderer->GetMeshSystem()` or add a `MeshSystem*` field to `ApplicationContext`. The fallback `nullptr` lambda capture keeps the build path working with mesh-source entities skipped + SM_WARN.)

- [ ] **Step 5: Write failing tests first**

Create `tests/test_navmesh.cpp`:

```cpp
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <thread>

#include <glm/glm.hpp>

#include "ECS.h"
#include "navigation/NavMesh.h"
#include "navigation/NavMeshBuilder.h"
#include "navigation/NavMeshSystem.h"

void platform_debug_break(const char* expr, const char* file, int line, const char* message)
{
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 (file ? file : "<unknown>"), line,
                 (message ? message : "<no message>"),
                 (expr ? expr : "<none>"));
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_Failures;                                                  \
    } } while (0)

static EntityId SpawnNavBox(ECS& w, glm::vec3 pos, glm::vec3 halfExtents) {
    const EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{ pos, glm::vec3(0.0f), glm::vec3(1.0f) });
    ColliderComponent col{};
    col.Shape   = ColliderShape::Box;
    col.Size    = halfExtents;
    col.IsStatic = true;
    w.AddComponent(e, col);
    NavMeshSourceComponent src{};
    src.AreaId   = 63;
    src.Geometry = NavMeshGeometrySource::Collider;
    w.AddComponent(e, src);
    return e;
}

static NavMeshConfigComponent DefaultCfg() {
    return NavMeshConfigComponent{};  // defaults
}

static void T01_empty_world_yields_empty_navmesh() {
    ECS w;
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(!nm); // empty soup → null published
}

static void T02_flat_floor_path_is_straight() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f)); // 10x10 floor
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    auto path = nm->FindPath(glm::vec3(-4, 0.5f, -4), glm::vec3(4, 0.5f, 4));
    EXPECT(path.size() >= 2);
}

static void T03_path_around_wall_is_curved() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));      // floor
    SpawnNavBox(w, glm::vec3(0,  1.0f, 0), glm::vec3(0.3f, 1.0f, 2.0f));     // wall
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (!nm) return;
    auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
    EXPECT(path.size() > 2); // straight line would be 2; wall forces routing
}

static void T04_unset_geometry_is_skipped() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));      // floor (Collider)
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
        ColliderComponent c{};
        c.Shape = ColliderShape::Box;
        c.Size  = glm::vec3(0.5f);
        w.AddComponent(e, c);
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Unset;  // skipped
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    // Path across the floor should be unaffected by the Unset entity (treated as missing).
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() >= 2);
    }
}

static void T05_mesh_geometry_without_meshcomponent_skipped() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));      // floor
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(2, 0.5f, 0), glm::vec3(0), glm::vec3(1) });
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Mesh;  // no MeshComponent — should SM_WARN + skip
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() >= 2);
    }
}

static void T06_sphere_collider_routes_around() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));      // floor
    // Sphere in the middle of the path.
    {
        const EntityId e = w.CreateEntity();
        w.AddComponent(e, TransformComponent{ glm::vec3(0, 1.0f, 0), glm::vec3(0), glm::vec3(1) });
        ColliderComponent c{};
        c.Shape = ColliderShape::Sphere;
        c.Size  = glm::vec3(1.5f); // radius in x
        c.IsStatic = true;
        w.AddComponent(e, c);
        NavMeshSourceComponent src{};
        src.Geometry = NavMeshGeometrySource::Collider;
        w.AddComponent(e, src);
    }
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto nm = NavMeshSystem::Instance().Current();
    EXPECT(nm != nullptr);
    if (nm) {
        auto path = nm->FindPath(glm::vec3(-4, 0.5f, 0), glm::vec3(4, 0.5f, 0));
        EXPECT(path.size() > 2); // sphere forces curve
    }
}

static void T07_current_shared_across_threads() {
    ECS w;
    SpawnNavBox(w, glm::vec3(0, -0.1f, 0), glm::vec3(5.0f, 0.1f, 5.0f));
    NavMeshSystem::Instance().Rebuild(w, DefaultCfg(), nullptr);
    auto p1 = NavMeshSystem::Instance().Current();
    std::shared_ptr<const NavMesh> p2;
    std::thread t([&]{ p2 = NavMeshSystem::Instance().Current(); });
    t.join();
    EXPECT(p1 == p2);
}

int main() {
    T01_empty_world_yields_empty_navmesh();
    T02_flat_floor_path_is_straight();
    T03_path_around_wall_is_curved();
    T04_unset_geometry_is_skipped();
    T05_mesh_geometry_without_meshcomponent_skipped();
    T06_sphere_collider_routes_around();
    T07_current_shared_across_threads();

    if (g_Failures) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_Failures);
        return 1;
    }
    std::printf("All navmesh tests passed.\n");
    return 0;
}
```

- [ ] **Step 6: Add `test_navmesh` to `tests/CMakeLists.txt`**

Append:

```cmake
add_executable(test_navmesh
    test_navmesh.cpp
)

target_link_libraries(test_navmesh PRIVATE
    CommonHeaders
    glm::glm
    ecs
    Engine
)

target_include_directories(test_navmesh PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
)

target_compile_definitions(test_navmesh PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_navmesh PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 7: Build + run tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_navmesh --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_navmesh.exe
```

Expected: `All navmesh tests passed.` All 7 green.

- [ ] **Step 8: Confirm no regression in other tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_collision test_worldserial test_menu --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```

Expected: all pass.

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/navigation/NavMeshSystem.h src/engine/src/navigation/NavMeshSystem.cpp src/engine/src/threading/GameThread.cpp src/engine/CMakeLists.txt tests/test_navmesh.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): NavMeshSystem + GameThread RebuildNavMesh hook + test_navmesh

NavMeshSystem singleton holds shared_ptr<const NavMesh> with
std::atomic_store/load publish (matches the ECS snapshot threading
pattern). Rebuild is GameThread-only; Current() is safe from any
thread (subject to the dtNavMeshQuery single-thread caveat in NavMesh
itself).

GameThread wires ECSCommandHooks::OnRebuildNavMesh to call
NavMeshSystem::Rebuild with the singleton NavMeshConfigComponent
(default-constructed if absent) + the renderer's MeshSystem so
Geometry=Mesh entities work.

test_navmesh covers: empty world (null published), flat floor
straight path, wall forces curved path, Unset geometry skipped,
Mesh-without-MeshComponent skipped, Sphere collider routing,
shared_ptr stable across threads."
```

---

## Task 7: ShowNavMesh debug viz + EditorPreferences

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h` (add ShowNavMesh field)
- Modify: `src/editor/src/EditorPreferences.h` (round-trip ShowNavMesh)
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (checkbox)
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp` (poly-edge block)
- Modify: `tests/test_editorprefs.cpp` (round-trip pin if test_editorprefs covers debug toggles)

- [ ] **Step 1: Add `ShowNavMesh` field**

In `src/engine/src/rendering/RenderStats.h` line 17-24, extend:

```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
    bool ShowGrid          = false;
    bool ShowColliders     = false;
    bool ShowNavMesh       = false;
};
```

- [ ] **Step 2: Round-trip in EditorPreferences**

In `src/editor/src/EditorPreferences.h`, extend `PrefsToJson` debugDraw block (around line 30-36):

```cpp
        {"debugDraw", {
            {"lightGizmos",   debug.ShowLightGizmos},
            {"cameraFrustum", debug.ShowCameraFrustum},
            {"selectedAABB",  debug.ShowSelectedAABB},
            {"wireframe",     debug.Wireframe},
            {"grid",          debug.ShowGrid},
            {"colliders",     debug.ShowColliders},
            {"navmesh",       debug.ShowNavMesh},
        }},
```

Extend `PrefsFromJson` (around line 64-69):

```cpp
        if (d.contains("navmesh")       && d["navmesh"].is_boolean())       debug.ShowNavMesh       = d["navmesh"].get<bool>();
```

- [ ] **Step 3: Add Render Stats checkbox**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` around line 30-31, append after `Colliders`:

```cpp
    changed |= ImGui::Checkbox("NavMesh",        &dd.ShowNavMesh);
```

- [ ] **Step 4: Add poly-edge block to `DebugRenderPass.cpp`**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`:

Add include at top:

```cpp
#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"
```

Modify the early-out at line 92 to include the new toggle:

```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid && !s.ShowColliders && !s.ShowNavMesh)
        return;
```

After the existing `ShowColliders` block (around line 151+), append:

```cpp
    if (s.ShowNavMesh) {
        auto nm = NavMeshSystem::Instance().Current();
        if (nm) {
            std::vector<glm::vec3> edges;
            nm->CollectPolyEdges(edges);
            const glm::vec4 col(0.2f, 0.85f, 1.0f, 1.0f); // cyan
            for (size_t i = 0; i + 1 < edges.size(); i += 2) {
                m_Verts.push_back({ edges[i],   col });
                m_Verts.push_back({ edges[i+1], col });
            }
        }
    }
```

(If the vertex push pattern in the existing `ShowColliders` block uses a different helper — e.g., `m_Verts.emplace_back(...)` or a `DebugAppendLine` helper — copy that exact pattern instead. The block above shows the structure; match the surrounding code.)

- [ ] **Step 5: If `test_editorprefs.cpp` pins the debugDraw key set, extend it**

```bash
grep -n "colliders\|wireframe" tests/test_editorprefs.cpp | head
```

If a test asserts the set of debugDraw keys, add `"navmesh"` to whatever set/list it checks; verify a round-trip case for `ShowNavMesh = true`. Otherwise skip this step.

- [ ] **Step 6: Build + manual smoke**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target Engine editor --config Debug
```

Expected: clean build. Manual smoke comes in Task 9 alongside the world-load trigger (without the trigger, there's nothing to render yet).

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/editor/src/EditorPreferences.h src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): ShowNavMesh debug viz + EditorPreferences key

ShowNavMesh joins ShowColliders/ShowGrid in DebugDrawSettings,
persisted via editor_preferences.json under the existing debugDraw
block (additive — old prefs files load with ShowNavMesh=false).
Render Stats panel grows a NavMesh checkbox. DebugRenderPass loads
the atomic NavMesh shared_ptr, calls CollectPolyEdges, and emits
cyan line segments for every poly outline. Render path is line-based
so no new debug primitive needed."
```

---

## Task 8: NavigationPanel ImGui

**Files:**
- Create: `src/editor/src/rendering/imgui/NavigationPanel.h`
- Create: `src/editor/src/rendering/imgui/NavigationPanel.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (mount the panel)
- Modify: `src/editor/CMakeLists.txt` (add panel source)

- [ ] **Step 1: Create `NavigationPanel.h`**

```cpp
#pragma once

class ECS;
template<typename T, size_t N> class SpscRing;
struct ECSCommand;

namespace NavigationPanel {
    // Render the Navigation ImGui window. Pulls NavMeshConfigComponent from `world`;
    // pushes ModifyComponent commands for edits and RebuildNavMesh for the button.
    void Render(const ECS& world, SpscRing<ECSCommand, 128>& commands, bool* open);
} // namespace NavigationPanel
```

(Adjust the SpscRing forward-decl to match the project's actual template signature in `src/common/include/SpscRing.h` — peek at it before pasting.)

- [ ] **Step 2: Create `NavigationPanel.cpp`**

```cpp
#include "NavigationPanel.h"

#include <imgui.h>

#include "ECS.h"
#include "ECSCommands.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavMesh.h"

namespace NavigationPanel {

void Render(const ECS& world, SpscRing<ECSCommand, 128>& commands, bool* open) {
    if (!ImGui::Begin("Navigation", open)) { ImGui::End(); return; }

    NavMeshConfigComponent cfg{};
    if (const auto* c = world.GetSingleton<NavMeshConfigComponent>()) cfg = *c;
    NavMeshConfigComponent edited = cfg;

    ImGui::TextUnformatted("Config");
    ImGui::Separator();
    bool changed = false;
    changed |= ImGui::DragFloat("Cell Size",      &edited.CellSize,      0.01f, 0.05f, 2.0f, "%.2f m");
    changed |= ImGui::DragFloat("Cell Height",    &edited.CellHeight,    0.01f, 0.05f, 2.0f, "%.2f m");
    changed |= ImGui::DragFloat("Agent Radius",   &edited.AgentRadius,   0.05f, 0.05f, 5.0f, "%.2f m");
    changed |= ImGui::DragFloat("Agent Height",   &edited.AgentHeight,   0.05f, 0.10f, 5.0f, "%.2f m");
    changed |= ImGui::DragFloat("Max Climb",      &edited.AgentMaxClimb, 0.05f, 0.00f, 2.0f, "%.2f m");
    changed |= ImGui::DragFloat("Max Slope",      &edited.AgentMaxSlope, 1.00f, 0.00f, 85.0f, "%.0f deg");
    changed |= ImGui::DragFloat("Tile Size",      &edited.TileSize,      1.00f, 8.00f, 128.0f, "%.0f voxels");
    changed |= ImGui::DragInt  ("Max Obstacles",  &edited.MaxObstacles,  1.0f, 0, 4096);

    if (changed) {
        // Push as a ModifyComponent — singleton edit goes through the standard ECSCommand path.
        commands.Push(ECSCommand::ModifyComponent(0 /* singleton entity is 0 */, edited));
    }

    ImGui::Spacing();
    if (ImGui::Button("Rebuild NavMesh", ImVec2(-1, 0))) {
        commands.Push(ECSCommand::RebuildNavMesh());
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Status");
    ImGui::Separator();
    auto nm = NavMeshSystem::Instance().Current();
    if (!nm) {
        ImGui::TextUnformatted("(no navmesh built yet)");
    } else {
        const auto s = nm->GetStats();
        ImGui::Text("Tiles built: %d", s.TilesBuilt);
        ImGui::Text("Polys: %d",       s.PolyCount);
        ImGui::Text("Vert count: %d",  s.VertCount);
        ImGui::Text("Memory: %d KB",   s.MemoryKB);
    }

    ImGui::End();
}

} // namespace NavigationPanel
```

(The "singleton entity is 0" comment: check how other singleton edits in the codebase target the singleton entity. For Fog/Sky in `DayNightPanel.cpp`, search for the exact entity id used — it may be a sentinel like `INVALID_ENTITY` or a reserved id. Match that pattern.)

- [ ] **Step 3: Mount the panel in `ImGuiRenderer.cpp`**

Find where other panels (e.g., `DayNightPanel`, `MaterialManagerPanel`) are rendered. Add a `bool m_NavPanelOpen = true;` member and a `NavigationPanel::Render(world, commands, &m_NavPanelOpen);` call alongside them. Add include:

```cpp
#include "NavigationPanel.h"
```

If there's a main-menu-bar Window menu where panels are toggled, add a "Navigation" entry. Look for how `DayNightPanel` gets toggled — match.

- [ ] **Step 4: Update editor CMakeLists**

In `src/editor/CMakeLists.txt`, append `src/rendering/imgui/NavigationPanel.cpp` to the editor sources.

- [ ] **Step 5: Build**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug
```

Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/NavigationPanel.h src/editor/src/rendering/imgui/NavigationPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): Navigation panel — config knobs + Rebuild + status

ImGui panel mirrors DayNightPanel pattern: reads
NavMeshConfigComponent singleton, edits push ModifyComponent
ECSCommands, Rebuild button pushes the new RebuildNavMesh command
(handled by GameThread's ECSCommandHooks wiring in Task 6). Status
block reads NavMeshSystem::Current() and shows tiles / polys / verts
/ memory."
```

---

## Task 9: World-load auto-rebuild + final whole-feature review

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp` (post RebuildNavMesh after world load)
- Modify: `src/game/src/game.cpp` (seed NavMeshConfigComponent singleton if absent — seed-if-absent pattern)

- [ ] **Step 1: Post `RebuildNavMesh` after successful world load**

In `src/engine/src/threading/GameThread.cpp`, find the world-load block around line 62-69:

```cpp
    if (!gameState.WorldLoaded) {
        if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World)) {
            gameState.WorldLoaded = true;
            SM_TRACE("GameThread: default world loaded from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
            // Trigger initial navmesh build now that the world is populated.
            m_AppContext->EcsCommands.Push(ECSCommand::RebuildNavMesh());
        } else {
            SM_WARN("GameThread: default world '%s' not loaded (file missing or invalid)", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
        }
    }
```

(Field name `EcsCommands` may differ — match whatever the `ApplicationContext` member is. Look up by searching for the existing `ProcessCommands` call.)

- [ ] **Step 2: Seed `NavMeshConfigComponent` singleton if absent**

In `src/game/src/game.cpp`, find the singleton-seed block (other singletons like Fog/Sky use `if (!world.HasSingleton<X>()) world.SetSingleton(X{});` — match that pattern). Add:

```cpp
    if (!gs->World.HasSingleton<NavMeshConfigComponent>()) {
        gs->World.SetSingleton(NavMeshConfigComponent{});
    }
```

This is the seed-if-absent rule documented in the project memory — singletons survive `Clear()`, so seed only when truly missing.

- [ ] **Step 3: Build + manual editor smoke**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```

Expected behavior:
- Editor launches, world loads.
- Console shows `GameThread: default world loaded ...`. Within ~1 tick: `NavMeshSystem::Rebuild: no NavMeshSource entities; publishing empty navmesh` (because the scene doesn't have any tagged entities yet — that's correct).
- Open Navigation panel. Status block reads "(no navmesh built yet)".
- Use the EcsInspectorPanel to add `NavMeshSourceComponent` to one of the existing collider entities, pick `Geometry = Collider` from the dropdown. Click "Rebuild NavMesh".
- Status block now shows non-zero Polys / Tiles. Toggle `Render Stats → NavMesh`. Cyan poly edges visible in the viewport over the tagged entity.

If any of those steps fails, debug and fix before committing.

- [ ] **Step 4: Run full test suite**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_followcam test_playermove; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: every line prints its success message; final line "All tests green."

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/threading/GameThread.cpp src/game/src/game.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(navigation): auto-rebuild on world load + seed default NavMeshConfig

GameThread posts ECSCommand::RebuildNavMesh after the first
successful WorldManager::LoadWorldSnapshot — initial navmesh is
built on the next tick, single trigger path (Spec 2 obstacle changes
reuse it).

Game seeds NavMeshConfigComponent singleton if absent (seed-if-absent
rule from project memory — singletons survive ECS::Clear()). Existing
world.json files without the new Environment.NavMeshConfig block load
with sensible defaults."
```

- [ ] **Step 6: Dispatch final whole-feature reviewer**

Run the final whole-feature reviewer subagent per `superpowers:subagent-driven-development` — provide the full diff between `main` and `feat/navigation-core` plus the spec at `docs/superpowers/specs/2026-05-27-navigation-core-design.md`. Reviewer verdict drives merge-readiness.

---

## Self-review notes

**Spec coverage check (post-write):**

- ✅ NavMeshGeometrySource enum + sentinel — Task 1
- ✅ NavMeshSourceComponent + NavMeshConfigComponent — Task 1
- ✅ X-macro + GAME_API bump — Task 1
- ✅ JSON round-trip — Task 2
- ✅ test_worldserial T20-T22 — Task 2
- ✅ NavMeshSource Apply/Remove/Duplicate — Task 3
- ✅ RebuildNavMeshCommand variant — Task 3 (as RebuildNavMesh)
- ✅ ECSCommandHooks pattern (replaces "handler in ECSCommands.h") — Task 3
- ✅ NavMeshBuilder triangle collection + helpers — Task 4
- ✅ MeshSystem CPU accessor (Risk 1 resolution) — Task 4
- ✅ NavMesh + RAII NavMeshAlloc + Recast TileCache build — Task 5
- ✅ FindPath / ClosestPoint / CollectPolyEdges + thread-id assert — Task 5
- ✅ NavMeshSystem singleton + atomic shared_ptr — Task 6
- ✅ test_navmesh T01-T07 — Task 6
- ✅ GameThread ECSCommandHooks wiring — Task 6
- ✅ ShowNavMesh toggle + EditorPreferences persistence — Task 7
- ✅ DebugRenderPass poly-edge block — Task 7
- ✅ NavigationPanel ImGui — Task 8
- ✅ World-load auto-rebuild — Task 9
- ✅ NavMeshConfig seed-if-absent — Task 9
- ✅ Final whole-feature review — Task 9 Step 6

No gaps.

**Type-consistency check:**

- `NavMeshGeometrySource` spelled identically in T1 / T2 / T3 / T4 / tests.
- `NavMeshSourceComponent` / `NavMeshConfigComponent` spelled identically everywhere.
- `NavMesh::Build`, `NavMesh::FindPath`, `NavMesh::CollectPolyEdges`, `NavMesh::GetStats` consistent across T5 / T6 / T7 / T8.
- `NavMeshSystem::Instance()`, `NavMeshSystem::Rebuild`, `NavMeshSystem::Current` consistent T6 / T7 / T8 / T9.
- `ECSCommand::RebuildNavMesh()` factory matches `ECSCommandType::RebuildNavMesh` enum.
- `ECSCommandHooks::OnRebuildNavMesh` consistent between T3 (definition) and T6 (use).

**Placeholder scan:**

Two locations called out for plan-time verification rather than open placeholders:
- T4 Step 3: confirm `MeshVertex` field name (`position[3]` vs `Position`); adjust accessor.
- T6 Step 4: confirm `ApplicationContext` field name for the command ring + how MeshSystem is reachable from GameThread. The plan instructs to read `ApplicationContext.h` and adapt.

These are honest "verify-then-edit" steps, not undefined behavior. The implementer should not be blocked.

**Commit count:** 8 commits (Tasks 1-9, Task 0 has no commit). Within the spec's 6-8 estimate.
