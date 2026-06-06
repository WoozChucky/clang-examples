# Registered Component Serialization (Boundary Piece 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `WorldManager`'s parallel explicit per-type save/load lists with a generic, registry-driven path so a game-defined component can persist to `world.json` by registering one (de)serializer — no engine edit.

**Architecture:** A single `ECS_API`-exported `ComponentSerializerRegistry` lives in `ecs.dll`, keyed by a stable string name (the `world.json` key). Each entry holds captureless function pointers (`has`/`save`/`load`) compiled in the registering module — so built-ins live in `ecs.dll` and game types (later) live in `Game.dll`, both sharing the one registry instance. Built-in persisted components self-register lazily. Exported `SaveEntityComponents`/`LoadEntityComponents` helpers drive per-entity (de)serialization generically; `WorldManager` calls them. The top-level `Environment` singleton block (Fog/Sky/DayNight/NavMeshConfig) stays bespoke.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, nlohmann/json. Builds on Piece 1 (header-instantiable `ECS::AddComponent<T>`/`GetComponent<T>`/`HasComponent<T>`, which make the registry's `<T>` lambdas instantiable in any module).

**Scope:** Piece 4 of the engine/game boundary spec (`docs/superpowers/specs/2026-06-06-engine-game-boundary-design.md`). Generalizes **per-entity** component persistence only; the bespoke `Environment` singleton block is unchanged (singleton generalization deferred — no consumer). No game component persists yet, so the registry is proven by a synthetic test type; **no `game.cpp` change** here.

**Deviation from the spec (intentional improvement):** the spec described generic save/load via new `IComponentArray` virtuals (`Name`/`SerializeEntity`/`DeserializeEntity`). This plan instead uses a **separate registry** of (de)serializer function pointers. Rationale: (1) keeps `nlohmann/json` out of `ECS.h` and the core `IComponentArray` interface (json stays a serialization concern, not an ECS-core one); (2) the registry naturally holds only the **persisted subset** — virtualizing every `ComponentArray<T>` would also rope in runtime-only types (InputStateComponent, cameras, ViewportComponent) that must NOT persist, requiring an extra opt-in flag anyway. The registry achieves the same "register one (de)serializer per type, generic save/load" goal and still feeds Piece 5's generic inspector (its entries produce/consume the JSON the inspector edits). (de)serializers come from nlohmann `to_json`/`from_json` via ADL — the same functions built-ins already define in `ComponentSerialization.h` — rather than explicit `serialize`/`deserialize` params, so a game type persists by defining `to_json`/`from_json` exactly like a built-in.

---

## Background facts (verified)

- `WorldManager::SaveWorldSnapshot` (`src/engine/src/utilities/WorldManager.cpp:14-107`) writes `j["Entities"]` where each entity object has `EntityId` + an explicit `if (world->HasComponent<T>(e)) jEntity["T"] = *GetComponent<T>(e);` block for **17 per-entity types**: Transform, Mesh, Material, Lightning, Text, SunMarker, Player, UIRect, StateScope, MenuButton, Collider, NavMeshSource, NavObstacle, NavAgent, NavTarget, NavConstrained, NavClass. Then a top-level `j["Environment"] = BuildEnvironmentJson(fog, sky, dayNight, navmesh)` for the 4 singletons.
- `LoadWorldSnapshot` (`:109-183`) mirrors it: `world->Clear()`, recreate each entity with `CreateEntity()` (the saved `EntityId` is **informational only — ignored**), `if (jEntity.contains("T")) AddComponent(e, jEntity["T"].get<T>())` for the same 17 (SunMarker/NavConstrained added as `T{}`), then `ParseEnvironmentJson` for singletons. Wrapped in try/catch; a corrupt file leaves the world intact.
- `ComponentSerialization.h` (`src/common/include/`) is self-contained (`nlohmann/json.hpp` + glm + `ECS.h`) and provides `to_json` **and** `from_json` for all 17 persisted types — including trivial ones for the zero-size `SunMarker` (`:117-118`) and `NavConstrainedComponent` (`:120-121`), so `j.get<SunMarker>()` compiles and yields a default. It also holds `BuildEnvironmentJson`/`ParseEnvironmentJson` (`:361`, `:386`) for the Environment block.
- `test_worldserial.cpp` tests the per-type `to_json`/`from_json` and Environment helpers **directly** (it links `ecs` + nlohmann + `Fog.cpp`, NOT `Engine`/`WorldManager`). So the `WorldManager` refactor cannot break it; a separate test covers the registry path.
- `ecs` target (`src/ecs/CMakeLists.txt`) compiles `src/ecs.cpp`, `src/systems.cpp`, `src/EcsLogSink.cpp`, links `CommonHeaders` + `glm::glm`, defines `ECS_EXPORTS`. It does **NOT** currently link nlohmann.
- Piece 1 made `ECS::AddComponent<T>`/`GetComponent<T>`/`HasComponent<T>` header-instantiable, so a registry lambda referencing them for a non-built-in `T` instantiates locally in the registering module.
- `ECS_API` is `dllexport` under `ECS_EXPORTS` (ecs.dll's TUs) and `dllimport` elsewhere (`ECS.h:29-39`).

## Type/symbol contract (keep exact)

- New header `src/common/include/ComponentSerializerRegistry.h`:
  - `struct ComponentSerializerEntry { std::string name; bool(*has)(const ECS&,EntityId); void(*save)(const ECS&,EntityId,nlohmann::json&); void(*load)(ECS&,EntityId,const nlohmann::json&); };`
  - `class ComponentSerializerRegistry { template<class T> void Register(const std::string& name); const std::vector<ComponentSerializerEntry>& Entries() const; const ComponentSerializerEntry* Find(const std::string& name) const; };` — `Register` **upserts** (replace by name).
  - `ECS_API ComponentSerializerRegistry& SerializerRegistry();`
  - `ECS_API void SaveEntityComponents(const ECS&, EntityId, nlohmann::json&);`
  - `ECS_API void LoadEntityComponents(ECS&, EntityId, const nlohmann::json&);`
- New source `src/ecs/src/ComponentSerializers.cpp`: defines the three exported functions + lazy built-in registration.

---

### Task 1: The serializer registry (built-ins + exported helpers) and its test

**Files:**
- Create: `src/common/include/ComponentSerializerRegistry.h`
- Create: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `src/ecs/CMakeLists.txt`
- Create: `tests/test_compserial.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_compserial.cpp`:
```cpp
#include <cstdio>
#include <cmath>
#include <nlohmann/json.hpp>
#include "ComponentSerializerRegistry.h"

// Test exe stub for SM_ASSERT's break hook (matches sibling tests like test_ecs.cpp).
void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 file ? file : "<unknown>", line, message ? message : "<none>", expr ? expr : "<none>");
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

// A synthetic game-style component defined OUTSIDE ecs.dll (in this TU) and NOT in the
// ECS X-macro — exactly the case Piece 4 must support. Serializers via nlohmann ADL.
struct PersistProbe { int A = 0; float B = 0.0f; };
inline void to_json(nlohmann::json& j, const PersistProbe& p)   { j = nlohmann::json{ {"A", p.A}, {"B", p.B} }; }
inline void from_json(const nlohmann::json& j, PersistProbe& p) { p.A = j.at("A").get<int>(); p.B = j.at("B").get<float>(); }

static void T01_builtins_registered()
{
    EXPECT(SerializerRegistry().Find("TransformComponent") != nullptr);
    EXPECT(SerializerRegistry().Find("StateScopeComponent") != nullptr);
    EXPECT(SerializerRegistry().Find("NavClassComponent")   != nullptr);
    EXPECT(SerializerRegistry().Find("NopeNotReal")         == nullptr);
}

static void T02_register_and_roundtrip_game_component()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe");

    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<PersistProbe>(e, PersistProbe{ 42, 1.5f });

    nlohmann::json jEntity;
    jEntity["EntityId"] = e;
    SaveEntityComponents(w, e, jEntity);
    EXPECT(jEntity.contains("PersistProbe"));
    EXPECT(jEntity["PersistProbe"]["A"].get<int>() == 42);

    ECS w2;
    const EntityId e2 = w2.CreateEntity();
    LoadEntityComponents(w2, e2, jEntity); // must skip "EntityId", load "PersistProbe"
    EXPECT(w2.HasComponent<PersistProbe>(e2));
    const PersistProbe* got = w2.GetComponent<PersistProbe>(e2);
    EXPECT(got != nullptr);
    EXPECT(got && got->A == 42);
    EXPECT(got && std::fabs(got->B - 1.5f) < 1e-6f);
}

static void T03_register_upserts_no_duplicate()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe");
    const size_t before = SerializerRegistry().Entries().size();
    SerializerRegistry().Register<PersistProbe>("PersistProbe"); // same name again
    EXPECT(SerializerRegistry().Entries().size() == before); // upsert, not append
}

int main()
{
    T01_builtins_registered();
    T02_register_and_roundtrip_game_component();
    T03_register_upserts_no_duplicate();
    if (g_Failures == 0) { std::printf("All component-serializer tests passed.\n"); return 0; }
    std::printf("%d component-serializer test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the test target to `tests/CMakeLists.txt`** (append):
```cmake
add_executable(test_compserial
    test_compserial.cpp
)

target_link_libraries(test_compserial PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_compserial PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_compserial PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_compserial PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build — expect RED**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_compserial
```
Expected: build error `Cannot open include file: 'ComponentSerializerRegistry.h'`. (Reconfigure needed for the new target.)

- [ ] **Step 4: Create the registry header**

Create `src/common/include/ComponentSerializerRegistry.h`:
```cpp
#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "ECS.h"   // ECS, EntityId, ECS_API

// Registry of per-component (de)serializers keyed by a stable string name (the world.json
// key). Built-in component types register once in ecs.dll; Game.dll registers its own types
// additively — header-instantiable ECS<T> methods (boundary Piece 1) make the <T> lambdas
// compile in whatever module registers the type. A single exported instance is shared by all
// modules.
//
// Entry function pointers are CAPTURELESS lambdas compiled in the registering module:
//   has(world, e)       -> does entity e have this component?
//   save(world, e, out) -> out = the component's json value (caller keys it by `name`)
//   load(world, e, in)  -> AddComponent<T>(e, in.get<T>())
struct ComponentSerializerEntry {
    std::string name;
    bool (*has)(const ECS&, EntityId);
    void (*save)(const ECS&, EntityId, nlohmann::json&);
    void (*load)(ECS&, EntityId, const nlohmann::json&);
};

class ComponentSerializerRegistry {
public:
    // Upsert: re-registering an existing name REPLACES its function pointers. Required for
    // Game.dll hot-reload — a reloaded DLL re-registers its types so the registry never holds
    // dangling pointers into the unloaded module.
    template <class T>
    void Register(const std::string& name) {
        ComponentSerializerEntry e{
            name,
            [](const ECS& w, EntityId en)                        { return w.HasComponent<T>(en); },
            [](const ECS& w, EntityId en, nlohmann::json& out)   { out = *w.GetComponent<T>(en); },
            [](ECS& w, EntityId en, const nlohmann::json& in)    { w.AddComponent<T>(en, in.template get<T>()); }
        };
        for (auto& existing : m_Entries) {
            if (existing.name == name) { existing = e; return; }
        }
        m_Entries.push_back(std::move(e));
    }

    [[nodiscard]] const std::vector<ComponentSerializerEntry>& Entries() const { return m_Entries; }

    [[nodiscard]] const ComponentSerializerEntry* Find(const std::string& name) const {
        for (const auto& e : m_Entries) if (e.name == name) return &e;
        return nullptr;
    }

private:
    std::vector<ComponentSerializerEntry> m_Entries;
};

// Single process-wide instance (defined + exported from ecs.dll). Lazily registers all
// built-in persisted component serializers on first use.
ECS_API ComponentSerializerRegistry& SerializerRegistry();

// Generic per-entity (de)serialization used by WorldManager (and tests). Save writes each
// present component under its registered name into `jEntity`. Load reads every key (skipping
// "EntityId") through the registry, warning on an unknown component key.
ECS_API void SaveEntityComponents(const ECS& world, EntityId e, nlohmann::json& jEntity);
ECS_API void LoadEntityComponents(ECS& world, EntityId e, const nlohmann::json& jEntity);
```

- [ ] **Step 5: Create the ecs.dll definition file**

Create `src/ecs/src/ComponentSerializers.cpp`:
```cpp
#include "ComponentSerializerRegistry.h"
#include "ComponentSerialization.h"  // built-in to_json/from_json for the persisted types
#include "lib.h"                      // SM_WARN

ComponentSerializerRegistry& SerializerRegistry() {
    static ComponentSerializerRegistry reg = [] {
        ComponentSerializerRegistry r;
        // The persisted built-in set (mirrors WorldManager's previous explicit lists).
        r.Register<TransformComponent>("TransformComponent");
        r.Register<MeshComponent>("MeshComponent");
        r.Register<MaterialComponent>("MaterialComponent");
        r.Register<LightningComponent>("LightningComponent");
        r.Register<TextComponent>("TextComponent");
        r.Register<SunMarker>("SunMarker");
        r.Register<PlayerComponent>("PlayerComponent");
        r.Register<UIRectComponent>("UIRectComponent");
        r.Register<StateScopeComponent>("StateScopeComponent");
        r.Register<MenuButtonComponent>("MenuButtonComponent");
        r.Register<ColliderComponent>("ColliderComponent");
        r.Register<NavMeshSourceComponent>("NavMeshSourceComponent");
        r.Register<NavObstacleComponent>("NavObstacleComponent");
        r.Register<NavAgentComponent>("NavAgentComponent");
        r.Register<NavTargetComponent>("NavTargetComponent");
        r.Register<NavConstrainedComponent>("NavConstrainedComponent");
        r.Register<NavClassComponent>("NavClassComponent");
        return r;
    }();
    return reg;
}

void SaveEntityComponents(const ECS& world, EntityId e, nlohmann::json& jEntity) {
    for (const auto& en : SerializerRegistry().Entries())
        if (en.has(world, e)) en.save(world, e, jEntity[en.name]);
}

void LoadEntityComponents(ECS& world, EntityId e, const nlohmann::json& jEntity) {
    for (auto it = jEntity.begin(); it != jEntity.end(); ++it) {
        if (it.key() == "EntityId") continue;
        if (const auto* en = SerializerRegistry().Find(it.key()))
            en->load(world, e, it.value());
        else
            SM_WARN("LoadEntityComponents: no serializer for component '%s' — skipped", it.key().c_str());
    }
}
```

- [ ] **Step 6: Wire the ecs target (`src/ecs/CMakeLists.txt`)**

Add the new source to the `add_library(ecs SHARED ...)` list:
```cmake
add_library(ecs SHARED
    src/ecs.cpp
    src/systems.cpp
    src/EcsLogSink.cpp
    src/ComponentSerializers.cpp
)
```
And add nlohmann to its link libraries (the registry header includes `nlohmann/json.hpp`):
```cmake
target_link_libraries(ecs PUBLIC
    CommonHeaders
    glm::glm
    nlohmann_json::nlohmann_json
)
```

- [ ] **Step 7: Build + run — expect GREEN**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_compserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
```
Expected: `All component-serializer tests passed.` (exit 0). Watch for `LNK` errors on `SerializerRegistry`/`SaveEntityComponents`/`LoadEntityComponents` (would mean the `ECS_API` export/import or the new ecs source wasn't picked up — confirm Step 6 added the file and the reconfigure ran).

- [ ] **Step 8: Commit**

```
git -C /c/dev/clang-examples add src/common/include/ComponentSerializerRegistry.h src/ecs/src/ComponentSerializers.cpp src/ecs/CMakeLists.txt tests/test_compserial.cpp tests/CMakeLists.txt
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(serial): exported component-serializer registry + generic Save/LoadEntityComponents"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists exactly those five files.

---

### Task 2: Route `WorldManager` through the generic helpers

**Files:**
- Modify: `src/engine/src/utilities/WorldManager.cpp`

- [ ] **Step 1: Replace the explicit per-entity save block**

In `src/engine/src/utilities/WorldManager.cpp`:
- Add include near the top (after the existing `#include "ComponentSerialization.h"`):
```cpp
#include "ComponentSerializerRegistry.h"
```
- In `SaveWorldSnapshot`, replace the 17 explicit `if (world->HasComponent<T>...) jEntity["T"] = ...;` lines (`:34-84`) with a single call, leaving the `jEntity["EntityId"] = entity;` line above and the `j["Entities"].push_back(jEntity);` line below intact:
```cpp
        jEntity["EntityId"] = entity;

        // Generic: every registered (built-in + game-registered) component present on this
        // entity is written under its registered name. See ComponentSerializerRegistry.h.
        SaveEntityComponents(*world, entity, jEntity);

        j["Entities"].push_back(jEntity);
```
- Leave the `Environment` block (`:89-100`) and the file write unchanged.

- [ ] **Step 2: Replace the explicit per-entity load block**

In `LoadWorldSnapshot`, replace the 17 explicit `if (jEntity.contains("T")) world->AddComponent(...)` lines (`:129-162`) with a single call, keeping the `CreateEntity()` line and the `Environment` parse below intact:
```cpp
        for (const auto& jEntity : j.at("Entities")) {
            const EntityId createdEntity = world->CreateEntity();
            // Generic: load every component key (skips "EntityId", warns on unknown keys).
            LoadEntityComponents(*world, createdEntity, jEntity);
        }
```
- Leave the `ParseEnvironmentJson` block (`:165-171`) and the `try/catch` + `NavMeshSystem::SetWorldPath` unchanged.

- [ ] **Step 3: Full build (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean build of all targets. `WorldManager.cpp` no longer references the 17 component types directly (only `BuildEnvironmentJson`/`ParseEnvironmentJson` + the generic helpers). No errors.

- [ ] **Step 4: Regression suites**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All component-serializer tests passed.`, `All world-serialization tests passed.`, `All ECS tests passed.`

- [ ] **Step 5: Manual world round-trip smoke (editor)**

Launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`. Confirm:
- The default world loads and renders as before (meshes, lights, menu UI present) — proves generic LOAD reproduces every built-in component.
- Use **File → Save** (or trigger a save), then reload the scene (File → Reload) and confirm the scene is unchanged — proves generic SAVE writes every built-in component round-trip.
Close the editor. (`world.json` is gitignored runtime state — do not commit it.)

- [ ] **Step 6: Commit**

```
git -C /c/dev/clang-examples add src/engine/src/utilities/WorldManager.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "refactor(serial): WorldManager per-entity save/load via the generic registry"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists only `WorldManager.cpp`.

---

## Done criteria

- A component type defined outside `ecs.dll` round-trips through `SaveEntityComponents`/`LoadEntityComponents` after a single `SerializerRegistry().Register<T>("Name")`; built-ins are present; `Register` upserts (Task 1).
- `WorldManager` save/load drive per-entity (de)serialization through the registry; the explicit 17-type lists are gone; the `Environment` singleton block is unchanged; default world round-trips in the editor (Task 2).
- Full tree builds; `test_compserial`/`test_worldserial`/`test_ecs` green.

## Known limitations / notes

- **Hot-reload safety:** `Register` upsert refreshes function pointers on each `Game.dll` load, which covers re-registration. A game type whose registration is *removed* across a reload would leave a stale entry; a proper `Unregister`-on-unload (mirroring `NetSubsystem::ReleaseGameResidentConnections`) is deferred until a real game component persists. Documented because it bites only when game-component persistence lands.
- **Singletons:** the `Environment` block stays bespoke. Generalizing singleton persistence through the registry is deferred (no consumer; would restructure `world.json`).
- **Cross-DLL json:** entry `save`/`load` pass `nlohmann::json&` across module boundaries (Engine → ecs.dll → Game.dll). Safe because all modules share one fetched nlohmann version + one CRT, the same assumption the codebase already relies on for `WorldManager`.

## Next plan (not this one)

Piece 5 (generic inspector editing — reuses this registry's (de)serializers to render/edit any component as JSON in the editor, and replaces the hardcoded `StateScopeEditor` mirror with a registered name table). Its own plan.
