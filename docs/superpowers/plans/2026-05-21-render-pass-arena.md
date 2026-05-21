# Render-Pass Arena Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route the editor render passes' per-frame heap allocations through the existing `FrameAllocator` bump arena, replacing the MeshRenderPass batching `unordered_map` with a flat array + sort, and stop `GetMeshResources` copying its sub-mesh list every batch.

**Architecture:** Single-threaded RenderThread, transient-per-frame allocations reset once per frame. A new self-contained header `MeshBatching.h` holds a pure `BuildBatchRuns` helper (sort entries, emit contiguous equal-key runs) that is unit-tested without any GPU/ECS dependency. Pass edits are verified by building the editor and a user smoke test.

**Tech Stack:** C++23, CMake (`msvc-win64-vs2026-community` preset — enterprise is NOT installed), NVRHI render passes, the `Engine::ArenaAllocator`/`FrameAllocator` toolkit, the custom `test_alloc` harness.

**Spec:** `docs/superpowers/specs/2026-05-21-render-pass-arena-design.md`

**Model guidance:** Per project convention for this work, dispatch **all** implementer and reviewer subagents on **Opus 4.7**. Tasks 1 (batching helper — must group identically) and 3 (MeshRenderPass rewrite — must preserve identical draw output) and 2 (span lifetime) are the higher-risk ones.

**Branch:** Already on `allocator-toolkit` (this refactor depends on the unmerged toolkit and stacks on it). Do not switch branches.

---

## File Structure

- **Create** `src/editor/src/rendering/passes/MeshBatching.h` — self-contained `BatchEntry`/`BatchRun` POD types + `BuildBatchRuns` (sort + run extraction). No nvrhi/ECS/editor includes (only `<cstdint>`, `<algorithm>`), so it is unit-testable in `test_alloc`.
- **Modify** `tests/test_alloc.cpp` — unit tests for `BuildBatchRuns`.
- **Modify** `tests/CMakeLists.txt` — add the `passes` dir to `test_alloc`'s include path.
- **Modify** `src/editor/src/rendering/MeshSystem.h` — `MeshResources::subMeshes` becomes `std::span<const SubMesh>`.
- **Modify** `src/editor/src/rendering/MeshSystem.cpp` — span assignment + lifetime comment.
- **Modify** `src/editor/src/rendering/passes/MeshRenderPass.cpp` — lights + batching + instances onto the arena.
- **Modify** `src/editor/src/rendering/passes/UiRenderPass.cpp` — `usedFontSizes` onto the arena.

## Conventions for every task

- Build preset: `msvc-win64-vs2026-community`. Configure (after CMake changes): `cmake --preset msvc-win64-vs2026-community`. Build a target: `cmake --build --preset msvc-win64-vs2026-community --target <name>`.
- Unit-test exe: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe` (expected final line `All allocator tests passed.`).
- Use the Bash tool for cmake/git. Author identity is personal `Nuno Silva <nuno.levezinho@live.com.pt>` (already configured). Never stage the untracked `.claude/` directory.
- The `test_alloc` run prints a stray colored `ERROR` line from an earlier arena-overflow negative test (pre-existing `lib.h` `SM_ERROR` quirk) — ignore it; only the final pass line + exit 0 matter.

---

### Task 1: BuildBatchRuns helper + unit tests

Pure, GPU-free batching core. TDD: tests first.

**Files:**
- Create: `src/editor/src/rendering/passes/MeshBatching.h`
- Modify: `tests/test_alloc.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add the passes include dir to test_alloc**

In `tests/CMakeLists.txt`, the `test_alloc` target currently is:

```cmake
add_executable(test_alloc
    test_alloc.cpp
)

target_link_libraries(test_alloc PRIVATE
    CommonHeaders
    Engine
)

set_target_properties(test_alloc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

Add an include-directories line after `target_link_libraries`:

```cmake
target_include_directories(test_alloc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/passes
)
```

- [ ] **Step 2: Write the failing tests**

In `tests/test_alloc.cpp`, add near the other includes:

```cpp
#include <MeshBatching.h>
```

Add these tests before `main`:

```cpp
static void T40_batchruns_empty()
{
    BatchRun runs[1];
    EXPECT_EQ(BuildBatchRuns(nullptr, 0, runs, 1), (uint32_t)0);
    BatchEntry e[1] = {};
    EXPECT_EQ(BuildBatchRuns(e, 0, runs, 1), (uint32_t)0);
}

static void T41_batchruns_single()
{
    BatchEntry e[1] = { {5, 7, 100} };
    BatchRun runs[1];
    EXPECT_EQ(BuildBatchRuns(e, 1, runs, 1), (uint32_t)1);
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[0].count, (uint32_t)1);
}

static void T42_batchruns_all_same_key()
{
    BatchEntry e[3] = { {1,2,10}, {1,2,11}, {1,2,12} };
    BatchRun runs[3];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 3), (uint32_t)1);
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[0].count, (uint32_t)3);
}

static void T43_batchruns_all_distinct()
{
    BatchEntry e[3] = { {1,0,10}, {2,0,11}, {3,0,12} };
    BatchRun runs[3];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 3), (uint32_t)3);
    EXPECT_EQ(runs[0].count, (uint32_t)1);
    EXPECT_EQ(runs[1].count, (uint32_t)1);
    EXPECT_EQ(runs[2].count, (uint32_t)1);
}

static void T44_batchruns_mixed_unsorted()
{
    // keys before sort: (2,0)(1,0)(2,0)(1,1)(1,0)
    // after sort:       (1,0)(1,0)(1,1)(2,0)(2,0) -> runs [0,2],[2,1],[3,2]
    BatchEntry e[5] = { {2,0,10}, {1,0,11}, {2,0,12}, {1,1,13}, {1,0,14} };
    BatchRun runs[5];
    uint32_t n = BuildBatchRuns(e, 5, runs, 5);
    EXPECT_EQ(n, (uint32_t)3);

    // Runs are contiguous and cover all entries.
    EXPECT_EQ(runs[0].begin, (uint32_t)0);
    EXPECT_EQ(runs[1].begin, runs[0].begin + runs[0].count);
    EXPECT_EQ(runs[2].begin, runs[1].begin + runs[1].count);
    uint32_t covered = 0;
    for (uint32_t r = 0; r < n; ++r) {
        covered += runs[r].count;
        const uint32_t b = runs[r].begin;
        for (uint32_t i = 1; i < runs[r].count; ++i) {
            EXPECT(e[b + i].meshId == e[b].meshId);
            EXPECT(e[b + i].materialId == e[b].materialId);
        }
    }
    EXPECT_EQ(covered, (uint32_t)5);
}

static void T45_batchruns_maxruns_cap()
{
    BatchEntry e[3] = { {1,0,10}, {2,0,11}, {3,0,12} };
    BatchRun runs[2];
    EXPECT_EQ(BuildBatchRuns(e, 3, runs, 2), (uint32_t)2); // stops at maxRuns
}
```

Register them in `main` after the existing test calls:

```cpp
    T40_batchruns_empty();
    T41_batchruns_single();
    T42_batchruns_all_same_key();
    T43_batchruns_all_distinct();
    T44_batchruns_mixed_unsorted();
    T45_batchruns_maxruns_cap();
```

- [ ] **Step 3: Run to verify failure**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_alloc
```
Expected: compile error — `MeshBatching.h` not found / `BuildBatchRuns` undefined.

- [ ] **Step 4: Implement MeshBatching.h**

Create `src/editor/src/rendering/passes/MeshBatching.h`:

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
// Stops early if the run count would exceed `maxRuns`.
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

- [ ] **Step 5: Run to verify pass**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: `All allocator tests passed.`

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/passes/MeshBatching.h tests/test_alloc.cpp tests/CMakeLists.txt
git commit -m "feat(render): BuildBatchRuns batching helper + unit tests"
```

---

### Task 2: GetMeshResources returns a span (no per-frame copy)

**Files:**
- Modify: `src/editor/src/rendering/MeshSystem.h`
- Modify: `src/editor/src/rendering/MeshSystem.cpp`

- [ ] **Step 1: Change MeshResources::subMeshes to a span**

In `src/editor/src/rendering/MeshSystem.h`, add the include near the top (with the other standard includes):

```cpp
#include <span>
```

In the `MeshResources` struct, change:

```cpp
        std::vector<SubMesh> subMeshes;
```

to:

```cpp
        std::span<const SubMesh> subMeshes; // non-owning view into the MeshEntry's vector
```

(Leave the `MeshEntry::subMeshes` member — the owning `std::vector<SubMesh>` — unchanged.)

- [ ] **Step 2: Assign a span in GetMeshResources + document lifetime**

In `src/editor/src/rendering/MeshSystem.cpp`, in `GetMeshResources`, change:

```cpp
    resources.subMeshes = entry.subMeshes;
```

to:

```cpp
    // Non-owning view into the entry's vector. Valid only while m_Meshes is not
    // mutated; mesh adds are drained before render passes run, so the span is
    // valid for the duration of a frame's Render calls.
    resources.subMeshes = std::span<const SubMesh>(entry.subMeshes);
```

- [ ] **Step 3: Verify no other consumer relies on an owning subMeshes**

Grep the editor for uses of `GetMeshResources` and `.subMeshes` to confirm the only consumer is `MeshRenderPass` (which only calls `.size()` and range-iterates — both valid on a span). If any other consumer stores or outlives the span, STOP and report it before continuing.

Run: search `src/editor` for `GetMeshResources` and for `.subMeshes`.

- [ ] **Step 4: Build the editor to verify it compiles**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds and links cleanly. (`MeshRenderPass.cpp` still compiles because `.size()` and range-`for` work on `std::span`.)

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/MeshSystem.h src/editor/src/rendering/MeshSystem.cpp
git commit -m "perf(render): GetMeshResources returns a span instead of copying subMeshes"
```

---

### Task 3: MeshRenderPass — lights, batching, instances onto the arena

Rewrites the `if (world)` body of `MeshRenderPass::Render` to use the `frameAllocator` (currently ignored) and `BuildBatchRuns`. Draw output must be identical.

**Files:**
- Modify: `src/editor/src/rendering/passes/MeshRenderPass.cpp`

- [ ] **Step 1: Include the batching helper**

In `src/editor/src/rendering/passes/MeshRenderPass.cpp`, add near the top includes:

```cpp
#include "MeshBatching.h"
```

- [ ] **Step 2: Use the frameAllocator parameter**

Change the signature line so the parameter is named (it is currently commented out):

```cpp
                            FrameAllocator* /*frameAllocator*/)
```

to:

```cpp
                            FrameAllocator* frameAllocator)
```

- [ ] **Step 3: Replace the point-light gathering with an arena array**

Replace:

```cpp
        // Collect point lights into a temporary CPU array (capped to m_MaxPointLights)
        std::vector<PointLightCPU> pointLights;
        pointLights.reserve(16);

        for (EntityId entity : world->View<TransformComponent, LightningComponent>()) {
            const auto* transform = world->GetComponent<TransformComponent>(entity);
            const auto* lightning = world->GetComponent<LightningComponent>(entity);
            if (!transform || !lightning) continue;

            if (lightning->Type == LightningType::Directional)
            {
                lightningDirection = lightning->Direction;
                lightningColor = lightning->Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning->Type == LightningType::Point)
            {
                if (pointLights.size() < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform->Position, 1.0f);
                    pl.Color = lightning->Color;
                    pl.Intensity = lightning->Intensity;
                    pl.Range = lightning->Range;
                    pointLights.push_back(pl);
                }
            }
        }
```

with:

```cpp
        // Collect point lights into an arena array (capped to m_MaxPointLights)
        auto* pointLights = frameAllocator->AllocateArray<PointLightCPU>(m_MaxPointLights);
        uint32_t pointLightCount = 0;

        for (EntityId entity : world->View<TransformComponent, LightningComponent>()) {
            const auto* transform = world->GetComponent<TransformComponent>(entity);
            const auto* lightning = world->GetComponent<LightningComponent>(entity);
            if (!transform || !lightning) continue;

            if (lightning->Type == LightningType::Directional)
            {
                lightningDirection = lightning->Direction;
                lightningColor = lightning->Color;
                // keep scanning for points in case; do not break
            }
            else if (lightning->Type == LightningType::Point)
            {
                if (pointLights && pointLightCount < m_MaxPointLights)
                {
                    PointLightCPU pl{};
                    pl.Position = glm::vec4(transform->Position, 1.0f);
                    pl.Color = lightning->Color;
                    pl.Intensity = lightning->Intensity;
                    pl.Range = lightning->Range;
                    pointLights[pointLightCount++] = pl;
                }
            }
        }
```

- [ ] **Step 4: Update the per-frame CB + point-light upload to use the count**

Replace:

```cpp
        perFrame.PointLightCount = static_cast<uint32_t>(pointLights.size());
        perFrame.Ambient = 0.1f; // hardcoded ambient for now
        commandList->writeBuffer(m_PerFrameCB, &perFrame, sizeof(perFrame));

        // Upload point lights data (if any)
        if (m_PointLightBuffer && !pointLights.empty())
        {
            commandList->writeBuffer(m_PointLightBuffer, pointLights.data(), static_cast<uint32_t>(pointLights.size() * sizeof(PointLightCPU)));
        }
```

with:

```cpp
        perFrame.PointLightCount = pointLightCount;
        perFrame.Ambient = 0.1f; // hardcoded ambient for now
        commandList->writeBuffer(m_PerFrameCB, &perFrame, sizeof(perFrame));

        // Upload point lights data (if any)
        if (m_PointLightBuffer && pointLightCount > 0)
        {
            commandList->writeBuffer(m_PointLightBuffer, pointLights, pointLightCount * sizeof(PointLightCPU));
        }
```

- [ ] **Step 5: Replace the batching map + draw loop with the flat array + runs**

Replace the entire block from the `// Group entities by (MeshId, MaterialId)` comment through the end of the `for (const auto& [batchKey, entities] : batches)` loop (i.e. the original lines defining `struct BatchKey`/`struct BatchKeyHash`, the `std::unordered_map ... batches` build loop, and the whole `for (const auto& [batchKey, entities] : batches)` draw loop) with:

```cpp
        // Group entities by (MeshId, MaterialId) into a flat arena array, then
        // sort into contiguous runs (one run == one draw batch). No node-based map.
        auto meshEnts = world->View<TransformComponent, MeshComponent>();
        auto* entries = frameAllocator->AllocateArray<BatchEntry>(meshEnts.size());
        uint32_t entryCount = 0;
        if (entries)
        {
            for (EntityId entity : meshEnts)
            {
                const auto* meshComp = world->GetComponent<MeshComponent>(entity);
                if (!meshComp || !meshComp->Visible)
                    continue;

                const auto* materialComp = world->GetComponent<MaterialComponent>(entity);
                uint32_t materialId = materialComp ? materialComp->MaterialId : MaterialSystem::MissingMaterial;

                entries[entryCount++] = BatchEntry{ meshComp->MeshId, materialId, entity };
            }
        }

        BatchRun* runs = (entryCount > 0) ? frameAllocator->AllocateArray<BatchRun>(entryCount) : nullptr;
        uint32_t runCount = (entries && runs) ? BuildBatchRuns(entries, entryCount, runs, entryCount) : 0;

        // Render each batch (run) with instancing
        nvrhi::GraphicsState state;
        state.pipeline = m_Pipeline;
        state.framebuffer = frameBuffer;
        state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());

        for (uint32_t r = 0; r < runCount; ++r)
        {
            const BatchRun& run = runs[r];
            const BatchEntry& head = entries[run.begin];

            // Query systems for GPU resources
            auto meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(head.meshId);
            if (!meshResources.valid)
            {
                SM_WARN("MeshRenderPass: Invalid mesh ID %u", head.meshId);
                meshResources = m_Renderer->GetMeshSystem()->GetMeshResources(MeshSystem::MissingMesh);
            }

            const uint32_t instanceCount = std::min(run.count, m_MaxInstances);

            // Build instance data into an arena array
            auto* instances = frameAllocator->AllocateArray<MeshInstanceCPU>(instanceCount);
            if (!instances)
                continue;
            uint32_t instanceOut = 0;

            for (uint32_t i = 0; i < instanceCount; ++i)
            {
                EntityId entity = entries[run.begin + i].entity;
                const auto* transform = world->GetComponent<TransformComponent>(entity);
                const auto* material = world->GetComponent<MaterialComponent>(entity);

                if (!transform)
                    continue;

                // Build world transform: T * Rz * Ry * Rx * S
                glm::mat4 T = glm::translate(glm::mat4(1.0f), transform->Position);
                glm::mat4 Rx = glm::rotate(glm::mat4(1.0f), transform->Rotation.x, glm::vec3(1.f, 0.f, 0.f));
                glm::mat4 Ry = glm::rotate(glm::mat4(1.0f), transform->Rotation.y, glm::vec3(0.f, 1.f, 0.f));
                glm::mat4 Rz = glm::rotate(glm::mat4(1.0f), transform->Rotation.z, glm::vec3(0.f, 0.f, 1.f));
                glm::mat4 S = glm::scale(glm::mat4(1.0f), transform->Scale);
                glm::mat4 M = T * Rz * Ry * Rx * S;

                glm::mat3 M3(M);
                glm::mat3 N3 = glm::transpose(glm::inverse(M3));

                // Material properties
                glm::vec4 baseColor = material ? material->BaseColor : glm::vec4(1.0f);
                uint32_t flags = (material && (material->Flags & 1u)) ? 1u : 0u;

                MeshInstanceCPU inst{};
                inst.Model = M;
                inst.NormalMatrix = glm::mat4(N3);
                inst.BaseColor = baseColor;
                inst.Flags = flags;

                instances[instanceOut++] = inst;
            }

            if (instanceOut == 0)
                continue;

            // Upload instance data
            commandList->writeBuffer(m_InstanceBuffer, instances, instanceOut * sizeof(MeshInstanceCPU));

            if (meshResources.subMeshes.size() > 0) {

                nvrhi::DrawArguments args{};
                for (const auto &subMesh: meshResources.subMeshes)
                {
                    auto materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(subMesh.MaterialIndex);
                    if (!materialResources.valid)
                    {
                        SM_WARN("MeshRenderPass: Invalid material ID %u", head.materialId);
                        materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(MaterialSystem::MissingMaterial);
                    }

                    // Create binding set dynamically for this batch
                    nvrhi::BindingSetDesc bindingDesc;
                    bindingDesc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameCB),
                        nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
                        nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                        nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer)
                    };
                    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                    // Set state and draw ALL instances in one call
                    state.bindings = { bindingSet };
                    state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
                    state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);

                    commandList->setGraphicsState(state);

                    args.vertexCount = subMesh.IndexCount;
                    args.instanceCount = instanceOut;
                    args.startIndexLocation = subMesh.IndexStart;
                    args.startVertexLocation = 0;
                    commandList->drawIndexed(args);
                }

            } else {

                auto materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(head.materialId);
                if (!materialResources.valid)
                {
                    SM_WARN("MeshRenderPass: Invalid material ID %u", head.materialId);
                    materialResources = m_Renderer->GetMaterialSystem()->GetMaterialResources(MaterialSystem::MissingMaterial);
                }

                // Create binding set dynamically for this batch
                nvrhi::BindingSetDesc bindingDesc;
                bindingDesc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, m_PerFrameCB),
                    nvrhi::BindingSetItem::ConstantBuffer(1, m_PerDrawCB),
                    nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                    nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_PointLightBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer)
                };
                nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

                // Set state and draw ALL instances in one call
                state.bindings = { bindingSet };
                state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
                state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);

                commandList->setGraphicsState(state);

                nvrhi::DrawArguments args{};
                args.vertexCount = meshResources.indexCount;
                args.instanceCount = instanceOut;
                args.startIndexLocation = 0;
                args.startVertexLocation = 0;
                commandList->drawIndexed(args);
            }
        }
```

Note the changes vs. the original draw loop: iterate `runs` instead of map entries; `head` (the run's first entry) supplies `meshId`/`materialId`; `instances`/`instanceOut` replace the `std::vector` `instances.data()`/`instances.size()`; `args.instanceCount = instanceOut`. Everything else (binding sets, submesh loop, draw args) is unchanged.

- [ ] **Step 6: Build the editor to verify it compiles + links**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds and links cleanly.

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/rendering/passes/MeshRenderPass.cpp
git commit -m "perf(render): MeshRenderPass per-frame allocations onto the frame arena"
```

---

### Task 4: UiRenderPass usedFontSizes onto the arena

**Files:**
- Modify: `src/editor/src/rendering/passes/UiRenderPass.cpp`

- [ ] **Step 1: Replace the usedFontSizes vector with an arena array**

In `UiRenderPass::Render`, replace:

```cpp
        // 1) Gather unique font sizes used by text entities
        std::vector<size_t> usedFontSizes;
        for (EntityId entity : world->View<TransformComponent, TextComponent>()) {
            const auto* text = world->GetComponent<TextComponent>(entity);
            if (text) {
                const size_t fontSize = text->FontSize;
                bool found = false;
                for (size_t fs : usedFontSizes) {
                    if (fs == fontSize) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    usedFontSizes.push_back(fontSize);
                }
            }
        }

        // 2) For each unique font size, render all text using that atlas
        for (size_t fontSize : usedFontSizes) {
```

with:

```cpp
        // 1) Gather unique font sizes used by text entities (arena-backed dedup)
        auto textEnts = world->View<TransformComponent, TextComponent>();
        auto* usedFontSizes = frameAllocator->AllocateArray<size_t>(textEnts.size());
        uint32_t fontSizeCount = 0;
        if (usedFontSizes) {
            for (EntityId entity : textEnts) {
                const auto* text = world->GetComponent<TextComponent>(entity);
                if (text) {
                    const size_t fontSize = text->FontSize;
                    bool found = false;
                    for (uint32_t k = 0; k < fontSizeCount; ++k) {
                        if (usedFontSizes[k] == fontSize) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        usedFontSizes[fontSizeCount++] = fontSize;
                    }
                }
            }
        }

        // 2) For each unique font size, render all text using that atlas
        for (uint32_t fsIdx = 0; fsIdx < fontSizeCount; ++fsIdx) {
            const size_t fontSize = usedFontSizes[fsIdx];
```

(The body of the loop — atlas lookup, glyph counting, the existing `frameAllocator->AllocateArray<UIInstanceCPU>` glyph allocation, and instance generation — is unchanged. Only the loop header and the `usedFontSizes` collection change. Ensure the loop's closing brace still matches.)

- [ ] **Step 2: Build the editor to verify it compiles + links**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: builds and links cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/rendering/passes/UiRenderPass.cpp
git commit -m "perf(render): UiRenderPass usedFontSizes onto the frame arena"
```

---

### Task 5: Full verification

**Files:** none (verification only).

- [ ] **Step 1: Build the feature targets + run unit tests**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_alloc
cmake --build --preset msvc-win64-vs2026-community --target editor
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: both build clean; `test_alloc.exe` prints `All allocator tests passed.` (ignore the stray pre-existing arena-overflow ERROR line). (Note: a full `cmake --build` of ALL targets will fail in the unrelated legacy `runtime` target — `windows_platform.cpp: 'm_Input' is not a member of 'PlatformContext'` — which is pre-existing and untouched by this work. Build the `editor` and `test_alloc` targets specifically, not the whole solution.)

- [ ] **Step 2: Editor smoke test (user-driven)**

Launch:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe
```
Confirm:
- The scene renders identically to before — same meshes, same instancing, same text (the flat+sort batching must produce the same draws).
- No crash.
- The Memory panel shows the `Frame` allocator's `Used`/`Peak` (now higher, since lights/batches/instances/glyphs all flow through it) rising during frames and resetting.

This requires the running UI; report explicitly that it must be smoke-tested rather than claiming success from a compile.

---

## Self-Review

**1. Spec coverage:**
- MeshRenderPass point lights → arena: Task 3 Step 3-4. ✓
- MeshRenderPass batching map → flat array + sort (BuildBatchRuns): Task 1 (helper+tests) + Task 3 Step 5. ✓
- MeshRenderPass per-batch instances → arena: Task 3 Step 5. ✓
- GetMeshResources subMeshes → span (zero-copy) + lifetime comment: Task 2. ✓
- UiRenderPass usedFontSizes → arena: Task 4. ✓
- Null-guards on every new arena site: Task 3 (pointLights, entries, runs, instances) + Task 4 (usedFontSizes). ✓
- BuildBatchRuns unit-tested in test_alloc, passes include dir added: Task 1. ✓
- Build via community preset; editor build + smoke verification: Tasks 2-5. ✓
- Out of scope (ECS View vectors, cross-thread paths) correctly untouched. ✓

**2. Placeholder scan:** No TBD/TODO/"handle edge cases". Every code step shows full code. The MeshRenderPass rewrite (Task 3 Step 5) gives the complete replacement block, not a sketch.

**3. Type consistency:** `BatchEntry{ meshId(uint32_t), materialId(uint32_t), entity(uint64_t) }` and `BatchRun{ begin(uint32_t), count(uint32_t) }` defined in Task 1 and used identically in Task 3. `BuildBatchRuns(BatchEntry*, uint32_t, BatchRun*, uint32_t)` signature matches between Task 1 def and Task 3 call. `MeshResources::subMeshes` as `std::span<const SubMesh>` (Task 2) is consumed via `.size()` + range-`for` in Task 3 (valid on span). `pointLightCount`/`instanceOut`/`entryCount`/`runCount`/`fontSizeCount` are all `uint32_t`. `frameAllocator->AllocateArray<T>(n)` matches the ArenaAllocator API (returns `T*`, null on overflow/zero).

**Note for executor:** the EntityId type is `uint64_t` (from ECS.h); `BatchEntry::entity` is `uint64_t` and is assigned from/compared against `EntityId` freely. `m_MaxPointLights` and `m_MaxInstances` are existing `MeshRenderPass` members (uint32_t). Do not introduce new members.
