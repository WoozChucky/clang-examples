# Staging Buffer Pool + Upload-Path Leak Fixes — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** `allocator-toolkit` (stacking).

## Goal

Recycle the GameThread→RenderThread "staging buffers" — the raw `std::malloc`'d
mesh/texture upload buffers that are allocated when a background model load completes,
handed to the RenderThread through the `RendererCommand` ring, uploaded to the GPU, then
`std::free`'d — and fix the correctness bugs in that path surfaced while mapping it. Add a
read-only "Staging Pool" section to the editor Memory panel for visibility, mirroring the
existing "Snapshot Pool" section.

**Honest framing:** these allocations are **bursty / load-time** (per model + per material
load, mostly a startup burst then idle), not steady-state. The *pool* itself is therefore a
**lower-marginal-value** change than the steady churn already addressed (snapshot pool,
sparse-set). The genuine win in this path is the **leak fixes**; the pool is the requested,
nice-to-have recycling on top. This is recorded so future readers don't over-value the pool.

## Background — the upload lifecycle (current)

```
[ModelWorker thread]  assimp parse → std::vector<MeshVertex/uint32_t/SubMesh>
                      + malloc'd RGBA8 Texture (w*h*4)   → ModelLoadResult
        │  (m_CompletedJobs queue, mutex-guarded)
[GameThread tick]     malloc V/I/S + memcpy from the vectors → RendererCommand
                      → GRCommandRing.Push   (texture pointer forwarded by value)
        │  (SpscRing<RendererCommand,64>, POD copy of the struct incl. raw ptrs)
[RenderThread loop]   GRCommandRing.Pop → Renderer::AddMesh / AddMaterial
                      (NVRHI writeBuffer/writeTexture, synchronous) → std::free(staging)
```

Allocation sites (all `std::malloc`, size known exactly at the call):
- Mesh **vertices** — `GameThread.cpp:246` — `VertexCount * sizeof(MeshVertex)` (32 B/vert).
- Mesh **indices** — `GameThread.cpp:251` — `IndexCount * sizeof(uint32_t)`.
- Mesh **submeshes** — `GameThread.cpp:256` — `SubMeshCount * sizeof(SubMesh)` (12 B; count is 0 for single-submesh models).
- **Texture** — `GameThread.cpp:639` (on the **ModelWorker** thread) — `w*h*4` (RGBA8).

Free sites (all `std::free`):
- RenderThread mesh — `RenderThread.cpp:112,113,114` (after `AddMesh` returns).
- RenderThread texture — `RenderThread.cpp:141` (after `AddMaterial` returns).
- GameThread mesh ring-full retry — `GameThread.cpp:263,264,265`.

The ring command (`src/common/include/ApplicationContext.h`) is a POD `RendererCommand`
with an anonymous union (`MeshRequest` / `MaterialRequest` arms) tagged by
`RendererCommandType Type`. It carries **raw pointers + sizes/counts**, copied by value
through the SpscRing — the pointed-to buffers live outside the ring and ownership transfers
by convention (producer allocates, consumer frees). Sizes are **highly variable** (a 96 B
triangle to a 64 MB texture). Per model load: 1 mesh command (up to 3 buffers) + 0-or-1
material command (1 texture). `MeshSystem`/`MaterialSystem` keep their own *separate*
deep-copied CPU caches (`cpuVertices`/`cpuIndices`/`cpuPixels`) for backend hot-swap — those
are independent of the staging buffers and out of scope here.

### Pre-existing bugs in this path (to fix)

1. **Material ring-full leak** (`GameThread.cpp:282-287`): if the *material* push fails, the
   code only logs and falls through — `res` is destroyed at the end of the loop iteration,
   so `res.Texture` is **leaked and the material upload is silently lost** (never retried).
   The *mesh* path handles ring-full correctly (frees + re-queues the job); the material path
   does not.
2. **Worker texture-overwrite leak** (`GameThread.cpp:637-641`): `processMesh` sets
   `result.Texture = nullptr` and re-allocates per submesh. A multi-submesh model where more
   than one submesh has a diffuse texture **leaks the prior allocation**.
3. **Ring-full mid-drain drops remaining loads** (`GameThread.cpp:268` `break`): on a
   ring-full retry the code re-queues only the *current* `res` then `break`s out of the drain
   loop — the **other already-completed loads still in the local queue are destroyed**
   (lost, and with the pool their buffers would leak). Latent today; entangled with the pool.
4. **`uint32` overflow** in `width * height` before the `ull` cast
   (`GameThread.cpp:639-640`) — only at absurd dimensions, but trivial to make correct.

## Scope

**In scope:**
- A header-only, thread-safe `StagingBufferPool` (`src/editor/src/threading/StagingBufferPool.h`):
  best-fit, grow-to-fit free-list; per-buffer capacity carried in a 16-byte header so a raw
  pointer can be returned without its size; mutex-guarded; `StagingPoolStats` + a
  `GetStagingPool()` singleton accessor and a `GetStagingPoolStats()` free function.
- Rewire the 4 allocation sites to `Acquire` and the (4 + 3) free sites to `Return`.
- Fix the 4 bugs above (the material/mesh ring-full handling restructured with a
  `MeshUploaded` flag; worker overwrite returns the prior buffer; remaining local loads
  re-queued on ring-full; `uint32` overflow cast).
- A "Staging Pool" `CollapsingHeader` in the editor `MemoryPanel`, humanized via the existing
  `FormatBytes`.
- Unit tests of the pool in `test_alloc`.

**Out of scope:** routing these buffers through the Engine allocator toolkit (its
`PoolAllocator` is single-thread / fixed-block — wrong fit); eliminating the
`MeshSystem`/`MaterialSystem` CPU-cache deep copies (a separate, larger refactor); supporting
N textures per model (the path is single-texture today); any steady-state / per-frame
allocation work.

## Design

### `StagingBufferPool` (header-only, thread-safe)

A buffer is `[ Header | user data ]`. The header is 16 bytes so the returned user pointer is
16-aligned (more than enough for `MeshVertex`/`uint32_t`/`SubMesh`, all ≤4-aligned):

```cpp
// src/editor/src/threading/StagingBufferPool.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

struct StagingPoolStats {
    size_t   Free;            // blocks currently on the free-list
    size_t   InUse;           // blocks handed out and not yet returned
    size_t   Created;         // blocks ever malloc'd
    uint64_t Reuses;          // Acquire calls satisfied from the free-list
    size_t   ReservedBytes;   // total capacity of all blocks the pool owns (free + in use)
    size_t   FreeBytes;       // total capacity sitting on the free-list
};

// Best-fit, grow-to-fit free-list of variable-size staging buffers. Acquire on the
// GameThread / ModelWorker, Return on the RenderThread / GameThread retry — so every
// method is mutex-guarded. Bursty (load-time) workload, so a mutex is the right tool
// (mirrors the ECS SnapshotPool decision).
class StagingBufferPool {
public:
    static constexpr size_t kHeaderSize = 16; // >= sizeof(size_t), keeps user ptr 16-aligned

    void* Acquire(size_t size) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        // Best-fit: smallest free block that fits, to avoid a huge buffer serving tiny requests.
        size_t best = SIZE_MAX;
        size_t bestIdx = SIZE_MAX;
        for (size_t i = 0; i < m_Free.size(); ++i) {
            if (m_Free[i].Capacity >= size && m_Free[i].Capacity < best) {
                best = m_Free[i].Capacity;
                bestIdx = i;
            }
        }
        if (bestIdx != SIZE_MAX) {
            FreeBlock b = m_Free[bestIdx];
            m_Free[bestIdx] = m_Free.back();
            m_Free.pop_back();
            m_FreeBytes -= b.Capacity;
            ++m_InUse;
            ++m_Reuses;
            return b.UserPtr;
        }
        // Grow: malloc header + payload, stamp capacity into the header.
        auto* block = static_cast<char*>(std::malloc(kHeaderSize + size));
        *reinterpret_cast<size_t*>(block) = size;        // capacity == requested size
        void* userPtr = block + kHeaderSize;
        m_ReservedBytes += size;
        ++m_Created;
        ++m_InUse;
        return userPtr;
    }

    void Return(void* userPtr) {
        if (!userPtr) return;
        auto* block = static_cast<char*>(userPtr) - kHeaderSize;
        const size_t capacity = *reinterpret_cast<size_t*>(block);
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Free.push_back(FreeBlock{ userPtr, capacity });
        m_FreeBytes += capacity;
        --m_InUse;
    }

    StagingPoolStats Stats() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return StagingPoolStats{ m_Free.size(), m_InUse, m_Created,
                                 m_Reuses, m_ReservedBytes, m_FreeBytes };
    }

    ~StagingBufferPool() {
        // Free only the free-list. Unlike the ECS SnapshotPool we do NOT assert InUse==0:
        // at teardown the GameThread's pending m_CompletedJobs may still hold acquired
        // buffers (texture acquired on the worker, not yet pushed). Those leak at process
        // exit — benign, and identical to the prior std::malloc behaviour. We just don't
        // own them, so we can't free them here.
        for (const FreeBlock& b : m_Free)
            std::free(static_cast<char*>(b.UserPtr) - kHeaderSize);
    }

private:
    struct FreeBlock { void* UserPtr; size_t Capacity; };
    mutable std::mutex     m_Mutex;
    std::vector<FreeBlock> m_Free;
    size_t   m_InUse        = 0;
    size_t   m_Created      = 0;
    uint64_t m_Reuses       = 0;
    size_t   m_ReservedBytes = 0;
    size_t   m_FreeBytes     = 0;
};

// Single editor-wide instance. Non-leaked Meyers singleton: the app joins both threads
// before static destruction, so no Acquire/Return races the destructor. Header-only inline
// → one instance shared across the editor's translation units. Tests instantiate their own
// local StagingBufferPool instead (deterministic, process-global counters avoided).
inline StagingBufferPool& GetStagingPool() {
    static StagingBufferPool pool;
    return pool;
}

inline StagingPoolStats GetStagingPoolStats() {
    return GetStagingPool().Stats();
}
```

**Why best-fit, not first-fit:** first-fit could hand a 64 MB texture block to a 96 B vertex
request, then that block is retained at 64 MB capacity. Best-fit (smallest fitting) keeps a
giant block available for the next giant request. The free-list stays small (bounded by
in-flight buffers — ring depth 64), so the linear scan is cheap.

**Why capacity in a header:** the RenderThread returns a bare `void*` pulled from the POD
ring command — it has the pointer but not the original size. Storing capacity 16 bytes ahead
of the user pointer lets `Return` recover it. The user pointer is what crosses the ring and
what NVRHI reads from; the header is never seen by anything but the pool.

### Wiring the allocation / free sites

`GameThread.cpp` and `RenderThread.cpp` are both in `src/editor/src/threading/`, which is on
the editor's include path, so each adds `#include "StagingBufferPool.h"`.

- `GameThread.cpp:246/251/256` `std::malloc(...)` → `GetStagingPool().Acquire(...)` (same
  size expressions; same `static_cast<T*>`).
- `GameThread.cpp:639` (worker) `std::malloc(...)` → `GetStagingPool().Acquire(...)`.
- `RenderThread.cpp:112/113/114/141` `std::free(p)` → `GetStagingPool().Return(p)`.
- `GameThread.cpp:263/264/265` (retry) `std::free(p)` → `GetStagingPool().Return(p)`.

### Bug fixes (folded into the rewire)

**(1)+(3) Ring-full handling, restructured with a `MeshUploaded` flag.** Add
`bool MeshUploaded{false};` to `ModelLoadResult` (`GameThread.h`). The drain loop becomes:

```cpp
while (!local.empty())
{
    ModelLoadResult res = std::move(local.front());
    local.pop();
    if (!res.success) { SM_ERROR(/* ...ticket/error... */); continue; }

    if (!res.MeshUploaded)
    {
        RendererCommand meshCmd{};
        meshCmd.Type = RendererCommandType::RequestMesh;
        meshCmd.TicketId = res.ticketId;
        meshCmd.MeshRequest.VertexCount  = res.vertices.size();
        meshCmd.MeshRequest.IndexCount   = res.indices.size();
        meshCmd.MeshRequest.SubMeshCount = res.subMeshes.size() > 1 ? res.subMeshes.size() : 0;
        meshCmd.MeshRequest.Vertices = nullptr;
        meshCmd.MeshRequest.Indices  = nullptr;
        meshCmd.MeshRequest.SubMeshes = nullptr;
        if (meshCmd.MeshRequest.VertexCount > 0) {
            meshCmd.MeshRequest.Vertices = static_cast<MeshVertex*>(
                GetStagingPool().Acquire(meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex)));
            std::memcpy(meshCmd.MeshRequest.Vertices, res.vertices.data(),
                        meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex));
        }
        if (meshCmd.MeshRequest.IndexCount > 0) {
            meshCmd.MeshRequest.Indices = static_cast<uint32_t*>(
                GetStagingPool().Acquire(meshCmd.MeshRequest.IndexCount * sizeof(uint32_t)));
            std::memcpy(meshCmd.MeshRequest.Indices, res.indices.data(),
                        meshCmd.MeshRequest.IndexCount * sizeof(uint32_t));
        }
        if (meshCmd.MeshRequest.SubMeshCount > 0) {
            meshCmd.MeshRequest.SubMeshes = static_cast<SubMesh*>(
                GetStagingPool().Acquire(meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh)));
            std::memcpy(meshCmd.MeshRequest.SubMeshes, res.subMeshes.data(),
                        meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh));
        }

        if (!m_AppContext->GRCommandRing.Push(meshCmd)) {
            SM_WARN("GRCommandRing full, retrying mesh upload next frame (ticket %llu)",
                    (unsigned long long)res.ticketId);
            if (meshCmd.MeshRequest.Vertices)  GetStagingPool().Return(meshCmd.MeshRequest.Vertices);
            if (meshCmd.MeshRequest.Indices)   GetStagingPool().Return(meshCmd.MeshRequest.Indices);
            if (meshCmd.MeshRequest.SubMeshes) GetStagingPool().Return(meshCmd.MeshRequest.SubMeshes);
            RequeueAndStop(res, local);   // re-queue this res (MeshUploaded still false) + remaining local
            break;
        }
        res.MeshUploaded = true;          // mesh is on the ring; never re-upload it
    }

    if (!res.Texture) continue;           // no material to upload

    RendererCommand materialCmd{};
    materialCmd.Type = RendererCommandType::RequestMaterial;
    materialCmd.TicketId = res.ticketId;
    materialCmd.MaterialRequest.Width   = res.Width;
    materialCmd.MaterialRequest.Height  = res.Height;
    materialCmd.MaterialRequest.Texture = res.Texture;

    if (!m_AppContext->GRCommandRing.Push(materialCmd)) {
        SM_WARN("GRCommandRing full, retrying material upload next frame (ticket %llu)",
                (unsigned long long)res.ticketId);
        // Do NOT Return res.Texture — it is the only copy and is needed for the retry.
        RequeueAndStop(res, local);       // re-queue (MeshUploaded == true) + remaining local
        break;
    }
}
```

`RequeueAndStop` is a small local lambda (declared before the drain loop) that, under
`m_JobMutex`, pushes `res` and then every remaining item in `local` back onto
`m_CompletedJobs`:

```cpp
auto RequeueAndStop = [&](ModelLoadResult& cur, std::queue<ModelLoadResult>& remaining) {
    std::scoped_lock lg(m_JobMutex);
    m_CompletedJobs.push(std::move(cur));
    while (!remaining.empty()) { m_CompletedJobs.push(std::move(remaining.front())); remaining.pop(); }
};
```

This fixes the material leak (the texture is kept and the job retried, not dropped), prevents
a double mesh upload (the `MeshUploaded` guard), and stops the mid-drain drop of other
completed loads (bug 3).

**(2) Worker overwrite** (`GameThread.cpp:637-641`): before re-allocating, return any prior
buffer:

```cpp
if (result.Texture) { GetStagingPool().Return(result.Texture); result.Texture = nullptr; }
if (!pixels.empty()) {
    const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint32_t); // (4) no overflow
    result.Texture = static_cast<uint32_t*>(GetStagingPool().Acquire(bytes));
    std::memcpy(result.Texture, pixels.data(), bytes);
}
```

(`result.Texture = nullptr;` at `:637` is preserved by the guard above resetting it.)

### Panel section

In `MemoryPanel.cpp`, add `#include "StagingBufferPool.h"` and, after the "Snapshot Pool"
header, a "Staging Pool" `CollapsingHeader`:

```cpp
if (ImGui::CollapsingHeader("Staging Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
    const StagingPoolStats s = GetStagingPoolStats();
    ImGui::Text("Free:      %zu", s.Free);
    ImGui::Text("In use:    %zu", s.InUse);
    ImGui::Text("Created:   %zu", s.Created);
    ImGui::Text("Reuses:    %llu", (unsigned long long)s.Reuses);
    ImGui::Text("Reserved:  %s", FormatBytes(s.ReservedBytes).c_str());
    ImGui::Text("Free bytes:%s", FormatBytes(s.FreeBytes).c_str());
}
```

No signature change — the panel reads the global pool directly (the editor links nothing new).

## Build / ABI

Editor + tests only. **No** ECS / Engine / game change; **no `GAME_API_VERSION` bump**
(no `GameState` / game-export / component-type change) and no ABI-affecting header change in a
shared DLL. `StagingBufferPool.h` is header-only and added to no `.cpp` list (the editor lists
sources explicitly, but a header needs no entry). `GameThread.h` gains the `MeshUploaded`
field. `test_alloc` gets `${CMAKE_SOURCE_DIR}/src/editor/src/threading` added to its
`target_include_directories` (alongside the existing `passes` entry) so it can include the
header. Build preset `msvc-win64-vs2026-community`. Editor rebuild + restart for the smoke
test (load a model, watch the Staging Pool section show Created then Reuses climb across loads).

## Testing (`test_alloc`, local `StagingBufferPool` instances)

Tests instantiate their own `StagingBufferPool` (not the singleton) for deterministic
counters:

- **Acquire alignment + usable size:** `Acquire(100)` returns a non-null, 16-aligned pointer;
  writing 100 bytes through it is valid (no overlap with the header).
- **Return→Acquire reuses the block:** `Acquire(64)` → `Return(p)` → `Acquire(64)` returns the
  same pointer; `Stats().Reuses == 1`, `Stats().Created == 1`.
- **Grow when nothing fits:** with one free 64-byte block, `Acquire(128)` mallocs a new block
  (`Created == 2`), does not reuse (`Reuses` unchanged).
- **Best-fit picks the smallest fitting block:** free a 1024-byte block and a 64-byte block;
  `Acquire(32)` returns the 64-byte block (verify by pointer identity), leaving the 1024-byte
  block free.
- **Capacity is reused, not shrunk:** `Acquire(256)` → `Return` → `Acquire(8)` reuses the
  256-byte block; a subsequent `Acquire(200)` after returning it still fits without growing.
- **Stats math:** `InUse` rises while held and falls on `Return`; `Free`/`FreeBytes` mirror
  the free-list; `ReservedBytes` equals the sum of distinct block capacities ever created.

Existing `test_ecs` is unaffected (no ECS change) but should remain green.

## Risks

- **Marginal value:** stated up front — the pool recycles a bursty/load-time workload. The
  leak fixes carry the real correctness value; the pool is the requested recycling layer.
- **Cross-thread alloc/free:** two allocating threads (GameThread, ModelWorker) and two
  freeing threads (RenderThread, GameThread retry). Mitigated by the single mutex on every
  pool method; the bursty rate makes lock contention a non-issue.
- **Header underflow / non-pool pointer:** `Return` blindly reads `userPtr - 16`. Every
  pointer reaching `Return` originates from `Acquire` (all malloc sites are converted), so
  this holds; passing a non-pool pointer is a programming error, not a guarded case
  (consistent with the codebase's "trust internal code" stance).
- **Teardown leak of in-flight buffers:** if the app exits with pending model loads holding
  acquired buffers, those leak at process exit (the dtor only frees the free-list). Benign and
  identical to the prior `std::malloc` behaviour — hence no `InUse==0` assert (deliberately
  unlike the SnapshotPool, which *can* guarantee full reclaim).
- **Best-fit retention of a large block:** a one-off 64 MB texture block is retained for reuse
  rather than freed. Acceptable for a debug/editor tool and bounded by the largest asset; a
  size cap is intentionally omitted (YAGNI) and can be added later if footprint matters.
