# ECS `Each` Iteration Primitive — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Depends on:** the allocator toolkit / `FrameAllocator` (branch `allocator-toolkit`), for the collect-site arena buffers.

## Goal

Eliminate the per-call heap allocation in ECS entity iteration. Replace
`ECS::View<Components...>()` — which allocates a fresh `std::vector<EntityId>` on
every call — with a zero-allocation callback primitive `Each<Components...>(fn)`,
and migrate all callers. `View()` is **removed** (it has no remaining callers once
the migration is done).

## Scope

**In scope:**
- Add `ECS::Each<Components...>(F&& fn) const` (header template, zero allocation,
  allocator-agnostic — ECS does **not** depend on `Engine.dll`).
- Remove `ECS::View<Components...>()`.
- Migrate all 5 call sites to `Each` (editor render passes + game.dll systems).
- Update the ECS.h example-usage comment block (which demonstrates `View`).
- Unit-test `Each` in `test_ecs`.

**Out of scope (separate later work, unchanged here):**
- The join-scan cost: `Each` still scans all active entities and probes
  `HasComponent`/`GetComponent`. A faster dense/archetype join is a later storage
  change.
- The per-array `unordered_map<EntityId,size_t>` → sparse-set change (the shared
  lever for both join speed and COW clone cost) — a separate later step.
- The ECS copy-on-write snapshot pooling (the deferred prime target). `Each`
  deliberately establishes a stable caller seam to shrink that future refactor's
  blast radius.

## Background

Current `View` (`ECS.h:510`):
```cpp
template<typename... Components>
[[nodiscard]] std::vector<EntityId> View() const {
    std::vector<EntityId> result;
    for (EntityId entity : m_EntityStore.GetActiveEntities())
        if (HasComponents<Components...>(entity))
            result.push_back(entity);
    return result;
}
```
It allocates a vector per call and is invoked on both GameThread (master ECS, per
tick) and RenderThread (snapshot ECS, per frame). Each call is single-threaded
(the returned vector is caller-local), so there is no cross-thread lifetime
concern — which is why this is a clean target independent of the COW work.

The 5 real call sites (the only other `View<` reference is inside an example
comment):
- `src/game/src/game.cpp:20` — TextRotation system (pure iterate).
- `src/game/src/game.cpp:47` — DayNight system (pure iterate).
- `src/editor/src/rendering/passes/MeshRenderPass.cpp:332` — point-light gather (pure iterate).
- `src/editor/src/rendering/passes/MeshRenderPass.cpp:379` — mesh batching (collect into the `entries` arena array).
- `src/editor/src/rendering/passes/UiRenderPass.cpp:300` — text-entity list (collect, iterated 3× per frame).

## Design

### `Each` API

In the `ECS` class (`ECS.h`), header template, `const`, allocator-agnostic:

```cpp
template<typename... Components, typename F>
void Each(F&& fn) const {
    for (EntityId e : m_EntityStore.GetActiveEntities()) {
        if constexpr (std::is_invocable_v<F, EntityId, const Components&...>) {
            // Components form: fetch each queried component once; all non-null
            // by construction when every component is present.
            const std::tuple ptrs{ GetComponent<Components>(e)... };
            const bool all = std::apply([](auto*... p){ return (p && ...); }, ptrs);
            if (all) std::apply([&](auto*... p){ fn(e, *p...); }, ptrs);
        } else {
            // Entity-only form.
            if ((HasComponent<Components>(e) && ...)) fn(e);
        }
    }
}
```

Properties:
- **Flexible callback:** if the callback accepts `(EntityId, const Components&...)`
  it receives the queried components by const reference (guaranteed non-null, since
  `Each` only fires on full matches); otherwise it is called with `(EntityId)` only.
  Per-site ergonomics: read-heavy sites take the components; collect-only sites take
  just the id. Mismatched arity is a compile error, not silent.
- **Read-only:** const refs preserve the COW model — mutation still goes through
  `Modify`/`MutateArray`. (`Each` is `const`, callable on snapshots.)
- **No allocation, no Engine dependency.** Requires adding `#include <tuple>` to
  `ECS.h` (`<type_traits>` is already included).
- **Single fetch in the components branch** (no separate `HasComponents` re-probe).

### Remove `View`

Delete the `View<Components...>()` template from `ECS.h`. `HasComponents` and the
`GetComponents` tuple accessor remain (used elsewhere / by `Each`). Update the
example-usage comment block in `ECS.h` so its `View` examples become `Each`
examples (or are removed). After migration, no `View<` reference may remain in
`src/` outside this spec/docs.

### Migration (5 sites)

- **`game.cpp:20` / `game.cpp:47`** — convert the `for (EntityId e : world.View<...>())`
  loops to `world.Each<...>(...)`. Use the components form where the body reads the
  queried components; entity-only otherwise. Systems that mutate keep using
  `Modify`/`MutateArray` inside the callback. (The implementer reads each body to
  pick the exact form.)
- **`MeshRenderPass.cpp:332` (lights)** — `Each<TransformComponent, LightningComponent>(
  [&](EntityId, const TransformComponent& t, const LightningComponent& l){ ... })`,
  dropping the two `GetComponent` calls per entity. Same directional/point logic,
  same `m_MaxPointLights` cap, same arena `pointLights` array.
- **`MeshRenderPass.cpp:379` (batching)** — size the `entries` arena array to
  `world->GetEntityCount()` (the active-entity upper bound, replacing
  `meshEnts.size()`), then fill it via
  `Each<TransformComponent, MeshComponent>([&](EntityId e, const TransformComponent&, const MeshComponent& m){ if (!m.Visible) return; ... })`.
  Material is still looked up separately (not in the query). Null-guard the
  `entries` allocation as today.
- **`UiRenderPass.cpp:300` (text list)** — replace the `View` vector with an arena
  buffer `auto* textEnts = frameAllocator->AllocateArray<EntityId>(world->GetEntityCount());`
  filled via entity-only `Each<TransformComponent, TextComponent>([&](EntityId e){ textEnts[textCount++] = e; })`,
  null-guarded; then iterate `textEnts[0..textCount)` for the existing 3 passes
  (dedup, glyph count, instance gen). Removes the last `View()` heap vector here.

Collect-site arena arrays are sized to `GetEntityCount()` (upper bound) rather than
the exact match count, since `Each` does not pre-materialize a count. This slightly
over-allocates the (transient, arena) array — acceptable; the MeshRenderPass
`entries` array is allocated once per pass and the UiRenderPass `textEnts` once per
frame.

## Testing

Unit tests in `tests/test_ecs.cpp` (the ECS harness):
- `Each` visits exactly the entities that have all queried components (and no
  others); count matches a hand-rolled expectation.
- **Components form:** the references passed are the correct component instances
  (verify a field value per visited entity).
- **Entity-only form:** a callback taking just `EntityId` compiles and is invoked
  for each match.
- **Zero matches** (no entity has the component set) → callback never called.
- **Single-component** and **multi-component** queries both work.
- Iterating does not mutate the world (entity count unchanged after `Each`).

## Files

- Modify: `src/common/include/ECS.h` — add `Each` + `#include <tuple>`; remove `View`;
  update the example comment.
- Modify: `src/game/src/game.cpp` — migrate 2 sites.
- Modify: `src/editor/src/rendering/passes/MeshRenderPass.cpp` — migrate 2 sites
  (lights + batching; `entries` sized to `GetEntityCount()`).
- Modify: `src/editor/src/rendering/passes/UiRenderPass.cpp` — migrate the text list
  to an arena buffer filled via `Each`.
- Modify: `tests/test_ecs.cpp` — `Each` unit tests.

No new files, no CMake changes. (ECS.h is a header; `ecs.dll` recompiles; editor +
game + test_ecs recompile.)

## Risks

- **`if constexpr` dispatch edge:** a generic/`auto` callback could be invocable
  both ways and would take the components branch. Not a concern for the concrete
  lambdas at the call sites; documented behavior (components form preferred when
  both match).
- **Verification of full migration:** the change is incomplete if any `View<`
  reference remains. The plan must grep `src/` for `View<` after migration and
  confirm only docs/spec references remain.
- **Collect-site sizing to `GetEntityCount()`** over-allocates the arena array when
  many entities lack the queried components. Bounded and transient; acceptable. A
  two-pass count-then-fill could size exactly but doubles the scan — not worth it.
- **Behavior parity:** render output and system behavior must be unchanged. Render
  parity is covered by the existing editor smoke test; system behavior by the
  `Each` unit tests plus the game still running.
