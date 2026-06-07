# Entity dev-names Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional engine-builtin `NameComponent { std::string Name; }` so the ECS inspector lists entities by a dev-authored name (falling back to `Entity <id>`), renamable via an always-on field + F2.

**Architecture:** Three implementation tasks + regression: (1) the builtin component — struct + X-macro + JSON serializer + builtin registration + a round-trip unit test (TDD); (2) the inspector list label (name-or-id); (3) the always-on rename field + F2, driven through the existing name+JSON ECSCommand path (no new command type, no typed branch, no dedicated editor). Builds on the registry-driven command apply path (`AddComponentByName`/`ModifyComponentJson`/`RemoveComponentByName`) already on `main`.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake, nlohmann/json, Dear ImGui (editor only).

**Scope:** Implements `docs/superpowers/specs/2026-06-07-entity-names-design.md`. Touches `ECS.h` (X-macro) ⇒ rebuild **ecs + Engine + editor + game** and **restart the editor**. No `GAME_API_VERSION` bump (engine-side; `GameState` unchanged).

> **Branch:** Work happens on `feat/entity-names` (already created off `main`). Stay on it.

---

## Background facts (verified)

- **Line numbers are approximate — locate every edit by the quoted code.**
- `EcsInspectorPanel.cpp:92-99`: the entity-list loop does `snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);` then `ImGui::Selectable(entityLabel, ...)`. `PushID(static_cast<int>(entity))` wraps each row. `ctx.WorldSnapshot` is a `std::shared_ptr<const ECS>`; `GetComponent<T>(EntityId) const` returns `const T*` or `nullptr` and is instantiated for every registered `T`.
- `EcsInspectorPanel.cpp:167-181`: a keyboard-ops block gated on `selectedEntity != INVALID_ENTITY && !io.WantTextInput` already handles Ctrl+D / Delete via `ImGui::IsKeyChordPressed` / `ImGui::IsKeyPressed`.
- `EcsInspectorPanel.cpp:186-202`: the `=== COMPONENT EDITOR FOR SELECTED ENTITY ===` block, inside `if (selectedEntity != INVALID_ENTITY && ctx.WorldSnapshot->IsValidEntity(selectedEntity))`, prints `Editing Entity %llu`, then runs the `m_Editors` bespoke editors, then the `SerializerRegistry().Entries()` generic loop **skipping `en.builtin`**.
- `EcsInspectorPanel.cpp` already `#include`s the headers that declare `ECSCommand` (uses `ECSCommand::DestroyEntity/DuplicateEntity/AddComponentByName`) and `SerializerRegistry()`. `nlohmann::json` is available transitively via those headers (it builds `*ByName`/`ModifyComponentJson` commands which carry JSON strings).
- `src/ecs/src/ComponentSerializers.cpp:6-28`: `SerializerRegistry()` lazily registers the builtins with `r.Register<T>("Name", true)`. `Register<T>` (`ComponentSerializerRegistry.h:56-91`) installs `has`/`save`/`load`/`addDefault`/`remove`/`copyTo`; **`load` = `w.AddComponent<T>(en, in.get<T>())`, which is an UPSERT** (adds if absent, replaces if present — Piece-5 generic editing of existing components relies on this). So a single `ModifyComponentJson` both adds-and-sets `NameComponent`; no separate `AddComponentByName` is needed.
- Command apply path (`ECSCommands.h:250-269`) is builtin-agnostic: `ModifyComponentJson` → `en->load`; `RemoveComponentByName` → `en->remove`; both just look the entry up by name. A builtin can be driven entirely through this path.
- `ECS.h:12` already `#include <string>`. The X-macro `ECS_FOR_EACH_REGISTERED_COMPONENT` (`ECS.h:361-393`) ends with `    X(NavClassComponent)` (no trailing backslash).
- `tests/test_worldserial.cpp` is a flat list of `static void Txx_*()` functions called from `int main()` (line ~330). Last numbered roundtrips: `T10_collider_roundtrip`, `T11_collider_backward_compatible_defaults`. It `#include`s `ComponentSerialization.h` (→ `ECS.h`), so `NameComponent` + its serializer are visible. `EXPECT(cond)` macro counts failures; `main` prints `All world-serialization tests passed.` when `g_Failures == 0`.

## Type/symbol contract (keep exact)

- `struct NameComponent { std::string Name; };` in `ECS.h`.
- `X(NameComponent)` appended to the X-macro.
- `to_json`/`from_json` over the single `Name` field in `ComponentSerialization.h`.
- `r.Register<NameComponent>("NameComponent", true);` in `ComponentSerializers.cpp`.
- New `EcsInspectorPanel` members: `char m_RenameBuf[128] = {};`, `EntityId m_RenameBufFor = INVALID_ENTITY;`, `bool m_FocusRename = false;`.
- Registry key string is exactly `"NameComponent"` everywhere.

---

### Task 1: `NameComponent` builtin + serializer + round-trip test (TDD)

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/common/include/ComponentSerialization.h`
- Modify: `src/ecs/src/ComponentSerializers.cpp`
- Test: `tests/test_worldserial.cpp`

- [ ] **Step 1: Write the failing test (`tests/test_worldserial.cpp`)**

Add this function immediately after `T11_collider_backward_compatible_defaults()` (locate that function; add the new one right after its closing brace):
```cpp
static void T12_name_roundtrip()
{
    NameComponent in;
    in.Name = "Hero";
    const nlohmann::json j = in;
    const auto out = j.get<NameComponent>();
    EXPECT(out.Name == in.Name);

    // Default-constructed name is empty (unnamed entities carry no component, but the
    // default value must be empty so an accidental empty NameComponent serializes cleanly).
    NameComponent def;
    EXPECT(def.Name.empty());
}
```
And register the call in `main()` — add `    T12_name_roundtrip();` immediately after the `T11_collider_backward_compatible_defaults();` line.

- [ ] **Step 2: Run the test target to confirm it FAILS to build**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_worldserial
```
Expected: COMPILE ERROR — `'NameComponent': undeclared identifier` (the type and its serializer don't exist yet). This is the failing state.

- [ ] **Step 3: Add the `NameComponent` struct + X-macro entry (`src/common/include/ECS.h`)**

Locate the `UIRectComponent` struct:
```cpp
struct UIRectComponent {
    glm::vec2 Size{160.0f, 48.0f};
    glm::vec4 Color{0.15f, 0.15f, 0.18f, 1.0f};
};
```
Immediately AFTER its closing `};`, add:
```cpp

// Optional dev-authored display name for an entity. Editor/tooling concern (shown in the ECS
// inspector list); persisted in world.json. Absent on most entities — the inspector falls back to
// "Entity <id>". First std::string-bearing builtin; cheap under COW snapshots (only the rename
// tick clones the array).
struct NameComponent {
    std::string Name;
};
```
Then locate the end of the X-macro:
```cpp
    X(NavConstrainedComponent) \
    X(NavClassComponent)
```
Change it to (add a backslash to the `NavClassComponent` line and append the new entry as the new last line):
```cpp
    X(NavConstrainedComponent) \
    X(NavClassComponent) \
    X(NameComponent)
```

- [ ] **Step 4: Add the serializer (`src/common/include/ComponentSerialization.h`)**

Add anywhere among the other component `to_json`/`from_json` definitions (e.g. after the `UIRectComponent` serializers if present, else near the top of the per-component serializers):
```cpp
inline void to_json(nlohmann::json& j, const NameComponent& t) {
    j = nlohmann::json{ {"Name", t.Name} };
}
inline void from_json(const nlohmann::json& j, NameComponent& t) {
    j.at("Name").get_to(t.Name);
}
```

- [ ] **Step 5: Register as builtin (`src/ecs/src/ComponentSerializers.cpp`)**

In the `SerializerRegistry()` initializer lambda, add after the last `r.Register<...>(..., true);` line (currently `r.Register<NavClassComponent>("NavClassComponent", true);`):
```cpp
        r.Register<NameComponent>("NameComponent", true);
```

- [ ] **Step 6: Build + run — GREEN**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: builds clean; prints `All world-serialization tests passed.` (T12 passes).

- [ ] **Step 7: Commit**
```
git -C C:/dev/clang-examples add src/common/include/ECS.h src/common/include/ComponentSerialization.h src/ecs/src/ComponentSerializers.cpp tests/test_worldserial.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): optional builtin NameComponent + JSON serializer + round-trip test"
```
Verify with `git -C C:/dev/clang-examples show --stat HEAD` — exactly those four files.

---

### Task 2: Inspector list label (name-or-id)

**Files:**
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`

Build-verified (ImGui layer; no unit test).

- [ ] **Step 1: Replace the entity-list label (`EcsInspectorPanel.cpp`)**

Locate, in the entity-list loop:
```cpp
            bool isSelected = (selectedEntity == entity);
            char entityLabel[64];
            snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);
            if (ImGui::Selectable(entityLabel, isSelected)) {
```
Replace the label construction so a present, non-empty `NameComponent` wins, else the id fallback:
```cpp
            bool isSelected = (selectedEntity == entity);
            char entityLabel[128];
            const NameComponent* nameComp = ctx.WorldSnapshot->GetComponent<NameComponent>(entity);
            if (nameComp && !nameComp->Name.empty())
                snprintf(entityLabel, sizeof(entityLabel), "%s", nameComp->Name.c_str());
            else
                snprintf(entityLabel, sizeof(entityLabel), "Entity %llu", entity);
            if (ImGui::Selectable(entityLabel, isSelected)) {
```
(`PushID(entity)` above already disambiguates duplicate-named rows for ImGui.)

- [ ] **Step 2: Build — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: editor builds clean.

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/EcsInspectorPanel.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): ECS inspector lists entities by NameComponent (falls back to Entity <id>)"
```

---

### Task 3: Always-on rename field + F2

**Files:**
- Modify: `src/editor/src/panels/EcsInspectorPanel.h`
- Modify: `src/editor/src/panels/EcsInspectorPanel.cpp`

Build-verified (ImGui layer; no unit test). Manual smoke covers the interaction.

- [ ] **Step 1: Add panel members (`src/editor/src/panels/EcsInspectorPanel.h`)**

In the `private:` section, after `EntityId selectedEntity = INVALID_ENTITY;`, add:
```cpp
    // Rename field state for the selected entity. m_RenameBuf mirrors the selected entity's
    // NameComponent (re-synced when the selection changes, tracked by m_RenameBufFor so live
    // typing isn't clobbered). m_FocusRename = one-shot: focus the field next frame (F2).
    char m_RenameBuf[128] = {};
    EntityId m_RenameBufFor = INVALID_ENTITY;
    bool m_FocusRename = false;
```

- [ ] **Step 2: Add the F2 trigger (`src/editor/src/panels/EcsInspectorPanel.cpp`)**

In the keyboard-ops block (locate `if (selectedEntity != INVALID_ENTITY && !io.WantTextInput) {` and its `Ctrl+D` / `Delete` handling), add an F2 case. After the existing `else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) { ... }` block, add:
```cpp
            else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                m_FocusRename = true;
            }
```
(F2 is gated by the same `!io.WantTextInput` so it only triggers when not already editing text.)

- [ ] **Step 3: Add the rename field (`src/editor/src/panels/EcsInspectorPanel.cpp`)**

Locate the editor-section header inside the valid-selection block:
```cpp
        if (selectedEntity != INVALID_ENTITY && ctx.WorldSnapshot->IsValidEntity(selectedEntity)) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Editing Entity %llu", selectedEntity);
            ImGui::Separator();
```
Immediately AFTER that `ImGui::Separator();` (before the `m_Editors` loop), insert the rename field:
```cpp
            // --- Dev name (always-on rename field; drives the optional builtin NameComponent) ---
            // Re-sync the buffer from the snapshot when the selection changes, so we don't clobber
            // live typing on the same entity. Empty buffer => unnamed.
            if (m_RenameBufFor != selectedEntity) {
                const NameComponent* nc = ctx.WorldSnapshot->GetComponent<NameComponent>(selectedEntity);
                snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", (nc && !nc->Name.empty()) ? nc->Name.c_str() : "");
                m_RenameBufFor = selectedEntity;
            }
            if (m_FocusRename) {
                ImGui::SetKeyboardFocusHere();
                m_FocusRename = false;
            }
            const bool committed =
                ImGui::InputText("Name", m_RenameBuf, sizeof(m_RenameBuf), ImGuiInputTextFlags_EnterReturnsTrue)
                | (ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0);
            if (committed) {
                // Trim leading/trailing whitespace.
                std::string name(m_RenameBuf);
                const size_t b = name.find_first_not_of(" \t\r\n");
                const size_t e = name.find_last_not_of(" \t\r\n");
                name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);

                const bool has = ctx.WorldSnapshot->GetComponent<NameComponent>(selectedEntity) != nullptr;
                if (!name.empty()) {
                    // load() is AddComponent (upsert): adds-or-sets in one command.
                    const std::string json = nlohmann::json{{"Name", name}}.dump();
                    if (!ctx.App->ECSCommandRing.Push(
                            ECSCommand::ModifyComponentJson(selectedEntity, "NameComponent", json)))
                        SM_WARN("ECS command queue full! Rename (ModifyComponentJson) dropped.");
                } else if (has) {
                    if (!ctx.App->ECSCommandRing.Push(
                            ECSCommand::RemoveComponentByName(selectedEntity, "NameComponent")))
                        SM_WARN("ECS command queue full! Rename (RemoveComponentByName) dropped.");
                }
                // empty + !has => no-op.
            }
            ImGui::Separator();
```
Notes for the implementer:
- `<string>` is already pulled in transitively via `ECS.h`; if the compiler disagrees, add `#include <string>` at the top of the `.cpp`.
- `nlohmann::json` is available transitively (the file already builds JSON-carrying commands). If not, add `#include <nlohmann/json.hpp>`.
- The `| (... ? 1 : 0)` forces both `InputText`'s Enter-commit AND `IsItemDeactivatedAfterEdit` (click-away) to commit, without short-circuiting `IsItemDeactivatedAfterEdit` (it must be evaluated every frame right after the widget). Keep the bitwise `|`, not `||`.

- [ ] **Step 4: Build — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: editor builds clean.

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/editor/src/panels/EcsInspectorPanel.h src/editor/src/panels/EcsInspectorPanel.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(editor): always-on entity rename field + F2 (NameComponent via name+JSON command path)"
```

---

### Task 4: Full regression

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets (ecs, Engine, editor, game, runtime, all test_*) build, no errors / `LNK`.

- [ ] **Step 2: Run the suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line (`All ... tests passed.`). `test_compserial` emits the expected `NopeNotReal` SM_WARN; `test_reloadpreserve` the expected `BadComp` SM_WARN — both are expected, not failures.

- [ ] **Step 3: Manual smoke (optional/deferred, human-owned)**

After restarting the editor (required — `ECS.h` changed): select an entity, then —
1. **List + field:** type a name in the **Name** field → the list row updates to the name; persists to `world.json` on save.
2. **F2:** with an entity selected (not editing other text), press **F2** → the Name field is focused; type + Enter commits.
3. **Remove:** clear the Name field to empty + commit → the `NameComponent` is removed; the list row falls back to `Entity <id>`.
4. **Duplicate:** Ctrl+D a named entity → the copy carries the same name.
5. **Persistence:** reload `world.json` → names restored; an old world without names still loads (entities simply unnamed).

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `NameComponent` exists as an engine builtin (struct + X-macro + serializer + builtin registration); `test_worldserial` T12 round-trip green (Task 1).
- Inspector list shows the name when present, `Entity <id>` otherwise (Task 2).
- Always-on rename field commits via `ModifyComponentJson` (add-or-set) / `RemoveComponentByName` (empty); **F2** focuses it for the selected entity; buffer re-syncs on selection change (Task 3).
- Full tree builds; `test_ecs`/`test_worldserial`/`test_compserial`/`test_reloadpreserve`/`test_playermove` green (Task 4).
- No `GAME_API_VERSION` bump; full rebuild (ecs + Engine + editor + game) + editor restart documented (ECS.h X-macro changed).

## Notes

- Single-command rename path (`ModifyComponentJson` upserts) is a verified refinement of the spec's "add-then-modify" — same observable behavior, fewer commands, no two-command ring edge case.
- `NameComponent` is a builtin that is deliberately editor-special: it has NO `IComponentEditor` and is skipped by the generic JSON-tree loop (`en.builtin`), so the rename field is its only editing surface and it never appears in the Add/Remove component menus.
- First `std::string`-bearing builtin; safe (builtins are excluded from the game-component byte reload path) and cheap (COW snapshots — only the rename tick clones the Name array).
