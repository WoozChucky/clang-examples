# ECS ComponentArray Paged Sparse-Set — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** `allocator-toolkit` (stacking).

## Goal

Replace the per-array `std::unordered_map<EntityId,size_t> m_EntityToIndex` inside
`ComponentArray<T>` (`src/common/include/ECS.h`) with a **paged sparse set** (EnTT-style).
Two payoffs:

1. **Cheap, contiguous COW clones** — the per-tick `make_shared<ComponentArray<T>>(*this)`
   snapshot clone no longer re-allocates a hash node per entity; it copies flat arrays
   (memcpy) plus a handful of fixed pages. The hashmap clone churn is the dominant
   remaining per-tick allocation, so this is the headline win.
2. **O(1) array-indexed `Has`/`Get`** — no hashing, which speeds the `Each` join.

This is the **first, foundational step** of the deferred ECS COW snapshot work. The
cross-thread snapshot **pooling/reclaim** (recycling clone storage back to the
GameThread once the RenderThread drops a snapshot) is a **separate follow-up** after
this — out of scope here.

## Background

`ComponentArray<T>` today keeps three members:
- `std::vector<T> m_Components` — dense, packed component data.
- `std::vector<EntityId> m_IndexToEntity` — dense, parallel: entity owning `m_Components[i]`.
- `std::unordered_map<EntityId,size_t> m_EntityToIndex` — reverse lookup.

The two vectors are already dense. The `unordered_map` is the problem: a heap node per
entry means every clone re-allocates all nodes + rehashes, and lookups hash + chase
pointers. This is structurally already a sparse-set design with a hashmap standing in
for the sparse half — so swapping in a real (paged) sparse array is the natural,
minimal, production-aligned evolution (it's the EnTT storage model).

Entity ids: `EntityStore` hands out sequential ids from 1, recycles via a free-list,
and reserves one hidden singleton id (id 1). `INVALID_ENTITY` is 0 (never stored). Paging
removes any reliance on ids staying small/dense — only touched pages allocate.

## Design

### Members (replace the hashmap with paged sparse storage)

```cpp
private:
    static constexpr uint32_t kInvalid  = UINT32_MAX;        // "entity absent" sentinel
    static constexpr uint32_t kPageSize = 1024;              // slots per page (power of 2)
    using SparsePage = std::array<uint32_t, kPageSize>;       // dense index per slot

    std::vector<T>        m_Components;                       // dense (unchanged)
    std::vector<EntityId> m_IndexToEntity;                   // dense (unchanged)
    std::vector<std::unique_ptr<SparsePage>> m_SparsePages;  // [page] -> page or nullptr
```

- Outer vector indexed by `page = e / kPageSize`; **absent page = `nullptr` → no
  allocation** (true on-demand paging). A present page is one tight 4 KB fixed array,
  no vector header / capacity slack.
- `slot = e % kPageSize`.
- Requires `#include <array>` (and `<memory>`, already present for `shared_ptr`).

### Rule of 5 (because of the `unique_ptr` members)

`ComponentArray` owns `unique_ptr` pages, so it needs an explicit copy that deep-copies
pages; move stays trivial. `Clone()` (`make_shared<ComponentArray<T>>(*this)`) and the
COW path depend on this copy ctor producing a fully independent array.

```cpp
public:
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
            ComponentArray tmp(other);          // copy-and-move-assign members
            m_Components    = std::move(tmp.m_Components);
            m_IndexToEntity = std::move(tmp.m_IndexToEntity);
            m_SparsePages   = std::move(tmp.m_SparsePages);
        }
        return *this;
    }
```

Clone cost = #present-pages allocations + memcpy each (1-2 pages for typical worlds),
versus the old hashmap's node-per-entity churn.

### Sparse helpers (private)

```cpp
    [[nodiscard]] uint32_t SparseGet(EntityId e) const {
        const size_t p = static_cast<size_t>(e / kPageSize);
        if (p >= m_SparsePages.size() || !m_SparsePages[p]) return kInvalid;
        return (*m_SparsePages[p])[e % kPageSize];
    }
    void SparseSet(EntityId e, uint32_t denseIndex) {
        const size_t p = static_cast<size_t>(e / kPageSize);
        if (p >= m_SparsePages.size()) m_SparsePages.resize(p + 1);
        if (!m_SparsePages[p]) {
            m_SparsePages[p] = std::make_unique<SparsePage>();
            m_SparsePages[p]->fill(kInvalid);
        }
        (*m_SparsePages[p])[e % kPageSize] = denseIndex;
    }
    void SparseClear(EntityId e) {
        const size_t p = static_cast<size_t>(e / kPageSize);
        if (p < m_SparsePages.size() && m_SparsePages[p])
            (*m_SparsePages[p])[e % kPageSize] = kInvalid;
    }
```

### Public methods (signatures + swap-and-pop semantics unchanged)

- `Has(e)` → `SparseGet(e) != kInvalid`.
- `Get(e)` (both overloads) → `i = SparseGet(e); return i == kInvalid ? nullptr : &m_Components[i];`.
- `Add(e, c)`:
  ```cpp
  const uint32_t existing = SparseGet(entity);
  if (existing != kInvalid) { m_Components[existing] = component; return; }
  const uint32_t newIndex = static_cast<uint32_t>(m_Components.size());
  SparseSet(entity, newIndex);
  m_Components.push_back(component);
  m_IndexToEntity.push_back(entity);
  ```
- `Remove(e)` — swap-and-pop, identical to today but via helpers:
  ```cpp
  const uint32_t removed = SparseGet(entity);
  if (removed == kInvalid) return;
  const uint32_t last = static_cast<uint32_t>(m_Components.size() - 1);
  m_Components[removed] = m_Components[last];
  const EntityId lastEntity = m_IndexToEntity[last];
  m_IndexToEntity[removed] = lastEntity;
  SparseSet(lastEntity, removed);   // redirect the moved entity
  SparseClear(entity);
  m_IndexToEntity.pop_back();
  m_Components.pop_back();
  ```
- `Size`, `GetComponents` (both), `GetEntity`, `Clone` — unchanged.

`m_EntityToIndex` is private and used only inside `ComponentArray`, so nothing outside
(world save/load, ECSCommandProcessor, etc. all go through the public API) is affected.

## Scope & build

- **Only** `src/common/include/ECS.h` (ComponentArray internals + `<array>` include) and
  `tests/test_ecs.cpp` (tests) change.
- Public `ComponentArray` / `ComponentStore` / `ECS` API unchanged → **no caller,
  X-macro, or explicit-instantiation changes**.
- **No `GAME_API_VERSION` bump** (no `GameState` layout / export / component-type change).
- But `ComponentArray<T>`'s internal layout changes, and it's instantiated in `ecs.dll`
  and consumed across modules → **rebuild `ecs.dll` + `editor` + `game` together, and
  restart the editor** for the smoke test (per CLAUDE.md's ECS.h-change rule).

## Testing

`tests/test_ecs.cpp` (the ECS harness, which already uses `ComponentArray<T>` directly):

- **Regression:** the existing clone/add/remove tests (e.g. T01 clone-independence) must
  still pass unchanged.
- **New:**
  - Cross-page ids: add id 5 and id 5000 (`5000/1024 == 4` → a high page); both `Has`/`Get`
    return correctly; an absent low id (e.g. 5000 present, 5 absent) reports `Has==false`.
  - Paging correctness: with only a high id present (e.g. 5000), `Has(0)`/`Has(5)` are
    false (low pages untouched) and `Has(5000)` true.
  - Swap-and-pop preserves others: add a,b,c; remove b; a and c still `Has`/`Get` correct
    with right values; `Size()==2`.
  - Remove-then-readd same id: add e, remove e (`Has(e)==false`), add e again → `Has`/`Get`
    correct (sparse slot reset + reused).
  - Update existing: add e with v1 then v2 → `Get(e)` is v2, `Size()==1`.
  - Clone independence after swap-and-pop: build an array, remove one element (exercising
    the swap-and-pop sparse redirect), `Clone()`, mutate the original, confirm the clone
    is unchanged (covers the custom copy ctor over a non-trivial sparse layout).

## Risks

- **Custom copy ctor is the correctness lynchpin** — a shallow/incorrect copy would
  break COW snapshot isolation (cross-thread reads of stale/shared data). Mitigated by
  the clone-independence tests (existing T01 + the new swap-and-pop clone test).
- **Memory:** ~4 KB per component type per *touched* page. Dense small ids → ~1 page/type
  (≈60 KB across the ~15 types) — comparable to or better than the hashmap, and bounded.
  Sparse/high ids only allocate touched pages (the paging payoff).
- **`uint32_t` dense index** caps a single component array at ~4 B entries — far beyond
  any realistic entity count.
