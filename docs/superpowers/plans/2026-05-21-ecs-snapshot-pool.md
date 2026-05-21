# ECS Snapshot Object Pool (COW Part 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pool and recycle the per-tick ECS snapshot object (shell + EntityStore vectors + component-array map) so `CreateSnapshot` stops allocating that scaffolding every tick and never destroys it on the consumer thread.

**Architecture:** An `ecs.dll`-internal `SnapshotPool` (mutex-guarded LIFO free-list of `ECS*`, non-leaked Meyers singleton). `CreateSnapshot` acquires a recycled `ECS`, repopulates it (copy-assigns reuse retained capacity), and publishes a `shared_ptr<const ECS>` whose **custom deleter recycles instead of deletes**. A small exported stats accessor feeds an editor Memory-panel section. The per-dirty-type `ComponentArray` clone buffers are out of scope (deferred Part 2).

**Tech Stack:** C++23, `ecs.dll`, custom `test_ecs` harness (no GPU), editor ImGui Memory panel.

**Spec:** `docs/superpowers/specs/2026-05-21-ecs-snapshot-pool-design.md`

**Model guidance:** Dispatch **all** implementer + reviewer subagents on **Opus 4.7**. Highest-risk: the recycling custom deleter + Meyers-singleton shutdown safety, and `ResetForRecycle` (must be assert-free / thread-agnostic and leak no stale state between reuses).

**Branch:** Already on `allocator-toolkit` (stacking). Do not switch branches. Do NOT offer to merge.

**Build/ABI note:** No `GAME_API_VERSION` bump (no `GameState`/export/component-type change). `ECS.h` gains a struct + a free-function decl + a member, so rebuild `ecs.dll` + `editor` + `game` together; restart the editor for the smoke test. New exported symbol `GetSnapshotPoolStats` from `ecs.dll`. Build preset: `msvc-win64-vs2026-community`.

**Critical correctness note (verified against the code):** `ECS::ResetForRecycle` runs from the recycling deleter on **whatever thread drops the last ref** (commonly the RenderThread). It MUST call only `ComponentStore::Cleanup()` (assert-free `m_ComponentArrays.clear()`). It MUST NOT call `ComponentStore::ClearDirty()` — that runs `AssertOwnerThread()` (GameThread-only) and would fire in debug on a cross-thread recycle. The snapshot's dirty set is always empty anyway (snapshots never mutate).

---

## File Structure

- **Modify** `src/common/include/ECS.h` — add `struct SnapshotPoolStats` + `ECS_API SnapshotPoolStats GetSnapshotPoolStats();` (before the `ECS` class) and a public inline `ECS::ResetForRecycle()` member (Task 1).
- **Modify** `src/ecs/src/ecs.cpp` — add `#include <mutex>`, the internal `SnapshotPool` + `GetSnapshotPool()` (anon namespace), the exported `GetSnapshotPoolStats()` definition, and rewrite `ECS::CreateSnapshot` (Task 1).
- **Modify** `tests/test_ecs.cpp` — pool reuse + isolation + stats tests (Task 1).
- **Modify** `src/editor/src/rendering/imgui/MemoryPanel.cpp` — a "Snapshot Pool" section (Task 2).

## Conventions for every task

- Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <test_ecs|editor|game>`. Run ECS tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`).
- Use the Bash tool. Author identity personal `Nuno Silva <nuno.levezinho@live.com.pt>` (already configured). Never stage the untracked `.claude/` directory.

---

### Task 1: SnapshotPool + recycling CreateSnapshot + stats + tests

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_ecs.cpp`, add before `main` (the harness provides `EXPECT`/`EXPECT_EQ`; `ECS`, `TransformComponent`, `EntityId`, `Modify`, `GetComponent`, `GetArray` are available; `SnapshotPoolStats`/`GetSnapshotPoolStats` will exist after Step 3):

```cpp
static void TP01_snapshot_pool_reuses_object()
{
    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    auto s1 = w.CreateSnapshot();
    const ECS* p = s1.get();
    s1.reset();                       // recycle p
    auto s2 = w.CreateSnapshot();     // LIFO -> reacquires p
    EXPECT_EQ(s2.get(), p);
}

static void TP02_snapshot_isolated_after_recycle()
{
    ECS w;
    EntityId e = w.CreateEntity();
    w.AddComponent(e, TransformComponent{{1.0f, 0, 0}, {}, {1, 1, 1}});

    auto s1 = w.CreateSnapshot();
    s1.reset();                       // recycle the object s2 will reuse

    w.Modify<TransformComponent>(e, [](auto& t){ t.Position.x = 9.0f; });
    auto s2 = w.CreateSnapshot();     // reuses the recycled object

    // Must reflect CURRENT master (9.0), not stale s1 state (1.0):
    const auto* t = s2->GetComponent<TransformComponent>(e);
    EXPECT(t != nullptr);
    EXPECT_EQ(t->Position.x, 9.0f);

    // And remain isolated: a later master mutation must not change s2 (COW):
    w.Modify<TransformComponent>(e, [](auto& tt){ tt.Position.x = 5.0f; });
    EXPECT_EQ(s2->GetComponent<TransformComponent>(e)->Position.x, 9.0f);
}

static void TP03_snapshot_pool_stats_deltas()
{
    const SnapshotPoolStats before = GetSnapshotPoolStats();

    ECS w;
    w.AddComponent(w.CreateEntity(), TransformComponent{});
    auto s = w.CreateSnapshot();
    const SnapshotPoolStats held = GetSnapshotPoolStats();
    EXPECT_EQ(held.InUse, before.InUse + 1);

    s.reset();
    const SnapshotPoolStats dropped = GetSnapshotPoolStats();
    EXPECT_EQ(dropped.InUse, before.InUse);
    EXPECT(dropped.Free >= 1);

    auto s2 = w.CreateSnapshot();     // served from the free-list
    const SnapshotPoolStats reused = GetSnapshotPoolStats();
    EXPECT(reused.Reuses > before.Reuses);
    s2.reset();
}
```

Register in `main` (after the existing test calls):

```cpp
    TP01_snapshot_pool_reuses_object();
    TP02_snapshot_isolated_after_recycle();
    TP03_snapshot_pool_stats_deltas();
```

- [ ] **Step 2: Run to verify failure**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
```
Expected: compile error — `SnapshotPoolStats` / `GetSnapshotPoolStats` undefined. (Even once those exist, `TP01` would fail against the old `make_shared` `CreateSnapshot` because each snapshot is a fresh object — that's the RED the pooling turns green.)

- [ ] **Step 3: Add the ECS.h declarations + ResetForRecycle**

In `src/common/include/ECS.h`, immediately BEFORE the `class ECS_API ECS` declaration (the "ECS World (Main API)" section), add:

```cpp
// Stats for the snapshot object pool (see ecs.cpp). Rendered by the editor Memory panel.
struct SnapshotPoolStats {
    size_t   Free;     // idle ECS objects in the pool free-list
    size_t   InUse;    // currently handed out (live snapshots)
    size_t   Created;  // total ECS objects ever allocated by the pool
    uint64_t Reuses;   // # of Acquire calls served from the free-list
};
ECS_API SnapshotPoolStats GetSnapshotPoolStats();
```

Then, inside the `ECS` class public section, immediately AFTER the `CreateSnapshot()` declaration (`[[nodiscard]] std::shared_ptr<const ECS> CreateSnapshot();`), add:

```cpp
    /**
     * @brief Resets a recycled snapshot for reuse by the pool. Releases this
     *        snapshot's component-array refs but keeps the map's bucket capacity.
     * @threading Runs from the pool's recycling deleter on WHATEVER thread drops
     *            the last ref. Must stay assert-free: only Cleanup() (no
     *            AssertOwnerThread). Do NOT call ClearDirty() here.
     */
    void ResetForRecycle() { m_ComponentStore.Cleanup(); }
```

- [ ] **Step 4: Add the pool + rewrite CreateSnapshot in ecs.cpp**

In `src/ecs/src/ecs.cpp`, add `#include <mutex>` right after `#include "ECS.h"`:

```cpp
#include "ECS.h"
#include <mutex>
```

Add the pool in an anonymous namespace immediately above `ECS::CreateSnapshot` (i.e. just before the current `std::shared_ptr<const ECS> ECS::CreateSnapshot()` at line ~114):

```cpp
namespace {

// Recycles ECS snapshot objects so CreateSnapshot doesn't allocate the shell +
// EntityStore vectors + array map every tick. Mutex-guarded (touched once/tick by
// Acquire on GameThread and once/snapshot by the recycling deleter on whatever
// thread drops the last ref). LIFO so single-threaded reuse is deterministic.
class SnapshotPool {
public:
    ECS* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ECS* e = m_Free.back();
            m_Free.pop_back();
            ++m_InUse;
            ++m_Reuses;
            return e;
        }
        ECS* e = new ECS();
        ++m_Created;
        ++m_InUse;
        return e;
    }
    void Recycle(ECS* e) {
        e->ResetForRecycle();                 // release array refs (assert-free)
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(e);
        --m_InUse;
    }
    SnapshotPoolStats Stats() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return SnapshotPoolStats{ m_Free.size(), m_InUse, m_Created, m_Reuses };
    }
    ~SnapshotPool() { for (ECS* e : m_Free) delete e; }
private:
    mutable std::mutex m_Mutex;
    std::vector<ECS*>  m_Free;
    size_t   m_InUse   = 0;
    size_t   m_Created = 0;
    uint64_t m_Reuses  = 0;
};

// Non-leaked Meyers singleton. Safe: the app joins both threads and resets
// LatestWorldSnapshot before teardown, so no recycling deleter fires during
// static destruction.
SnapshotPool& GetSnapshotPool() {
    static SnapshotPool pool;
    return pool;
}

} // namespace
```

Replace the existing `CreateSnapshot` body:

```cpp
std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    auto snap = std::make_shared<ECS>();
    snap->m_EntityStore = m_EntityStore;                     // value copy
    snap->m_SingletonEntity = m_SingletonEntity;             // preserve reserved id
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // shallow shared_ptr copy
    m_ComponentStore.ClearDirty();
    return snap;
}
```

with:

```cpp
std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    ECS* snap = GetSnapshotPool().Acquire();                  // recycled or fresh
    snap->m_EntityStore = m_EntityStore;                     // reuses snap's vector capacity
    snap->m_SingletonEntity = m_SingletonEntity;             // preserve reserved id
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // reuses snap's map buckets
    m_ComponentStore.ClearDirty();
    return std::shared_ptr<const ECS>(snap, [](const ECS* p) {
        GetSnapshotPool().Recycle(const_cast<ECS*>(p));
    });
}
```

Add the exported stats accessor definition (global scope — matches the `ECS.h` decl). Put it immediately after `CreateSnapshot`:

```cpp
SnapshotPoolStats GetSnapshotPoolStats() {
    return GetSnapshotPool().Stats();
}
```

- [ ] **Step 5: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.` (TP01-TP03 + all pre-existing tests pass).

- [ ] **Step 6: Commit**

```bash
git add src/common/include/ECS.h src/ecs/src/ecs.cpp tests/test_ecs.cpp
git commit -m "perf(ecs): pool + recycle ECS snapshot objects (COW Part 1)"
```

---

### Task 2: Editor Memory-panel "Snapshot Pool" section

**Files:**
- Modify: `src/editor/src/rendering/imgui/MemoryPanel.cpp`

- [ ] **Step 1: Add the section**

In `src/editor/src/rendering/imgui/MemoryPanel.cpp`, add the include near the other includes:

```cpp
#include <ECS.h>
```

Then, inside `DrawMemoryPanel`, immediately before the final `ImGui::End();`, add a "Snapshot Pool" section (it sits alongside the existing "By Category" / "Allocators" `CollapsingHeader`s):

```cpp
    if (ImGui::CollapsingHeader("Snapshot Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const SnapshotPoolStats s = GetSnapshotPoolStats();
        ImGui::Text("Free:    %zu", s.Free);
        ImGui::Text("In use:  %zu", s.InUse);
        ImGui::Text("Created: %zu", s.Created);
        ImGui::Text("Reuses:  %llu", (unsigned long long)s.Reuses);
    }
```

(Read the file first to confirm the structure — there is one `ImGui::Begin("Memory", open)` / matching `ImGui::End();`; the new block goes just before that `End()`. `GetSnapshotPoolStats()` is the `ecs.dll`-exported function from Task 1; the editor already links `ecs`, so no new link dependency — no `Engine` coupling.)

- [ ] **Step 2: Build the editor**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds + links cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/rendering/imgui/MemoryPanel.cpp
git commit -m "feat(editor): Memory panel shows ECS snapshot pool stats"
```

---

### Task 3: Full build + verification

**Files:** none (verification only).

- [ ] **Step 1: Rebuild ecs + game + editor together**

`ECS.h` changed; all consumers rebuild.

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

Restart + launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe` (full restart — `ECS.h`/`ecs.dll` changed). Confirm: scene renders identically; entities behave (text spins/cycles, day/night light moves); add/remove components via inspector works; world save/load works if exercised; **no crash over an extended run** (the recycling deleter fires continuously on the RenderThread — a shutdown/threading bug would surface here). In the Memory panel, the "Snapshot Pool" section shows `Reuses` climbing and `Created` staying small/bounded (a few objects), `In use` ~1-3. Report that this requires the running UI rather than claiming success from a compile. Also exercise shutdown (close the editor cleanly) — confirm no crash on exit (Meyers-singleton teardown safety).

---

## Self-Review

**1. Spec coverage:**
- `SnapshotPool` (mutex LIFO free-list, counters, Acquire/Recycle/Stats/dtor): Task 1 Step 4. ✓
- Non-leaked Meyers `GetSnapshotPool()`: Task 1 Step 4. ✓
- `CreateSnapshot` acquires recycled ECS + recycling custom deleter: Task 1 Step 4. ✓
- `ECS::ResetForRecycle()` = `Cleanup()` only (assert-free; NOT `ClearDirty`): Task 1 Step 3 + critical note. ✓ (spec corrected to match)
- `SnapshotPoolStats` + `ECS_API GetSnapshotPoolStats()`: Task 1 Steps 3-4. ✓
- Editor Memory-panel "Snapshot Pool" section, no ecs→Engine coupling: Task 2. ✓
- Tests: reuse (pointer identity), isolation regression (fresh-state-after-recycle + COW independence), stats deltas: Task 1 Step 1. ✓
- No GAME_API_VERSION bump; rebuild all + restart: header note + Task 3. ✓

**2. Placeholder scan:** No TBD/TODO. Every code step shows full before/after. (Task 2 Step 1 asks the implementer to read MemoryPanel.cpp for exact placement of one block — the block's full code is given; placement is "before the final `ImGui::End()`".)

**3. Type consistency:** `SnapshotPoolStats { size_t Free, InUse, Created; uint64_t Reuses; }` defined in Task 1 Step 3, used identically in the pool `Stats()` (Step 4), the tests (Step 1), and the panel (Task 2). `GetSnapshotPoolStats()` signature matches between ECS.h decl and ecs.cpp def. `SnapshotPool::Acquire()/Recycle()/Stats()` and the anon `GetSnapshotPool()` are consistent. `ResetForRecycle()` is a public ECS member called by `Recycle`. The recycling lambda matches the `shared_ptr<const ECS>` deleter signature `void(const ECS*)`.

**Note for executor:** `ECS::CreateSnapshot` is an `ECS` member, so it may access `snap->m_EntityStore`/`m_SingletonEntity`/`m_ComponentStore` (private members of another `ECS` instance — same-class access; the old code already did this). The anon-namespace `GetSnapshotPool()` has internal linkage and is visible to both `CreateSnapshot` and the exported `GetSnapshotPoolStats()` since all three live in `ecs.cpp`. `new ECS()` in `Acquire` uses the public default ctor. Do not bump `GAME_API_VERSION`.
