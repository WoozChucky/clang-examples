# Allocator Toolkit + Engine Library — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.

## Goal

Reduce heap fragmentation caused by scattered `new`/`make_shared`/`malloc` and gain
per-subsystem visibility into memory use, by introducing a small, reusable allocator
toolkit (arena + pool) plus a new `Engine` shared library that hosts a cross-DLL
allocator registry. Each allocator carries a category tag and cheap stats, so
visibility falls out of the toolkit itself without a global `new`/`delete` hook.

This is the **first** of a planned sequence. The highest-value fragmentation target —
the ECS per-tick copy-on-write clone churn — is explicitly **deferred** to a follow-up
spec, because its allocations cross threads and outlive the tick (the RenderThread holds
`shared_ptr<const ECS>` snapshots for 1–2 frames), which makes it the hardest consumer
to get right. This spec builds and proves the tools first.

## Background

The codebase already has two relevant pieces:

- `src/editor/src/rendering/FrameAllocator.h` — a linear bump allocator (default 16 MB),
  owned by `Renderer` as `m_FrameAllocator`, reset each frame, passed to render passes.
  This is exactly the arena pattern, already working in production.
- `src/editor/src/alloc.h` — a global `operator new`/`delete` override leak tracker
  (`ALLOC_TRACKER_ENABLED`, OFF by default). It captures a full backtrace and takes a
  global mutex **on every allocation**, which is microseconds per alloc — far too heavy
  for always-on use. **This spec does not extend or rely on it.** It remains as-is,
  independent, and may be retired in a later cleanup (out of scope here).

DLL boundary context: all targets share one MSVC runtime
(`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded...DLL`), so allocations made in one module
and freed in another are safe. `ecs.dll` exists specifically so its template
instantiations and `shared_ptr` deleters are a *single instance* across `editor.exe` and
the hot-reloadable `game.dll` — not because ECS is reloaded (it isn't). The new
`Engine.dll` follows the same rationale.

## Non-goals (out of scope for this spec)

- ECS copy-on-write clone churn (the cross-thread snapshot lifetime case) — separate spec.
- Replacing the global `new`/`delete` (would be a drop-in third-party heap like
  mimalloc/rpmalloc, not a hand-rolled one).
- Thread-safe allocator variants (allocators are single-threaded by contract).
- A growable *arena* (arena stays fixed by design — see below).
- Retiring `alloc.h`.
- Wiring `ecs.dll`/`game.dll` to register allocators (they link `Engine` later, when
  they actually own allocators).

## Architecture overview

```
Engine.dll (SHARED, loaded once, never unloaded)
  └── AllocatorRegistry  (single cross-DLL instance, exported)   ← observer only

engine/include/memory/*.h  (header-only value types, zero Engine link to use)
  ├── IAllocator           (minimal virtual interface)
  ├── AllocatorStats       (POD numeric stats)
  ├── MemoryCategory       (enum + ToString)
  ├── ArenaAllocator       (linear/bump, FIXED capacity)
  ├── PoolAllocator        (fixed-block free-list, GROWABLE via chained slabs)
  └── MemUtil              (AlignUp, New/Delete helpers)

editor.exe
  ├── FrameAllocator : ArenaAllocator         (owned by Renderer; registered)
  └── MemoryPanel (ImGui)  reads Engine::Registry()   ← view only
```

Key principles:

- **Allocator instances are owned by their consuming subsystem** (e.g. `Renderer` owns
  the frame arena). There is no central allocator store. The registry holds only
  non-owning `IAllocator*` weak references for observation.
- **Category is a per-allocator tag**, set at construction — not a per-allocation
  argument. In allocator-object routing you make one allocator per subsystem, so the
  allocator inherently *is* a category.
- **Engine is data; editor is view.** Engine has no ImGui dependency. The panel lives in
  the editor and reads the registry.
- **Engine is SHARED for single-instance correctness, not for hot-reload.** A static lib
  linked into multiple in-process modules would give each its own duplicate registry,
  breaking cross-module aggregation. Engine.dll is loaded at startup and never unloaded,
  so the registry and stats survive `game.dll` hot-reloads.

## Components

### IAllocator + AllocatorStats (header-only)

```cpp
struct AllocatorStats {
    size_t   Used = 0;       // bytes currently handed out
    size_t   Peak = 0;       // high-water mark of Used
    size_t   Capacity = 0;   // total backing bytes
    uint64_t AllocCount = 0;
    uint64_t FreeCount  = 0;
};

class IAllocator {
public:
    virtual ~IAllocator() = default;
    virtual void* Allocate(size_t size, size_t align = alignof(std::max_align_t)) = 0;
    virtual void  Deallocate(void* ptr, size_t size = 0) = 0;
    [[nodiscard]] virtual const AllocatorStats& Stats() const = 0;
    [[nodiscard]] virtual MemCategory Category() const = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
};
```

The virtual interface exists only for polymorphic use (the registry holds `IAllocator*`).
Hot paths use the **concrete** allocator type directly, so the call is devirtualized and
costs nothing. `IAllocator` is header-only with a consistent layout across modules; it
needs no export annotation. `Reset()` is **not** on the interface (not universal); it
lives on the concrete Arena/Pool.

### MemoryCategory (header-only)

```cpp
enum class MemCategory : uint8_t {
    General, FrameTransient, Renderer, Mesh, Material,
    Texture, UI, ECS, Snapshot, Game, Count
};
const char* ToString(MemCategory);   // inline; used by the panel
```

Extended by editing the enum. The registry can index per-category aggregates by the
enum value.

### ArenaAllocator (header-only, FIXED capacity)

Linear bump allocator generalizing the existing `FrameAllocator`.

- Owns a `malloc`'d buffer of fixed `capacity`. Also supports a view-over-external-buffer
  construction (mirrors `FrameAllocator::m_OwnsMemory`).
- `Allocate(size, align)`: aligns the offset, bumps, returns the pointer; on overflow
  returns `nullptr` and logs `SM_ERROR` (same loud behavior as `FrameAllocator`). Updates
  `Used`, `Peak`, `AllocCount`.
- `Deallocate(...)`: no-op — the arena frees in bulk.
- `Reset()`: sets offset to 0 (reclaims everything); `Peak` is retained for diagnostics.
- Markers for stack-style nested frees without a separate StackAllocator:
  `using Marker = size_t; Marker GetMarker() const; void RewindTo(Marker);`
- API-compat helpers so render code is untouched: `template<class T> T* Allocate();`,
  `template<class T> T* AllocateArray(size_t count);`, plus accessors equivalent to
  `FrameAllocator`'s (`GetUsedBytes/GetPeakUsage/GetCapacity/GetUsagePercent`) expressed
  over `Stats()`.

**Fixed by design:** the frame arena's fixed budget is a guardrail. Silent growth would
hide a runaway per-frame allocation instead of tripping the error. A growable arena is a
future addition only if a genuine unbounded-scratch consumer appears.

### PoolAllocator (header-only, GROWABLE via chained slabs)

Fixed-block free-list allocator; eliminates fragmentation for same-size churn.

```cpp
PoolAllocator(size_t blockSize, size_t blockCount, size_t blockAlign,
              MemCategory cat, const char* name);
```

- Pre-allocates one slab (`blockSize`-aligned × `blockCount`) and threads an intrusive
  free-list (each free block stores the next pointer in its first bytes; requires
  `blockSize >= sizeof(void*)`, asserted).
- `Allocate(size, align)`: asserts `size <= blockSize && align <= blockAlign`; pops the
  free-list head. **On exhaustion it allocates a new slab** and continues. Updates `Used`,
  `Peak`, `AllocCount`.
- `Deallocate(ptr)`: pushes the block back to the free-list head, O(1). Updates `Used`,
  `FreeCount`. (Debug builds may validate `ptr` lies within an owned slab and is aligned.)
- `Reset()`: rebuilds the free-list so all blocks across all slabs are free again.
- Destructor frees every slab.
- An optional max-slab cap can bound growth.

**Growth is safe and fragmentation-friendly:** slabs are never moved or freed until the
pool dies, so every outstanding pointer stays valid; each growth is one large, infrequent
`malloc` (the opposite of scattered small `new`s).

### MemUtil (header-only)

```cpp
constexpr size_t AlignUp(size_t value, size_t alignment);
template<class A, class T, class... Args> T* New(A& alloc, Args&&... args); // placement new
template<class A, class T>                void Delete(A& alloc, T* ptr);     // dtor + Deallocate
```

Object construction is explicit through these helpers so the `IAllocator` vtable stays
minimal (no templated virtuals).

### AllocatorRegistry (exported from Engine.dll)

```cpp
class ENGINE_API AllocatorRegistry {
public:
    void Register(IAllocator*);     // non-owning
    void Unregister(IAllocator*);
    void ForEach(const std::function<void(IAllocator*)>&) const;
    AllocatorStats SumByCategory(MemCategory) const;
    [[nodiscard]] size_t Count() const;
};
ENGINE_API AllocatorRegistry& Registry();   // single instance, lives in Engine.dll TU
```

- **Single instance** via an exported accessor whose static lives in Engine.dll's TU, so
  all modules that link Engine see the same registry.
- **Locked** with a mutex / `CRITICAL_SECTION`. The lock is taken only on
  `Register`/`Unregister` (rare) and `ForEach`/`SumByCategory` (once per panel frame).
  **Never on the allocation hot path** → zero per-allocation cost.
- Holds non-owning `IAllocator*`. `ForEach` calls back through the allocator's vtable into
  its owning module; valid while that module is loaded and the allocator is alive.
- **Registration is explicit**, not automatic in the ctor. This keeps the allocator value
  types free of any Engine dependency (usable in tests/tools with no link); only code that
  opts into tracking — e.g. `Renderer` registering its `m_FrameAllocator` — pulls the
  Engine.dll link.

**DLL discipline (forward note):** when `game.dll` eventually owns allocators, each must
be unregistered and destroyed *before* `FreeLibrary`, identical to the existing `ISystem`
lifetime rule. Non-issue for v1 (allocators are render-side, in `editor.exe`).

### MemoryPanel (editor-side ImGui)

`src/editor/src/rendering/imgui/MemoryPanel.{h,cpp}` (its own file — `ImGuiRenderer.cpp`
is already large).

- A "Memory" window listing each registered allocator: Name, Category, Used / Peak /
  Capacity, usage %, Alloc / Free counts; plus per-category rollups via `SumByCategory`.
- Reads `Engine::Registry()` only. No Engine→ImGui dependency.
- Called from the existing ImGui pass; toggled like other debug windows.

## Data flow

1. `Renderer` constructs `m_FrameAllocator` (an `ArenaAllocator` subclass, category
   `FrameTransient`) and calls `Engine::Registry().Register(&m_FrameAllocator)` at init,
   `Unregister` at shutdown.
2. Each frame, render passes bump-allocate from it (unchanged); `Reset()` at frame end.
   Stats update inline (no locking).
3. The ImGui pass draws `MemoryPanel`, which locks the registry briefly, enumerates
   allocators, reads their `Stats()`, and renders the table + per-category sums.

This single consumer (the frame arena) exercises the entire stack end-to-end —
allocator → category → stats → registry → panel — proving the toolkit before any harder
consumer adopts it.

## File layout & build

New SHARED target `Engine`, mirroring `ecs.dll`:

```
src/engine/CMakeLists.txt
src/engine/include/Engine.h                     # ENGINE_API macro + Registry() decl
src/engine/include/memory/AllocatorStats.h
src/engine/include/memory/MemoryCategory.h
src/engine/include/memory/IAllocator.h
src/engine/include/memory/ArenaAllocator.h
src/engine/include/memory/PoolAllocator.h
src/engine/include/memory/MemUtil.h
src/engine/include/memory/AllocatorRegistry.h
src/engine/src/AllocatorRegistry.cpp            # exported impl + singleton + ToString
```

- `Engine` is `add_library(Engine SHARED ...)`, defines `ENGINE_EXPORTS` privately,
  `RUNTIME/ARCHIVE/LIBRARY_OUTPUT_DIRECTORY = RUNTIME_DIR` (lands next to the exe like
  `ecs.dll`, so **no copy step needed**), `MSVC_DEBUG_INFORMATION_FORMAT Embedded` in
  Debug, `FOLDER Libraries`.
- `ENGINE_API` mirrors `ECS_API`: `dllexport` under `ENGINE_EXPORTS`, else `dllimport`.
- Added to root `CMakeLists.txt` before `editor` (e.g. right after `src/ecs`).
- `editor` links `Engine` (added to its `target_link_libraries`) and adds
  `src/engine/include` to its include dirs.
- `FrameAllocator.h` stays under `rendering/` but now `#include <memory/ArenaAllocator.h>`
  and becomes:
  ```cpp
  class FrameAllocator : public ArenaAllocator {
  public:
      explicit FrameAllocator(size_t capacity = MB(16))
          : ArenaAllocator(capacity, MemCategory::FrameTransient, "Frame") {}
  };
  ```
  The `class FrameAllocator;` forward-decl in `IRenderPass.h` and all `FrameAllocator*`
  signatures stay valid; render code is untouched.

## Testing

New `test_alloc` target, mirroring `test_ecs` (custom EXPECT-style harness in
`tests/test_alloc.cpp`, links `Engine`, output `RUNTIME_DIR`). Expected final line:
`All allocator tests passed.`

- **Arena:** bump + alignment correctness; overflow returns `nullptr` (no crash);
  `Reset` reclaims and retains `Peak`; `GetMarker`/`RewindTo` nested frees;
  `AllocateArray<T>`; external-buffer construction; `Stats` accuracy (Used/Peak/Capacity).
- **Pool:** alloc/free reuse returns the same block; alignment; **exhaustion grows a new
  slab and pointers from earlier slabs remain valid**; `Reset` frees all blocks across
  slabs; `blockSize < sizeof(void*)` guard fires; `Stats` accuracy.
- **Registry:** `Register`/`Unregister` adjust `Count`; `ForEach` enumerates;
  `SumByCategory` aggregates across multiple allocators of one category.

The allocator value types are unit-testable in isolation; the registry test links Engine.

## Error handling

- Arena overflow: `nullptr` + `SM_ERROR` (caller checks, matching `FrameAllocator`).
- Pool growth failure (`malloc` returns null, or max-slab cap hit): `nullptr` + `SM_ERROR`.
- Misuse (pool `blockSize < sizeof(void*)`, oversized request): `SM_ASSERT` in debug.
- Registry: idempotent `Unregister` of an unknown pointer is a no-op.

## Future work (explicitly later, not this spec)

- ECS copy-on-write clone churn: pool/arena-backed component storage and snapshot
  allocation, handling the cross-thread `shared_ptr<const ECS>` lifetime. Promotes the
  registry's first cross-DLL consumers (`ecs.dll`).
- `runtime.exe` adoption (separate process, own Engine state).
- Growable arena (only if an unbounded-scratch consumer appears).
- Thread-safe allocator variants.
- Retiring `alloc.h`.
