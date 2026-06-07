# Game-Component Hot-Reload Survival Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a game-defined (`Game.dll`-owned) ECS component retain its per-entity values across a `Game.cpp` hot-reload, by extracting non-builtin component data to a DLL-neutral host blob before `FreeLibrary` and restoring it after the new DLL re-registers its types.

**Architecture:** At the GameThread reload barrier (where game-defined component arrays are already cleared), first **extract** each registered non-builtin component into a host-owned blob (raw bytes for trivially-copyable types, json for serializable ones), clear + reload as today, have the new DLL re-register its component types via a new `GameRegisterComponents` export, then **restore** the blob onto the same `EntityId`s. Disk persistence (`world.json`) is kept on a separate path so POD components without a serializer don't leak binary blobs into the scene file.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-enterprise`), CMake, nlohmann/json. Builds on the serializer registry (`ComponentSerializerRegistry.h`) and the reload barrier (`GameThread.cpp`).

**Scope:** Implements `docs/superpowers/specs/2026-06-07-game-component-reload-survival-design.md`. Touches an ecs.dll/common header (`ComponentSerializerRegistry.h`) and adds a `Game.dll` export → rebuild **ecs + Engine + game + editor** and restart the editor (`GAME_API_VERSION` bump).

---

## Background facts (verified)

- `ComponentSerializerEntry` (`src/common/include/ComponentSerializerRegistry.h`) currently ends with `bool builtin;` then `bool (*editorDraw)(const EditorUI&, nlohmann::json&) = nullptr;` (added by the editor-hook work). `Register<T>` builds the entry and upserts by name (replacing the whole entry). The registry singleton `SerializerRegistry()` is exported from ecs.dll. Free functions `SaveEntityComponents`/`LoadEntityComponents` + the singleton are `ECS_API`.
- `Register<T>` today unconditionally builds json `save`/`load` lambdas (`out = *w.GetComponent<T>(en)`, `w.AddComponent<T>(en, in.template get<T>())`), so it only compiles for types with ADL `to_json`/`from_json`. This must become conditional (POD components may have no serializer).
- `ECS::RemoveNonBuiltinComponentArrays()` (`src/ecs/src/ecs.cpp:124`) drops arrays whose `type_index` is not in `BuiltinComponentTypes()` (the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro set). Entities are **not** destroyed.
- Reload barrier: `src/engine/src/threading/GameThread.cpp:212-237`. The clear is at line 227; extract (before) and restore (after `LoadOrReload`) live in this same block. Shutdown path at line 587 stays clear-only.
- `GameLibrary::LoadOrReload` (`src/engine/src/threading/GameLibrary.cpp:35-119`) resolves optional exports by name (`GetProcAddress`) and null-checks before calling — see `GameRegisterSystems` at lines 84-85, 103, 114-117. New optional exports follow the same pattern.
- Game exports declared in `src/game/include/game.h` (inside `extern "C"`, `EXPORT_FN`) and defined in `src/game/src/game.cpp` (e.g. `GameRegisterSystems` at line 906, plainly — linkage comes from the header). `GAME_API_VERSION` is `21u` (`game.h:24`). No game-owned components are registered today (`GameRegisterComponents` will be the first, and starts empty).
- `ECS` API used here: `CreateEntity()`, `GetActiveEntities()` → `const std::vector<EntityId>&`, `IsValidEntity(EntityId)`, `AddComponent<T>(EntityId, T)`, `GetComponent<T>(EntityId) const` → `const T*`, `HasComponent<T>(EntityId) const`.
- Test harness pattern: `tests/test_compserial.cpp` provides `platform_debug_break` stub + `EXPECT` macro + a synthetic out-of-dll component `PersistProbe` (has `to_json`/`from_json`, and is trivially copyable). Its CMake target (`tests/CMakeLists.txt:680-704`) links `CommonHeaders glm::glm ecs nlohmann_json::nlohmann_json`.

## Type/symbol contract (keep exact)

- `ComponentSerializerRegistry.h`: add a `JsonSerializable<T>` concept; add two LAST members to `ComponentSerializerEntry`:
  ```cpp
  void (*reloadExtract)(const ECS&, EntityId, std::vector<std::byte>&) = nullptr;
  void (*reloadIngest)(ECS&, EntityId, const std::vector<std::byte>&)  = nullptr;
  ```
  add free functions:
  ```cpp
  struct PreservedComponent { std::string name; EntityId entity; bool useBytes; nlohmann::json json; std::vector<std::byte> bytes; };
  ECS_API std::vector<PreservedComponent> PreserveNonBuiltinComponents(const ECS& world);
  ECS_API void RestoreNonBuiltinComponents(ECS& world, const std::vector<PreservedComponent>& blob);
  ```
- `game.h`: new export `EXPORT_FN void GameRegisterComponents();` + `using GameRegisterComponentsFunc = void(*)();`; bump `GAME_API_VERSION` to `22u`.

---

### Task 1: Registry — conditional `Register<T>` + byte fn-ptrs + concept + unit test

**Files:**
- Modify: `src/common/include/ComponentSerializerRegistry.h`
- Modify: `tests/test_compserial.cpp`

- [ ] **Step 1: Write the failing test (extend `tests/test_compserial.cpp`)**

Add these includes near the top (after the existing includes):
```cpp
#include <cstddef> // std::byte
#include <string>
```
Add two new synthetic probes after the existing `PersistProbe` definition (around line 22):
```cpp
// Trivially-copyable, NO to_json/from_json → byte (memcpy) preservation only.
struct PodOnlyProbe { int X = 0; float Y = 0.0f; };

// Has heap member → NOT trivially-copyable; has to_json/from_json → json preservation only.
struct StrProbe { std::string S; int N = 0; };
inline void to_json(nlohmann::json& j, const StrProbe& p)   { j = nlohmann::json{ {"S", p.S}, {"N", p.N} }; }
inline void from_json(const nlohmann::json& j, StrProbe& p) { p.S = j.at("S").get<std::string>(); p.N = j.at("N").get<int>(); }
```
Add the test after `T07_register_editor_hook` (or after `T06` if the editor-hook test is absent):
```cpp
static void T08_preservation_strategy_selection()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe"); // POD + to_json → BOTH paths
    SerializerRegistry().Register<PodOnlyProbe>("PodOnlyProbe"); // POD, no to_json → byte only
    SerializerRegistry().Register<StrProbe>("StrProbe");         // complex + to_json → json only

    const auto* pp = SerializerRegistry().Find("PersistProbe");
    const auto* pod = SerializerRegistry().Find("PodOnlyProbe");
    const auto* str = SerializerRegistry().Find("StrProbe");
    EXPECT(pp && pp->save != nullptr && pp->reloadExtract != nullptr);   // both
    EXPECT(pod && pod->save == nullptr && pod->reloadExtract != nullptr); // byte only
    EXPECT(str && str->save != nullptr && str->reloadExtract == nullptr); // json only

    // Builtins: serializable + (mostly) trivially copyable — save present.
    EXPECT(SerializerRegistry().Find("TransformComponent")->save != nullptr);
}
```
Register `T08_preservation_strategy_selection();` in `main()`.

- [ ] **Step 2: Build — expect RED**

Run:
```
cmake --build --preset msvc-win64-vs2026-enterprise --target test_compserial
```
Expected: compile errors — `reloadExtract` is not a member of `ComponentSerializerEntry` (and the `pod->save == nullptr` assumption can't hold while `Register` still unconditionally sets `save`).

- [ ] **Step 3: Add the concept + entry fields (`ComponentSerializerRegistry.h`)**

Add includes at the top (with the existing `#include <nlohmann/json.hpp>`):
```cpp
#include <cstddef>   // std::byte
#include <type_traits>
#include <vector>
```
After the `struct EditorUI;` forward declaration, add the concept:
```cpp
// True when nlohmann can round-trip T via ADL to_json/from_json (i.e. T has a serializer).
// Used to decide whether Register<T> installs the json save/load path.
template <class T>
concept JsonSerializable = requires(nlohmann::json& j, const T& cv, T& v) {
    { j = cv };
    { v = j.template get<T>() };
};
```
Add the two new members as the LAST members of `ComponentSerializerEntry` (after `editorDraw`):
```cpp
    // Reload-preservation (byte path), distinct from the json save/load used for world.json disk
    // persistence. Auto-installed for trivially-copyable T. Null => no byte path for this type.
    void (*reloadExtract)(const ECS&, EntityId, std::vector<std::byte>&) = nullptr; // memcpy T out
    void (*reloadIngest)(ECS&, EntityId, const std::vector<std::byte>&)  = nullptr; // memcpy T in + AddComponent
```

- [ ] **Step 4: Rewrite `Register<T>` to be conditional (`ComponentSerializerRegistry.h`)**

Replace the entire `Register` method body with field-assignment + `if constexpr` branches:
```cpp
    template <class T>
    void Register(const std::string& name, bool builtin = false) {
        ComponentSerializerEntry e{};
        e.name       = name;
        e.has        = [](const ECS& w, EntityId en)                      { return w.HasComponent<T>(en); };
        e.addDefault = [](ECS& w, EntityId en)                           { w.AddComponent<T>(en, T{}); };
        e.remove     = [](ECS& w, EntityId en)                           { w.RemoveComponent<T>(en); };
        e.builtin    = builtin;

        if constexpr (JsonSerializable<T>) {
            e.save = [](const ECS& w, EntityId en, nlohmann::json& out) { out = *w.GetComponent<T>(en); };
            e.load = [](ECS& w, EntityId en, const nlohmann::json& in)  { w.AddComponent<T>(en, in.template get<T>()); };
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            e.reloadExtract = [](const ECS& w, EntityId en, std::vector<std::byte>& out) {
                const T* p = w.GetComponent<T>(en);
                if (!p) { out.clear(); return; }
                out.resize(sizeof(T));
                std::memcpy(out.data(), p, sizeof(T));
            };
            e.reloadIngest = [](ECS& w, EntityId en, const std::vector<std::byte>& in) {
                if (in.size() != sizeof(T)) return;
                T t{};
                std::memcpy(&t, in.data(), sizeof(T));
                w.AddComponent<T>(en, std::move(t));
            };
        }

        for (auto& existing : m_Entries) {
            if (existing.name == name) { existing = e; return; }
        }
        m_Entries.push_back(std::move(e));
    }
```
Add `#include <cstring>` at the top for `std::memcpy`.

(Note: the upsert still replaces the whole entry, which clears any `editorDraw` set via `RegisterEditorHook` — unchanged from prior behavior; a game re-registers its editor hook after re-registering the type.)

- [ ] **Step 5: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs --target test_compserial
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
```
Expected: `All component-serializer tests passed.` If the `JsonSerializable` concept wrongly resolves `true` for `PodOnlyProbe` (nlohmann edge), the `pod->save == nullptr` assert fails — in that case replace the concept's `{ j = cv };` with an explicit ADL probe: `template<class T> concept JsonSerializable = requires(nlohmann::json& j, const T& cv, T& v){ to_json(j, cv); from_json(static_cast<const nlohmann::json&>(j), v); };`.

- [ ] **Step 6: Commit**
```
git -C C:/dev/personal/clang-examples add src/common/include/ComponentSerializerRegistry.h tests/test_compserial.cpp
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): conditional Register<T> + byte reload-preservation fn-ptrs + JsonSerializable concept"
```
Verify exactly those two files.

---

### Task 2: `PreserveNonBuiltinComponents` / `RestoreNonBuiltinComponents` + round-trip test

**Files:**
- Modify: `src/common/include/ComponentSerializerRegistry.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Create: `tests/test_reloadpreserve.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test (`tests/test_reloadpreserve.cpp`)**
```cpp
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>
#include "ComponentSerializerRegistry.h"

// Test exe stub for SM_ASSERT's break hook (matches sibling tests).
void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    std::fprintf(stderr, "ASSERT FAIL %s:%d: %s (expr: %s)\n",
                 file ? file : "<unknown>", line, message ? message : "<none>", expr ? expr : "<none>");
    std::abort();
}

static int g_Failures = 0;
#define EXPECT(c) do { if(!(c)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); ++g_Failures; } } while(0)

// POD game component (no to_json) → byte path.
struct PodComp { int A = 0; float B = 0.0f; };
// Complex game component (heap member) → json path.
struct CplxComp { std::string S; int N = 0; };
inline void to_json(nlohmann::json& j, const CplxComp& p)   { j = nlohmann::json{ {"S", p.S}, {"N", p.N} }; }
inline void from_json(const nlohmann::json& j, CplxComp& p) { p.S = j.at("S").get<std::string>(); p.N = j.at("N").get<int>(); }
// Unpreservable: heap member, no to_json → neither path.
struct BadComp { std::string S; };

static void T01_byte_and_json_roundtrip_survive_clear()
{
    SerializerRegistry().Register<PodComp>("PodComp");
    SerializerRegistry().Register<CplxComp>("CplxComp");

    ECS w;
    const EntityId e1 = w.CreateEntity();
    const EntityId e2 = w.CreateEntity();
    w.AddComponent<PodComp>(e1, PodComp{ 7, 1.5f });
    w.AddComponent<CplxComp>(e2, CplxComp{ "hello", 9 });

    auto blob = PreserveNonBuiltinComponents(w);
    EXPECT(blob.size() == 2);

    w.RemoveNonBuiltinComponentArrays();
    EXPECT(!w.HasComponent<PodComp>(e1));
    EXPECT(!w.HasComponent<CplxComp>(e2));

    RestoreNonBuiltinComponents(w, blob);
    EXPECT(w.HasComponent<PodComp>(e1));
    EXPECT(w.HasComponent<CplxComp>(e2));
    const PodComp*  p = w.GetComponent<PodComp>(e1);
    const CplxComp* c = w.GetComponent<CplxComp>(e2);
    EXPECT(p && p->A == 7 && std::fabs(p->B - 1.5f) < 1e-6f);
    EXPECT(c && c->S == "hello" && c->N == 9);
}

static void T02_unpreservable_is_skipped()
{
    SerializerRegistry().Register<BadComp>("BadComp"); // neither path

    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<BadComp>(e, BadComp{ "x" });

    auto blob = PreserveNonBuiltinComponents(w); // warns about BadComp
    for (const auto& pc : blob) EXPECT(pc.name != "BadComp"); // not preserved

    w.RemoveNonBuiltinComponentArrays();
    RestoreNonBuiltinComponents(w, blob);
    EXPECT(!w.HasComponent<BadComp>(e)); // dropped, as designed
}

static void T03_builtins_untouched_by_preserve()
{
    ECS w;
    const EntityId e = w.CreateEntity();
    w.AddComponent<TransformComponent>(e, TransformComponent{});
    auto blob = PreserveNonBuiltinComponents(w);
    for (const auto& pc : blob) EXPECT(pc.name != "TransformComponent"); // builtin excluded
}

int main()
{
    T01_byte_and_json_roundtrip_survive_clear();
    T02_unpreservable_is_skipped();
    T03_builtins_untouched_by_preserve();
    if (g_Failures == 0) { std::printf("All reload-preservation tests passed.\n"); return 0; }
    std::printf("%d reload-preservation test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Add the test target (`tests/CMakeLists.txt`)**

Append (mirrors the `test_compserial` block):
```cmake
add_executable(test_reloadpreserve
    test_reloadpreserve.cpp
)

target_link_libraries(test_reloadpreserve PRIVATE
    CommonHeaders
    glm::glm
    ecs
    nlohmann_json::nlohmann_json
)

target_include_directories(test_reloadpreserve PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

target_compile_definitions(test_reloadpreserve PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_reloadpreserve PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Build — expect RED**
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise --target test_reloadpreserve
```
Expected: link error — unresolved `PreserveNonBuiltinComponents` / `RestoreNonBuiltinComponents` (and the `PreservedComponent` struct missing if the header decl isn't added yet).

- [ ] **Step 4: Declare the API (`ComponentSerializerRegistry.h`)**

After the `SaveEntityComponents`/`LoadEntityComponents` declarations, add:
```cpp
// One preserved component instance captured across a Game.dll reload. Either `bytes` (POD/byte
// path) or `json` (serializer path) is populated per `useBytes`. Stored in the shared CRT heap so
// it survives FreeLibrary of the module that produced it.
struct PreservedComponent {
    std::string            name;
    EntityId               entity = 0;
    bool                   useBytes = false;
    nlohmann::json         json;
    std::vector<std::byte> bytes;
};

// Capture every REGISTERED non-builtin component on every active entity into a DLL-neutral blob.
// Byte path preferred when available (trivially-copyable), else json. A registered non-builtin with
// neither strategy is SM_WARN'd and skipped (it will be cleared). Call while the defining DLL is
// still mapped.
ECS_API std::vector<PreservedComponent> PreserveNonBuiltinComponents(const ECS& world);

// Re-create the captured components on their original entities. Looks each component up by name in
// the (freshly re-registered) registry; a name no longer registered is SM_WARN'd and skipped. Call
// after the new DLL has re-registered its component types.
ECS_API void RestoreNonBuiltinComponents(ECS& world, const std::vector<PreservedComponent>& blob);
```

- [ ] **Step 5: Define the functions (`src/ecs/src/ComponentSerializers.cpp`)**

Add after `LoadEntityComponents` (the file already includes `ComponentSerializerRegistry.h` + `lib.h`):
```cpp
std::vector<PreservedComponent> PreserveNonBuiltinComponents(const ECS& world) {
    std::vector<PreservedComponent> out;
    for (const auto& entry : SerializerRegistry().Entries()) {
        if (entry.builtin) continue;
        for (const EntityId e : world.GetActiveEntities()) {
            if (!entry.has(world, e)) continue;
            if (entry.reloadExtract) {
                PreservedComponent pc;
                pc.name = entry.name; pc.entity = e; pc.useBytes = true;
                entry.reloadExtract(world, e, pc.bytes);
                out.push_back(std::move(pc));
            } else if (entry.save) {
                PreservedComponent pc;
                pc.name = entry.name; pc.entity = e; pc.useBytes = false;
                entry.save(world, e, pc.json);
                out.push_back(std::move(pc));
            } else {
                SM_WARN("PreserveNonBuiltinComponents: '%s' not preservable "
                        "(non-trivially-copyable, no serializer) — dropped on reload", entry.name.c_str());
            }
        }
    }
    return out;
}

void RestoreNonBuiltinComponents(ECS& world, const std::vector<PreservedComponent>& blob) {
    for (const auto& pc : blob) {
        if (!world.IsValidEntity(pc.entity)) {
            SM_WARN("RestoreNonBuiltinComponents: entity %llu no longer valid — '%s' dropped",
                    pc.entity, pc.name.c_str());
            continue;
        }
        const auto* entry = SerializerRegistry().Find(pc.name);
        if (!entry) {
            SM_WARN("RestoreNonBuiltinComponents: '%s' not registered after reload — dropped", pc.name.c_str());
            continue;
        }
        if (pc.useBytes && entry->reloadIngest) entry->reloadIngest(world, pc.entity, pc.bytes);
        else if (!pc.useBytes && entry->load)   entry->load(world, pc.entity, pc.json);
        else SM_WARN("RestoreNonBuiltinComponents: '%s' has no matching strategy after reload — dropped", pc.name.c_str());
    }
}
```

- [ ] **Step 6: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target ecs --target test_reloadpreserve
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_reloadpreserve.exe
```
Expected: `All reload-preservation tests passed.` (a single `WARN: ... 'BadComp' not preservable ...` line is expected from T02).

- [ ] **Step 7: Commit**
```
git -C C:/dev/personal/clang-examples add src/common/include/ComponentSerializerRegistry.h src/ecs/src/ComponentSerializers.cpp tests/test_reloadpreserve.cpp tests/CMakeLists.txt
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): PreserveNonBuiltinComponents/RestoreNonBuiltinComponents + round-trip test"
```
Verify exactly those four files.

---

### Task 3: `GameRegisterComponents` export + GameLibrary hook + API bump

**Files:**
- Modify: `src/game/include/game.h`
- Modify: `src/game/src/game.cpp`
- Modify: `src/engine/src/threading/GameLibrary.h`
- Modify: `src/engine/src/threading/GameLibrary.cpp`

Build-verified (no unit test — exercised by Task 4 + the existing suites).

- [ ] **Step 1: Declare the export + bump version (`src/game/include/game.h`)**

Change the version line:
```cpp
#define GAME_API_VERSION 22u
```
Add the typedef alongside the other `using ...Func` lines (after `GameRegisterSystemsFunc`):
```cpp
using GameRegisterComponentsFunc = void(*)();
```
Add the export inside the `extern "C"` block (after `GameRegisterSystems`):
```cpp
    // Re-register game-owned ECS component types (serializers + reload-preservation strategies).
    // Called by the host after every (re)load, before reload-preserved components are restored.
    EXPORT_FN void GameRegisterComponents();
```

- [ ] **Step 2: Implement the export (`src/game/src/game.cpp`)**

Add after `GameRegisterSystems` (around line 924). It is intentionally empty until the game introduces its first owned component; the comment documents the one-line pattern:
```cpp
void GameRegisterComponents() {
    // Register each game-owned (non-builtin) component here so it (de)serializes to world.json
    // (if it has to_json/from_json) and survives Game.dll hot-reload. Example:
    //   SerializerRegistry().Register<MyGameComponent>("MyGameComponent");
    // POD components need no to_json — the registry installs a byte (memcpy) reload path
    // automatically. Re-runs on every reload; Register upserts, replacing stale fn-ptrs.
}
```
Add `#include "ComponentSerializerRegistry.h"` near the top of `game.cpp` if not already included (needed for `SerializerRegistry()` once components are added; harmless now).

- [ ] **Step 3: Resolve + call the export (`src/engine/src/threading/GameLibrary.h`)**

Add a member next to `m_pGameRegisterSystems`:
```cpp
    GameRegisterComponentsFunc m_pGameRegisterComponents = nullptr;
```

- [ ] **Step 4: Resolve + call the export (`src/engine/src/threading/GameLibrary.cpp`)**

Resolve it next to `pRegisterSystems` (after line 85):
```cpp
    auto pRegisterComponents = reinterpret_cast<GameRegisterComponentsFunc>(
        GetProcAddress(newModule, "GameRegisterComponents"));
```
Assign it next to `m_pGameRegisterSystems = pRegisterSystems;` (line 103):
```cpp
    m_pGameRegisterComponents = pRegisterComponents;
```
Call it right after the `GameRegisterSystems` block (after line 117), before `return true;`. Component types must be registered before the GameThread restores preserved data:
```cpp
    if (m_pGameRegisterComponents) {
        m_pGameRegisterComponents();
        SM_TRACE("GameLibrary: registered game component types");
    }
```
Null the pointer in `Unload` next to `m_pGameRegisterSystems = nullptr;` (around line 132):
```cpp
    m_pGameRegisterComponents = nullptr;
```

- [ ] **Step 5: Build (green) — game + Engine**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target game --target Engine
```
Expected: both build clean. (`GameLibrary` is part of `Engine`.)

- [ ] **Step 6: Commit**
```
git -C C:/dev/personal/clang-examples add src/game/include/game.h src/game/src/game.cpp src/engine/src/threading/GameLibrary.h src/engine/src/threading/GameLibrary.cpp
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(reload): GameRegisterComponents export + GameLibrary hook + GAME_API_VERSION 22"
```
Verify exactly those four files.

---

### Task 4: Wire extract → restore into the reload barrier

**Files:**
- Modify: `src/engine/src/threading/GameThread.cpp`

Build-verified.

- [ ] **Step 1: Add the include (`src/engine/src/threading/GameThread.cpp`)**

Ensure `#include "ComponentSerializerRegistry.h"` is present near the other includes at the top (add it if absent).

- [ ] **Step 2: Extract before clear, restore after reload (`GameThread.cpp:~224-233`)**

Replace this existing block:
```cpp
			// Drop game-defined arrays from the master, then replace the published snapshot
			// (which shares those array objects) with a built-in-only one. Both releases
			// destroy the ComponentArray<GameType> objects HERE on the GameThread, DLL mapped.
			gameState.World.RemoveNonBuiltinComponentArrays();
			m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(),
			                                        std::memory_order_release);

			if (m_GameLib.LoadOrReload("Game.dll", &gameState)) {
				SM_TRACE("GameThread: Game.dll reloaded successfully");
			}
			// On failure, GameLibrary already logged and kept the previous module.
```
with:
```cpp
			// Capture game-defined component data (byte/json) into a host-owned blob WHILE the old
			// DLL is still mapped (the extract fn-ptrs are old-DLL code). Then drop the arrays and
			// publish a built-in-only snapshot. Both releases destroy the ComponentArray<GameType>
			// objects HERE on the GameThread, DLL mapped.
			auto preserved = PreserveNonBuiltinComponents(gameState.World);
			gameState.World.RemoveNonBuiltinComponentArrays();
			m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(),
			                                        std::memory_order_release);

			if (m_GameLib.LoadOrReload("Game.dll", &gameState)) {
				SM_TRACE("GameThread: Game.dll reloaded successfully");
			}
			// On failure, GameLibrary already logged and kept the previous module.

			// Restore the captured components onto their original entities. LoadOrReload has already
			// called GameRegisterComponents on the new module, so the registry now holds fresh
			// (new-DLL) ingest/load fn-ptrs. Republish so the RenderThread sees the restored world.
			RestoreNonBuiltinComponents(gameState.World, preserved);
			m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(),
			                                        std::memory_order_release);
```
(The shutdown path at line ~587 is unchanged — clear-only, no restore.)

- [ ] **Step 3: Build (green) — Engine**
```
cmake --build --preset msvc-win64-vs2026-enterprise --target Engine
```
Expected: builds clean.

- [ ] **Step 4: Commit**
```
git -C C:/dev/personal/clang-examples add src/engine/src/threading/GameThread.cpp
git -C C:/dev/personal/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(reload): preserve+restore non-builtin components across the GameThread reload barrier"
```
Verify exactly that one file.

---

### Task 5: Full regression

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-enterprise
cmake --build --preset msvc-win64-vs2026-enterprise
```
Expected: all targets (incl. `ecs`, `Engine`, `game`, `editor`, `runtime`) build, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_worldserial.exe
```
Expected: each prints its pass line (`test_reloadpreserve` emits one expected `BadComp` WARN before its pass line).

- [ ] **Step 3: Manual smoke (optional/deferred, human-owned)**

To verify the live path: temporarily add a POD game component + `SerializerRegistry().Register<T>("T")` in `GameRegisterComponents`, place it on an entity (with a non-default value), edit `Game.cpp` (`.cpp`-only), rebuild `game`, and confirm in the editor that after the hot-reload the entity still has the component with its value intact. Remove the throwaway component afterward. (Requires the editor restart from this task's `GAME_API_VERSION` bump before the first run.)

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `Register<T>` installs the json path only for serializable types and a byte (memcpy) path for trivially-copyable types; non-preservable types register with neither — unit-tested (Task 1).
- `PreserveNonBuiltinComponents`/`RestoreNonBuiltinComponents` round-trip POD (byte) and complex (json) game components across an array clear, preserving values + entity identity; unpreservable types warn + drop; builtins untouched — unit-tested (Task 2).
- The new `GameRegisterComponents` export is resolved + called on every (re)load before restore (Task 3), and the GameThread reload barrier extracts before clear + restores after reload (Task 4).
- Full tree builds; `test_compserial`/`test_reloadpreserve`/`test_ecs`/`test_worldserial` green (Task 5).
- A game can make a component survive hot-reload by adding one `SerializerRegistry().Register<T>("Name")` line in `GameRegisterComponents` — POD types need no serializer; complex types reuse their `to_json`/`from_json`. `world.json` disk format is unchanged.

## Notes

- Disk persistence (`world.json`) and reload preservation are separate paths on purpose: a POD component with no `to_json` is preserved across reload (byte path) but is **not** written to `world.json` (no `save`).
- The byte path assumes identical struct layout across the reload — true for a `.cpp`-only edit. A struct-layout change already requires a `GAME_API_VERSION` bump + editor restart (which reloads the world from disk), so stale-layout bytes are never restored.
- Preservation is opt-in via `Register`. Unregistered game components are still cleared on reload (current behavior) — these are per-tick scratch that self-heals.
- `Register`'s upsert replaces the whole entry, clearing any `editorDraw` hook; a game that uses both should call `Register<T>` then `RegisterEditorHook(...)` inside `GameRegisterComponents`.
```
