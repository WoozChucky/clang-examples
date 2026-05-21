# Staging Buffer Pool + Upload-Path Leak Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recycle the GameThread→RenderThread mesh/texture staging buffers through a thread-safe pool and fix four pre-existing leaks/bugs in that upload path, with a Memory-panel section for visibility.

**Architecture:** A header-only, mutex-guarded `StagingBufferPool` (best-fit, grow-to-fit free-list; per-buffer capacity stamped in a 16-byte header so a bare pointer can be returned) replaces the `std::malloc`/`std::free` on the upload path. Allocation happens on the GameThread and ModelWorker; return happens on the RenderThread and the GameThread ring-full retry. The swap of allocator↔deallocator is done atomically in one task to avoid a heap-corruption intermediate state, and the ring-full handling is restructured with a `MeshUploaded` flag so a retry never re-uploads the mesh and never drops other completed loads.

**Tech Stack:** C++23, CMake (preset `msvc-win64-vs2026-community`), the existing `test_alloc` custom test harness, Dear ImGui (Memory panel).

**Spec:** `docs/superpowers/specs/2026-05-21-staging-buffer-pool-design.md`

**Branch:** `allocator-toolkit` (stacking). Do NOT merge or offer to merge the branch.

---

## File Structure

- **Create** `src/editor/src/threading/StagingBufferPool.h` — the entire pool: `StagingPoolStats`, `StagingBufferPool` (Acquire/Return/Stats/dtor), `GetStagingPool()` singleton, `GetStagingPoolStats()`. Header-only so `test_alloc` can instantiate a local pool. Sole responsibility: recycle variable-size byte buffers across threads.
- **Modify** `tests/CMakeLists.txt` — add the `threading` dir to `test_alloc`'s includes.
- **Modify** `tests/test_alloc.cpp` — add `T50`–`T55` pool unit tests + register them.
- **Modify** `src/editor/src/threading/GameThread.h` — add `bool MeshUploaded{false};` to `ModelLoadResult`.
- **Modify** `src/editor/src/threading/GameThread.cpp` — include the header; rewrite the `ProcessCompletedModelLoads` drain loop (mesh Acquire ×3, `MeshUploaded` flag, `requeueAndStop`, material-full re-queue); rewrite the worker `processMesh` texture path (Acquire + overwrite-Return + overflow cast).
- **Modify** `src/editor/src/threading/RenderThread.cpp` — include the header; replace the 4 `std::free` calls with `Return`.
- **Modify** `src/editor/src/rendering/imgui/MemoryPanel.cpp` — include the header; add the "Staging Pool" section.

---

### Task 1: StagingBufferPool + unit tests

**Files:**
- Create: `src/editor/src/threading/StagingBufferPool.h`
- Modify: `tests/CMakeLists.txt:31-33`
- Test: `tests/test_alloc.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_alloc.cpp`, add `#include <cstring>` to the include block (after line 4, `#include <string>`) and add `#include "StagingBufferPool.h"` after the existing `#include <MeshBatching.h>` (line 13). Then add these six test functions immediately before `int main()` (line 374):

```cpp
static void T50_staging_acquire_alignment()
{
    StagingBufferPool pool;
    void* p = pool.Acquire(100);
    EXPECT_NE(p, nullptr);
    EXPECT_EQ((uintptr_t)p % 16, (uintptr_t)0);
    std::memset(p, 0xAB, 100); // writing the full requested size must be valid
    pool.Return(p);
}

static void T51_staging_return_then_acquire_reuses()
{
    StagingBufferPool pool;
    void* a = pool.Acquire(64);
    pool.Return(a);
    void* b = pool.Acquire(64);
    EXPECT_EQ(a, b);                                  // freed block reused
    EXPECT_EQ(pool.Stats().Reuses, (uint64_t)1);
    EXPECT_EQ(pool.Stats().Created, (size_t)1);
    pool.Return(b);
}

static void T52_staging_grows_when_nothing_fits()
{
    StagingBufferPool pool;
    void* a = pool.Acquire(64);
    pool.Return(a);                                   // one free 64-byte block
    void* big = pool.Acquire(128);                    // 64 < 128 -> must grow
    EXPECT_NE(big, nullptr);
    EXPECT_EQ(pool.Stats().Created, (size_t)2);
    EXPECT_EQ(pool.Stats().Reuses, (uint64_t)0);
    pool.Return(big);
}

static void T53_staging_best_fit_smallest()
{
    StagingBufferPool pool;
    void* big = pool.Acquire(1024);
    void* small = pool.Acquire(64);
    pool.Return(big);
    pool.Return(small);                               // free-list: 1024 and 64
    void* got = pool.Acquire(32);                     // best-fit -> the 64 block
    EXPECT_EQ(got, small);
    pool.Return(got);
}

static void T54_staging_capacity_reused_not_shrunk()
{
    StagingBufferPool pool;
    void* a = pool.Acquire(256);
    pool.Return(a);
    void* b = pool.Acquire(8);                        // reuses 256 block; cap stays 256
    EXPECT_EQ(a, b);
    EXPECT_EQ(pool.Stats().Created, (size_t)1);
    pool.Return(b);
    void* c = pool.Acquire(200);                      // still fits in 256 block, no growth
    EXPECT_EQ(c, a);
    EXPECT_EQ(pool.Stats().Created, (size_t)1);
    pool.Return(c);
}

static void T55_staging_stats_math()
{
    StagingBufferPool pool;
    void* a = pool.Acquire(100);
    void* b = pool.Acquire(200);
    StagingPoolStats s1 = pool.Stats();
    EXPECT_EQ(s1.InUse, (size_t)2);
    EXPECT_EQ(s1.Free, (size_t)0);
    EXPECT_EQ(s1.ReservedBytes, (size_t)300);
    EXPECT_EQ(s1.FreeBytes, (size_t)0);

    pool.Return(a);
    StagingPoolStats s2 = pool.Stats();
    EXPECT_EQ(s2.InUse, (size_t)1);
    EXPECT_EQ(s2.Free, (size_t)1);
    EXPECT_EQ(s2.FreeBytes, (size_t)100);
    EXPECT_EQ(s2.ReservedBytes, (size_t)300);         // never decremented

    pool.Return(b);
    StagingPoolStats s3 = pool.Stats();
    EXPECT_EQ(s3.InUse, (size_t)0);
    EXPECT_EQ(s3.Free, (size_t)2);
    EXPECT_EQ(s3.FreeBytes, (size_t)300);
}
```

Register them in `main()` immediately after the line `T46_batchruns_cap_leaves_trailing_unbatched();` (line 399):

```cpp
    T50_staging_acquire_alignment();
    T51_staging_return_then_acquire_reuses();
    T52_staging_grows_when_nothing_fits();
    T53_staging_best_fit_smallest();
    T54_staging_capacity_reused_not_shrunk();
    T55_staging_stats_math();
```

- [ ] **Step 2: Add the include dir so the test can find the header**

In `tests/CMakeLists.txt`, change the `test_alloc` include block (lines 31-33) from:

```cmake
target_include_directories(test_alloc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/passes
)
```

to:

```cmake
target_include_directories(test_alloc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/passes
    ${CMAKE_SOURCE_DIR}/src/editor/src/threading
)
```

- [ ] **Step 3: Run the build to verify it fails**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_alloc`
Expected: FAIL — `StagingBufferPool.h` cannot be found / `StagingBufferPool`/`StagingPoolStats` undeclared (the header does not exist yet).

- [ ] **Step 4: Create the header**

Create `src/editor/src/threading/StagingBufferPool.h` with exactly this content:

```cpp
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
// method is mutex-guarded. The workload is bursty (model load time), so a mutex is the
// right tool (mirrors the ECS SnapshotPool decision).
class StagingBufferPool {
public:
    static constexpr size_t kHeaderSize = 16; // >= sizeof(size_t); keeps user ptr 16-aligned

    void* Acquire(size_t size) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        // Best-fit: the smallest free block that fits, so a giant block is not consumed
        // by a tiny request.
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
        auto* block = static_cast<char*>(std::malloc(kHeaderSize + size));
        *reinterpret_cast<size_t*>(block) = size;        // stamp capacity into the header
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
        // Free only the free-list. Unlike the ECS SnapshotPool we deliberately do NOT
        // assert InUse==0: at teardown the GameThread's pending m_CompletedJobs may still
        // hold acquired buffers. Those leak at process exit — benign, identical to the
        // prior std::malloc behaviour; the pool does not own them and cannot free them.
        for (const FreeBlock& b : m_Free)
            std::free(static_cast<char*>(b.UserPtr) - kHeaderSize);
    }

private:
    struct FreeBlock { void* UserPtr; size_t Capacity; };
    mutable std::mutex     m_Mutex;
    std::vector<FreeBlock> m_Free;
    size_t   m_InUse         = 0;
    size_t   m_Created       = 0;
    uint64_t m_Reuses        = 0;
    size_t   m_ReservedBytes = 0;
    size_t   m_FreeBytes     = 0;
};

// Single editor-wide instance. Non-leaked Meyers singleton: the app joins both threads
// before static destruction, so no Acquire/Return races the destructor. Header-only inline
// → one instance shared across the editor's translation units. Tests instantiate their own
// local StagingBufferPool instead.
inline StagingBufferPool& GetStagingPool() {
    static StagingBufferPool pool;
    return pool;
}

inline StagingPoolStats GetStagingPoolStats() {
    return GetStagingPool().Stats();
}
```

- [ ] **Step 5: Run the build to verify it passes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_alloc`
Expected: PASS (clean compile/link). A benign `LF will be replaced by CRLF` git note may appear later; ignore.

- [ ] **Step 6: Run the tests**

Run: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe`
Expected: `All allocator tests passed.` (exit 0)

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/threading/StagingBufferPool.h tests/test_alloc.cpp tests/CMakeLists.txt
git commit -m "Add thread-safe StagingBufferPool with unit tests"
```

---

### Task 2: Rewire the upload path through the pool + fix the four bugs

This task swaps `std::malloc`/`std::free` for `Acquire`/`Return` on the whole upload path **atomically** (allocator and deallocator must change together — a half-swap would `std::free` a pool pointer that is offset past its header and corrupt the heap). It also folds in the four spec bug fixes. There is no automated test for the threaded upload path; verification is a clean editor build plus a manual smoke (loading models, no crash). State this honestly when reporting.

**Files:**
- Modify: `src/editor/src/threading/GameThread.h:52`
- Modify: `src/editor/src/threading/GameThread.cpp` (top includes; drain loop ~222-288; worker `processMesh` ~635-641)
- Modify: `src/editor/src/threading/RenderThread.cpp` (top includes; mesh frees ~111-114; texture free ~140-141)

- [ ] **Step 1: Add the `MeshUploaded` flag to `ModelLoadResult`**

In `src/editor/src/threading/GameThread.h`, change the `ModelLoadResult` struct (lines 41-53). Add the flag after the `Texture` member (line 52):

```cpp
    struct ModelLoadResult
    {
        uint64_t ticketId{0};
        bool success{false};
        std::string error;
        std::vector<MeshVertex> vertices{};
        std::vector<uint32_t> indices{};
        std::vector<SubMesh> subMeshes{}; // will contain elements if model has multiple sub-meshes
        // For now only 1 material is supported for the whole model, we will expand on this later
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t* Texture{nullptr}; // optional RGBA8 pixels (w*h entries)
        bool MeshUploaded{false};   // set once the mesh command is on the ring; gates retry re-upload
    };
```

- [ ] **Step 2: Include the pool header in `GameThread.cpp`**

In `src/editor/src/threading/GameThread.cpp`, add this include alongside the other local `#include "..."` headers near the top of the file (e.g. directly after the existing `#include "GameThread.h"`):

```cpp
#include "StagingBufferPool.h"
```

- [ ] **Step 3: Rewrite the drain loop (mesh Acquire + MeshUploaded gate + requeue-all + material-full fix)**

In `src/editor/src/threading/GameThread.cpp`, replace the entire `while (!local.empty()) { ... }` block (currently lines 222-288) with the following. Note the `requeueAndStop` lambda is declared **before** the loop:

```cpp
                auto requeueAndStop = [this](ModelLoadResult& cur,
                                             std::queue<ModelLoadResult>& remaining) {
                    std::scoped_lock lg(m_JobMutex);
                    m_CompletedJobs.push(std::move(cur));
                    while (!remaining.empty()) {
                        m_CompletedJobs.push(std::move(remaining.front()));
                        remaining.pop();
                    }
                };

                while (!local.empty())
                {
                    ModelLoadResult res = std::move(local.front());
                    local.pop();

                    if (!res.success)
                    {
                        SM_ERROR("Model load failed for ticket %llu: %s", (unsigned long long)res.ticketId, res.error.c_str());
                        continue;
                    }

                    if (!res.MeshUploaded)
                    {
                        // Send mesh upload command
                        RendererCommand meshCmd{};
                        meshCmd.Type = RendererCommandType::RequestMesh;
                        meshCmd.TicketId = res.ticketId; // Use entity ID as ticket
                        meshCmd.MeshRequest.VertexCount = res.vertices.size();
                        meshCmd.MeshRequest.IndexCount = res.indices.size();
                        meshCmd.MeshRequest.SubMeshCount = res.subMeshes.size() > 1 ? res.subMeshes.size() : 0;
                        meshCmd.MeshRequest.Vertices = nullptr;
                        meshCmd.MeshRequest.Indices = nullptr;
                        meshCmd.MeshRequest.SubMeshes = nullptr;

                        if (meshCmd.MeshRequest.VertexCount > 0)
                        {
                            meshCmd.MeshRequest.Vertices = static_cast<MeshVertex*>(GetStagingPool().Acquire(meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex)));
                            std::memcpy(meshCmd.MeshRequest.Vertices, res.vertices.data(), meshCmd.MeshRequest.VertexCount * sizeof(MeshVertex));
                        }
                        if (meshCmd.MeshRequest.IndexCount > 0)
                        {
                            meshCmd.MeshRequest.Indices = static_cast<uint32_t*>(GetStagingPool().Acquire(meshCmd.MeshRequest.IndexCount * sizeof(uint32_t)));
                            std::memcpy(meshCmd.MeshRequest.Indices, res.indices.data(), meshCmd.MeshRequest.IndexCount * sizeof(uint32_t));
                        }
                        if (meshCmd.MeshRequest.SubMeshCount > 0)
                        {
                            meshCmd.MeshRequest.SubMeshes = static_cast<SubMesh*>(GetStagingPool().Acquire(meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh)));
                            std::memcpy(meshCmd.MeshRequest.SubMeshes, res.subMeshes.data(), meshCmd.MeshRequest.SubMeshCount * sizeof(SubMesh));
                        }

                        if (!m_AppContext->GRCommandRing.Push(meshCmd))
                        {
                            SM_WARN("GRCommandRing full, retrying mesh upload next frame (ticket %llu)", (unsigned long long)res.ticketId);
                            if (meshCmd.MeshRequest.Vertices) GetStagingPool().Return(meshCmd.MeshRequest.Vertices);
                            if (meshCmd.MeshRequest.Indices) GetStagingPool().Return(meshCmd.MeshRequest.Indices);
                            if (meshCmd.MeshRequest.SubMeshes) GetStagingPool().Return(meshCmd.MeshRequest.SubMeshes);
                            requeueAndStop(res, local);
                            break;
                        }
                        res.MeshUploaded = true; // mesh is on the ring; a retry must not re-upload it
                    }

                    if (!res.Texture)
                        continue; // No texture to upload

                    // Send material upload command
                    RendererCommand materialCmd{};
                    materialCmd.Type = RendererCommandType::RequestMaterial;
                    materialCmd.TicketId = res.ticketId; // Same ticket ID to associate with entity
                    materialCmd.MaterialRequest.Width = res.Width;
                    materialCmd.MaterialRequest.Height = res.Height;
                    materialCmd.MaterialRequest.Texture = res.Texture;

                    if (!m_AppContext->GRCommandRing.Push(materialCmd))
                    {
                        SM_WARN("GRCommandRing full, retrying material upload next frame (ticket %llu)", (unsigned long long)res.ticketId);
                        // Keep res.Texture (the only copy) for the retry; do NOT Return it here.
                        requeueAndStop(res, local);
                        break;
                    }
                }
```

- [ ] **Step 4: Fix the worker texture path (Acquire + overwrite-Return + overflow cast)**

In `src/editor/src/threading/GameThread.cpp`, in `WorkerThreadFunc`'s `processMesh`, replace the texture-loading block (currently lines 635-641, the `result.Width = width;` through the closing brace of `if (!pixels.empty()) { ... }`) with:

```cpp
                            result.Width = width;
                            result.Height = height;
                            if (result.Texture) { GetStagingPool().Return(result.Texture); result.Texture = nullptr; }
                            if (!pixels.empty()) {
                                const size_t texBytes = static_cast<size_t>(width) * height * sizeof(uint32_t);
                                result.Texture = static_cast<uint32_t*>(GetStagingPool().Acquire(texBytes));
                                std::memcpy(result.Texture, pixels.data(), texBytes);
                            }
```

- [ ] **Step 5: Include the pool header in `RenderThread.cpp` and return buffers instead of freeing**

In `src/editor/src/threading/RenderThread.cpp`, add the include alongside the other local headers near the top:

```cpp
#include "StagingBufferPool.h"
```

Replace the mesh free block (currently lines 111-114):

```cpp
                    // Free allocated memory from GameThread
                    if (cmd.MeshRequest.Vertices) std::free(cmd.MeshRequest.Vertices);
                    if (cmd.MeshRequest.Indices) std::free(cmd.MeshRequest.Indices);
                    if (cmd.MeshRequest.SubMeshes) std::free(cmd.MeshRequest.SubMeshes);
```

with:

```cpp
                    // Return staging buffers to the pool
                    if (cmd.MeshRequest.Vertices) GetStagingPool().Return(cmd.MeshRequest.Vertices);
                    if (cmd.MeshRequest.Indices) GetStagingPool().Return(cmd.MeshRequest.Indices);
                    if (cmd.MeshRequest.SubMeshes) GetStagingPool().Return(cmd.MeshRequest.SubMeshes);
```

Replace the texture free (currently lines 140-141):

```cpp
                    // Free allocated memory from GameThread
                    if (cmd.MaterialRequest.Texture) std::free(cmd.MaterialRequest.Texture);
```

with:

```cpp
                    // Return staging buffer to the pool
                    if (cmd.MaterialRequest.Texture) GetStagingPool().Return(cmd.MaterialRequest.Texture);
```

- [ ] **Step 6: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: PASS (clean compile/link). No `GAME_API_VERSION` bump and no ecs/game rebuild needed — the only header changed (`GameThread.h`) is editor-internal.

- [ ] **Step 7: Manual smoke (report honestly)**

This path is multi-threaded and load-time; it cannot be unit-tested here. Launch the editor (`./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`), let the `assets/models` load, and confirm models render and there is no crash / heap assert (this validates the Acquire/Return header-offset symmetry). If you cannot run the GUI in this environment, state that the build passed and the manual smoke is pending the user. Do not claim runtime success you did not observe.

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/threading/GameThread.h src/editor/src/threading/GameThread.cpp src/editor/src/threading/RenderThread.cpp
git commit -m "Recycle mesh/texture staging buffers via StagingBufferPool + fix upload-path leaks"
```

---

### Task 3: "Staging Pool" Memory-panel section

**Files:**
- Modify: `src/editor/src/rendering/imgui/MemoryPanel.cpp:9-12,85-91`

- [ ] **Step 1: Include the pool header**

In `src/editor/src/rendering/imgui/MemoryPanel.cpp`, add the include after `#include <ECS.h>` (line 9):

```cpp
#include "StagingBufferPool.h"
```

(`src/threading` is on the editor's include path, so the quoted include resolves.)

- [ ] **Step 2: Add the panel section**

In `src/editor/src/rendering/imgui/MemoryPanel.cpp`, immediately after the existing "Snapshot Pool" `CollapsingHeader` block (which currently ends at line 91 with its closing `}`), insert:

```cpp
    if (ImGui::CollapsingHeader("Staging Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        const StagingPoolStats s = GetStagingPoolStats();
        ImGui::Text("Free:       %zu", s.Free);
        ImGui::Text("In use:     %zu", s.InUse);
        ImGui::Text("Created:    %zu", s.Created);
        ImGui::Text("Reuses:     %llu", (unsigned long long)s.Reuses);
        ImGui::Text("Reserved:   %s", FormatBytes(s.ReservedBytes).c_str());
        ImGui::Text("Free bytes: %s", FormatBytes(s.FreeBytes).c_str());
    }
```

- [ ] **Step 3: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: PASS.

- [ ] **Step 4: Manual smoke (report honestly)**

Launch the editor, open the Memory window, expand "Staging Pool". On first model load expect `Created > 0` and `In use` returning to `0` after uploads complete; loading more models should grow `Reuses`. If you cannot run the GUI here, report build success and leave the visual smoke to the user. Do not claim observed runtime behavior you did not see.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/MemoryPanel.cpp
git commit -m "Add Staging Pool section to the Memory panel"
```

---

## Self-Review

**Spec coverage:**
- Pool (header-only, best-fit grow-to-fit, 16-byte capacity header, mutex, Meyers singleton, no InUse assert) → Task 1 Step 4. ✓
- `StagingPoolStats` + `GetStagingPoolStats()` → Task 1 Step 4. ✓
- Rewire 4 Acquire sites (mesh ×3, worker texture) → Task 2 Steps 3,4. ✓
- Rewire 7 Return sites (RenderThread ×4, GameThread retry ×3) → Task 2 Steps 3,5. ✓
- Bug 1 material ring-full leak → re-queue keeping texture (Task 2 Step 3). ✓
- Bug 2 worker overwrite → Return prior (Task 2 Step 4). ✓
- Bug 3 mid-drain drop → `requeueAndStop` requeues remaining `local` (Task 2 Step 3). ✓
- Bug 4 uint32 overflow → `static_cast<size_t>(width) * height` (Task 2 Step 4). ✓
- `MeshUploaded` flag → Task 2 Step 1, gated in Step 3. ✓
- Panel section → Task 3. ✓
- Tests → Task 1 Step 1 (T50–T55 cover alignment, reuse, grow, best-fit, capacity retention, stats). ✓
- CMake include dir for test_alloc → Task 1 Step 2. ✓
- Build/ABI: no GAME_API_VERSION bump, editor + tests only → noted in Task 2 Step 6. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/uncoded steps — every code step has complete code. ✓

**Type consistency:** `StagingBufferPool` / `StagingPoolStats` / `Acquire(size_t)` / `Return(void*)` / `Stats()` / `GetStagingPool()` / `GetStagingPoolStats()` and the field names `Free/InUse/Created/Reuses/ReservedBytes/FreeBytes` are used identically in the header (Task 1), the tests (Task 1), and the panel (Task 3). `MeshUploaded` defined in Task 2 Step 1 and used in Step 3. `requeueAndStop(ModelLoadResult&, std::queue<ModelLoadResult>&)` defined and called consistently in Task 2 Step 3. ✓
