# Render-Pass Arena Refactor — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Depends on:** the allocator toolkit (`Engine::ArenaAllocator` / `FrameAllocator`), branch `allocator-toolkit`.

## Goal

Route the per-frame heap allocations in the editor's render passes through the
already-plumbed `FrameAllocator` (a bump arena, reset once per frame), eliminating
per-frame `std::vector`/`std::unordered_map` churn on the RenderThread. This is the
first application of the allocator toolkit to a real hot path, and the lowest-risk
one: everything here is single-threaded (RenderThread), transient-per-frame, and
bulk-reclaimed — a textbook arena fit.

## Scope

**In scope (RenderThread, per-frame):**
- `MeshRenderPass::Render` — the per-frame point-light array, the entity-batching
  `std::unordered_map`, and the per-batch instance array.
- `MeshSystem::GetMeshResources` — stop copying `std::vector<SubMesh>` by value every batch.
- `UiRenderPass::Render` — the `usedFontSizes` dedup vector (glyph instances already
  use the arena).

**Out of scope (separate later refactors):**
- The ECS copy-on-write snapshot churn (cross-thread; the deferred prime target).
- GameThread→RenderThread staging buffers (cross-thread).
- `ECS::View<...>()` returning a `std::vector` — changing it touches the `ecs.dll`
  public API and is also called game-side. The `View` vectors here are left as-is;
  this refactor only removes the allocations *downstream* of them.

## Background

`Renderer` owns one `FrameAllocator m_FrameAllocator` (default 16 MB) and passes it
to every render pass via `IRenderPass::Render(..., FrameAllocator*)`. It is
`Reset()` once per frame at `Renderer.cpp:216` (after all passes). Today only
`UiRenderPass` uses it (for glyph instances, `UiRenderPass.cpp:346`); `MeshRenderPass`
and `PrimitiveRenderPass` receive the pointer and ignore it. All allocations in this
refactor draw from that single arena and accumulate across passes within a frame,
then reset together.

## Non-goals

- No change to draw output, batching results, instancing, or visuals — this is a
  pure allocation-strategy refactor; the rendered frame must be identical.
- No new allocator types; reuse `FrameAllocator`/`ArenaAllocator::AllocateArray<T>`.
- No `std::pmr` adapter (considered and rejected for this refactor in favor of the
  flat+sort restructure, which removes the node-based container entirely).

## Design

### 1. MeshRenderPass batching: `unordered_map` → flat array + sort

The bump arena cannot host a node-based `std::unordered_map`, so the batching is
restructured to be arena-native and the map is removed entirely. A small, pure,
unit-testable helper does the grouping.

**New header `src/editor/src/rendering/passes/MeshBatching.h`** (self-contained —
includes only `<cstdint>` and `<algorithm>`, no nvrhi/ECS/editor dependencies, so it
is unit-testable in isolation):

```cpp
#pragma once
#include <cstdint>
#include <algorithm>

// One visible mesh entity tagged with its batch key. POD; arena-friendly.
struct BatchEntry {
    uint32_t meshId;
    uint32_t materialId;
    uint64_t entity;   // EntityId
};

// A contiguous run of entries sharing one (meshId, materialId) == one draw batch.
struct BatchRun {
    uint32_t begin;    // index into entries
    uint32_t count;
};

// Sorts entries[0..count) by (meshId, materialId) in place, then fills `runs`
// with the contiguous equal-key runs. Returns the number of runs (<= count).
// Caller must size `runs` to at least `count` (worst case: all-distinct keys).
inline uint32_t BuildBatchRuns(BatchEntry* entries, uint32_t count,
                               BatchRun* runs, uint32_t maxRuns) {
    if (count == 0 || !entries || !runs || maxRuns == 0) return 0;
    std::sort(entries, entries + count, [](const BatchEntry& a, const BatchEntry& b) {
        if (a.meshId != b.meshId) return a.meshId < b.meshId;
        return a.materialId < b.materialId;
    });
    uint32_t runCount = 0;
    uint32_t i = 0;
    while (i < count && runCount < maxRuns) {
        uint32_t j = i + 1;
        while (j < count &&
               entries[j].meshId == entries[i].meshId &&
               entries[j].materialId == entries[i].materialId) {
            ++j;
        }
        runs[runCount++] = BatchRun{ i, j - i };
        i = j;
    }
    return runCount;
}
```

**In `MeshRenderPass::Render`** (use the `frameAllocator` param; un-comment it):

1. `auto meshEnts = world->View<TransformComponent, MeshComponent>();` (the ECS
   `View` vector stays — out of scope).
2. `auto* entries = frameAllocator->AllocateArray<BatchEntry>(meshEnts.size());`
   If null (arena exhausted), log is already emitted by the arena; skip the mesh
   draw section gracefully. Fill `entries` with the visible entities (Visible &&
   transform present), resolving `materialId` exactly as today
   (`materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial`),
   tracking the actual `count`.
3. `auto* runs = frameAllocator->AllocateArray<BatchRun>(count);` (null-guard).
   `uint32_t runCount = BuildBatchRuns(entries, count, runs, count);`
4. For each run: `instanceCount = min(run.count, m_MaxInstances)`;
   `auto* instances = frameAllocator->AllocateArray<MeshInstanceCPU>(instanceCount);`
   (null-guard → skip run). Fill it from `entries[run.begin .. run.begin+instanceCount)`
   using the existing per-instance matrix/material logic. Track the filled
   `instanceOut` count. The draw logic (binding sets, submesh loop, `drawIndexed`)
   is unchanged, using `instances` / `instanceOut` in place of the old
   `instances.data()` / `instances.size()`, and `batchKey` replaced by the run's key
   (read from `entries[run.begin]`).

This removes the `std::unordered_map<BatchKey, std::vector<EntityId>>` and the
per-batch `std::vector<EntityId>` completely. Output is identical (same set of
batches, same instances per batch, same draws) — only the grouping mechanism and
the backing memory change.

### 2. MeshRenderPass point lights: arena array

Replace `std::vector<PointLightCPU> pointLights; pointLights.reserve(16);` with
`auto* pointLights = frameAllocator->AllocateArray<PointLightCPU>(m_MaxPointLights);`
and a `uint32_t pointLightCount = 0` (null-guard: if null, treat as zero point
lights). Push by `pointLights[pointLightCount++] = pl;` capped at `m_MaxPointLights`
(the existing cap). `writeBuffer` uses `pointLights` / `pointLightCount`.

### 3. GetMeshResources: stop the per-frame copy

In `src/editor/src/rendering/MeshSystem.h`, change `MeshResources::subMeshes` from
`std::vector<SubMesh>` to **`std::span<const SubMesh>`** (add `#include <span>`).
In `MeshSystem.cpp:192`, `resources.subMeshes = entry.subMeshes;` becomes a span over
the entry's vector (implicit `vector → span<const>` conversion). `GetMeshResources`
still returns `MeshResources` by value, but the value is now cheap (handles + counts
+ a span; no heap copy). Caller usage in `MeshRenderPass` (`.size()`, range-`for`
over `subMeshes`) is unchanged.

**Lifetime contract (documented in a comment):** the returned span points into
`MeshSystem::m_Meshes[id].subMeshes`, valid only while `m_Meshes` is not mutated.
Mesh additions are drained from the renderer command ring *before* render passes run,
not mid-pass, so the span is valid for the duration of a `Render` call. The
implementation must verify no other consumer of `GetMeshResources`/`MeshResources`
stores `subMeshes` beyond the current frame (audit found only `MeshRenderPass`).

### 4. UiRenderPass: usedFontSizes arena array

Replace `std::vector<size_t> usedFontSizes;` with:
`auto textEnts = world->View<TransformComponent, TextComponent>();`
`auto* usedFontSizes = frameAllocator->AllocateArray<size_t>(textEnts.size());`
(null-guard: if null/empty, `fontSizeCount = 0`). Dedup distinct font sizes into it
with a running `uint32_t fontSizeCount`, then iterate `usedFontSizes[0..fontSizeCount)`.
The subsequent per-font glyph counting and the existing glyph-instance arena
allocation are unchanged. (The repeated `View<...>()` calls inside the font loop are
ECS-side and out of scope.)

### 5. Arena capacity & overflow

All sites draw from the existing 16 MB frame arena (accumulating across MeshRenderPass
+ UiRenderPass within a frame, reset after). For typical scenes this is far under
budget. On overflow, `AllocateArray<T>` returns `nullptr` and the arena logs an
`SM_ERROR` (the deliberate guardrail). Every new arena call site is null-guarded so
an exhausted arena degrades loudly (skips that batch/draw) rather than dereferencing
null. If overflow is ever observed in practice, raise the `FrameAllocator` capacity
in `Renderer` — out of scope to change here.

## Testing

- **Unit tests (`tests/test_alloc.cpp`, the existing harness):** `BuildBatchRuns`
  is pure and POD-only, so it is tested directly:
  - empty input (`count == 0`) → 0 runs;
  - single entry → 1 run of count 1;
  - all-same-key → 1 run of count N;
  - all-distinct keys → N runs of count 1 each;
  - mixed/unsorted input → entries end up sorted, runs cover all entries with no
    gaps/overlaps, and every run has a uniform key;
  - `maxRuns` smaller than the natural run count → stops at `maxRuns`.
  `test_alloc` gains the include dir `src/editor/src/rendering/passes` so it can
  include the self-contained `MeshBatching.h` (no editor/nvrhi deps pulled in).
- **Editor build:** `cmake --build --preset msvc-win64-vs2026-community --target editor`
  must compile and link.
- **Smoke test (user):** launch the editor and confirm the scene renders identically
  — same meshes, same instancing, same text — and the Memory panel shows the `Frame`
  allocator's `Used` rising during a frame (now higher, since more goes through it)
  and resetting, with no crash.

## Files

- Create: `src/editor/src/rendering/passes/MeshBatching.h` (BatchEntry/BatchRun/BuildBatchRuns).
- Modify: `src/editor/src/rendering/passes/MeshRenderPass.cpp` (lights + batching + instances onto arena; include MeshBatching.h).
- Modify: `src/editor/src/rendering/MeshSystem.h` (`subMeshes` → `std::span<const SubMesh>`, `#include <span>`).
- Modify: `src/editor/src/rendering/MeshSystem.cpp` (span assignment + lifetime comment).
- Modify: `src/editor/src/rendering/passes/UiRenderPass.cpp` (`usedFontSizes` onto arena).
- Modify: `tests/test_alloc.cpp` (BuildBatchRuns unit tests).
- Modify: `tests/CMakeLists.txt` (add the passes include dir to `test_alloc`).

## Risks

- **GetMeshResources span lifetime** — the main correctness risk. Mitigated by the
  documented contract (entries stable during passes) and verifying `MeshRenderPass`
  is the only consumer. If a future consumer needs an owning copy, it can copy the
  span into arena memory.
- **Behavior parity of flat+sort batching** — mitigated by the `BuildBatchRuns` unit
  tests plus the visual smoke test. The draw loop is otherwise untouched.
- **Arena exhaustion on very large scenes** — mitigated by null-guards + the loud
  arena error; resolution is a capacity bump, deferred until observed.
