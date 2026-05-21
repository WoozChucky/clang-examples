# ECS COW Part 2 — ComponentArray Buffer Recycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recycle the per-tick `ComponentArray` clone buffers (the deep copy `MutateArray`/`Clone` allocate every tick) through a per-type pool, eliminating the last steady-state ECS allocation while preserving snapshot isolation.

**Architecture:** A capacity-reusing `ComponentArray<T>::CopyFrom` replaces the reallocating copy. A per-type `ComponentArrayPool<T>` (mutex free-list, Meyers singleton) hands out recycled arrays via `MakePooledClone<T>` (Acquire → CopyFrom → `shared_ptr` with a recycling deleter). The clone paths (`ComponentStore::MutateArray`, `ComponentArray<T>::Clone`) use it. Safety rests on the refcount-0 gate: the pool only holds arrays no snapshot references, so reusing one cannot corrupt a live snapshot. Aggregate atomic stats feed a Memory-panel section.

**Tech Stack:** C++23, ECS in `ecs.dll` (explicit template instantiation via the `ECS_FOR_EACH_REGISTERED_COMPONENT` X-macro), the `test_ecs` custom harness, Dear ImGui (Memory panel).

**Spec:** `docs/superpowers/specs/2026-05-21-ecs-cow-part2-design.md`

**Branch:** `allocator-toolkit` (stacking). Do NOT merge or offer to merge the branch.

**Build/run:** preset `msvc-win64-vs2026-community`. `test_ecs` build: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`; run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`). ECS.h changes recompile `ecs.dll` + `editor` + `game` + `test_ecs`; restart editor for the smoke. **No `GAME_API_VERSION` bump.**

---

## File Structure

- **Modify** `src/common/include/ECS.h` — add `ComponentArray<T>::CopyFrom` (inline); change `Clone()` to a declaration (body moves to `ecs.cpp`); add `ComponentArrayPoolStats` + `GetComponentArrayPoolStats()` declaration.
- **Modify** `src/ecs/src/ecs.cpp` — add the pool machinery (`ArrayPoolCounters`, `ComponentArrayPool<T>`, `GetArrayPool<T>`, `MakePooledClone<T>`, out-of-line `Clone`) ABOVE the explicit `template class` instantiation; change the `MutateArray` clone line; define `GetComponentArrayPoolStats()`.
- **Modify** `tests/test_ecs.cpp` — `CopyFrom` tests (Task 1) + pool reuse/isolation/stats tests (Task 2) + registrations.
- **Modify** `src/editor/src/rendering/imgui/MemoryPanel.cpp` — add a "ComponentArray Pool" section (Task 3).

---

### Task 1: `ComponentArray<T>::CopyFrom` + unit tests

The riskiest piece, isolated and pure (no pool, no threads). TDD: write the `CopyFrom` tests, watch them fail to compile, add `CopyFrom`, pass.

**Files:**
- Modify: `src/common/include/ECS.h` (add `CopyFrom` after the copy-assignment operator, ~line 209)
- Test: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_ecs.cpp`, add these three functions immediately before `int main()` (around line 842):

```cpp
static void TCF01_copyfrom_reproduces_source_over_stale_dest()
{
    // src: ids spanning >= 3 sparse pages (page size 1024)
    ComponentArray<TransformComponent> src;
    src.Add(1,    TransformComponent{{1.0f,  0, 0}, {}, {1,1,1}});
    src.Add(5,    TransformComponent{{5.0f,  0, 0}, {}, {1,1,1}});
    src.Add(1025, TransformComponent{{25.0f, 0, 0}, {}, {1,1,1}}); // page 1
    src.Add(2049, TransformComponent{{49.0f, 0, 0}, {}, {1,1,1}}); // page 2

    // dest: DIFFERENT prior layout — different ids and MORE pages than src
    ComponentArray<TransformComponent> dest;
    dest.Add(2,    TransformComponent{{2.0f,  0, 0}, {}, {1,1,1}});
    dest.Add(3000, TransformComponent{{30.0f, 0, 0}, {}, {1,1,1}}); // page 2 (different id)
    dest.Add(9000, TransformComponent{{90.0f, 0, 0}, {}, {1,1,1}}); // page 8 -> dest has more pages

    dest.CopyFrom(src);

    // dest now mirrors src exactly
    EXPECT_EQ(dest.Size(), src.Size());
    EXPECT(dest.Has(1));    EXPECT_EQ(dest.Get(1)->Position.x,    1.0f);
    EXPECT(dest.Has(5));    EXPECT_EQ(dest.Get(5)->Position.x,    5.0f);
    EXPECT(dest.Has(1025)); EXPECT_EQ(dest.Get(1025)->Position.x, 25.0f);
    EXPECT(dest.Has(2049)); EXPECT_EQ(dest.Get(2049)->Position.x, 49.0f);
    // stale entries from the larger old layout must be gone
    EXPECT(!dest.Has(2));
    EXPECT(!dest.Has(3000));
    EXPECT(!dest.Has(9000));

    // deep copy: mutating src must not affect dest
    src.Get(1)->Position.x = 999.0f;
    EXPECT_EQ(dest.Get(1)->Position.x, 1.0f);
}

static void TCF02_copyfrom_empty_src_clears_dest()
{
    ComponentArray<TransformComponent> src; // empty
    ComponentArray<TransformComponent> dest;
    dest.Add(1,    TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});
    dest.Add(1025, TransformComponent{{2.0f, 0, 0}, {}, {1,1,1}});

    dest.CopyFrom(src);
    EXPECT_EQ(dest.Size(), (size_t)0);
    EXPECT(!dest.Has(1));
    EXPECT(!dest.Has(1025));
}

static void TCF03_copyfrom_reuses_dense_capacity()
{
    ComponentArray<TransformComponent> dest;
    for (EntityId i = 1; i <= 64; ++i) dest.Add(i, TransformComponent{});
    const size_t capBefore = dest.GetComponents().capacity();

    ComponentArray<TransformComponent> src;
    src.Add(1, TransformComponent{{7.0f, 0, 0}, {}, {1,1,1}}); // far smaller than dest

    dest.CopyFrom(src);
    EXPECT_EQ(dest.Size(), (size_t)1);
    EXPECT_EQ(dest.Get(1)->Position.x, 7.0f);
    EXPECT_EQ(dest.GetComponents().capacity(), capBefore); // capacity reused, not shrunk/realloc'd
}
```

Register them in `main()` immediately after `TM03_ecs_memorystats_aggregates();` (around line 891):

```cpp
    TCF01_copyfrom_reproduces_source_over_stale_dest();
    TCF02_copyfrom_empty_src_clears_dest();
    TCF03_copyfrom_reuses_dense_capacity();
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`
Expected: FAIL — `'CopyFrom': is not a member of 'ComponentArray<T>'`.

- [ ] **Step 3: Add `CopyFrom`**

In `src/common/include/ECS.h`, inside `class ComponentArray`, immediately after the copy-assignment `operator=` (which ends at the line `return *this;` / `}` around line 209) and before `void Add(...)`, insert:

```cpp
    // Deep-copies src into *this, reusing existing buffer capacity where possible.
    // Result is structurally identical to a copy-constructed clone of src (same dense
    // contents, same per-page null/non-null layout). Used by the array recycle pool.
    void CopyFrom(const ComponentArray& src) {
        m_Components.assign(src.m_Components.begin(), src.m_Components.end());
        m_IndexToEntity.assign(src.m_IndexToEntity.begin(), src.m_IndexToEntity.end());

        m_SparsePages.resize(src.m_SparsePages.size()); // drop any surplus dest pages
        for (size_t i = 0; i < src.m_SparsePages.size(); ++i) {
            if (!src.m_SparsePages[i]) {
                m_SparsePages[i].reset();                                   // match src null slot
            } else if (!m_SparsePages[i]) {
                m_SparsePages[i] = std::make_unique<SparsePage>(*src.m_SparsePages[i]);
            } else {
                *m_SparsePages[i] = *src.m_SparsePages[i];                  // reuse 4 KB buffer
            }
        }
    }
```

- [ ] **Step 4: Run the build to verify it passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`
Expected: clean compile/link (rebuilds `ecs.dll` first because `ECS.h` changed).

- [ ] **Step 5: Run the tests**

Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`
Expected: `All ECS tests passed.` (exit 0)

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h tests/test_ecs.cpp
git commit -m "Add capacity-reusing ComponentArray::CopyFrom + tests"
```

---

### Task 2: Per-type array pool + recycling clone + stats

Wires the pool into both clone paths atomically (the pool, `MakePooledClone`, `MutateArray`, and the `Clone` move must land together — a half-wiring would leave `Clone` calling a not-yet-defined helper). No threading is exercised by the tests; correctness rests on the refcount-0 gate plus the Task 1 `CopyFrom` coverage.

**Files:**
- Modify: `src/common/include/ECS.h` (Clone → declaration; add stats struct + accessor)
- Modify: `src/ecs/src/ecs.cpp` (pool machinery + Clone def above the explicit instantiation; MutateArray clone line; stats accessor def)
- Test: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_ecs.cpp`, add these three functions immediately before `int main()`:

```cpp
static void TCOW01_pool_reuses_recycled_array()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});

    auto s1 = w.CreateSnapshot();                                   // master slot = A; s1 shares A
    const void* pA = static_cast<const void*>(s1->GetArray<TransformComponent>());
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone -> master = B
    const void* pB = static_cast<const void*>(w.GetArray<TransformComponent>());
    EXPECT_NE(pA, pB);                                              // B is a fresh array
    s1.reset();                                                    // A refcount 0 -> recycled

    auto s2 = w.CreateSnapshot();                                  // master = B; s2 shares B; dirty cleared
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 3.0f; }); // clone -> Acquire pool -> reuse A
    const void* pReused = static_cast<const void*>(w.GetArray<TransformComponent>());
    EXPECT_EQ(pReused, pA);                                        // recycled A came back from the pool
    EXPECT_NE(pReused, pB);
    s2.reset();
}

static void TCOW02_pooled_clone_preserves_isolation_after_recycle()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1,1,1}});

    auto s1 = w.CreateSnapshot();
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone B
    s1.reset();                                                    // recycle A

    auto s2 = w.CreateSnapshot();                                  // s2 shares B (x == 2)
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 3.0f; }); // reuse A from pool, CopyFrom B

    // the reuse-clone into recycled A must NOT corrupt the live snapshot B
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 2.0f);
    EXPECT_EQ(w.GetComponent<TransformComponent>(e)->Position.x, 3.0f);
    s2.reset();
}

static void TCOW03_array_pool_stats_deltas()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{});
    auto s1 = w.CreateSnapshot();                                  // s1 shares A1
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 1.0f; }); // clone A1 -> A2 (master)

    const ComponentArrayPoolStats held = GetComponentArrayPoolStats();
    s1.reset();                                                    // A1 recycled -> Free +1, InUse -1
    const ComponentArrayPoolStats afterDrop = GetComponentArrayPoolStats();
    EXPECT_EQ(afterDrop.Free, held.Free + 1);
    EXPECT_EQ(afterDrop.InUse, held.InUse - 1);

    auto s2 = w.CreateSnapshot();                                  // shares A2 (no array-pool op)
    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 2.0f; }); // clone -> reuse A1
    const ComponentArrayPoolStats reused = GetComponentArrayPoolStats();
    EXPECT(reused.Reuses > held.Reuses);                           // a free-list reuse happened
    EXPECT_EQ(reused.Free, afterDrop.Free - 1);                    // free-list consumed by the reuse
    s2.reset();
}
```

Register them in `main()` immediately after the three `TCF` registrations from Task 1:

```cpp
    TCOW01_pool_reuses_recycled_array();
    TCOW02_pooled_clone_preserves_isolation_after_recycle();
    TCOW03_array_pool_stats_deltas();
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`
Expected: FAIL — `'ComponentArrayPoolStats': undeclared identifier` / `'GetComponentArrayPoolStats': identifier not found`.

- [ ] **Step 3: Declare the stats type + accessor in `ECS.h`**

In `src/common/include/ECS.h`, immediately after the `SnapshotPoolStats` block + its `GetSnapshotPoolStats()` declaration (ends ~line 524), insert:

```cpp
// Stats for the per-type ComponentArray recycle pool (COW Part 2), aggregated across
// all component types. Rendered by the editor Memory panel.
struct ComponentArrayPoolStats {
    size_t   Free;     // arrays sitting on the free-lists (summed over types)
    size_t   InUse;    // arrays handed out and not yet recycled
    size_t   Created;  // total arrays ever allocated by the pools
    uint64_t Reuses;   // # of Acquire calls served from a free-list
};
ECS_API ComponentArrayPoolStats GetComponentArrayPoolStats();
```

- [ ] **Step 4: Change `Clone()` to a declaration in `ECS.h`**

In `src/common/include/ECS.h`, in `class ComponentArray`, replace the inline `Clone()` definition (currently around lines 266-268):

```cpp
    [[nodiscard]] std::shared_ptr<IComponentArray> Clone() const override {
        return std::make_shared<ComponentArray<T>>(*this);
    }
```

with a declaration only:

```cpp
    [[nodiscard]] std::shared_ptr<IComponentArray> Clone() const override; // defined in ecs.cpp (uses the array pool)
```

- [ ] **Step 5: Add the pool machinery + out-of-line `Clone` in `ecs.cpp`**

In `src/ecs/src/ecs.cpp`, add `#include <atomic>` right after the existing `#include <mutex>` (line 3). Then, BETWEEN the includes and the existing `// Explicit class template instantiations` block (the `ECS_INSTANTIATE_CLASS` macro), insert:

```cpp
namespace {

// Aggregate counters across all per-type ComponentArray pools. Atomic so the panel
// (RenderThread) can read while GameThread/RenderThread Acquire/Recycle. Free stays
// equal to the summed free-list sizes (++ on Recycle, -- on reuse-Acquire).
struct ArrayPoolCountersT {
    std::atomic<size_t>   Free{0};
    std::atomic<size_t>   InUse{0};
    std::atomic<size_t>   Created{0};
    std::atomic<uint64_t> Reuses{0};
    void OnCreate()  noexcept { ++InUse; ++Created; }
    void OnReuse()   noexcept { --Free;  ++InUse; ++Reuses; }
    void OnRecycle() noexcept { ++Free;  --InUse; }
};
ArrayPoolCountersT& ArrayPoolCounters() { static ArrayPoolCountersT c; return c; }

// Per-type free-list of recycled ComponentArray<T>. Mutex-guarded: Recycle fires from a
// shared_ptr deleter on whatever thread drops the last ref (RenderThread when a snapshot
// recycles, GameThread otherwise). Non-leaked Meyers singleton per T; safe because the
// app joins both threads + resets LatestWorldSnapshot before static destruction, so no
// Recycle races the dtor. No InUse==0 assert (shutdown ordering, matching the staging pool).
template<typename T>
class ComponentArrayPool {
public:
    ComponentArray<T>* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ComponentArray<T>* a = m_Free.back();
            m_Free.pop_back();
            ArrayPoolCounters().OnReuse();
            return a;
        }
        ArrayPoolCounters().OnCreate();
        return new ComponentArray<T>();
    }
    void Recycle(ComponentArray<T>* a) noexcept {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(a);
        ArrayPoolCounters().OnRecycle();
    }
    ~ComponentArrayPool() { for (ComponentArray<T>* a : m_Free) delete a; }
private:
    std::mutex m_Mutex;
    std::vector<ComponentArray<T>*> m_Free;
};

template<typename T>
ComponentArrayPool<T>& GetArrayPool() { static ComponentArrayPool<T> pool; return pool; }

// Acquire a recycled (or fresh) array, copy src into it reusing capacity, and wrap it in a
// shared_ptr whose deleter returns it to the pool instead of freeing.
template<typename T>
std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>& src) {
    ComponentArray<T>* arr = GetArrayPool<T>().Acquire();
    arr->CopyFrom(src);
    return std::shared_ptr<ComponentArray<T>>(arr, [](ComponentArray<T>* p) noexcept {
        GetArrayPool<T>().Recycle(p);
    });
}

} // namespace

// Out-of-line so it can reach the file-local array pool. Instantiated for each registered
// T by the explicit `template class ComponentArray<T>` block below.
template<typename T>
std::shared_ptr<IComponentArray> ComponentArray<T>::Clone() const {
    return MakePooledClone(*this);
}
```

- [ ] **Step 6: Use the pool in `MutateArray`**

In `src/ecs/src/ecs.cpp`, in `ComponentStore::MutateArray` (the clone line, currently around line 23), replace:

```cpp
        slot = std::make_shared<ComponentArray<T>>(
                   static_cast<const ComponentArray<T>&>(*slot));
```

with:

```cpp
        slot = MakePooledClone<T>(static_cast<const ComponentArray<T>&>(*slot));
```

- [ ] **Step 7: Define `GetComponentArrayPoolStats()` in `ecs.cpp`**

In `src/ecs/src/ecs.cpp`, immediately after the existing `GetSnapshotPoolStats()` definition (around line 188), insert:

```cpp
ComponentArrayPoolStats GetComponentArrayPoolStats() {
    const auto& c = ArrayPoolCounters();
    return ComponentArrayPoolStats{
        c.Free.load(std::memory_order_relaxed),
        c.InUse.load(std::memory_order_relaxed),
        c.Created.load(std::memory_order_relaxed),
        c.Reuses.load(std::memory_order_relaxed)
    };
}
```

- [ ] **Step 8: Run the build to verify it passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`
Expected: clean compile/link (rebuilds `ecs.dll`).

- [ ] **Step 9: Run the tests**

Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`
Expected: `All ECS tests passed.` (exit 0) — including the existing COW/snapshot tests (T05/T06/T09/T10/TP01-03), which now exercise the pooled clone path and must still pass.

- [ ] **Step 10: Confirm consumers still compile against the changed ECS.h**

Run: `cmake --build --preset msvc-win64-vs2026-community --target game` then `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: both clean (the `Clone` decl move + new stats symbols don't break `editor`/`game`). No `GAME_API_VERSION` bump.

- [ ] **Step 11: Commit**

```bash
git add src/common/include/ECS.h src/ecs/src/ecs.cpp tests/test_ecs.cpp
git commit -m "Recycle ComponentArray clone buffers via per-type pool (COW Part 2)"
```

---

### Task 3: "ComponentArray Pool" Memory-panel section

**Files:**
- Modify: `src/editor/src/rendering/imgui/MemoryPanel.cpp`

- [ ] **Step 1: Add the panel section**

In `src/editor/src/rendering/imgui/MemoryPanel.cpp`, immediately after the existing "Staging Pool" `CollapsingHeader` block's closing `}` and before the `if (world && ImGui::CollapsingHeader("ECS Memory", ...))` block, insert:

```cpp
    if (ImGui::CollapsingHeader("ComponentArray Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ComponentArrayPoolStats s = GetComponentArrayPoolStats();
        ImGui::Text("Free:    %zu", s.Free);
        ImGui::Text("In use:  %zu", s.InUse);
        ImGui::Text("Created: %zu", s.Created);
        ImGui::Text("Reuses:  %llu", (unsigned long long)s.Reuses);
    }
```

`ComponentArrayPoolStats` and `GetComponentArrayPoolStats()` are declared in `<ECS.h>`, which `MemoryPanel.cpp` already includes — no new include needed.

- [ ] **Step 2: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: clean compile/link.

- [ ] **Step 3: Manual smoke (report honestly)**

Restart the editor (`./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`); the ECS.h change means the running editor must be relaunched. Open the Memory window, expand "ComponentArray Pool". With a moving scene, expect `Created` to settle to a small number and `Reuses` to climb each tick (per-tick clones served from the free-list), `In use` bounded. If you cannot run the GUI in this environment, report that the build passed and the visual smoke is pending the user. Do not claim runtime behavior you did not observe.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/rendering/imgui/MemoryPanel.cpp
git commit -m "Add ComponentArray Pool section to the Memory panel"
```

---

## Self-Review

**Spec coverage:**
- `CopyFrom` (dense `assign` + page-wise reuse, exact src reproduction) → Task 1 Step 3. ✓
- Per-type `ComponentArrayPool<T>` + `GetArrayPool<T>` Meyers singleton + mutex free-list + dtor frees list → Task 2 Step 5. ✓
- `MakePooledClone<T>` (Acquire→CopyFrom→recycling deleter) → Task 2 Step 5. ✓
- `MutateArray` uses the pool → Task 2 Step 6. ✓
- `Clone()` body moved to `ecs.cpp`, uses the pool, placed before the explicit instantiation → Task 2 Steps 4,5. ✓
- `RegisterComponent` left as `make_shared` → unchanged (no task touches it). ✓
- Aggregate atomic stats (`ArrayPoolCountersT` + `Free/InUse/Created/Reuses` semantics) + `ComponentArrayPoolStats` + `GetComponentArrayPoolStats()` → Task 2 Steps 3,5,7. ✓
- Panel section → Task 3. ✓
- Tests: CopyFrom correctness/empty/capacity (Task 1); pool reuse, isolation-after-recycle, stats deltas (Task 2); existing COW tests still green (Task 2 Step 9). ✓
- Build/ABI: no GAME_API_VERSION bump; rebuild ecs+game+editor+test_ecs; restart editor → Task 2 Steps 8-10, Task 3 Steps 2-3. ✓

**Placeholder scan:** every code step has complete code; no TBD/TODO/"handle edge cases". ✓

**Type consistency:** `CopyFrom(const ComponentArray&)`, `ComponentArrayPool<T>`, `GetArrayPool<T>`, `MakePooledClone<T>`, `ArrayPoolCounters()` (+ `OnCreate/OnReuse/OnRecycle`), `ComponentArrayPoolStats{Free,InUse,Created,Reuses}`, `GetComponentArrayPoolStats()` are spelled identically across ECS.h, ecs.cpp, tests, and the panel. `Clone()` signature unchanged (only its definition relocates). The `MutateArray` replacement matches the existing `static_cast<const ComponentArray<T>&>(*slot)` form. ✓

**Ordering note (locked):** in `ecs.cpp` the pool block + out-of-line `Clone` MUST sit above the `ECS_INSTANTIATE_CLASS` (`template class ComponentArray<T>`) block so the explicit instantiation emits `Clone`, and above `MutateArray` so it sees `MakePooledClone`. Task 2 Step 5 places them right after the includes, before that macro. ✓
