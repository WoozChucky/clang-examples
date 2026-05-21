# ECS ComponentArray Paged Sparse-Set Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ComponentArray<T>`'s per-array `std::unordered_map<EntityId,size_t>` with an EnTT-style paged sparse set, eliminating per-tick COW-clone hash-node churn and making `Has`/`Get` O(1) array indexing.

**Architecture:** Behavior-preserving internal refactor of one class in a header. Dense `m_Components`/`m_IndexToEntity` stay; the reverse index becomes `std::vector<std::unique_ptr<std::array<uint32_t,1024>>>` (absent page = nullptr = no alloc). Because it's behavior-preserving, the new tests are **characterization tests**: they pass against the *current* hashmap impl (Task 1, baseline), then must still pass after the rewrite (Task 2).

**Tech Stack:** C++23, custom `test_ecs` harness (no GPU), `ecs.dll` (ComponentArray explicit instantiations) consumed by editor + game.

**Spec:** `docs/superpowers/specs/2026-05-21-ecs-componentarray-sparse-set-design.md`

**Model guidance:** Dispatch **all** implementer + reviewer subagents on **Opus 4.7**. Highest-risk: Task 2's custom copy ctor (COW clone-isolation lynchpin) and the swap-and-pop sparse redirect.

**Branch:** Already on `allocator-toolkit` (stacking). Do not switch branches. Do NOT offer to merge — keep stacking.

**Build/ABI note:** No `GAME_API_VERSION` bump (no `GameState`/export/component-type change). But `ComponentArray<T>`'s internal layout changes and it's instantiated in `ecs.dll` + consumed by editor/game, so all three must rebuild together; restart the editor for the smoke test. Build preset: `msvc-win64-vs2026-community` (enterprise not installed).

---

## File Structure

- **Modify** `tests/test_ecs.cpp` — add characterization tests TS-prefixed below (Task 1).
- **Modify** `src/common/include/ECS.h` — rewrite `ComponentArray<T>` internals: members, rule-of-5, sparse helpers, `Add`/`Remove`/`Get`/`Has` bodies; add `#include <array>` (Task 2).

No other files. Public API unchanged.

## Conventions for every task

- Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <test_ecs|game|editor>`. Run ECS tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`).
- Use the Bash tool. Author identity personal `Nuno Silva <nuno.levezinho@live.com.pt>` (already configured). Never stage the untracked `.claude/` directory.

---

### Task 1: Characterization tests (pass on the CURRENT hashmap impl)

These lock down `ComponentArray<T>`'s observable behavior BEFORE the refactor. They must pass against today's `unordered_map` implementation — that's the baseline that proves the Task-2 rewrite preserves behavior.

**Files:**
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Add the tests**

In `tests/test_ecs.cpp`, add before `main` (the harness provides `EXPECT`/`EXPECT_EQ`/`EXPECT_NE`; `ComponentArray`, `TransformComponent`, `EntityId`, `INVALID_ENTITY` are available; `<cstdint>` is included):

```cpp
static void TS01_componentarray_cross_page_ids()
{
    // kPageSize is 1024 in the new impl; ids 5 and 5000 land in different pages.
    // Behavior must be page-agnostic on the current impl too.
    ComponentArray<TransformComponent> arr;
    arr.Add(5,    TransformComponent{{5.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(5000, TransformComponent{{50.0f, 0, 0}, {}, {1, 1, 1}});

    EXPECT(arr.Has(5));
    EXPECT(arr.Has(5000));
    EXPECT(!arr.Has(6));
    EXPECT(!arr.Has(0));
    EXPECT_EQ(arr.Get(5)->Position.x, 5.0f);
    EXPECT_EQ(arr.Get(5000)->Position.x, 50.0f);
    EXPECT_EQ(arr.Size(), (size_t)2);
}

static void TS02_componentarray_high_id_only()
{
    // Only a high id present: low ids must report absent (paging must not falsely report).
    ComponentArray<TransformComponent> arr;
    arr.Add(5000, TransformComponent{});
    EXPECT(arr.Has(5000));
    EXPECT(!arr.Has(0));
    EXPECT(!arr.Has(5));
    EXPECT(!arr.Has(4999));
    EXPECT(arr.Get(5) == nullptr);
    EXPECT(arr.Get(5000) != nullptr);
    EXPECT_EQ(arr.Size(), (size_t)1);
}

static void TS03_componentarray_swap_and_pop_preserves_others()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(10, TransformComponent{{10.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(20, TransformComponent{{20.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(30, TransformComponent{{30.0f, 0, 0}, {}, {1, 1, 1}});

    arr.Remove(20); // middle removal -> last (30) swaps into 20's slot

    EXPECT(!arr.Has(20));
    EXPECT(arr.Has(10));
    EXPECT(arr.Has(30));
    EXPECT_EQ(arr.Get(10)->Position.x, 10.0f);
    EXPECT_EQ(arr.Get(30)->Position.x, 30.0f); // 30 still resolves after being moved
    EXPECT_EQ(arr.Size(), (size_t)2);
}

static void TS04_componentarray_remove_then_readd_same_id()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(7, TransformComponent{{7.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Remove(7);
    EXPECT(!arr.Has(7));
    EXPECT(arr.Get(7) == nullptr);

    arr.Add(7, TransformComponent{{77.0f, 0, 0}, {}, {1, 1, 1}});
    EXPECT(arr.Has(7));
    EXPECT_EQ(arr.Get(7)->Position.x, 77.0f);
    EXPECT_EQ(arr.Size(), (size_t)1);
}

static void TS05_componentarray_update_existing()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(3, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(3, TransformComponent{{2.0f, 0, 0}, {}, {1, 1, 1}}); // same id -> update, not insert
    EXPECT_EQ(arr.Size(), (size_t)1);
    EXPECT_EQ(arr.Get(3)->Position.x, 2.0f);
}

static void TS06_componentarray_clone_independent_after_swap_and_pop()
{
    ComponentArray<TransformComponent> arr;
    arr.Add(1, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(2, TransformComponent{{2.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Add(3, TransformComponent{{3.0f, 0, 0}, {}, {1, 1, 1}});
    arr.Remove(2); // exercise swap-and-pop before cloning

    std::shared_ptr<IComponentArray> clonedBase = arr.Clone();
    auto* cloned = static_cast<ComponentArray<TransformComponent>*>(clonedBase.get());

    EXPECT_EQ(cloned->Size(), arr.Size());
    EXPECT(cloned->Has(1));
    EXPECT(cloned->Has(3));
    EXPECT(!cloned->Has(2));
    EXPECT_EQ(cloned->Get(3)->Position.x, 3.0f);

    // Mutating the original must not affect the clone (COW isolation).
    arr.Get(1)->Position.x = 999.0f;
    arr.Remove(3);
    EXPECT_EQ(cloned->Get(1)->Position.x, 1.0f);
    EXPECT(cloned->Has(3));
    EXPECT_EQ(cloned->Get(3)->Position.x, 3.0f);
}
```

Register them in `main` after the existing test calls:

```cpp
    TS01_componentarray_cross_page_ids();
    TS02_componentarray_high_id_only();
    TS03_componentarray_swap_and_pop_preserves_others();
    TS04_componentarray_remove_then_readd_same_id();
    TS05_componentarray_update_existing();
    TS06_componentarray_clone_independent_after_swap_and_pop();
```

- [ ] **Step 2: Build + run — verify they PASS on the current impl**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.` — these characterize the EXISTING `unordered_map` behavior, so they pass now. (If any fails, the test encodes a wrong expectation — fix the test, not the ECS.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_ecs.cpp
git commit -m "test(ecs): characterization tests for ComponentArray (pre sparse-set refactor)"
```

---

### Task 2: Rewrite ComponentArray internals to a paged sparse set

Swap the hashmap for paged sparse storage. All Task-1 tests (and the pre-existing ones) must still pass — that's the proof the refactor preserved behavior.

**Files:**
- Modify: `src/common/include/ECS.h`

- [ ] **Step 1: Add the `<array>` include**

In `src/common/include/ECS.h`, add `#include <array>` to the top include block (near `<vector>`/`<memory>`).

- [ ] **Step 2: Replace the ComponentArray member declarations**

Find the private members at the bottom of `class ComponentArray`:

```cpp
private:
    std::vector<T> m_Components;
    std::unordered_map<EntityId, size_t> m_EntityToIndex;
    std::vector<EntityId> m_IndexToEntity;
```

Replace with:

```cpp
private:
    static constexpr uint32_t kInvalid  = UINT32_MAX;
    static constexpr uint32_t kPageSize = 1024;
    using SparsePage = std::array<uint32_t, kPageSize>;

    [[nodiscard]] uint32_t SparseGet(EntityId entity) const {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page >= m_SparsePages.size() || !m_SparsePages[page]) return kInvalid;
        return (*m_SparsePages[page])[entity % kPageSize];
    }
    void SparseSet(EntityId entity, uint32_t denseIndex) {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page >= m_SparsePages.size()) m_SparsePages.resize(page + 1);
        if (!m_SparsePages[page]) {
            m_SparsePages[page] = std::make_unique<SparsePage>();
            m_SparsePages[page]->fill(kInvalid);
        }
        (*m_SparsePages[page])[entity % kPageSize] = denseIndex;
    }
    void SparseClear(EntityId entity) {
        const size_t page = static_cast<size_t>(entity / kPageSize);
        if (page < m_SparsePages.size() && m_SparsePages[page])
            (*m_SparsePages[page])[entity % kPageSize] = kInvalid;
    }

    std::vector<T> m_Components;
    std::vector<EntityId> m_IndexToEntity;
    std::vector<std::unique_ptr<SparsePage>> m_SparsePages;
```

- [ ] **Step 3: Add the rule-of-5 special members**

Immediately after the `class ECS_API ComponentArray final : public IComponentArray {` line and its `public:` (i.e. at the very top of the public section, before `Add`), insert:

```cpp
    ComponentArray() = default;
    ~ComponentArray() override = default;
    ComponentArray(ComponentArray&&) noexcept = default;
    ComponentArray& operator=(ComponentArray&&) noexcept = default;

    ComponentArray(const ComponentArray& other)
        : m_Components(other.m_Components)
        , m_IndexToEntity(other.m_IndexToEntity)
    {
        m_SparsePages.reserve(other.m_SparsePages.size());
        for (const auto& page : other.m_SparsePages)
            m_SparsePages.push_back(page ? std::make_unique<SparsePage>(*page) : nullptr);
    }

    ComponentArray& operator=(const ComponentArray& other) {
        if (this != &other) {
            ComponentArray tmp(other);
            m_Components    = std::move(tmp.m_Components);
            m_IndexToEntity = std::move(tmp.m_IndexToEntity);
            m_SparsePages   = std::move(tmp.m_SparsePages);
        }
        return *this;
    }
```

- [ ] **Step 4: Rewrite `Add`**

Replace:

```cpp
    void Add(const EntityId entity, T component) {
        if (m_EntityToIndex.contains(entity)) {
            // Update existing component
            size_t index = m_EntityToIndex[entity];
            m_Components[index] = component;
            return;
        }

        // Add new component
        const size_t newIndex = m_Components.size();
        m_EntityToIndex[entity] = newIndex;
        m_Components.push_back(component);
        m_IndexToEntity.push_back(entity);
    }
```

with:

```cpp
    void Add(const EntityId entity, T component) {
        const uint32_t existing = SparseGet(entity);
        if (existing != kInvalid) {
            // Update existing component
            m_Components[existing] = component;
            return;
        }

        // Add new component
        const uint32_t newIndex = static_cast<uint32_t>(m_Components.size());
        SparseSet(entity, newIndex);
        m_Components.push_back(component);
        m_IndexToEntity.push_back(entity);
    }
```

- [ ] **Step 5: Rewrite `Remove`**

Replace:

```cpp
    void Remove(const EntityId entity) override {
        if (!m_EntityToIndex.contains(entity)) {
            return; // Entity doesn't have this component
        }

        // Swap-and-pop for efficient removal
        size_t indexOfRemoved = m_EntityToIndex[entity];
        size_t indexOfLast = m_Components.size() - 1;

        // Swap with last element
        m_Components[indexOfRemoved] = m_Components[indexOfLast];

        // Update mappings for the swapped entity
        EntityId entityOfLast = m_IndexToEntity[indexOfLast];
        m_EntityToIndex[entityOfLast] = indexOfRemoved;
        m_IndexToEntity[indexOfRemoved] = entityOfLast;

        // Remove old mappings
        m_EntityToIndex.erase(entity);
        m_IndexToEntity.pop_back();

        m_Components.pop_back();
    }
```

with:

```cpp
    void Remove(const EntityId entity) override {
        const uint32_t indexOfRemoved = SparseGet(entity);
        if (indexOfRemoved == kInvalid) {
            return; // Entity doesn't have this component
        }

        // Swap-and-pop for efficient removal
        const uint32_t indexOfLast = static_cast<uint32_t>(m_Components.size() - 1);

        // Swap with last element
        m_Components[indexOfRemoved] = m_Components[indexOfLast];

        // Redirect the moved (last) entity to the removed slot
        const EntityId entityOfLast = m_IndexToEntity[indexOfLast];
        m_IndexToEntity[indexOfRemoved] = entityOfLast;
        SparseSet(entityOfLast, indexOfRemoved);

        // Clear the removed entity and pop the tails
        SparseClear(entity);
        m_IndexToEntity.pop_back();
        m_Components.pop_back();
    }
```

- [ ] **Step 6: Rewrite `Get` (both overloads) and `Has`**

Replace:

```cpp
    T* Get(const EntityId entity) {
        if (!m_EntityToIndex.contains(entity)) {
            return nullptr;
        }
        return &m_Components[m_EntityToIndex[entity]];
    }

    const T* Get(const EntityId entity) const {
        if (!m_EntityToIndex.contains(entity)) {
            return nullptr;
        }
        return &m_Components[m_EntityToIndex.at(entity)];
    }

    [[nodiscard]] bool Has(const EntityId entity) const override {
        return m_EntityToIndex.contains(entity);
    }
```

with:

```cpp
    T* Get(const EntityId entity) {
        const uint32_t index = SparseGet(entity);
        return index == kInvalid ? nullptr : &m_Components[index];
    }

    const T* Get(const EntityId entity) const {
        const uint32_t index = SparseGet(entity);
        return index == kInvalid ? nullptr : &m_Components[index];
    }

    [[nodiscard]] bool Has(const EntityId entity) const override {
        return SparseGet(entity) != kInvalid;
    }
```

(`Size`, `GetComponents` both overloads, `GetEntity`, `Clone` are unchanged.)

- [ ] **Step 7: Build + run tests — must still pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.` — all pre-existing tests + TS01-TS06 pass against the new sparse-set impl, proving behavior was preserved.

- [ ] **Step 8: Commit**

```bash
git add src/common/include/ECS.h
git commit -m "perf(ecs): ComponentArray paged sparse-set (cheap COW clone, O(1) Has/Get)"
```

---

### Task 3: Full build + verification

**Files:** none (verification only).

- [ ] **Step 1: Rebuild ecs + game + editor together**

`ComponentArray` layout changed; all consumers must rebuild against the new `ECS.h`.

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

Restart + launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe` (full restart needed — `ComponentArray` layout changed and the running editor links the old layout). Confirm the scene renders identically (meshes/instancing/text), entities behave as before (text spins/cycles, day/night light moves), add/remove of components via the inspector still works, and world save/load still works if exercised. No crash. Report that this requires the running UI rather than claiming success from a compile.

---

## Self-Review

**1. Spec coverage:**
- Replace `unordered_map` with `vector<unique_ptr<array<uint32_t,1024>>>` + kInvalid/kPageSize: Task 2 Step 2. ✓
- SparseGet/SparseSet/SparseClear helpers: Task 2 Step 2. ✓
- Rule-of-5 (custom copy ctor/assign, defaulted move/dtor/default-ctor): Task 2 Step 3. ✓
- Rewrite Add/Remove/Get×2/Has via helpers, swap-and-pop preserved: Task 2 Steps 4-6. ✓
- Size/GetComponents/GetEntity/Clone unchanged: noted Task 2 Step 6. ✓
- `#include <array>`: Task 2 Step 1. ✓
- Public API/callers/X-macro unchanged (only ECS.h + test_ecs): file structure. ✓
- Characterization tests (cross-page, high-id-only/paging, swap-and-pop, remove-readd, update-existing, clone-after-swap): Task 1 (TS01-TS06). ✓
- No GAME_API_VERSION bump; rebuild all + restart editor: header note + Task 3. ✓

**2. Placeholder scan:** No TBD/TODO. Every code step shows full before/after.

**3. Type consistency:** `kInvalid` (uint32_t), `kPageSize` (uint32_t), `SparsePage` (`std::array<uint32_t,1024>`), `m_SparsePages` (`vector<unique_ptr<SparsePage>>`) defined in Task 2 Step 2 and used identically in Steps 3-6. `SparseGet` returns `uint32_t` (kInvalid sentinel), consumed by Add/Remove/Get/Has consistently. Dense index is `uint32_t` throughout (`newIndex`, `indexOfRemoved`, `indexOfLast`). The copy ctor deep-copies pages via `make_unique<SparsePage>(*page)`.

**Note for executor:** `make_unique<SparsePage>()` value-initializes (zeros) the array; `->fill(kInvalid)` then sets the absent sentinel — both are needed (zero is a valid dense index 0, so the page MUST be filled with kInvalid, not left zeroed). The copy ctor's `make_unique<SparsePage>(*page)` copy-constructs from the source page (element-wise copy) — correct, no fill needed there. Do not bump `GAME_API_VERSION`.
