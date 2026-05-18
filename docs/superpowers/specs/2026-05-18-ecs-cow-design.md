# ECS Copy-On-Write Design

**Date:** 2026-05-18
**Status:** Draft — pending user review
**Scope:** `src/common/include/ECS.h`, `src/common/include/ApplicationContext.h`, all editor and game callers of the ECS API, new `tests/` target.

## Motivation

`ECS::CreateSnapshot()` does not produce an isolated snapshot. The current implementation:

```cpp
std::shared_ptr<const ECS> CreateSnapshot() const {
    auto snapshot = std::make_shared<ECS>();
    snapshot->m_EntityStore = m_EntityStore;
    snapshot->m_ComponentStore = m_ComponentStore;   // shallow copy
    return snapshot;
}
```

`ComponentStore::m_ComponentArrays` is `unordered_map<type_index, shared_ptr<IComponentArray>>`. Its default copy-assign duplicates the map, but the entries are `shared_ptr` copies — refcount increments only. The underlying `ComponentArray<T>` (and its `vector<T>`, `unordered_map<EntityId, size_t>`, `vector<EntityId>`) is **aliased**, not copied.

Consequences:

- RenderThread (and ImGuiRenderer running on RenderThread) reads components through the "snapshot" while GameThread mutates the **same** component arrays. This is a data race per the C++ memory model.
- `push_back` reallocations on the master's vector can invalidate the addresses RenderThread is iterating.
- `unordered_map` rehashes on the master's `m_EntityToIndex` can cause RenderThread's `find(e)` to traverse freed bucket nodes.

The bug rarely manifests as a visible crash because:
- Editor mutation rates are low (sparse ImGui edits).
- Most mutations modify existing slots in place (no realloc / no rehash).
- RenderThread's loop iter typically completes between two GameThread mutations.

It is nonetheless undefined behavior. This design fixes it while keeping snapshot publication cheap.

## Goal

Make `CreateSnapshot()` produce a genuinely isolated, immutable view of the world such that:

1. RenderThread / ImGuiRenderer reads through a snapshot never race with GameThread writes.
2. Common cases (few component types mutated per tick) cost approximately one shared_ptr refcount bump per type.
3. Snapshots cost only what was changed since the last snapshot (copy-on-write granularity at the per-component-array level).
4. Single-writer / snapshot-reader threading model is preserved unchanged.
5. New write API makes accidental write-through-snapshot a compile error rather than a silent race.

## Architecture

No changes to the thread model. PlatformThread / GameThread / RenderThread keep their existing responsibilities. All SPSC rings, `Seqlock<SimulationSnapshot>`, and `atomic<shared_ptr<const ECS>>` channels in `ApplicationContext` remain.

Changes are confined to:

| Layer | Change |
|---|---|
| `IComponentArray` | Adds `Clone()` virtual. No state added. |
| `ComponentArray<T>` | Implements `Clone()`. Storage unchanged. |
| `ComponentStore` | Adds master-only `unordered_set<type_index> m_DirtyThisTick`. New `MutateArray<T>()`, `GetArray<T>() const`, `CopyArraysFrom()`, `ClearDirty()`. |
| `ECS` | Adds `Modify<T,F>(e, fn)`, `MutateArray<T>()`, `GetArray<T>() const`. `CreateSnapshot()` becomes non-const. Removes non-const `GetComponent<T>(e)`, `GetComponentArray<T>()`, `GetComponents<T...>(e)`. |
| `Renderer::Render` / `IRenderPass::Execute` | Gains `const ECS* world` parameter. |
| `SimulationSnapshot::WorldSnapshotPtr` | Removed. Replaced by explicit `world` parameter threaded through render calls. |
| Callers | Game.cpp + GameThread.cpp port write-through patterns to `Modify<T>`. Render passes use the new `world` parameter. |

ImGuiRenderer requires no API changes — it already reads via `shared_ptr<const ECS>` (const path) and writes via `ECSCommandRing` (indirect through GameThread).

## Component data model

```cpp
class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void Remove(EntityId e) = 0;
    [[nodiscard]] virtual bool Has(EntityId e) const = 0;
    [[nodiscard]] virtual size_t Size() const = 0;

    /**
     * @brief Deep-copy this array. Used by RemoveAllComponents which iterates
     *        the type-erased map and cannot call MutateArray<T>().
     * @return A shared_ptr owning a fresh copy of the array contents.
     */
    [[nodiscard]] virtual std::shared_ptr<IComponentArray> Clone() const = 0;
};

template<typename T>
class ComponentArray final : public IComponentArray {
public:
    void Add(EntityId e, T component);
    void Remove(EntityId e) override;
    bool Has(EntityId e) const override;
    size_t Size() const override;
    std::shared_ptr<IComponentArray> Clone() const override {
        return std::make_shared<ComponentArray<T>>(*this);
    }
    T* Get(EntityId e);
    const T* Get(EntityId e) const;
    EntityId GetEntity(size_t index) const;
    std::vector<T>& GetComponents();
    const std::vector<T>& GetComponents() const;
private:
    std::vector<T> m_Components;
    std::unordered_map<EntityId, size_t> m_EntityToIndex;
    std::vector<EntityId> m_IndexToEntity;
};
```

No COW state is held on `IComponentArray` / `ComponentArray<T>`. The arrays are pure data containers. This keeps snapshot-aliased arrays free of any byte written by the master after snapshot — eliminates false-sharing and data-race concerns.

## Copy-on-write gate

COW state lives on the master's `ComponentStore` and is never copied into snapshots:

```cpp
class ComponentStore {
public:
    template<typename T>
    ComponentArray<T>& MutateArray() {
        AssertOwnerThread();
        auto t = std::type_index(typeid(T));
        auto& slot = m_Arrays[t];
        if (!slot) slot = std::make_shared<ComponentArray<T>>();
        auto [it, inserted] = m_DirtyThisTick.insert(t);
        if (inserted) {
            // First write to this array since last snapshot — clone.
            slot = std::make_shared<ComponentArray<T>>(
                       static_cast<const ComponentArray<T>&>(*slot));
        }
        return static_cast<ComponentArray<T>&>(*slot);
    }

    template<typename T>
    const ComponentArray<T>* GetArray() const {
        auto it = m_Arrays.find(std::type_index(typeid(T)));
        if (it == m_Arrays.end()) return nullptr;
        return static_cast<const ComponentArray<T>*>(it->second.get());
    }

    void CopyArraysFrom(const ComponentStore& other) { m_Arrays = other.m_Arrays; }
    void ClearDirty()                                { m_DirtyThisTick.clear(); }

    template<typename T> void AddComponent(EntityId e, T c)  { MutateArray<T>().Add(e, c); }
    template<typename T> void RemoveComponent(EntityId e)    { MutateArray<T>().Remove(e); }
    void RemoveAllComponents(EntityId e);

private:
#ifndef NDEBUG
    std::thread::id m_OwnerThread = std::this_thread::get_id();
    void AssertOwnerThread() const {
        SM_ASSERT(std::this_thread::get_id() == m_OwnerThread,
                  "ECS mutated from non-owner thread");
    }
#else
    void AssertOwnerThread() const {}
#endif

    std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_Arrays;
    std::unordered_set<std::type_index> m_DirtyThisTick;
};

inline void ComponentStore::RemoveAllComponents(EntityId e) {
    AssertOwnerThread();
    for (auto& [type, slot] : m_Arrays) {
        if (!slot->Has(e)) continue;
        if (m_DirtyThisTick.insert(type).second) {
            slot = slot->Clone();
        }
        slot->Remove(e);
    }
}
```

`MutateArray<T>()` is the single COW gate. All mutation paths funnel through it. `RemoveAllComponents` is the only non-templated mutation path; it uses virtual `Clone()` since it does not know `T` at compile time.

`unordered_set::insert` returns `{iterator, bool inserted}`. The single hashmap op gates both the dirty check and the dirty-set update — no double-hash.

## Write API on `ECS`

```cpp
class ECS {
public:
    EntityId CreateEntity();
    void DestroyEntity(EntityId e);

    template<typename T>
    void AddComponent(EntityId e, T component) { m_ComponentStore.AddComponent<T>(e, component); }

    template<typename T>
    void RemoveComponent(EntityId e) { m_ComponentStore.RemoveComponent<T>(e); }

    /**
     * @brief Single-entity in-place edit. Lambda receives a reference into the
     *        cloned-for-this-tick array.
     * @tparam T Component type.
     * @tparam F Callable taking T& and returning anything (return value discarded).
     * @param e Entity that should hold T. No-op if entity invalid or lacks T.
     * @threading GameThread only.
     * @cow Probes Has(e) first; clones only if the entity holds the component.
     */
    template<typename T, typename F>
    void Modify(EntityId e, F&& fn) {
        const auto* readArr = m_ComponentStore.GetArray<T>();
        if (!readArr || !readArr->Has(e)) return;
        auto& writeArr = m_ComponentStore.MutateArray<T>();
        if (T* c = writeArr.Get(e)) std::forward<F>(fn)(*c);
    }

    /**
     * @brief Bulk-write access to the array for type T. Clones once per tick.
     * @threading GameThread only.
     * @cow Triggers a clone on first call per tick.
     */
    template<typename T>
    ComponentArray<T>& MutateArray() { return m_ComponentStore.MutateArray<T>(); }
};
```

The non-const `GetComponent<T>(e)`, `GetComponentArray<T>()`, and `GetComponents<T...>(e)` overloads are **removed**. Any caller writing through their returned pointer becomes a compile error.

## Read API on `ECS`

```cpp
class ECS {
public:
    // Entity queries
    bool IsValidEntity(EntityId e) const;
    size_t GetEntityCount() const;
    const std::vector<EntityId>& GetActiveEntities() const;

    // Single-component reads
    template<typename T> const T* GetComponent(EntityId e) const;
    template<typename T> bool HasComponent(EntityId e) const;
    template<typename... C> bool HasComponents(EntityId e) const;
    template<typename... C> std::tuple<const C*...> GetComponents(EntityId e) const;

    /**
     * @brief Bulk-read access. Use for systems iterating one component type densely.
     * @return Pointer to const array, or nullptr if type unregistered.
     * @snapshot Safe to call through a snapshot reference; the returned array
     *           is immutable for the snapshot's lifetime.
     */
    template<typename T>
    const ComponentArray<T>* GetArray() const { return m_ComponentStore.GetArray<T>(); }

    template<typename... C> std::vector<EntityId> View() const;
};
```

`SimulationSnapshot::WorldSnapshotPtr` is removed. Render passes receive a `const ECS*` parameter whose lifetime is bounded by the RenderThread loop iter holding the `shared_ptr<const ECS>`:

```cpp
// Renderer.h
class Renderer {
    float Render(double dt, float r, float g, float b,
                 const SimulationSnapshot& snap,
                 const ECS* world);
};

// IRenderPass.h
class IRenderPass {
    virtual void Execute(/* existing args */,
                         const ECS* world) = 0;
};

// RenderThread.cpp
auto worldSnapshot = std::atomic_load_explicit(
                        &m_AppContext->LatestWorldSnapshot,
                        std::memory_order_acquire);
auto nextSnap     = m_AppContext->LatestSnapshot.load();
m_Renderer->Render(renderDelta, r, g, b, nextSnap, worldSnapshot.get());
```

`worldSnapshot.get()` is valid for the duration of the `Render(...)` call because `worldSnapshot` (the shared_ptr) is in scope.

## Snapshot lifecycle

```cpp
/**
 * @brief Publishes an immutable snapshot of the world.
 *
 * Shallow-copies the component-array map (refcount bumps), deep-copies
 * EntityStore, resets the master's per-tick dirty set. After return, the next
 * mutation on any component type clones-on-write to keep the returned
 * snapshot isolated.
 *
 * @threading GameThread only (single-writer rule).
 * @snapshot The returned ECS exposes only const accessors.
 */
std::shared_ptr<const ECS> ECS::CreateSnapshot() {
    auto snap = std::make_shared<ECS>();
    snap->m_EntityStore = m_EntityStore;                    // vector copy
    snap->m_ComponentStore.CopyArraysFrom(m_ComponentStore); // shared_ptr copies
    m_ComponentStore.ClearDirty();
    SM_ASSERT(snap->m_ComponentStore.m_DirtyThisTick.empty(), "snapshot must start clean");
    return snap;
}
```

`CreateSnapshot` becomes non-const because it resets master state.

GameThread's existing `PublishSnapshot` flow is unchanged in structure: store the new ECS into `LatestWorldSnapshot` via atomic store on shared_ptr, then store `SimulationSnapshot` via seqlock. Order is unchanged.

RenderThread's loop unchanged: load `worldSnapshot` first (acquires the refcount), then `nextSnap`. Pass `worldSnapshot.get()` into `Renderer::Render`.

At any moment, at most two ECS instances coexist (the latest published + whatever RenderThread captured in a prior iter). Cloned arrays referenced only by an outgoing snapshot are released when RenderThread drops its `shared_ptr`.

## DestroyEntity and clone amplification

`ComponentStore::RemoveAllComponents(e)` (called from `ECS::DestroyEntity`) iterates the type-erased map. Without a gate it would clone every component array on a single destroy. The implementation above gates with `slot->Has(e)` — only arrays that actually hold the entity are cloned. Worst case for a destroy is `K` clones where `K` is the number of component types the entity carries.

The dirty set is keyed on `std::type_index` and is shared between `MutateArray<T>` (templated path) and `RemoveAllComponents` (virtual-Clone path). A type cloned by one path is not re-cloned by the other in the same tick.

## EntityStore semantics

`EntityStore` (vectors `m_ActiveEntities`, `m_FreeEntities`, plus `m_NextEntityId`) is value-copied in `CreateSnapshot`. At 10k entities the memcpy is ~80 KB ≈ 10 μs. Acceptable at the editor's current scale.

If profiling later shows EntityStore copy is significant, the same COW pattern (shared_ptr to immutable vectors) can be applied. Out of scope for this design.

## Error handling and invariants

| Source | Handling |
|---|---|
| `ECSCommandRing.Push` returns false (ring full) | `SM_WARN` + drop. Caller (ImGuiRenderer) should coalesce edits via `ImGui::IsItemDeactivatedAfterEdit` for drag operations. |
| `ECSCommandProcessor` unregistered component type | Same footgun as today — register the type in both `ApplyComponentCommand` and `RemoveComponentByType` branches in `ApplicationContext.h`. |
| `Modify<T>(e, fn)` invalid entity or missing component | No-op. No clone. |
| `MutateArray<T>` unregistered type | Auto-registers an empty array. No clone (nothing to clone). |
| Allocation failure in clone / make_shared | `std::bad_alloc` propagates. Editor cannot recover; acceptable. |
| RenderThread / worker / plugin thread writes via mutable API | Compile error (non-const overloads removed). |
| Cross-thread mutation (e.g., plugin running in unexpected thread) | Debug-only `AssertOwnerThread()` fires inside `MutateArray<T>` / `RemoveAllComponents`. |

`CreateSnapshot` asserts `m_DirtyThisTick.empty()` on the freshly-constructed snapshot to defend against future refactors that accidentally propagate dirty state.

## Doc requirements

Every new public method on `ECS`, `ComponentStore`, `IComponentArray`, `ComponentArray<T>` carries a Doxygen `/** */` block with:

- `@brief` (one-line summary).
- Detail paragraph when non-obvious.
- `@tparam` / `@param` / `@return` as appropriate.
- `@threading` — caller thread, write-vs-read class.
- `@cow` — does it trigger a clone and when.
- `@snapshot` — invariants when reached via a snapshot reference.
- `@pre` — preconditions (entity validity, etc.).

Style: imperative voice, wrap at ~100 cols, no filler. Backticks for inline code references, `@ref` for symbol references. Header-only docs (most APIs are templates). Implementation comments only at gotcha sites (e.g., the `insert().second` clone-gate pattern).

No Doxygen build target is added in this work. Future ticket can wire one up.

## Migration plan

One PR, multiple compile-clean commits.

### Commit 1 — `ECS.h` additions (additive only)

- Add `IComponentArray::Clone()` pure virtual + `ComponentArray<T>::Clone()`.
- Add `ComponentStore::MutateArray<T>`, `GetArray<T>`, `CopyArraysFrom`, `ClearDirty`, `m_DirtyThisTick`, `AssertOwnerThread`.
- Add `ECS::Modify<T,F>`, `ECS::MutateArray<T>`, `ECS::GetArray<T>`.
- Rewrite `ComponentStore::RemoveAllComponents` with `Has` gate + virtual `Clone`.
- Make `ECS::CreateSnapshot` non-const, implement per §5.

Callers still compile via existing non-const `GetComponent`/`GetComponentArray` overloads.

### Commit 2 — Route AddComponent/RemoveComponent through `MutateArray<T>`

`ComponentStore::AddComponent` / `RemoveComponent` now use `MutateArray<T>()` internally. Existing ECSCommandProcessor-driven mutations become COW-safe immediately. Direct-write-through-`Get<T>` mutations (in game.cpp / GameThread.cpp) still race until commit 3.

### Commit 3 — Port game.cpp and GameThread.cpp

Rewrite the four known write-through sites (game.cpp:121–131, game.cpp:135, GameThread.cpp:231, GameThread.cpp:250) to use `Modify<T>`. Mechanical translations.

### Commit 4 — Remove `WorldSnapshotPtr`; thread `world` through render passes

- Remove `WorldSnapshotPtr` from `SimulationSnapshot` in `ApplicationContext.h`.
- `Renderer::Render` gains `const ECS* world`.
- `IRenderPass::Execute` gains `const ECS* world`.
- `MeshRenderPass.cpp`, `UiRenderPass.cpp` switch from `snapshot.WorldSnapshotPtr->...` to `world->...`.
- `RenderThread.cpp` passes `worldSnapshot.get()` into `Renderer::Render`.
- `GameThread::PublishSnapshot` stops filling `WorldSnapshotPtr`.

### Commit 5 — Remove non-const overloads (compile-error trip-wire)

Remove:
- `template<typename T> T* GetComponent(EntityId)` non-const on `ECS` and `ComponentStore`.
- `template<typename T> ComponentArray<T>* GetComponentArray()` non-const on `ECS` and `ComponentStore`.
- `template<typename... C> std::tuple<C*...> GetComponents(EntityId)` non-const on `ECS`.

Any remaining write-through site lights up the build. Fix in place via `Modify` / `MutateArray` / `AddComponent` / `RemoveComponent`.

After this commit, no write path bypasses the COW gate.

### Commit 6 — Test exe

Add `tests/test_ecs.cpp` + `tests/CMakeLists.txt`. Root `CMakeLists.txt` gets `add_subdirectory(tests)`.

### Files touched

| File | Change |
|---|---|
| `src/common/include/ECS.h` | Core API, COW gate, dirty set, Clone, RemoveAllComponents fix, doc blocks. |
| `src/common/include/ApplicationContext.h` | Remove `WorldSnapshotPtr` from `SimulationSnapshot`. |
| `src/game/src/Game.cpp` | Port write-through sites to `Modify<T>`. |
| `src/editor/src/threading/GameThread.cpp` | Port MeshUpload/MaterialUpload handlers; remove `snap.WorldSnapshotPtr` assignment. |
| `src/editor/src/threading/RenderThread.cpp` | Pass `worldSnapshot.get()` to `Renderer::Render`. |
| `src/editor/src/rendering/Renderer.{h,cpp}` | `Render` gains `const ECS*`; threads through passes. |
| `src/editor/src/rendering/IRenderPass.h` | `Execute` gains `const ECS*`. |
| `src/editor/src/rendering/passes/MeshRenderPass.cpp` | Use new `world` arg. |
| `src/editor/src/rendering/passes/UiRenderPass.cpp` | Use new `world` arg. |
| `src/editor/src/utilities/WorldManager.cpp` | Verify only `AddComponent` usage; no write-through `Get`. |
| `tests/CMakeLists.txt` + `tests/test_ecs.cpp` | New test exe. |
| Root `CMakeLists.txt` | `add_subdirectory(tests)`. |

`ImGuiRenderer.cpp` requires no changes — already const-path reads + ECSCommand writes.

## Testing

A standalone `test_ecs.exe` target is added. No external test framework. Assertions via `SM_ASSERT`. Exits 0 on success, 1 on failure.

### Cases

| Test | Asserts |
|---|---|
| `T01_add_get_basic` | `AddComponent` then `GetComponent` returns the value. |
| `T02_remove_component` | After `RemoveComponent`, `HasComponent` false; siblings intact. |
| `T03_destroy_entity_clears_all_components` | After `DestroyEntity`, no array reports `Has(e)`. |
| `T04_destroy_entity_only_clones_owning_arrays` | Entity holds 2 of 5 component types. After destroy, only those 2 array shared_ptrs differ pre/post; the other 3 keep identity. |
| `T05_snapshot_isolates_reads_from_writes` | Snapshot taken. Master mutates Transform. Snapshot's Transform unchanged. |
| `T06_snapshot_unchanged_arrays_share_storage` | Snapshot taken, master touches Transform only. Snapshot's MeshArray pointer == master's MeshArray pointer. |
| `T07_modify_no_clone_on_invalid_entity` | `Modify<T>` with entity that doesn't hold T keeps the same array shared_ptr. |
| `T08_mutate_array_clones_once_per_tick` | Two `MutateArray<T>` calls return same address. Snapshot in between → next call returns a different address. |
| `T09_create_snapshot_clears_dirty` | After `CreateSnapshot`, master's dirty set empty. Next mutation triggers fresh clone. |
| `T10_multi_snapshot_lifetime` | Two snapshots in flight. Mutate master between. Drop S1. S2 still readable, content unchanged. |
| `T11_destroyed_entity_id_reuse` | Destroy E, create new entity gets recycled id. New entity has no leftover components in snapshots taken after recycle. |
| `T12_concurrent_smoke` | Reader thread does 10k `GetComponent` calls on a snapshot while main thread does 10k `Modify` on master. Reader's snapshot data unchanged at end. Light stress only. |

If the alias bug regressed, `T05` / `T06` / `T12` would fail.

### Build integration

```cmake
# tests/CMakeLists.txt
add_executable(test_ecs test_ecs.cpp)
target_link_libraries(test_ecs PRIVATE CommonHeaders glm::glm)
set_target_properties(test_ecs PROPERTIES
    OUTPUT_NAME test_ecs
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

Run: `out/build/<preset>/bin/Debug/test_ecs.exe`. Suitable for a future `add_test` / CTest hook.

## Out of scope

- `View<...>()` allocation cost (each call allocates a `vector<EntityId>`). Separate ticket.
- EntityStore COW. Separate ticket if profiling demands it.
- Triple-buffered arrays / zero-allocation snapshot publishing. Future profile-driven optimization.
- `Doxyfile` and HTML doc generation. Future ticket.
- Performance benchmarks. Separate concern from correctness tests.
- Recovery from corrupted save files in WorldManager. Orthogonal.

## Rollback

Commits 1–4 are additive or internal-only. Commit 5 (API removal) is the only behavior-breaking change. If a regression is found post-merge, commit 5 can be reverted independently while leaving the COW gate in place.
