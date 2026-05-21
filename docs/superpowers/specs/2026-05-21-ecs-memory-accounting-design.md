# ECS Memory Accounting + Panel Section — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** `allocator-toolkit` (stacking).

## Goal

Give the editor Memory panel FrameAllocator-style visibility into ECS storage: a
read-only "ECS Memory" section reporting the live (Used) and reserved (footprint)
bytes held by the component arrays + entity store. Pure **measurement** — it does
**not** route ECS allocations through the toolkit allocators, and is unrelated to the
deferred COW Part 2 (ComponentArray buffer recycle).

## Scope

**In scope:**
- `ArrayMemory { size_t Used; size_t Reserved; }` + a virtual `IComponentArray::MemoryBytes() const`.
- `ComponentArray<T>::MemoryBytes()` summing its sparse-set buffers.
- `EntityStore::MemoryBytes()`.
- `EcsMemoryStats` + `ECS::MemoryStats() const` aggregating component arrays + entity store.
- A `ComponentStore` helper to sum over its (private) array map.
- An "ECS Memory" `CollapsingHeader` in the editor `MemoryPanel`, fed the panel's
  existing world-snapshot `const ECS*`.
- `test_ecs` coverage of the byte math.

**Out of scope:** routing ECS storage through tracked allocators (the heavyweight
"track + reduce ECS fragmentation via the toolkit" option); the ECS COW Part 2
(ComponentArray clone-buffer recycle); a "peak" high-water metric (needs continuous
tracking, not a point-in-time walk).

## Background

`ComponentArray<T>` (post sparse-set refactor) holds: `std::vector<T> m_Components`,
`std::vector<EntityId> m_IndexToEntity`, and
`std::vector<std::unique_ptr<SparsePage>> m_SparsePages` (`SparsePage =
std::array<uint32_t,1024>` = 4 KB/page; absent pages are nullptr). `EntityStore` holds
`std::vector<EntityId> m_ActiveEntities` + `m_FreeEntities`. None of this is routed
through a tracked allocator, so the Engine registry can't see it — hence the explicit
accounting. The editor `MemoryPanel` runs on the RenderThread inside
`ImGuiRenderer::Render`, which already has `const ECS* world` (the world snapshot,
reflecting current data) — so the panel measures that snapshot (no need to expose the
master).

## Design

### Per-array byte sum (type-erased virtual)

```cpp
struct ArrayMemory { size_t Used; size_t Reserved; };

// IComponentArray:
[[nodiscard]] virtual ArrayMemory MemoryBytes() const = 0;

// ComponentArray<T>:
[[nodiscard]] ArrayMemory MemoryBytes() const override {
    ArrayMemory m{};
    m.Used     = m_Components.size()   * sizeof(T)
               + m_IndexToEntity.size() * sizeof(EntityId);
    m.Reserved = m_Components.capacity()    * sizeof(T)
               + m_IndexToEntity.capacity() * sizeof(EntityId)
               + m_SparsePages.capacity()   * sizeof(std::unique_ptr<SparsePage>);
    for (const auto& page : m_SparsePages)
        if (page) m.Reserved += sizeof(SparsePage);   // 4 KB each, pure overhead
    return m;
}
```
- **Used** = live component payload + live index entries (`size()`-based; deterministic).
- **Reserved** = capacity bytes of the two dense vectors + the outer page-pointer
  vector's capacity + every allocated 4 KB page. Sparse pages are overhead, so they
  count toward Reserved only (not Used).

### EntityStore

```cpp
// EntityStore:
[[nodiscard]] ArrayMemory MemoryBytes() const {
    ArrayMemory m{};
    m.Used     = (m_ActiveEntities.size()     + m_FreeEntities.size())     * sizeof(EntityId);
    m.Reserved = (m_ActiveEntities.capacity() + m_FreeEntities.capacity()) * sizeof(EntityId);
    return m;
}
```

### Aggregate

```cpp
struct EcsMemoryStats {
    size_t ComponentUsed;
    size_t ComponentReserved;
    size_t EntityUsed;
    size_t EntityReserved;
    size_t ArrayCount;     // # registered component arrays
    size_t EntityCount;    // active entities
};

// ECS:
[[nodiscard]] EcsMemoryStats MemoryStats() const;   // walks ComponentStore + EntityStore
```
`ComponentStore` gets a helper to sum over its private `m_ComponentArrays`
(iterates calling `IComponentArray::MemoryBytes()`, also yields the array count). `ECS::MemoryStats`
combines it with `m_EntityStore.MemoryBytes()` and `m_EntityStore.GetEntityCount()`.

**Excluded:** the `unordered_map` bucket/node overhead and the per-`shared_ptr`/control-block
bytes — small and awkward to measure precisely. The panel labels the section as an
estimate of the component/entity *buffers*.

### Panel

`DrawMemoryPanel(bool* open, const ECS* world)` (signature gains `world`;
`ImGuiRenderer::Render` passes its existing `world`). An "ECS Memory" `CollapsingHeader`
(null-guarded on `world`) renders:
- Component: Used / Reserved
- Entity: Used / Reserved
- Total reserved
- Arrays: N · Entities: M

Sits alongside the existing "By Category" / "Allocators" / "Snapshot Pool" sections.
`%zu` for the sizes (mirroring the panel's style).

## Build / ABI

Adding a pure virtual to `IComponentArray` changes the `ComponentArray<T>` vtable layout,
so **rebuild `ecs.dll` + `editor` + `game` together**; restart the editor for the smoke
test. **No `GAME_API_VERSION` bump** (no `GameState`/game-export/component-type change).
`ECS::MemoryStats`/`EcsMemoryStats` are reached through the `ECS_API`-exported `ECS`
class. `MemoryBytes()` impls and `EcsMemoryStats`/`ArrayMemory` structs live in `ECS.h`
(header). The panel + `ImGuiRenderer` signature change are editor-only.

## Testing (`test_ecs`)

- **ComponentArray Used is exact:** add N `TransformComponent`s to a `ComponentArray`;
  `MemoryBytes().Used == N*sizeof(TransformComponent) + N*sizeof(EntityId)`. After
  removing one (swap-and-pop), Used reflects N-1.
- **Reserved ≥ Used and includes a page:** after ≥1 add, `MemoryBytes().Reserved >=
  MemoryBytes().Used` and `Reserved >= sizeof(SparsePage)` (at least one 4 KB page
  allocated). (Capacity is implementation-defined → assert `>=`, not exact.)
- **Empty array:** `MemoryBytes().Used == 0` (no entities → no pages required; Reserved
  may be 0).
- **`ECS::MemoryStats` aggregation:** an ECS with a couple component types + entities
  reports `ArrayCount` == # registered types, `EntityCount` == active count, and
  `ComponentUsed`/`EntityUsed` equal to the hand-summed per-array/entity-store values.

## Risks

- **Approximate, not exact total:** excludes map/control-block overhead — acceptable and
  labeled; the component/entity buffers are the bulk and the part that grows with the world.
- **Measures the snapshot, not the master:** the snapshot shares/clones the master's
  arrays, so its byte figures track the master closely; for a debug overview this is the
  right, low-coupling source. (Cold, never-mutated arrays are shared and counted once in
  the snapshot — correct.)
- **vtable ABI change:** mitigated by rebuilding all consumers together (documented).
