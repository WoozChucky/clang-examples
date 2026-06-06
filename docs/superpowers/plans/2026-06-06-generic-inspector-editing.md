# Generic Inspector Editing of Game Components (Boundary Piece 5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the editor inspect, add, edit, and remove **game-defined components it cannot name**, generically via JSON through the Piece-4 serializer registry — without touching the existing built-in editing path.

**Architecture:** ADDITIVE. The current typed command path (`ComponentData` = type_index + shared_ptr<void>, the 21-branch `ApplyComponentCommand`, the 17 bespoke `IComponentEditor`s) stays exactly as-is. A new **name+JSON** command path carries edits for any registered component as strings (ring-safe, type-agnostic); `ProcessCommands` applies them through the registry. The inspector keeps all bespoke editors and adds a **generic JSON-tree editor** for any registered, non-built-in component (i.e. game types). Plus: a small game-owned **state-name registry** replaces `StateScopeEditor`'s hardcoded 1..4 mirror (finishing the Piece-2 debt).

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, ImGui, nlohmann/json. Builds on Piece 1 (header-instantiable ECS<T>) and Piece 4 (`ComponentSerializerRegistry`).

**Scope:** Piece 5 (final) of the engine/game boundary spec (`docs/superpowers/specs/2026-06-06-engine-game-boundary-design.md`). Per design discussion: **additive** (full-collapse of the 21-branch dispatch is explicitly deferred to a possible future spec); **synthetic-test validation only** (no real game component is inspector-edited yet — the login flow uses a runtime singleton, so no `game.cpp` component is added for a manual GUI smoke beyond the state-name registration); **state-name table included**.

---

## Background facts (verified)

- `ECSCommand` (`ECSCommands.h:73-167`): `{ ECSCommandType Type; EntityId TargetEntity; ComponentData Component; std::type_index ComponentType; }` + ctors + typed factories (`AddComponent<T>`/`ModifyComponent<T>`/`RemoveComponent<T>`). `ComponentData::Create<T>` (type_index + `shared_ptr<void>`) is built in the editor (knows `T`) and read by `Get<T>` — so it CANNOT carry a game component the editor can't name. The ring is `SpscRing<ECSCommand, 128>`; `ECSCommand` is already non-trivially-copyable (holds `shared_ptr`), so adding `std::string` fields is fine.
- `ECSCommandProcessor::ProcessCommands` (`ECSCommands.h:185-242`) switches on `Type`; `AddComponent`/`ModifyComponent` → `ApplyComponentCommand` (21-branch), `RemoveComponent` → `RemoveComponentByType` (21-branch). It is a header-inline `static`.
- Inspector `EcsInspectorPanel` (`src/editor/src/panels/EcsInspectorPanel.cpp`): holds 17 `std::unique_ptr<IComponentEditor>` (`:31-50`). Add menu (`:127-132`) and Remove menu (`:137-142`) iterate them; the per-entity editor section (`:174-179`) iterates them. `IComponentEditor` (`src/editor/src/panels/inspector/IComponentEditor.h`): `Label()`, `Has(snap,e)`, `AddDefault(ctx,e)`, `Remove(ctx,e)`, `DrawEditor(ctx,e)`. `EditState<T>::Commit` pushes `ECSCommand::ModifyComponent(e, edit)`.
- The editor includes only `ECS.h` built-ins — never `game.h`. So a game type can only be reached generically (by name, via the registry).
- Piece-4 registry (`src/common/include/ComponentSerializerRegistry.h`): `ComponentSerializerEntry { std::string name; bool(*has); void(*save)(...,json&); void(*load)(...,const json&); }`; `ComponentSerializerRegistry::Register<T>(name)` (upsert), `Entries()`, `Find(name)`; exported `SerializerRegistry()`. Built-ins self-register in `src/ecs/src/ComponentSerializers.cpp`.
- JSON shapes (`ComponentSerialization.h`): `glm::vec3` → object `{X,Y,Z}`; `glm::vec4` → `{X,Y,Z,W}` or `{R,G,B,A}`; enums → integer. So the generic editor must handle nested objects, ints, floats, bools, strings, arrays.
- `StateScopeEditor.cpp` (post-Piece-2): hardcodes `{label, uint32_t bitIndex}` for Main Menu=1/In Level=2/In Editor=3/Paused=4 with a "keep in sync with GameStates.h" breadcrumb.
- `GameStateId` + `StateIndex` live in `src/game/include/GameStates.h` (game-owned, Piece 2). Game seeds singletons in `game.cpp` (`~832-934`, the `Uninitialized` boot block).

## Type/symbol contract (keep exact)

- `ComponentSerializerEntry` gains: `bool builtin;`, `void (*addDefault)(ECS&, EntityId);`, `void (*remove)(ECS&, EntityId);`.
- `ComponentSerializerRegistry::Register<T>(const std::string& name, bool builtin = false)` — sets all six fns + the flag (upsert).
- `ECSCommand` gains: `std::string ComponentName; std::string ComponentJson;` + `ECSCommandType` values `ModifyComponentJson`, `AddComponentByName`, `RemoveComponentByName` + factories `ModifyComponentJson(EntityId,std::string name,std::string json)`, `AddComponentByName(EntityId,std::string name)`, `RemoveComponentByName(EntityId,std::string name)`.
- New `src/common/include/StateNameRegistry.h`: `class StateNameRegistry { void Register(uint32_t bitIndex, const std::string& label); const std::vector<std::pair<uint32_t,std::string>>& Entries() const; };` + `ECS_API StateNameRegistry& StateNames();`
- New editor file `src/editor/src/panels/inspector/GenericComponentEditor.{h,cpp}`.

---

### Task 1: Registry lifecycle fns + name/JSON command path

**Files:**
- Modify: `src/common/include/ComponentSerializerRegistry.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Modify: `src/common/include/ECSCommands.h`
- Modify: `tests/test_compserial.cpp`

- [ ] **Step 1: Write the failing test (extend `tests/test_compserial.cpp`)**

Add an include at the top (after the existing includes): `#include "ECSCommands.h"`. Then add this test + register it in `main()` after the existing calls:
```cpp
// Drives the name+JSON command path end-to-end through ProcessCommands for a synthetic
// out-of-dll component (the case the editor needs for game types).
static void T04_name_json_command_path()
{
    SerializerRegistry().Register<PersistProbe>("PersistProbe"); // builtin defaults to false

    ECS world;
    const EntityId e = world.CreateEntity();
    SpscRing<ECSCommand, 128> ring;

    // Add by name -> default-constructed component.
    EXPECT(ring.Push(ECSCommand::AddComponentByName(e, "PersistProbe")));
    ECSCommandProcessor::ProcessCommands(world, ring);
    EXPECT(world.HasComponent<PersistProbe>(e));

    // Modify by JSON.
    nlohmann::json j = PersistProbe{ 99, 2.5f };
    EXPECT(ring.Push(ECSCommand::ModifyComponentJson(e, "PersistProbe", j.dump())));
    ECSCommandProcessor::ProcessCommands(world, ring);
    const PersistProbe* got = world.GetComponent<PersistProbe>(e);
    EXPECT(got != nullptr);
    EXPECT(got && got->A == 99);
    EXPECT(got && std::fabs(got->B - 2.5f) < 1e-6f);

    // Remove by name.
    EXPECT(ring.Push(ECSCommand::RemoveComponentByName(e, "PersistProbe")));
    ECSCommandProcessor::ProcessCommands(world, ring);
    EXPECT(!world.HasComponent<PersistProbe>(e));
}

static void T05_entry_flags_builtin_vs_game()
{
    const auto* tr = SerializerRegistry().Find("TransformComponent");
    const auto* pp = SerializerRegistry().Find("PersistProbe");
    EXPECT(tr && tr->builtin == true);    // built-ins self-register as builtin
    EXPECT(pp && pp->builtin == false);   // game/test types are non-builtin
}
```
Register `T04_name_json_command_path();` and `T05_entry_flags_builtin_vs_game();` in `main()`.

- [ ] **Step 2: Build — expect RED**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_compserial
```
Expected: compile errors — `AddComponentByName`/`ModifyComponentJson`/`RemoveComponentByName` are not members of `ECSCommand`, and `entry->builtin` doesn't exist. Confirms the test targets the new API.

- [ ] **Step 3: Extend the registry entry + `Register`**

In `src/common/include/ComponentSerializerRegistry.h`:
- Add fields to `ComponentSerializerEntry` (after `load`):
```cpp
    void (*addDefault)(ECS&, EntityId);   // AddComponent<T>(e, T{})
    void (*remove)(ECS&, EntityId);       // RemoveComponent<T>(e)
    bool builtin;                         // true for ecs.dll's built-ins; false for game types
```
- Replace `Register`'s signature + body:
```cpp
    template <class T>
    void Register(const std::string& name, bool builtin = false) {
        ComponentSerializerEntry e{
            name,
            [](const ECS& w, EntityId en)                      { return w.HasComponent<T>(en); },
            [](const ECS& w, EntityId en, nlohmann::json& out) { out = *w.GetComponent<T>(en); },
            [](ECS& w, EntityId en, const nlohmann::json& in)  { w.AddComponent<T>(en, in.template get<T>()); },
            [](ECS& w, EntityId en)                            { w.AddComponent<T>(en, T{}); },
            [](ECS& w, EntityId en)                            { w.RemoveComponent<T>(en); },
            builtin
        };
        for (auto& existing : m_Entries) {
            if (existing.name == name) { existing = e; return; }
        }
        m_Entries.push_back(std::move(e));
    }
```

- [ ] **Step 4: Built-ins register with `builtin = true`**

In `src/ecs/src/ComponentSerializers.cpp`, change every built-in registration to pass `true`, e.g.:
```cpp
        r.Register<TransformComponent>("TransformComponent", true);
        r.Register<MeshComponent>("MeshComponent", true);
        // ... all 17 with the trailing `, true` ...
        r.Register<NavClassComponent>("NavClassComponent", true);
```
(Every one of the 17 gets `, true`.)

- [ ] **Step 5: Add the name/JSON command kinds**

In `src/common/include/ECSCommands.h`:
- Add to `ECSCommandType` (after `BakeNavMesh = 7,`):
```cpp
    ModifyComponentJson = 8,   // payload: ComponentName + ComponentJson
    AddComponentByName  = 9,   // payload: ComponentName (default-constructed)
    RemoveComponentByName = 10,// payload: ComponentName
```
- Add string fields to `ECSCommand` (after `std::type_index ComponentType;`):
```cpp
    std::string ComponentName;  // for *ByName / ModifyComponentJson (registry key)
    std::string ComponentJson;  // for ModifyComponentJson (serialized component)
```
  (The existing ctors leave these default-empty — fine.)
- Add factories (next to the typed ones, ~line 166):
```cpp
    static ECSCommand ModifyComponentJson(EntityId entity, std::string name, std::string json) {
        ECSCommand c(ECSCommandType::ModifyComponentJson, entity);
        c.ComponentName = std::move(name);
        c.ComponentJson = std::move(json);
        return c;
    }
    static ECSCommand AddComponentByName(EntityId entity, std::string name) {
        ECSCommand c(ECSCommandType::AddComponentByName, entity);
        c.ComponentName = std::move(name);
        return c;
    }
    static ECSCommand RemoveComponentByName(EntityId entity, std::string name) {
        ECSCommand c(ECSCommandType::RemoveComponentByName, entity);
        c.ComponentName = std::move(name);
        return c;
    }
```
- Add includes at the top of `ECSCommands.h` (with the existing includes):
```cpp
#include <nlohmann/json.hpp>
#include "ComponentSerializerRegistry.h"
```
- Add cases to the `ProcessCommands` switch (after `ModifyComponent`):
```cpp
                case ECSCommandType::AddComponentByName: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        if (const auto* en = SerializerRegistry().Find(cmd.ComponentName)) en->addDefault(world, cmd.TargetEntity);
                        else SM_WARN("AddComponentByName: no serializer for '%s'", cmd.ComponentName.c_str());
                    }
                    break;
                }
                case ECSCommandType::RemoveComponentByName: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        if (const auto* en = SerializerRegistry().Find(cmd.ComponentName)) en->remove(world, cmd.TargetEntity);
                        else SM_WARN("RemoveComponentByName: no serializer for '%s'", cmd.ComponentName.c_str());
                    }
                    break;
                }
                case ECSCommandType::ModifyComponentJson: {
                    if (cmd.TargetEntity != INVALID_ENTITY) {
                        const auto* en = SerializerRegistry().Find(cmd.ComponentName);
                        if (!en) { SM_WARN("ModifyComponentJson: no serializer for '%s'", cmd.ComponentName.c_str()); break; }
                        try { en->load(world, cmd.TargetEntity, nlohmann::json::parse(cmd.ComponentJson)); }
                        catch (const std::exception& ex) { SM_WARN("ModifyComponentJson('%s') parse/apply failed: %s", cmd.ComponentName.c_str(), ex.what()); }
                    }
                    break;
                }
```
  (`SM_WARN` is already available — `ECSCommands.h` includes `lib.h` transitively via `ECS.h`/usage; if not, add `#include "lib.h"`.)

- [ ] **Step 6: Build + run — expect GREEN**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_compserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
```
Expected: `All component-serializer tests passed.` (exit 0). If `ECSCommands.h` now fails to compile in some TU for missing nlohmann/registry, confirm the two includes were added at the top of `ECSCommands.h`.

- [ ] **Step 7: Commit**

```
git -C /c/dev/clang-examples add src/common/include/ComponentSerializerRegistry.h src/ecs/src/ComponentSerializers.cpp src/common/include/ECSCommands.h tests/test_compserial.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): name+JSON ECSCommand path + registry addDefault/remove/builtin for generic component editing"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists exactly those four files.

---

### Task 2: Generic JSON-tree inspector editor for game components

**Files:**
- Create: `src/editor/src/panels/inspector/GenericComponentEditor.h`
- Create: `src/editor/src/panels/inspector/GenericComponentEditor.cpp`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

This is ImGui UI; it is verified by build + the Task-1 command-path test + code review (no unit test — ImGui needs a live context; consistent with the existing 17 editors, which are not unit-tested).

- [ ] **Step 1: Create `GenericComponentEditor.h`**
```cpp
#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "ECS.h"

struct EditorContext;

// Renders any REGISTERED component as an editable JSON tree (for game-defined types the
// editor cannot name; built-ins keep their bespoke IComponentEditor). Reads the component
// from the snapshot via the serializer registry, edits a working JSON copy, and commits a
// ModifyComponentJson command. One instance is reused for all generic components; it keys
// its working copy by (entity, component-name) so switching selection re-syncs.
class GenericComponentEditor {
public:
    // Draws a CollapsingHeader + the editable tree for `name` on `entity`. Call only when
    // the entity actually has the component (registry entry has() == true).
    void Draw(const EditorContext& ctx, EntityId entity, const std::string& name);

private:
    EntityId      m_Entity = INVALID_ENTITY;
    std::string   m_Name;
    nlohmann::json m_Edit;     // working copy
    bool          m_Modified = false;

    // Recursively render an editable widget for `value`. Returns true if any field changed.
    static bool DrawJsonValue(const char* label, nlohmann::json& value);
};
```

- [ ] **Step 2: Create `GenericComponentEditor.cpp`**
```cpp
#include "GenericComponentEditor.h"
#include "EditorContext.h"
#include "ApplicationContext.h"
#include "ECSCommands.h"
#include "ComponentSerializerRegistry.h"
#include "lib.h"
#include <imgui.h>

bool GenericComponentEditor::DrawJsonValue(const char* label, nlohmann::json& value) {
    bool changed = false;
    if (value.is_object()) {
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                ImGui::PushID(it.key().c_str());
                changed |= DrawJsonValue(it.key().c_str(), it.value());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    } else if (value.is_array()) {
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (size_t i = 0; i < value.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                char idx[16]; snprintf(idx, sizeof(idx), "[%zu]", i);
                changed |= DrawJsonValue(idx, value[i]);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    } else if (value.is_boolean()) {
        bool b = value.get<bool>();
        if (ImGui::Checkbox(label, &b)) { value = b; changed = true; }
    } else if (value.is_number_integer()) {
        int n = value.get<int>();
        if (ImGui::InputInt(label, &n)) { value = n; changed = true; }
    } else if (value.is_number_float()) {
        float f = value.get<float>();
        if (ImGui::InputFloat(label, &f)) { value = f; changed = true; }
    } else if (value.is_string()) {
        std::string s = value.get<std::string>();
        char buf[256]; snprintf(buf, sizeof(buf), "%s", s.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) { value = std::string(buf); changed = true; }
    } else {
        ImGui::TextDisabled("%s: (unsupported)", label);
    }
    return changed;
}

void GenericComponentEditor::Draw(const EditorContext& ctx, EntityId entity, const std::string& name) {
    const auto* entry = SerializerRegistry().Find(name);
    if (!entry || !ctx.WorldSnapshot) return;

    // (Re)sync the working copy from the snapshot when selection/component changes, or when
    // there is no pending local edit (so external changes show through).
    if (m_Entity != entity || m_Name != name) {
        m_Entity = entity; m_Name = name; m_Modified = false; m_Edit = nlohmann::json::object();
        entry->save(*ctx.WorldSnapshot, entity, m_Edit);
    } else if (!m_Modified) {
        m_Edit = nlohmann::json::object();
        entry->save(*ctx.WorldSnapshot, entity, m_Edit);
    }

    if (ImGui::CollapsingHeader(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (DrawJsonValue(name.c_str(), m_Edit)) m_Modified = true;

        if (m_Modified) {
            // Commit each frame the edit is dirty (matches EditState's commit-on-modified).
            if (!ctx.App->ECSCommandRing.Push(ECSCommand::ModifyComponentJson(entity, name, m_Edit.dump())))
                SM_WARN("ECS command queue full! ModifyComponentJson dropped.");
            m_Modified = false;
        }
    }
}
```
Note: `DrawJsonValue` for an object renders the component as `Name → {fields}`; the outer `CollapsingHeader(name)` + the inner `TreeNodeEx(name)` is intentional (header to match the bespoke-editor look, tree for the recursive body). If that double-label reads oddly in practice, the reviewer may suggest passing `""`/a child label — leave the structure but it's a cosmetic-only call.

- [ ] **Step 3: Integrate into `EcsInspectorPanel`**

In `src/editor/src/panels/EcsInspectorPanel.cpp`:
- Add include: `#include "inspector/GenericComponentEditor.h"`.
- In `EcsInspectorPanel.h`, add a member: `GenericComponentEditor m_GenericEditor;` (include the header there or forward-declare; simplest: `#include "inspector/GenericComponentEditor.h"` in the .h). [If `EcsInspectorPanel.h` wasn't shown, add the member next to `m_Editors`.]
- **Add menu** — after the bespoke add loop (`:127-132`), append registry-driven entries for non-built-in components not present:
```cpp
                // Generic add for game-registered (non-builtin) components.
                for (const auto& en : SerializerRegistry().Entries()) {
                    if (en.builtin || en.has(*ctx.WorldSnapshot, entity)) continue;
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "Add %s", en.name.c_str());
                    if (ImGui::MenuItem(lbl)) {
                        if (!ctx.App->ECSCommandRing.Push(ECSCommand::AddComponentByName(entity, en.name)))
                            SM_WARN("ECS command queue full! AddComponentByName dropped.");
                    }
                }
```
- **Remove menu** — after the bespoke remove loop (`:137-142`), append:
```cpp
                for (const auto& en : SerializerRegistry().Entries()) {
                    if (en.builtin || !en.has(*ctx.WorldSnapshot, entity)) continue;
                    char lbl[96]; snprintf(lbl, sizeof(lbl), "Remove %s", en.name.c_str());
                    if (ImGui::MenuItem(lbl)) {
                        if (!ctx.App->ECSCommandRing.Push(ECSCommand::RemoveComponentByName(entity, en.name)))
                            SM_WARN("ECS command queue full! RemoveComponentByName dropped.");
                    }
                }
```
- **Editor section** — after the bespoke editor loop (`:174-179`), append the generic editor for any present non-built-in component:
```cpp
            // Generic JSON editor for game-registered (non-builtin) components on this entity.
            for (const auto& en : SerializerRegistry().Entries()) {
                if (en.builtin || !en.has(*ctx.WorldSnapshot, selectedEntity)) continue;
                m_GenericEditor.Draw(ctx, selectedEntity, en.name);
            }
```

- [ ] **Step 4: Add the new source to `src/editor/CMakeLists.txt`**

Add `src/panels/inspector/GenericComponentEditor.cpp` to the editor target's source list (find where the other `inspector/*.cpp` files are listed and add it alongside them, matching the existing path style).

- [ ] **Step 5: Full build (green)**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean build of all targets including `editor`. (Reconfigure for the new source file.)

- [ ] **Step 6: Commit**

```
git -C /c/dev/clang-examples add src/editor/src/panels/inspector/GenericComponentEditor.h src/editor/src/panels/inspector/GenericComponentEditor.cpp src/editor/src/panels/EcsInspectorPanel.cpp src/editor/src/panels/EcsInspectorPanel.h src/editor/CMakeLists.txt
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): generic JSON-tree inspector editor for game-registered components"
```
(If `EcsInspectorPanel.h` was not modified, drop it from the `add`.) Verify the `--stat` matches what you changed; no stray files.

---

### Task 3: Game-owned state-name registry (replaces StateScopeEditor's hardcoded mirror)

**Files:**
- Create: `src/common/include/StateNameRegistry.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp` (define the exported accessor here — it already hosts ecs.dll registry singletons)
- Modify: `src/game/src/game.cpp` (register the state names)
- Modify: `src/editor/src/panels/inspector/StateScopeEditor.cpp` (read the registry)
- Modify: `tests/test_compserial.cpp` (unit test)

- [ ] **Step 1: Write the failing test (extend `tests/test_compserial.cpp`)**

Add `#include "StateNameRegistry.h"` at the top, and this test + register it in `main()`:
```cpp
static void T06_state_name_registry()
{
    StateNames().Register(1, "Main Menu");
    StateNames().Register(2, "In Level");
    StateNames().Register(1, "Main Menu (renamed)"); // upsert by index

    const auto* found = [](uint32_t idx) -> const std::string* {
        for (auto& e : StateNames().Entries()) if (e.first == idx) return &e.second;
        return nullptr;
    };
    EXPECT(found(1) && *found(1) == "Main Menu (renamed)"); // upserted
    EXPECT(found(2) && *found(2) == "In Level");
    // No duplicate index 1.
    size_t count1 = 0; for (auto& e : StateNames().Entries()) if (e.first == 1) ++count1;
    EXPECT(count1 == 1);
}
```

- [ ] **Step 2: Build — expect RED**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_compserial
```
Expected: `Cannot open include file: 'StateNameRegistry.h'`.

- [ ] **Step 3: Create `src/common/include/StateNameRegistry.h`**
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ECS.h"  // ECS_API

// Game-owned mapping from a state BIT INDEX (see the game's GameStateId) to a display label,
// shared across modules so the editor can show named StateScope checkboxes without naming the
// game's enum. The game registers its states at startup; the editor reads them. Single
// exported instance (defined in ecs.dll). Upsert by index (hot-reload safe).
class StateNameRegistry {
public:
    void Register(uint32_t bitIndex, const std::string& label) {
        for (auto& e : m_Entries) { if (e.first == bitIndex) { e.second = label; return; } }
        m_Entries.emplace_back(bitIndex, label);
    }
    [[nodiscard]] const std::vector<std::pair<uint32_t, std::string>>& Entries() const { return m_Entries; }

private:
    std::vector<std::pair<uint32_t, std::string>> m_Entries;
};

ECS_API StateNameRegistry& StateNames();
```

- [ ] **Step 4: Define the accessor in `src/ecs/src/ComponentSerializers.cpp`**

Add `#include "StateNameRegistry.h"` and, at the end of the file:
```cpp
StateNameRegistry& StateNames() {
    static StateNameRegistry reg;
    return reg;
}
```
(No built-in entries — the game owns the state vocabulary and registers them.)

- [ ] **Step 5: Game registers its state names (`src/game/src/game.cpp`)**

Add `#include "StateNameRegistry.h"` near the other includes. In the boot/seed block (the `g_GameState->StateId == GameStateId::Uninitialized` branch, ~832), register the four states (idempotent upsert; safe to run each boot/reload):
```cpp
        StateNames().Register(StateIndex(GameStateId::MainMenu), "Main Menu");
        StateNames().Register(StateIndex(GameStateId::InLevel),  "In Level");
        StateNames().Register(StateIndex(GameStateId::InEditor), "In Editor");
        StateNames().Register(StateIndex(GameStateId::Paused),   "Paused");
```
(No `GAME_API_VERSION` bump — no `GameState` layout change.)

- [ ] **Step 6: `StateScopeEditor` reads the registry**

In `src/editor/src/panels/inspector/StateScopeEditor.cpp`, add `#include "StateNameRegistry.h"` and replace the hardcoded `kStates[]` table + loop with a registry-driven loop:
```cpp
    ImGui::TextDisabled("Active in states (none = always):");
    // State labels come from the game-registered StateNameRegistry (game owns GameStateId).
    // Empty (game not loaded / not yet seeded) => no checkboxes this frame.
    for (const auto& [bitIndex, label] : StateNames().Entries()) {
        const uint32_t bit = 1u << bitIndex;
        bool on = (m_St.edit.StateMask & bit) != 0u;
        if (ImGui::Checkbox(label.c_str(), &on)) {
            if (on) m_St.edit.StateMask |= bit; else m_St.edit.StateMask &= ~bit;
            m_St.modified = true;
        }
    }
```
Remove the now-dead `keep in sync with GameStates.h` breadcrumb comment + the literal table.

- [ ] **Step 7: Build + run + commit**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
```
Expected: clean full build; `All component-serializer tests passed.`
Commit:
```
git -C /c/dev/clang-examples add src/common/include/StateNameRegistry.h src/ecs/src/ComponentSerializers.cpp src/game/src/game.cpp src/editor/src/panels/inspector/StateScopeEditor.cpp tests/test_compserial.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(state): game-registered state-name table; StateScopeEditor reads it (drops hardcoded mirror)"
```

---

### Task 4: Full regression + manual smoke

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Full clean build** — `cmake --build --preset msvc-win64-vs2026-community`. Expect all targets, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
```
Expect each pass line. (`test_navagent` is a known pre-existing RED — skip.)

- [ ] **Step 3: Manual editor smoke (human-owned, interactive)**

Launch `editor.exe`. Confirm: (a) existing built-in components still inspect/edit normally (bespoke widgets unchanged); (b) a `StateScopeComponent`'s "Active in states" checkboxes are labeled (Main Menu / In Level / In Editor / Paused) from the game registry and still toggle; (c) the typed Add/Remove/Modify path is unaffected. Generic game-component editing has no live target yet (no game component); it is covered by the Task-1 command-path test. Close the editor.

- [ ] **Step 4: Commit fixups (only if Steps 1-3 required edits).**

---

## Done criteria

- The name+JSON command path (`AddComponentByName`/`ModifyComponentJson`/`RemoveComponentByName`) applies through the registry; entry exposes `addDefault`/`remove`/`builtin`; proven by `test_compserial` for a synthetic out-of-dll component (Task 1).
- The inspector renders a generic JSON-tree editor + registry-driven add/remove for non-built-in components; the 17 bespoke editors + the 21-branch typed path are untouched (Task 2).
- `StateScopeEditor` shows game-registered state labels; the hardcoded 1..4 mirror is gone (Task 3).
- Full tree builds; `test_compserial`/`test_worldserial`/`test_ecs`/`test_menu` green; editor built-in editing + StateScope labels work (Task 4).

## Notes

- **Deferred (own future spec if justified):** full-collapse of the 21-branch `ApplyComponentCommand`/`RemoveComponentByType` and re-typing the 17 editors' commits through the JSON path. Additive leaves them as-is.
- **No live game-component consumer yet:** generic editing is validated by the Task-1 unit test; a manual GUI edit of a real game component arrives when game-side components exist.
- **`ECSCommands.h` now includes nlohmann + the registry** (for the new `ProcessCommands` cases). Acceptable — `ProcessCommands` already lives in that header; the typed path is unchanged.

## This completes the engine/game boundary spec (Pieces 1-5)

After this, the login-UI spec (appendix of the boundary design) can be built as pure `Game.dll` work.
