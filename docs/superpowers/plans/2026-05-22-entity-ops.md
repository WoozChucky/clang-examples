# Entity Ops (Delete + Duplicate) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete (`Del`) and Duplicate (`Ctrl+D`) the selected entity in the editor.

**Architecture:** Delete reuses the existing `DestroyEntity` ECSCommand. Duplicate adds a `DuplicateEntity(src)` command applied on the GameThread (creates an entity + copies the 6 editor components). Keyboard + context-menu triggers live in `EcsInspectorPanel`; mutations flow through the existing `ECSCommandRing`.

**Tech Stack:** C++23, custom ECS + command ring, Dear ImGui, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-entity-ops-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- No `GAME_API_VERSION` bump (new enum value + command case; no GameState/component-layout change). `ECSCommands.h` is shared, so rebuild engine/editor/game.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.

---

### Task 1: `DuplicateEntity` command + `test_ecs` coverage

**Files:**
- Modify: `src/common/include/ECSCommands.h` (enum + factory + ProcessCommands case + helper)
- Modify: `tests/test_ecs.cpp` (new test)

- [ ] **Step 1: Write the failing test** — in `tests/test_ecs.cpp`, add `#include "ECSCommands.h"` near the other includes (after `#include "Systems.h"`), then add this test function (place it next to the other `T..` functions):
```cpp
static void T60_duplicate_entity_copies_editor_components()
{
    ECS world;
    EntityId src = world.CreateEntity();
    world.AddComponent(src, TransformComponent{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}});
    world.AddComponent(src, MeshComponent{ 7u, true });

    SpscRing<ECSCommand, 128> ring;
    EXPECT(ring.Push(ECSCommand::DuplicateEntity(src)));
    ECSCommandProcessor::ProcessCommands(world, ring);

    // Find the duplicate: the one entity != src that has both copied components.
    EntityId dup = INVALID_ENTITY;
    int matches = 0;
    for (EntityId e : world.GetActiveEntities()) {
        if (e == src) continue;
        if (world.HasComponent<TransformComponent>(e) && world.HasComponent<MeshComponent>(e)) {
            dup = e; ++matches;
        }
    }
    EXPECT_EQ(matches, 1);
    EXPECT_NE(dup, INVALID_ENTITY);
    if (dup != INVALID_ENTITY) {
        const auto* t = world.GetComponent<TransformComponent>(dup);
        const auto* m = world.GetComponent<MeshComponent>(dup);
        EXPECT(t != nullptr);
        EXPECT(m != nullptr);
        if (t) { EXPECT(t->Position.x == 1.0f); EXPECT(t->Scale.x == 2.0f); }
        if (m) { EXPECT(m->MeshId == 7u); EXPECT(m->Visible == true); }
    }
    // Source is untouched.
    EXPECT(world.HasComponent<TransformComponent>(src));
    EXPECT(world.HasComponent<MeshComponent>(src));
}
```
Register it: add `T60_duplicate_entity_copies_editor_components();` in `main()` next to the other `T..();` calls.

- [ ] **Step 2: Build + run → expect FAIL (DuplicateEntity undefined)**
```
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
```
Expected: compile error — `ECSCommand` has no member `DuplicateEntity` (and `ECSCommandType::DuplicateEntity` undefined).

- [ ] **Step 3: Add the enum value** — in `src/common/include/ECSCommands.h`, extend `enum class ECSCommandType` (currently `CreateEntity=0 … ModifyComponent=4`):
```cpp
enum class ECSCommandType : uint8_t {
    CreateEntity = 0,
    DestroyEntity = 1,
    AddComponent = 2,
    RemoveComponent = 3,
    ModifyComponent = 4,
    DuplicateEntity = 5,
};
```

- [ ] **Step 4: Add the factory** — after the `DestroyEntity` factory (the `static ECSCommand DestroyEntity(EntityId entity) { return ECSCommand(ECSCommandType::DestroyEntity, entity); }`), add:
```cpp
    static ECSCommand DuplicateEntity(EntityId entity) {
        return ECSCommand(ECSCommandType::DuplicateEntity, entity);
    }
```
(Reuses the existing `ECSCommand(ECSCommandType, EntityId)` constructor that `DestroyEntity` uses.)

- [ ] **Step 5: Handle it in `ProcessCommands` + add the copy helper** — in the `ProcessCommands` switch, after the `ModifyComponent` case, add:
```cpp
                case ECSCommandType::DuplicateEntity: {
                    if (cmd.TargetEntity != INVALID_ENTITY && world.IsValidEntity(cmd.TargetEntity)) {
                        const EntityId dst = world.CreateEntity();
                        DuplicateEntityComponents(world, cmd.TargetEntity, dst);
                    }
                    break;
                }
```
And add this private static helper to `ECSCommandProcessor` (next to `ApplyComponentCommand` / `RemoveComponentByType`):
```cpp
    // Copy the editor-facing per-entity components from src to dst (the same set ApplyComponentCommand
    // handles). Deliberately excludes singletons (cameras/viewport/input) and hierarchy (Parent/Child).
    static void DuplicateEntityComponents(ECS& world, EntityId src, EntityId dst) {
        if (auto* c = world.GetComponent<TransformComponent>(src)) world.AddComponent(dst, *c);
        if (auto* c = world.GetComponent<LightningComponent>(src)) world.AddComponent(dst, *c);
        if (auto* c = world.GetComponent<MeshComponent>(src))      world.AddComponent(dst, *c);
        if (auto* c = world.GetComponent<MaterialComponent>(src))  world.AddComponent(dst, *c);
        if (auto* c = world.GetComponent<TextComponent>(src))      world.AddComponent(dst, *c);
        if (world.HasComponent<SunMarker>(src))                    world.AddComponent(dst, SunMarker{});
    }
```
(`world.IsValidEntity(EntityId)` exists in `ECS.h`. `GetComponent<T>` returns null when absent, so the `if`-init doubles as the presence check; `SunMarker` is a tag → `HasComponent` + default-construct.)

- [ ] **Step 6: Build + run → expect PASS**
```
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.` (If the ECS asserts an owner thread on mutation, note that the test is single-threaded so the constructing/test thread is the owner — it should pass; if it traps, report it.)

- [ ] **Step 7: Commit**
```bash
git add src/common/include/ECSCommands.h tests/test_ecs.cpp
git commit -m "feat: add DuplicateEntity ECS command (copies editor components) + test"
```

---

### Task 2: Editor triggers — `Del` / `Ctrl+D` + context menu

**Files:**
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.cpp`

- [ ] **Step 1: Add the "Duplicate Entity" context-menu item**
In the per-entity `if (ImGui::BeginPopupContextItem())` block, immediately AFTER the existing "Delete Entity" `MenuItem` block (the one that pushes `DestroyEntity(entity)` and clears `selectedEntity`) and BEFORE the `ImGui::Separator();` that follows, add:
```cpp
                if (ImGui::MenuItem("Duplicate Entity")) {
                    if (!ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(entity))) {
                        SM_WARN("ECS command queue full! Duplicate command dropped.");
                    }
                }
```

- [ ] **Step 2: Add the keyboard handler (Del / Ctrl+D), once per Draw**
In `EcsInspectorPanel::Draw`, AFTER the `for (EntityId entity : ctx.WorldSnapshot->GetActiveEntities())` entity-list loop closes (so it runs once per frame, not per entity; `selectedEntity` and `ctx` are in scope), add:
```cpp
        // Keyboard ops on the selected entity. Gate on !WantTextInput so editing a field's text
        // (e.g. pressing Delete in an input) doesn't destroy the entity. Check Ctrl+D before Del.
        ImGuiIO& io = ImGui::GetIO();
        if (selectedEntity != INVALID_ENTITY && !io.WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) {
                if (!ctx.App->ECSCommandRing.Push(ECSCommand::DuplicateEntity(selectedEntity))) {
                    SM_WARN("ECS command queue full! Duplicate command dropped.");
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                if (!ctx.App->ECSCommandRing.Push(ECSCommand::DestroyEntity(selectedEntity))) {
                    SM_WARN("ECS command queue full! Delete command dropped.");
                }
                selectedEntity = INVALID_ENTITY;
            }
        }
```
(`<imgui.h>`, `ECSCommands.h`/`ECSCommand`, and `SM_WARN` are already used in this file. `IsKeyChordPressed`/`IsKeyPressed` read global ImGui key state, so this fires whenever an entity is selected and no text field is active, regardless of which panel is focused — matching how editors handle Del/Ctrl+D. If a local `ImGuiIO& io` is already in scope at that point, reuse it instead of re-declaring.)

- [ ] **Step 3: Build + regression**
```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; `All ECS tests passed.` + `All allocator tests passed.` (test_alloc prints its one intentional ERROR line before the green pass).

- [ ] **Step 4: Commit**
```bash
git add src/editor/src/rendering/imgui/EcsInspectorPanel.cpp
git commit -m "feat: Del to delete + Ctrl+D / context-menu to duplicate the selected entity"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green.
- [ ] `test_ecs` / `test_alloc` / `test_frustum` / `test_input` / `test_picking` all print their `All ... passed.` lines.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):** select an entity, press `Ctrl+D` → Entity Count +1, an identical entity appears overlapping the source (move it with the gizmo to see both); right-click → "Duplicate Entity" does the same; press `Del` → the selected entity vanishes, selection clears (gizmo/outline gone); right-click → "Delete Entity" still works; click into a numeric/text inspector field, press `Delete` to edit the text → the entity is NOT deleted (text-edit guard); `runtime.exe` unaffected.

## Notes / non-goals
- No `GAME_API_VERSION` bump. Duplicate copies only the 6 editor components (Transform/Mesh/Material/Text/Lightning/SunMarker) — not singletons/hierarchy.
- Selection stays on the source after duplicate (the async command ring means the editor doesn't learn the new id; auto-selecting it is a deferred enhancement).
- Duplicate is created in place (overlaps source). Delete is instant — no undo (matches the existing menu).
