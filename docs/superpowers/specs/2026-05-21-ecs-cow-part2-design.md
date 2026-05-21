# ECS COW Part 2 — ComponentArray Buffer Recycle — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** `allocator-toolkit` (stacking).

## Goal

Kill the last steady-state per-tick allocation in the ECS hot path: the deep-copy clone
that `ComponentStore::MutateArray` performs on the first mutation of a component type each
tick. COW Part 1 (the snapshot pool) already recycles the ECS *shell* + EntityStore; this
recycles the *component-array buffers* that the shell's clone allocates and frees every tick.

## Background — the per-tick clone

Per tick, for each component type the game mutates (e.g. `TransformComponent` on movement),
`ComponentStore::MutateArray<T>` (`src/ecs/src/ecs.cpp:13-27`) runs once (gated by
`m_DirtyThisTick`) and does:

```cpp
slot = std::make_shared<ComponentArray<T>>(static_cast<const ComponentArray<T>&>(*slot));
```

This deep-copies the array via the copy constructor (`ECS.h:192-199`), allocating fresh
`m_Components` and `m_IndexToEntity` vector buffers plus a `make_unique<SparsePage>` (4 KB)
for every non-null page. The displaced (old) array stays alive in the published snapshot(s)
and is freed when the last snapshot referencing it is recycled (the snapshot pool's recycling
deleter → `ResetForRecycle` → `Cleanup` → `m_ComponentArrays.clear()` drops the refs).

So the steady-state churn per mutated type per tick is: **one array's worth of buffers
allocated (the clone) and, ~1-2 ticks later, one freed (the displaced array).** The 4 KB
sparse page is the largest single chunk (a small scene's dense data is a few hundred bytes;
a page is 4 KB).

A blocking detail: `ComponentArray::operator=` (`ECS.h:201-209`) is copy-and-swap — it
builds a fresh `tmp` (copy ctor, allocates) then moves — so it **reallocates** and gives no
capacity reuse. Recycling therefore needs a new capacity-reusing `CopyFrom` method, not
`operator=`.

## Safety invariant (why this is correct)

The pool holds an array **only after its refcount has hit 0** — i.e. neither the master nor
any snapshot references it. Therefore an `Acquire`d array is never aliased by a live snapshot,
and overwriting it via `CopyFrom` cannot corrupt a published snapshot. The master's current
array always has refcount ≥ 1 (the master's own slot), so it is never in the pool. This gate
is the entire correctness argument for buffer reuse.

## Scope

**In scope:**
- `ComponentArray<T>::CopyFrom(const ComponentArray& src)` — a deep copy that reuses the
  destination's existing capacity (dense vectors via `assign`; sparse pages reused page-wise).
- A per-type `ComponentArrayPool<T>` (mutex free-list) + `GetArrayPool<T>()` Meyers singleton,
  in `ecs.cpp` (anonymous namespace).
- `MakePooledClone<T>(src)` helper (Acquire → CopyFrom → `shared_ptr` with a recycling
  deleter), used by `MutateArray<T>` and `ComponentArray<T>::Clone()`.
- Move `ComponentArray<T>::Clone()`'s body from the header into `ecs.cpp` so it can reach the
  pool.
- Aggregate pool stats (global atomics) + `ComponentArrayPoolStats` + `GetComponentArrayPoolStats()`.
- A "ComponentArray Pool" section in the editor Memory panel.
- `test_ecs` coverage (CopyFrom correctness, pool reuse, snapshot-isolation regression, stats).

**Out of scope:**
- Routing `RegisterComponent`'s initial empty array through the pool — left as `make_shared`
  (one allocation per type at startup; its array is plain-`delete`d at most once when first
  displaced). No empty-acquire/clear path is added.
- Recycling the entity-store buffers (already shared via the snapshot pool's EntityStore copy)
  or the `m_ComponentArrays` map nodes (Part 1 territory).
- Any change to the per-tick *copy* itself — copying the data is intrinsic to COW; Part 2
  removes only the *allocation*, not the copy.

## Design

### `ComponentArray<T>::CopyFrom` (new; inline in `ECS.h`)

Pure (no pool dependency), so it is unit-testable in isolation. Reuses destination capacity:

```cpp
// Deep-copies src into *this, reusing existing buffer capacity where possible.
// Result is structurally identical to a copy-constructed clone of src.
void CopyFrom(const ComponentArray& src) {
    m_Components.assign(src.m_Components.begin(), src.m_Components.end());
    m_IndexToEntity.assign(src.m_IndexToEntity.begin(), src.m_IndexToEntity.end());

    m_SparsePages.resize(src.m_SparsePages.size());   // drops surplus dest pages
    for (size_t i = 0; i < src.m_SparsePages.size(); ++i) {
        if (!src.m_SparsePages[i]) {
            m_SparsePages[i].reset();                 // match src's null slot
        } else if (!m_SparsePages[i]) {
            m_SparsePages[i] = std::make_unique<SparsePage>(*src.m_SparsePages[i]);
        } else {
            *m_SparsePages[i] = *src.m_SparsePages[i]; // reuse the 4 KB buffer (array copy)
        }
    }
}
```

Correctness points:
- `vector::assign` reuses the destination buffer when its capacity ≥ src size, else grows
  once. The dense vectors end up exactly src's contents.
- `m_SparsePages.resize(src.size())` first, so the final page count equals src's; surplus
  destination pages (a recycled array that previously held more pages) are dropped, and any
  stale entries they held vanish. Each retained slot is set to exactly mirror src (null where
  src is null; buffer-reused copy where src has a page). The result is bit-for-bit equivalent
  to the copy constructor's output — verified by test.

`CopyFrom` fully overwrites, so a recycled array carries no observable stale state; no
separate "clear for reuse" step is needed.

### Per-type pool (`ecs.cpp`, anonymous namespace)

```cpp
template<typename T>
class ComponentArrayPool {
public:
    ComponentArray<T>* Acquire() {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Free.empty()) {
            ComponentArray<T>* a = m_Free.back();
            m_Free.pop_back();
            ArrayPoolCounters().OnReuse();   // --Free, ++InUse, ++Reuses
            return a;
        }
        ArrayPoolCounters().OnCreate();      // ++InUse, ++Created
        return new ComponentArray<T>();
    }
    void Recycle(ComponentArray<T>* a) noexcept {  // fires from a shared_ptr deleter
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(a);
        ArrayPoolCounters().OnRecycle();     // ++Free, --InUse
    }
    ~ComponentArrayPool() { for (auto* a : m_Free) delete a; }
private:
    std::mutex m_Mutex;
    std::vector<ComponentArray<T>*> m_Free;
};

template<typename T>
ComponentArrayPool<T>& GetArrayPool() { static ComponentArrayPool<T> pool; return pool; }
```

Per-type Meyers singletons — non-leaked. Lifetime: the master ECS (editor-owned, in
`GameState`) is destroyed when `Application` shuts down, *after* both threads are joined and
`LatestWorldSnapshot` is reset; the Meyers pools (constructed on first `Acquire`, early) are
destroyed later at static destruction, so every `Recycle` (including those firing from the
master ECS's own teardown and from snapshot-pool recycling) happens before the pool dtor.
Mirrors the snapshot pool's lifetime reasoning.

Aggregate counters are a separate global (so the panel reads one rollup, not per-type):

```cpp
struct ArrayPoolCountersT {
    std::atomic<size_t>   Free{0}, InUse{0}, Created{0};
    std::atomic<uint64_t> Reuses{0};
    void OnCreate()  noexcept { ++InUse; ++Created; }
    void OnReuse()   noexcept { --Free;  ++InUse; ++Reuses; }
    void OnRecycle() noexcept { ++Free;  --InUse; }
};
ArrayPoolCountersT& ArrayPoolCounters() { static ArrayPoolCountersT c; return c; }
```

(All counters are atomic; the free-list itself is guarded by the per-pool mutex. The
`--Free`/`++Free` on Acquire-from-free and Recycle keep `Free` equal to the summed free-list
sizes across all pools.)

### Recycling clone helper

```cpp
template<typename T>
std::shared_ptr<ComponentArray<T>> MakePooledClone(const ComponentArray<T>& src) {
    ComponentArray<T>* arr = GetArrayPool<T>().Acquire();
    arr->CopyFrom(src);
    return std::shared_ptr<ComponentArray<T>>(arr, [](ComponentArray<T>* p) noexcept {
        GetArrayPool<T>().Recycle(p);
    });
}
```

- `MutateArray<T>` (ecs.cpp): replace the `make_shared<ComponentArray<T>>(*old)` clone line
  with `slot = MakePooledClone<T>(static_cast<const ComponentArray<T>&>(*slot));`.
- `ComponentArray<T>::Clone()`: change the header declaration to a plain (non-inline)
  declaration and **define it in `ecs.cpp`** (before the explicit `template class` instantiation):
  `template<typename T> std::shared_ptr<IComponentArray> ComponentArray<T>::Clone() const { return MakePooledClone(*this); }`.
  The existing explicit instantiation `template class ComponentArray<T>` then emits the
  out-of-line `Clone` for each registered T. (`MakePooledClone`/`GetArrayPool` are file-local
  to `ecs.cpp`, which is why `Clone`'s body must live there.)
- `RegisterComponent` is unchanged (`make_shared`).

The recycling `shared_ptr<ComponentArray<T>>` converts implicitly to the
`shared_ptr<IComponentArray>` stored in `m_ComponentArrays`.

### Stats + panel

In `ECS.h`:
```cpp
struct ComponentArrayPoolStats { size_t Free; size_t InUse; size_t Created; uint64_t Reuses; };
ECS_API ComponentArrayPoolStats GetComponentArrayPoolStats();
```
Defined in `ecs.cpp` reading `ArrayPoolCounters()`. The editor `MemoryPanel.cpp` adds a
"ComponentArray Pool" `CollapsingHeader` (Free/In use/Created/Reuses), placed alongside the
existing "Snapshot Pool" / "Staging Pool" sections. Editor already links `ecs`; no new
coupling.

## Build / ABI

`ECS.h` changes (new inline `CopyFrom`; `Clone()` becomes a declaration with the definition
moved to `ecs.cpp`; new `ComponentArrayPoolStats` + `GetComponentArrayPoolStats()` decl) →
**rebuild `ecs.dll` + `editor` + `game` + `test_ecs` together; restart the editor** for the
smoke test. **No `GAME_API_VERSION` bump** — no `GameState` / game-export / component-type
change, and no data-member layout change to `ECS`/`ComponentStore`/`ComponentArray`
(`CopyFrom` adds no fields; the pool is internal to `ecs.cpp`). Build preset
`msvc-win64-vs2026-community`; `test_ecs` at
`out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (expect `All ECS tests passed.`).

## Testing (`test_ecs`)

- **`CopyFrom` correctness (highest-risk; pure, no threads):**
  - Multi-page src: add entities whose ids span ≥2 sparse pages (e.g. 1, 5, 1025, 2049) to a
    `ComponentArray<TransformComponent>`. `CopyFrom` into a destination array pre-populated
    with a *different* layout — more pages and stale entries (e.g. previously held ids 1..3,
    3000). After `CopyFrom`, assert: `Size()` equals src; `Has`/`Get` true with correct
    values for every src entity; `Has` false for ids that were only in the old dest; the
    dest's page count matches src's (no surplus pages leaking stale dense indices).
  - Empty src: `CopyFrom` from an empty array into a populated dest → dest becomes empty
    (`Size()==0`, `Has` false for prior entities).
  - Dense capacity retained: after `CopyFrom` of a smaller src into a dest with larger
    capacity, `GetComponents().capacity()` is unchanged (no reallocation/shrink).
- **Pool reuse:** `Acquire()` → `Recycle()` → `Acquire()` returns the same pointer;
  `GetComponentArrayPoolStats().Reuses` increments and `Created` does not on the second
  acquire. (Assert relative deltas — counters are process-global.)
- **Snapshot isolation regression:** take a snapshot, then mutate the same type (forcing a
  pooled clone), and assert the snapshot still observes the pre-mutation values; the master
  observes the new values. The existing COW tests must remain green.
- **Buffer-reuse end-to-end:** clone (via `MutateArray`) → drop the displacing snapshot so the
  old array recycles → mutate again → assert the second clone reused a pooled array
  (`Reuses` delta) and capacity is retained.
- **Aggregate stats deltas:** `InUse` +1 while a clone is held, `Free` +1 after it recycles,
  `Reuses` + on reacquire, `Created` + on a cold acquire.

## Risks

- **Snapshot corruption via a wrong `CopyFrom`** — the dominant risk. Mitigated by: (a) the
  refcount-0 pool gate (an Acquired array is never live), and (b) exhaustive `CopyFrom` unit
  tests (layout mismatch, stale dest pages, empty src) that run with no threading.
- **Cross-thread `Recycle`** — the recycling deleter fires on whichever thread drops the last
  ref (RenderThread when a snapshot recycles, GameThread otherwise). Mitigated by the
  per-pool mutex + atomic counters; `Recycle` is `noexcept` (deleter context). Direct
  precedent: the snapshot pool's recycling deleter.
- **Static-destruction ordering** — the Meyers pools must outlive the master ECS and all
  snapshots. They do: threads are joined and `LatestWorldSnapshot` reset before the editor
  tears down `GameState`/ECS, which is itself before static destruction. Same argument the
  snapshot pool relies on; no `InUse==0` assert is added (pending model loads / shutdown
  ordering make a hard assert inappropriate, matching the staging pool's stance).
- **Page-reuse `resize`-down frees pages on shrink** — acceptable: page count tracks max live
  entity id (stable in steady state), so shrink is rare; correctness is preserved (stale
  pages must not survive), and the common steady-state path reuses every page.
