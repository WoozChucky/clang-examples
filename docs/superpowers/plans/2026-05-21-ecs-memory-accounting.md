# ECS Memory Accounting + Panel Section Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add read-only ECS byte accounting (`Used`/`Reserved` over the sparse-set component arrays + entity store) and surface it as an "ECS Memory" section in the editor Memory panel.

**Architecture:** A type-erased `virtual IComponentArray::MemoryBytes()` returns an `ArrayMemory{Used,Reserved}`; `ComponentArray<T>` sums its buffers; `EntityStore` sums its vectors; `ECS::MemoryStats()` aggregates into `EcsMemoryStats`. The editor panel (fed the world snapshot it already holds) renders it. Pure measurement — no allocator routing, not COW Part 2.

**Tech Stack:** C++23, header-mostly ECS, custom `test_ecs` harness (no GPU), editor ImGui Memory panel.

**Spec:** `docs/superpowers/specs/2026-05-21-ecs-memory-accounting-design.md`

**Model guidance:** Dispatch **all** implementer + reviewer subagents on **Opus 4.7**.

**Branch:** Already on `allocator-toolkit` (stacking). Do not switch branches. Do NOT offer to merge.

**Build/ABI note:** Adding a pure virtual to `IComponentArray` changes `ComponentArray<T>` vtable layout → rebuild `ecs.dll` + `editor` + `game` together; restart the editor for the smoke test. No `GAME_API_VERSION` bump. All the new code is inline in `ECS.h` (no `ecs.cpp` change needed — the explicit `template class ComponentArray<T>` instantiations in `ecs.cpp` pick up the new override automatically). Build preset: `msvc-win64-vs2026-community`.

---

## File Structure

- **Modify** `src/common/include/ECS.h` — `ArrayMemory` struct (before `IComponentArray`) + the pure virtual + `ComponentArray<T>` override + `EntityStore::MemoryBytes()` + `ComponentStore` sum helper + `EcsMemoryStats` (before the `ECS` class) + `ECS::MemoryStats()`. (Task 1)
- **Modify** `tests/test_ecs.cpp` — `MemoryBytes`/`MemoryStats` tests. (Task 1)
- **Modify** `src/editor/src/rendering/imgui/MemoryPanel.h` + `.cpp` — `DrawMemoryPanel` gains a `const ECS*` param + an "ECS Memory" section. (Task 2)
- **Modify** `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — pass the world `const ECS*` to `DrawMemoryPanel`. (Task 2)

## Conventions for every task

- Build: `cmake --build --preset msvc-win64-vs2026-community --target <test_ecs|editor|game>`. Run ECS tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`).
- Use the Bash tool. Author identity personal `Nuno Silva <nuno.levezinho@live.com.pt>` (already configured). Never stage the untracked `.claude/` directory.

---

### Task 1: ECS memory accounting in ECS.h + tests

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_ecs.cpp`, add before `main` (the harness provides `EXPECT`/`EXPECT_EQ`; `ComponentArray`, `ECS`, `TransformComponent`, `MeshComponent`, `EntityId`, `ArrayMemory`, `EcsMemoryStats` will exist after Step 2):

```cpp
static void TM01_componentarray_memorybytes_used_exact()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(1, TransformComponent{});
    arr.Add(2, TransformComponent{});
    arr.Add(3, TransformComponent{});

    ArrayMemory m = arr.MemoryBytes();
    EXPECT_EQ(m.Used, (size_t)(3 * sizeof(TransformComponent) + 3 * sizeof(EntityId)));
    EXPECT(m.Reserved >= m.Used);
    EXPECT(m.Reserved >= (size_t)4096);   // >= one 4 KB sparse page allocated

    arr.Remove(2);                         // swap-and-pop -> 2 live
    ArrayMemory m2 = arr.MemoryBytes();
    EXPECT_EQ(m2.Used, (size_t)(2 * sizeof(TransformComponent) + 2 * sizeof(EntityId)));
}

static void TM02_componentarray_empty_memorybytes()
{
    ComponentArray<TransformComponent> arr;
    ArrayMemory m = arr.MemoryBytes();
    EXPECT_EQ(m.Used, (size_t)0);
}

static void TM03_ecs_memorystats_aggregates()
{
    ECS w;
    EntityId a = w.CreateEntity();
    EntityId b = w.CreateEntity();
    w.AddComponent(a, TransformComponent{});
    w.AddComponent(b, TransformComponent{});
    w.AddComponent(a, MeshComponent{});

    EcsMemoryStats s = w.MemoryStats();
    EXPECT_EQ(s.EntityCount, (size_t)2);    // singleton reserved entity excluded
    EXPECT_EQ(s.ArrayCount, (size_t)2);     // Transform + Mesh
    EXPECT(s.ComponentUsed >= (size_t)(2 * sizeof(TransformComponent) + 1 * sizeof(MeshComponent)));
    EXPECT(s.ComponentReserved >= s.ComponentUsed);
    EXPECT(s.EntityReserved >= s.EntityUsed);
}
```

Register in `main` after the existing test calls:

```cpp
    TM01_componentarray_memorybytes_used_exact();
    TM02_componentarray_empty_memorybytes();
    TM03_ecs_memorystats_aggregates();
```

- [ ] **Step 2: Run to verify failure**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
```
Expected: compile error — `ArrayMemory` / `MemoryBytes` / `EcsMemoryStats` / `MemoryStats` undefined.

- [ ] **Step 3: Add `ArrayMemory` + the pure virtual**

In `src/common/include/ECS.h`, immediately BEFORE `class IComponentArray {` (the "Component Storage" section), add:

```cpp
// Byte accounting for a component array (read-only diagnostics).
struct ArrayMemory { size_t Used; size_t Reserved; };
```

Inside `class IComponentArray`, add the pure virtual alongside the others (e.g. after `Clone()`):

```cpp
    [[nodiscard]] virtual ArrayMemory MemoryBytes() const = 0;
```

- [ ] **Step 4: Implement `ComponentArray<T>::MemoryBytes()`**

In `ComponentArray<T>`'s public section (e.g. after `Size()` / before `Clone()`), add:

```cpp
    [[nodiscard]] ArrayMemory MemoryBytes() const override {
        ArrayMemory m{};
        m.Used     = m_Components.size()    * sizeof(T)
                   + m_IndexToEntity.size() * sizeof(EntityId);
        m.Reserved = m_Components.capacity()    * sizeof(T)
                   + m_IndexToEntity.capacity() * sizeof(EntityId)
                   + m_SparsePages.capacity()   * sizeof(std::unique_ptr<SparsePage>);
        for (const auto& page : m_SparsePages)
            if (page) m.Reserved += sizeof(SparsePage);   // 4 KB each
        return m;
    }
```

(`SparsePage` is the existing `using SparsePage = std::array<uint32_t, kPageSize>;` private alias.)

- [ ] **Step 5: Add `EntityStore::MemoryBytes()`**

In the `EntityStore` class public section, add:

```cpp
    [[nodiscard]] ArrayMemory MemoryBytes() const {
        ArrayMemory m{};
        m.Used     = (m_ActiveEntities.size()     + m_FreeEntities.size())     * sizeof(EntityId);
        m.Reserved = (m_ActiveEntities.capacity() + m_FreeEntities.capacity()) * sizeof(EntityId);
        return m;
    }
```

- [ ] **Step 6: Add the `ComponentStore` sum helper**

In the `ComponentStore` class public section, add:

```cpp
    [[nodiscard]] ArrayMemory MemoryBytes(size_t& outArrayCount) const {
        ArrayMemory sum{};
        outArrayCount = m_ComponentArrays.size();
        for (const auto& [type, slot] : m_ComponentArrays) {
            const ArrayMemory m = slot->MemoryBytes();
            sum.Used     += m.Used;
            sum.Reserved += m.Reserved;
        }
        return sum;
    }
```

- [ ] **Step 7: Add `EcsMemoryStats` + `ECS::MemoryStats()`**

In `ECS.h`, immediately BEFORE the `class ECS_API ECS` declaration (next to where `SnapshotPoolStats` is declared), add:

```cpp
// Aggregate ECS storage bytes (read-only diagnostics; excludes map/control-block overhead).
struct EcsMemoryStats {
    size_t ComponentUsed;
    size_t ComponentReserved;
    size_t EntityUsed;
    size_t EntityReserved;
    size_t ArrayCount;
    size_t EntityCount;
};
```

Inside the `ECS` class public section, immediately after the `CreateSnapshot()` / `ResetForRecycle()` area, add:

```cpp
    [[nodiscard]] EcsMemoryStats MemoryStats() const {
        size_t arrayCount = 0;
        const ArrayMemory comp = m_ComponentStore.MemoryBytes(arrayCount);
        const ArrayMemory ent  = m_EntityStore.MemoryBytes();
        return EcsMemoryStats{
            comp.Used, comp.Reserved,
            ent.Used,  ent.Reserved,
            arrayCount, m_EntityStore.GetEntityCount()
        };
    }
```

- [ ] **Step 8: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

- [ ] **Step 9: Commit**

```bash
git add src/common/include/ECS.h tests/test_ecs.cpp
git commit -m "feat(ecs): byte accounting (MemoryBytes/MemoryStats) for component arrays + entity store"
```

---

### Task 2: Editor "ECS Memory" panel section

**Files:**
- Modify: `src/editor/src/rendering/imgui/MemoryPanel.h`
- Modify: `src/editor/src/rendering/imgui/MemoryPanel.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

- [ ] **Step 1: Update the DrawMemoryPanel signature**

In `src/editor/src/rendering/imgui/MemoryPanel.h`, change:

```cpp
// Draws the "Memory" debug window from the Engine allocator registry.
// `open` may be null (always draw) or point to a toggle bool.
void DrawMemoryPanel(bool* open);
```

to:

```cpp
class ECS;

// Draws the "Memory" debug window: Engine allocator registry, snapshot pool, and
// (if `world` is non-null) ECS storage byte accounting.
// `open` may be null (always draw) or point to a toggle bool.
void DrawMemoryPanel(bool* open, const ECS* world);
```

- [ ] **Step 2: Add the "ECS Memory" section**

In `src/editor/src/rendering/imgui/MemoryPanel.cpp`, change the function definition signature to match (`void DrawMemoryPanel(bool* open, const ECS* world)`). `<ECS.h>` is already included (from the snapshot-pool task); confirm it is — if not, add `#include <ECS.h>`.

Immediately BEFORE the final `ImGui::End();` (after the existing "Snapshot Pool" section), add:

```cpp
    if (world && ImGui::CollapsingHeader("ECS Memory", ImGuiTreeNodeFlags_DefaultOpen)) {
        const EcsMemoryStats s = world->MemoryStats();
        ImGui::Text("Component used:     %zu", s.ComponentUsed);
        ImGui::Text("Component reserved: %zu", s.ComponentReserved);
        ImGui::Text("Entity used:        %zu", s.EntityUsed);
        ImGui::Text("Entity reserved:    %zu", s.EntityReserved);
        ImGui::Text("Total reserved:     %zu", s.ComponentReserved + s.EntityReserved);
        ImGui::Text("Arrays: %zu   Entities: %zu", s.ArrayCount, s.EntityCount);
        ImGui::TextDisabled("(buffers only; excludes map/control-block overhead)");
    }
```

- [ ] **Step 3: Pass the world snapshot from ImGuiRenderer**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, find the call (currently `DrawMemoryPanel(&s_ShowMemoryPanel);` at ~line 482). It is inside `ImGuiRenderer::Render(...)`, which has a `const ECS*` parameter (read the method signature to get its exact name — it is the world snapshot pointer, e.g. `world`). Change the call to pass it:

```cpp
        DrawMemoryPanel(&s_ShowMemoryPanel, world);
```

(Use the actual parameter name from `ImGuiRenderer::Render`'s signature. If the ECS pointer parameter has a different name, use that.)

- [ ] **Step 4: Build the editor**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: compiles + links cleanly.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/MemoryPanel.h src/editor/src/rendering/imgui/MemoryPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat(editor): Memory panel shows ECS storage byte accounting"
```

---

### Task 3: Full build + verification

**Files:** none (verification only).

- [ ] **Step 1: Rebuild ecs + game + editor together**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: all build + link cleanly. (Do not build the whole solution — the legacy `runtime` target is pre-broken and unrelated.)

- [ ] **Step 2: Run unit tests**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: `All ECS tests passed.` and `All allocator tests passed.` (ignore the stray arena-overflow ERROR line in test_alloc — pre-existing lib.h quirk).

- [ ] **Step 3: Editor smoke test (user-driven)**

Restart + launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe` (full restart — `ECS.h`/`ecs.dll` changed). In the Memory panel, confirm the new "ECS Memory" section shows non-zero Component used/reserved and Entity used/reserved, Arrays/Entities counts that match the scene, and that the numbers grow when you add entities/components via the inspector and shrink on removal. Scene renders identically; no crash. Report that this requires the running UI rather than claiming success from a compile.

---

## Self-Review

**1. Spec coverage:**
- `ArrayMemory { Used; Reserved; }` + pure virtual `IComponentArray::MemoryBytes()`: Task 1 Step 3. ✓
- `ComponentArray<T>::MemoryBytes()` (dense vectors size/capacity + 4 KB pages in Reserved): Task 1 Step 4. ✓
- `EntityStore::MemoryBytes()`: Task 1 Step 5. ✓
- `ComponentStore` sum helper over the private array map: Task 1 Step 6. ✓
- `EcsMemoryStats` + `ECS::MemoryStats()`: Task 1 Step 7. ✓
- Panel "ECS Memory" section fed the world snapshot; `DrawMemoryPanel` gains `const ECS*`; ImGuiRenderer passes it: Task 2. ✓
- Used+Reserved (not reserved-only); no peak; map overhead excluded + labeled: Task 1 Step 4/7 + Task 2 Step 2. ✓
- Tests (Used exact, Reserved≥Used+page, empty, aggregate): Task 1 Step 1. ✓
- Rebuild all + no GAME_API_VERSION bump: header note + Task 3. ✓

**2. Placeholder scan:** No TBD/TODO. Every code step shows full code. Task 2 Step 3 asks the implementer to use the actual ECS-param name from `ImGuiRenderer::Render` — the change (pass that pointer) is fully specified.

**3. Type consistency:** `ArrayMemory { size_t Used; size_t Reserved; }` defined once (Task 1 Step 3), returned by `IComponentArray::MemoryBytes`, `ComponentArray<T>::MemoryBytes`, `EntityStore::MemoryBytes`, and the `ComponentStore` helper. `EcsMemoryStats` (6 `size_t` fields) defined in Task 1 Step 7, produced by `ECS::MemoryStats`, consumed by the panel (Task 2 Step 2) — field names match (`ComponentUsed`/`ComponentReserved`/`EntityUsed`/`EntityReserved`/`ArrayCount`/`EntityCount`). `DrawMemoryPanel(bool*, const ECS*)` signature matches across the `.h` decl, `.cpp` def, and the ImGuiRenderer call.

**Note for executor:** `SparsePage`/`m_SparsePages`/`kPageSize` are existing private members of `ComponentArray<T>` (from the sparse-set refactor), so `MemoryBytes` (a member) can use them. `m_ComponentArrays` is private to `ComponentStore` and the sum helper is a `ComponentStore` member, so access is fine. `GetEntityCount()` is an existing `EntityStore` method. Do not bump `GAME_API_VERSION`; no `ecs.cpp` change is required (all new code is inline in `ECS.h`).
