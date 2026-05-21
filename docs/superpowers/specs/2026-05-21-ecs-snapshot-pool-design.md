# ECS Snapshot Object Pool (COW Part 1) — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** `allocator-toolkit` (stacking).
**Depends on:** the ComponentArray paged sparse-set (already landed) — made the per-dirty-type clones cheap/contiguous, so this part targets the *fixed* per-tick scaffolding churn.

## Goal

Stop allocating (and cross-thread freeing) the per-tick ECS *snapshot scaffolding* — the
`make_shared<ECS>` shell, the two `EntityStore` vectors, and the
`unordered_map<type_index, shared_ptr>` copy — which `CreateSnapshot` does **every tick,
even when nothing changed**. Pool and reuse the snapshot ECS object instead, with its
internal vector/map capacity retained, so steady-state snapshot publishing does
near-zero heap work and never destroys the scaffolding on the consumer thread.

## Scope

**In scope (Part 1):**
- A `SnapshotPool` (free-list of reusable ECS objects) inside `ecs.dll`.
- `CreateSnapshot` acquires a recycled ECS and publishes a `shared_ptr<const ECS>` whose
  custom deleter **recycles** the object instead of `delete`ing it.
- Reuse the shell + `EntityStore` vectors + component-array map (retained capacity).
- Exported `GetSnapshotPoolStats()` rendered as a "Snapshot Pool" section in the editor
  `MemoryPanel`.
- `test_ecs` coverage (reuse, isolation regression, stats).

**Out of scope (Part 2, deferred):** recycling the per-dirty-type `ComponentArray` clone
buffers cross-thread. Those still alloc on the master's COW write and free when the
last snapshot referencing the old array drops (on whatever thread) — unchanged here, and
already cheap/contiguous post-sparse-set.

## Background — the lifecycle (why this is safe)

Per tick, `ECS::CreateSnapshot` (`ecs.cpp:114-121`):
```cpp
auto snap = std::make_shared<ECS>();                     // (a) shell — every tick
snap->m_EntityStore = m_EntityStore;                     // (b) 2 vectors — every tick
snap->m_SingletonEntity = m_SingletonEntity;
snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // (c) map copy (~15 nodes) — every tick
m_ComponentStore.ClearDirty();
```
`(c)` copies a map of `shared_ptr<IComponentArray>` (refcount bumps; arrays are *shared*,
not deep-copied). The published `shared_ptr<const ECS>` goes to the `LatestWorldSnapshot`
atomic slot; RenderThread (144 Hz) and ImGui each load their own ref and drop it ~1-2
frames later. The **last-ref drop — and thus the free — runs non-deterministically on the
RenderThread, GameThread, or ImGui** (RenderThread commonly, since it outpaces the 60 Hz
GameThread). In-flight snapshots are bounded (~1-3: the slot + up to 2 reader refs).

**Shutdown drains before teardown:** Application shutdown joins GameThread + RenderThread
and resets the `LatestWorldSnapshot` slot **before returning from `main`**. So no snapshot
`shared_ptr` is alive when static/DLL destructors run — which is what makes a non-leaked
singleton pool safe (no recycling deleter can fire during static destruction).

## Design

### `SnapshotPool` (ecs.dll-internal)

A free-list of reusable `ECS` objects, mutex-guarded (touched by GameThread `Acquire` and
by recycling deleters that fire on RenderThread/GameThread/ImGui). LIFO (push/pop back) so
single-threaded reuse is deterministic (testable).

```cpp
// in ecs.cpp (internal)
class SnapshotPool {
public:
    ECS* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ECS* e = m_Free.back();
            m_Free.pop_back();
            ++m_InUse; ++m_Reuses;
            return e;
        }
        ECS* e = new ECS();
        ++m_Created; ++m_InUse;
        return e;
    }
    void Recycle(ECS* e) {
        e->ResetForRecycle();                  // release array refs, keep capacity
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(e);
        --m_InUse;
    }
    SnapshotPoolStats Stats() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return SnapshotPoolStats{ m_Free.size(), m_InUse, m_Created, m_Reuses };
    }
    ~SnapshotPool() { for (ECS* e : m_Free) delete e; }   // clean teardown (no leak)
private:
    mutable std::mutex m_Mutex;
    std::vector<ECS*>  m_Free;
    size_t   m_InUse   = 0;
    size_t   m_Created = 0;
    uint64_t m_Reuses  = 0;
};

SnapshotPool& GetSnapshotPool() {
    static SnapshotPool pool;   // Meyers singleton — non-leaked.
    // Safe: the app joins both threads and clears LatestWorldSnapshot before
    // teardown, so no recycling deleter fires during static destruction.
    return pool;
}
```

`ResetForRecycle` runs at recycle time (when the snapshot dies) so the dead snapshot's
component-array refs drop promptly rather than lingering in an idle pool slot. `EntityStore`
holds no refs and is overwritten on the next `Acquire`, so only the component arrays must
be cleared:
```cpp
// new ECS member
void ECS::ResetForRecycle() {
    m_ComponentStore.Cleanup();    // m_ComponentArrays.clear() — releases array refs, keeps buckets
    m_ComponentStore.ClearDirty(); // m_DirtyThisTick.clear()
}
```
(`Cleanup` and `ClearDirty` already exist on `ComponentStore`. `unordered_map::clear`
keeps `bucket_count`, so capacity is retained.)

### `CreateSnapshot` rewrite (`ecs.cpp`)

```cpp
std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    ECS* snap = GetSnapshotPool().Acquire();
    snap->m_EntityStore     = m_EntityStore;                  // reuses snap's vector capacity
    snap->m_SingletonEntity = m_SingletonEntity;
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore);  // reuses snap's map buckets
    m_ComponentStore.ClearDirty();
    return std::shared_ptr<const ECS>(snap, [](const ECS* p) {
        GetSnapshotPool().Recycle(const_cast<ECS*>(p));
    });
}
```
A recycled `ECS` skips the `ECS()` ctor's reserved-id setup, but `CreateSnapshot` overwrites
`m_EntityStore`/`m_SingletonEntity` anyway — identical to today's `make_shared<ECS>()` path
which also constructs-then-overwrites.

**Per-tick allocation after this change:** the shell + 2 vectors + map are reused (zero
alloc when capacity suffices); the only remaining alloc is the one `shared_ptr` control
block (`shared_ptr(ptr, deleter)` can't use `make_shared`) — ~24 bytes, far below what's
removed. Pooling the control block too is a deferred micro-opt (YAGNI).

### Stats + editor panel

In `ECS.h`:
```cpp
struct SnapshotPoolStats { size_t Free; size_t InUse; size_t Created; uint64_t Reuses; };
ECS_API SnapshotPoolStats GetSnapshotPoolStats();   // defined in ecs.cpp -> GetSnapshotPool().Stats()
```
The editor `MemoryPanel` (`src/editor/src/rendering/imgui/MemoryPanel.cpp`) gains a
"Snapshot Pool" `CollapsingHeader` showing Free / In-use / Created / Reuses. It calls the
exported `GetSnapshotPoolStats()` directly — **no `ecs.dll → Engine.dll` dependency** (the
pool is an object pool, not an `IAllocator`, so it doesn't belong in the allocator
registry; the editor, which already links both, bridges them).

## Build / ABI

- `ECS.h` gains `SnapshotPoolStats` + `GetSnapshotPoolStats()` decl, and the
  `ECS::ResetForRecycle()` member. `ecs.cpp` gets the pool + rewritten `CreateSnapshot`.
  `MemoryPanel.cpp` gets the new section.
- **No `GAME_API_VERSION` bump** (no `GameState` layout / game-export / component-type
  change). But `ECS.h` is shared → rebuild `ecs.dll` + `editor` + `game` together; restart
  the editor for the smoke test.
- New exported symbol `GetSnapshotPoolStats` from `ecs.dll`; editor links it (already links
  `ecs`). `game.dll` doesn't use it.

## Testing (`test_ecs`, single-threaded — exercises the pool + isolation)

- **Reuse (pointer identity):** `auto s1 = w.CreateSnapshot(); const ECS* p = s1.get();
  s1.reset(); auto s2 = w.CreateSnapshot(); EXPECT(s2.get() == p);` — LIFO guarantees the
  just-recycled object is reacquired.
- **Isolation regression:** the existing snapshot/COW tests (snapshot independent from the
  master; COW-clone-once-per-tick) must still pass — recycling must not leak stale state
  between reuses (verify a fresh snapshot reflects the master's current state, not a prior
  snapshot's).
- **Stats deltas:** capture `GetSnapshotPoolStats()` before; hold a snapshot → `InUse`
  increases by 1; drop it → `InUse` returns and `Free` increases; recycle+reacquire →
  `Reuses` increases; first-ever acquire on an empty pool → `Created` increases. Assert
  relative deltas (the pool is process-global, so don't assert absolutes).

The cross-thread reclaim (deleter firing on the RenderThread) isn't unit-testable without
threads; the mutex makes it safe, and the editor smoke test (renders identically, no crash,
panel shows the pool recycling) is the integration check.

## Risks

- **Shutdown order:** handled — non-leaked Meyers singleton is safe because snapshots are
  drained (threads joined + slot reset) before static destruction; documented in a comment.
- **Stale state across reuse:** mitigated by `ResetForRecycle` (clears arrays) + the full
  overwrite in `CreateSnapshot`; the isolation regression tests guard it.
- **Idle pool retention:** clear-on-recycle drops array refs promptly; idle slots hold only
  empty ECS shells.
- **Control-block alloc per tick:** accepted (tiny); deferred micro-opt.
- **DLL locality:** pool + deleters live in `ecs.dll` (same side as the `ComponentArray`
  deleters) — preserves the hot-reload safety property.
