# Animation SP2 — Static bind-pose GPU skinning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A rigged glTF entity deforms via GPU skeletal skinning in the deferred G-buffer, driven by a per-entity bone-matrix palette (bind-pose = identity baseline + a debug bone-wiggle for visible proof).

**Architecture:** Six tasks: (1) pure `Skinning.h` — `SkinnedVertex` + top-4 weight reduction + palette formula + unit tests; (2) MeshSystem parallel bone buffer (`isSkinned`/`boneBuffer`) + ring/Renderer plumbing; (3) extract per-vertex weights in `WorkerThreadFunc` + thread bone data to upload; (4) `PaletteFrame` transport + GameThread skinning step + `SkinTest` flag; (5) skinned PSO + skinning VS + palette SB upload + skinned draw path in GBufferFillPass; (6) regression + manual smoke. No new ECS component, no `ECS.h` change, no `GAME_API_VERSION` bump.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), NVRHI (DX12/VK), DXC (inline-HLSL string shaders, no `-D`), assimp, glm, atomic-`shared_ptr` cross-thread publish.

**Scope:** Implements `docs/superpowers/specs/2026-06-08-anim-skinning-design.md` (SP2 of the animation effort). Touches `ApplicationContext.h` + rendering + GameThread ⇒ rebuild **Engine + editor + game**; `ECS.h` untouched.

> **Branch:** Work happens on `feat/anim-skinning` (already created off `main`). Stay on it.

---

## Background facts (verified — current code; locate edits by quoted code, line numbers approximate)

- `MeshVertex { float px,py,pz, nx,ny,nz, u,v }` (`ApplicationContext.h:67`), 32 bytes — **unchanged** by SP2.
- GBuffer VS/PS are inline HLSL strings `GBUF_VS_HLSL` (`GBufferFillPass.cpp:16-29`) / `GBUF_PS_HLSL` (`:31-50`); compiled via `m_Renderer->CreateShader(type, src, len, entry, target)` (`Renderer.cpp:421-433`, e.g. `"main_vs","vs_6_1"`). No `-D`/permutation.
- HLSL `struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4 BaseColor; uint Flags; uint3 _pad; }`; C++ mirror `MeshInstanceCPU { glm::mat4 Model; glm::mat4 NormalMatrix; glm::vec4 BaseColor; uint32_t Flags; uint32_t _pad[3]; }` (`GBufferFillPass.h:22-26`, `static_assert(...%16==0)`). Instances ride a `StructuredBuffer<InstanceData> @ t5`, indexed by `SV_InstanceID`. `b0` PerFrame CB = `{float4x4 uVP}`.
- Static input layout: 3 `nvrhi::VertexAttributeDesc` (POSITION RGB32F, NORMAL RGB32F, TEXCOORD RG32F), all `bufferIndex(0)`, `stride sizeof(MeshVertex)`, `createInputLayout(attrs,3,m_VS)` (`GBufferFillPass.cpp:65-73`).
- Binding layout: `ConstantBuffer(0), Texture_SRV(2), Sampler(3), StructuredBuffer_SRV(5)` + Vulkan offsets (`:75-92`).
- Pipeline + wireframe created lazily (`:129-153`); members `m_Pipeline/m_WireframePipeline/m_InputLayout/m_BindingLayout/m_FrameCB/m_InstanceBuffer/m_MaxInstances=4096` (`GBufferFillPass.h:20-38`).
- Draw loop (`:230-360`): gather `Each<TransformComponent, MeshComponent>` → `BatchEntry{meshId,materialId,entity}`; per run, fill `MeshInstanceCPU[]`, `writeBuffer(m_InstanceBuffer,...)`, per-submesh `BindingSetDesc` (`ConstantBuffer(0,m_FrameCB), Texture_SRV(2), Sampler(3), StructuredBuffer_SRV(5,m_InstanceBuffer)`), `setVertexBuffer({(vb,0,0)})`, `drawIndexed(instanceCount=instanceOut)`.
- `MeshEntry` (`MeshSystem.h:87-100`): key, vertex/index buffers, subMeshes, counts, bounds, `cpuVertices/cpuIndices`. `AddMesh(std::string key, const MeshVertex*, vcount, const uint32_t*, icount, SubMesh*, scount)` (`MeshSystem.cpp:68-161`) — de-dups by `AssetKeyHash(key)`, builds VB/IB, retains CPU copies, `m_SlotByHandle[handle]=slot`, returns `MeshHandle{handle}`. `MeshResources {vertexBuffer,indexBuffer,subMeshes,vertexCount,indexCount,valid}` (`:40-47`). `GetMeshResources(uint64)` resolves via `SlotForHandle` (`:190-203`). `RecreateGpuResources` replays `cpuVertices/cpuIndices` (`:280-344`).
- `Renderer::AddMesh` forwards to `m_MeshSystem.AddMesh(std::move(key),...)` (`Renderer.cpp:451-454`). `Renderer::CreateShader` (`:421-433`).
- `MeshRequest` (ring, `ApplicationContext.h:93-101`): `MeshVertex* Vertices; size_t VertexCount; uint32_t* Indices; size_t IndexCount; SubMesh* SubMeshes; size_t SubMeshCount; char Key[256];`.
- `RenderThread.cpp` RequestMesh case (`:112-136`) calls `m_Renderer->AddMesh(...)` then `GetStagingPool().Return(...)`. Per-frame loads `LatestWorldSnapshot.load(acquire)` then `LatestSnapshot.load()` (`:184-190`), calls `m_Renderer->Render(renderDelta, r,g,b, nextSnap, worldSnapshot.get())` (`:221`).
- `ApplicationContext.h:229-237`: `Seqlock<SimulationSnapshot> LatestSnapshot; std::atomic<std::shared_ptr<const ECS>> LatestWorldSnapshot;`.
- `GameThread::WorkerThreadFunc`: `processMesh` lambda builds verts/indices (`:768-840`); SP1 skeleton-extraction block after `processNode(...)` (`:866-906`) builds `boneInverseBind` (name→`AiToGlm(mOffsetMatrix)`) + DFS `walk` → `Skeleton` (`AiToGlm` is a file-local helper). `PublishSnapshot` (`:667-680`) does `state.World.CreateSnapshot()` + `LatestWorldSnapshot.store(...)`, called from `RunLoop:561`. Completed-jobs drain (`:359-421`) fills `MeshRequest` (Key/VertexCount/Indices via staging-pool `Acquire`+`memcpy`) and registers the skeleton.
- `ModelLoadResult` (`GameThread.h:43-60`) already has `skeleton/hasSkeleton/skeletonKey` (SP1).
- `DebugDrawSettings` (`RenderStats.h:17-29`) — flags incl. `ShowSkeleton`; `ENGINE_API GetDebugDrawSettings()`. RenderStatsPanel checkboxes (`RenderStatsPanel.cpp:24-43`).
- `Skeleton.h`: `Bone {name, int parent, glm::mat4 localBind, glm::mat4 inverseBind}`, `Skeleton {vector<Bone> bones}`, `ComputeBindPoseGlobals(sk)` (`global[b]=parent<0?localBind:global[parent]*localBind`).
- Engine CMake source list has an `# Animation` block (`src/animation/SkeletonStore.cpp`). `tests/CMakeLists.txt` `test_skeleton` block (links `CommonHeaders glm::glm ecs`, includes `src/common/include`, GLM defines, `RUNTIME_OUTPUT_DIRECTORY/FOLDER Tests`).

## Type/symbol contract (keep exact)

- `src/common/include/Skinning.h`: `struct SkinnedVertex { glm::uvec4 BoneIndices; glm::vec4 BoneWeights; };`, `SkinnedVertex MakeSkinnedVertex(const std::vector<std::pair<uint32_t,float>>& influences);`, `std::vector<glm::mat4> ComputeSkinningPalette(const Skeleton& sk, const std::vector<glm::mat4>& globals);`.
- `src/common/include/PaletteFrame.h`: `struct PaletteFrame { std::vector<glm::mat4> matrices; struct Range { EntityId entity; uint32_t offset; uint32_t count; }; std::vector<Range> ranges; };`.
- `ApplicationContext.h`: `std::atomic<std::shared_ptr<const PaletteFrame>> LatestPaletteFrame;` + `MeshRequest.BoneData` (`SkinnedVertex*`).
- `MeshEntry`: `+ bool isSkinned; nvrhi::BufferHandle boneBuffer; std::vector<SkinnedVertex> cpuSkinning;`. `MeshResources`: `+ bool isSkinned; nvrhi::IBuffer* boneBuffer;`. `AddMesh(..., const SkinnedVertex* boneData = nullptr)`.
- `MeshInstanceCPU`/HLSL `InstanceData`: `+ uint32_t PaletteOffset` (replace one `_pad` slot).
- `DebugDrawSettings`: `+ bool SkinTest = false;`.

---

### Task 1: `Skinning.h` — types + pure math + unit tests (TDD)

**Files:** Create `src/common/include/Skinning.h`; Test `tests/test_skinning.cpp` + `tests/CMakeLists.txt`.

- [ ] **Step 1: Write the failing test (`tests/test_skinning.cpp`)**
```cpp
#include <cstdio>
#include <cmath>
#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Skinning.h"

static int g_Failures = 0;
#define EXPECT(cond) do { if(!(cond)){ std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond); ++g_Failures; } } while(0)
static bool nearf(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void T_make_skinned_vertex() {
    // >4 influences: keep top-4 by weight, renormalized to sum 1.
    std::vector<std::pair<uint32_t,float>> inf = {{0,0.05f},{1,0.40f},{2,0.30f},{3,0.20f},{4,0.05f}};
    const SkinnedVertex v = MakeSkinnedVertex(inf);
    float sum = v.BoneWeights.x + v.BoneWeights.y + v.BoneWeights.z + v.BoneWeights.w;
    EXPECT(nearf(sum, 1.0f));
    // The two dropped (weight 0.05) are bones 0 and 4; kept set must be {1,2,3} + the larger 0.05.
    // Simplest robust check: bone 4 OR bone 0 dropped, and the top weight (bone 1) is present.
    bool has1 = (v.BoneIndices.x==1u||v.BoneIndices.y==1u||v.BoneIndices.z==1u||v.BoneIndices.w==1u);
    EXPECT(has1);

    // Unweighted vertex -> bone 0, weights (1,0,0,0).
    const SkinnedVertex u = MakeSkinnedVertex({});
    EXPECT(u.BoneIndices.x==0u);
    EXPECT(nearf(u.BoneWeights.x,1.0f) && nearf(u.BoneWeights.y,0.0f) && nearf(u.BoneWeights.z,0.0f) && nearf(u.BoneWeights.w,0.0f));
}

static void T_bind_pose_palette_identity() {
    // Build a 2-bone skeleton; set inverseBind = inverse(globalBind) so palette = global*inverseBind = I.
    Skeleton sk;
    Bone root;  root.name="root";  root.parent=-1; root.localBind = glm::translate(glm::mat4(1.0f), glm::vec3(1,0,0));
    Bone child; child.name="child"; child.parent=0;  child.localBind = glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0,0,1));
    sk.bones = { root, child };
    const auto globals = ComputeBindPoseGlobals(sk);
    sk.bones[0].inverseBind = glm::inverse(globals[0]);
    sk.bones[1].inverseBind = glm::inverse(globals[1]);

    const auto palette = ComputeSkinningPalette(sk, globals);
    EXPECT(palette.size() == 2);
    for (int b = 0; b < 2; ++b)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                EXPECT(nearf(palette[b][c][r], (c==r) ? 1.0f : 0.0f)); // ~identity
}

int main() {
    T_make_skinned_vertex();
    T_bind_pose_palette_identity();
    if (g_Failures == 0) std::printf("All skinning tests passed.\n");
    return g_Failures ? 1 : 0;
}
```
Register `test_skinning` in `tests/CMakeLists.txt` — copy the `test_skeleton` block verbatim, substituting `test_skinning`/`test_skinning.cpp` (same `CommonHeaders glm::glm ecs` libs, same include dir + GLM defines + `RUNTIME_OUTPUT_DIRECTORY/FOLDER Tests`).

- [ ] **Step 2: Configure + build — confirm it FAILS**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_skinning
```
Expected: COMPILE ERROR — `Skinning.h` not found / `MakeSkinnedVertex`/`ComputeSkinningPalette` undeclared (TDD red).

- [ ] **Step 3: Create `src/common/include/Skinning.h`**
```cpp
#pragma once
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include "Skeleton.h"

// GPU-skinning data + pure math, shared by the importer (weight extraction), the GameThread skinning
// step (palette), the renderer (vertex format), and tests. See
// docs/superpowers/specs/2026-06-08-anim-skinning-design.md.

// Per-vertex bone influences for GPU skinning: up to 4 (boneIndex, weight) pairs.
struct SkinnedVertex {
    glm::uvec4 BoneIndices{0u, 0u, 0u, 0u};
    glm::vec4  BoneWeights{0.0f, 0.0f, 0.0f, 0.0f};
};

// Reduce an arbitrary influence set for ONE vertex to the top-4 by weight, normalized to sum 1.
// No influences -> bone 0 with weights (1,0,0,0) so the vertex skins to bone 0's transform (the VS
// needs no zero-weight special case).
inline SkinnedVertex MakeSkinnedVertex(std::vector<std::pair<uint32_t,float>> influences) {
    SkinnedVertex out;
    if (influences.empty()) { out.BoneWeights.x = 1.0f; return out; }
    std::sort(influences.begin(), influences.end(),
              [](const auto& a, const auto& b){ return a.second > b.second; });
    const size_t n = std::min<size_t>(4, influences.size());
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) sum += influences[i].second;
    if (sum <= 0.0f) { out.BoneWeights.x = 1.0f; return out; }
    for (size_t i = 0; i < n; ++i) {
        out.BoneIndices[(glm::length_t)i] = influences[i].first;
        out.BoneWeights[(glm::length_t)i] = influences[i].second / sum;
    }
    return out;
}

// Skinning palette: palette[b] = globals[b] * inverseBind[b]. `globals` are model-space bone
// transforms (bind pose for SP2 via ComputeBindPoseGlobals; clip-sampled in SP3). For a true bind
// pose this is identity per bone (globals = inverse(inverseBind)).
inline std::vector<glm::mat4> ComputeSkinningPalette(const Skeleton& sk, const std::vector<glm::mat4>& globals) {
    const size_t n = sk.bones.size();
    std::vector<glm::mat4> palette(n);
    for (size_t b = 0; b < n && b < globals.size(); ++b)
        palette[b] = globals[b] * sk.bones[b].inverseBind;
    return palette;
}
```

- [ ] **Step 4: Build + run — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target test_skinning
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skinning.exe
```
Expected: `All skinning tests passed.`

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/common/include/Skinning.h tests/test_skinning.cpp tests/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): Skinning.h — SkinnedVertex + top-4 weight reduction + palette formula + tests"
```

---

### Task 2: MeshSystem parallel bone buffer + ring/Renderer plumbing

**Files:** `src/common/include/ApplicationContext.h`, `src/engine/src/rendering/MeshSystem.{h,cpp}`, `src/engine/src/rendering/Renderer.{h,cpp}`.

Build-verified (GPU buffer; behavior covered by Task 6 smoke). Skinned data is OPTIONAL — static meshes pass `boneData=nullptr` and are unaffected.

- [ ] **Step 1: Include `Skinning.h` + add `BoneData` to `MeshRequest` (`ApplicationContext.h`)**
Add `#include "Skinning.h"` near the `MeshVertex` definition. In the `MeshRequest` struct (the union member), after `char Key[256];` add:
```cpp
            SkinnedVertex* BoneData;   // optional; null => static mesh (no skinning)
```

- [ ] **Step 2: MeshEntry + MeshResources + AddMesh decl (`MeshSystem.h`)**
Add `#include "Skinning.h"` (top). In `struct MeshEntry`, after `std::vector<uint32_t> cpuIndices;` add:
```cpp
        bool isSkinned = false;
        nvrhi::BufferHandle boneBuffer;            // {uvec4 idx, vec4 weight} per vertex; null if !isSkinned
        std::vector<SkinnedVertex> cpuSkinning;    // hot-swap replay
```
In `struct MeshResources`, after `bool valid = false;` add:
```cpp
        bool isSkinned = false;
        nvrhi::IBuffer* boneBuffer = nullptr;
```
Change the `AddMesh` declaration to take optional bone data (last param):
```cpp
    MeshHandle AddMesh(std::string key,
                       const MeshVertex* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount,
                       SubMesh* subMeshes = nullptr, uint32_t subMeshCount = 0,
                       const SkinnedVertex* boneData = nullptr);
```

- [ ] **Step 3: AddMesh — build the bone buffer (`MeshSystem.cpp`)**
Change the definition signature to match Step 2. After the CPU copies block (`entry.cpuVertices.assign(...); entry.cpuIndices.assign(...);`) add:
```cpp
    if (boneData) {
        entry.isSkinned = true;
        entry.cpuSkinning.assign(boneData, boneData + vertexCount);
    }
```
After the index buffer is created/uploaded (after `setPermanentBufferState(entry.indexBuffer, ... IndexBuffer)`), before `cl->close()`, add the bone buffer upload:
```cpp
    if (entry.isSkinned) {
        nvrhi::BufferDesc bbDesc;
        bbDesc.debugName = "MeshSystem BoneVB " + std::to_string(m_Meshes.size());
        bbDesc.byteSize = sizeof(SkinnedVertex) * vertexCount;
        bbDesc.isVertexBuffer = true;
        bbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.boneBuffer = m_Device->createBuffer(bbDesc);
        if (!entry.boneBuffer) {
            SM_ERROR("MeshSystem::AddMesh: Failed to create bone buffer");
            cl->close();
            return MeshHandle{ UINT64_MAX };
        }
        cl->beginTrackingBufferState(entry.boneBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.boneBuffer, entry.cpuSkinning.data(), bbDesc.byteSize);
        cl->setPermanentBufferState(entry.boneBuffer, nvrhi::ResourceStates::VertexBuffer);
    }
```
In `GetMeshResources`, after `resources.valid = true;` add:
```cpp
    resources.isSkinned  = entry.isSkinned;
    resources.boneBuffer = entry.boneBuffer;
```
In `RecreateGpuResources`, inside the per-mesh loop after the index buffer is rebuilt (before `cl->close()`), add the bone-buffer replay:
```cpp
        if (entry.isSkinned && !entry.cpuSkinning.empty()) {
            nvrhi::BufferDesc bbDesc;
            bbDesc.debugName = "MeshSystem BoneVB " + std::to_string(i);
            bbDesc.byteSize = sizeof(SkinnedVertex) * entry.cpuSkinning.size();
            bbDesc.isVertexBuffer = true;
            bbDesc.initialState = nvrhi::ResourceStates::CopyDest;
            entry.boneBuffer = m_Device->createBuffer(bbDesc);
            if (entry.boneBuffer) {
                cl->beginTrackingBufferState(entry.boneBuffer, nvrhi::ResourceStates::CopyDest);
                cl->writeBuffer(entry.boneBuffer, entry.cpuSkinning.data(), bbDesc.byteSize);
                cl->setPermanentBufferState(entry.boneBuffer, nvrhi::ResourceStates::VertexBuffer);
            }
        }
```

- [ ] **Step 4: Renderer forwarder (`Renderer.{h,cpp}`)**
`Renderer.h` AddMesh decl: add `, const SkinnedVertex* boneData = nullptr` as the last param. `Renderer.cpp`:
```cpp
MeshHandle Renderer::AddMesh(std::string key, const MeshVertex* vertices, uint32_t vertexCount,
                              const uint32_t* indices, uint32_t indexCount, SubMesh* subMeshes, uint32_t subMeshCount,
                              const SkinnedVertex* boneData) {
    return m_MeshSystem.AddMesh(std::move(key), vertices, vertexCount, indices, indexCount, subMeshes, subMeshCount, boneData);
}
```
(`Renderer.h` needs `SkinnedVertex` visible — it includes `ApplicationContext.h`/`MeshSystem.h`; if not, add `#include "Skinning.h"`.)

- [ ] **Step 5: Build `Engine` — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: clean (no caller passes `boneData` yet → all static, unchanged behavior).

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/common/include/ApplicationContext.h src/engine/src/rendering/MeshSystem.h src/engine/src/rendering/MeshSystem.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): MeshSystem optional parallel bone buffer (isSkinned/boneBuffer) + ring/Renderer plumbing"
```

---

### Task 3: Extract per-vertex weights + thread bone data to upload

**Files:** `src/engine/src/threading/GameThread.h`, `src/engine/src/threading/GameThread.cpp`, `src/engine/src/threading/RenderThread.cpp`.

- [ ] **Step 1: ModelLoadResult skinning field (`GameThread.h`)**
Add `#include "Skinning.h"` (with the other includes). In `struct ModelLoadResult`, after the SP1 `std::string skeletonKey;` add:
```cpp
        std::vector<SkinnedVertex> skinning; // per-vertex bone idx+weights, aligned to `vertices`; empty if static
```

- [ ] **Step 2: Extract weights in `WorkerThreadFunc` (`GameThread.cpp`)**
Add `#include "Skinning.h"` (top). The SP1 skeleton-extraction block builds `boneInverseBind` + the `Skeleton` via the `walk` lambda. We need a bone-NAME → Skeleton-INDEX map (the same order the Skeleton uses) to translate assimp per-vertex weights. Inside the `if (!boneInverseBind.empty()) { Skeleton skel; ... }` block, augment the `walk` lambda to also record names→indices, then build the skinning array. Replace the SP1 block's body (from `Skeleton skel;` through the `if (!skel.bones.empty()) { ... }`) with:
```cpp
                Skeleton skel;
                std::unordered_map<std::string,int> boneNameToIndex;
                std::function<void(const aiNode*, int)> walk = [&](const aiNode* node, int parentBoneIdx) {
                    int myIdx = parentBoneIdx;
                    auto it = boneInverseBind.find(node->mName.C_Str());
                    if (it != boneInverseBind.end()) {
                        Bone bone;
                        bone.name        = node->mName.C_Str();
                        bone.parent      = parentBoneIdx;
                        bone.localBind   = AiToGlm(node->mTransformation);
                        bone.inverseBind = it->second;
                        myIdx = static_cast<int>(skel.bones.size());
                        boneNameToIndex[bone.name] = myIdx;
                        skel.bones.push_back(std::move(bone));
                    }
                    for (unsigned c = 0; c < node->mNumChildren; ++c)
                        walk(node->mChildren[c], myIdx);
                };
                walk(scene->mRootNode, -1);
                if (!skel.bones.empty()) {
                    result.skeleton    = std::move(skel);
                    result.hasSkeleton = true;
                    result.skeletonKey = result.assetKey + "#skeleton";
                    SM_TRACE("Skeleton extracted: '%s' (%zu bones)", result.skeletonKey.c_str(), result.skeleton.bones.size());

                    // Per-vertex weights, aligned to result.vertices. Accumulate influences across the
                    // scene's meshes in the SAME concatenation order processNode used (node pre-order,
                    // mesh order), then reduce to top-4. Bone index = the Skeleton's index (by name).
                    std::vector<std::vector<std::pair<uint32_t,float>>> perVertex(result.vertices.size());
                    uint32_t base = 0; // running vertex offset matching processNode's concatenation
                    std::function<void(const aiNode*)> collect = [&](const aiNode* node) {
                        for (unsigned m = 0; m < node->mNumMeshes; ++m) {
                            const aiMesh* mesh = scene->mMeshes[node->mMeshes[m]];
                            for (unsigned bi = 0; bi < mesh->mNumBones; ++bi) {
                                const aiBone* bone = mesh->mBones[bi];
                                auto ni = boneNameToIndex.find(bone->mName.C_Str());
                                if (ni == boneNameToIndex.end()) continue;
                                const uint32_t boneIdx = static_cast<uint32_t>(ni->second);
                                for (unsigned w = 0; w < bone->mNumWeights; ++w) {
                                    const aiVertexWeight& vw = bone->mWeights[w];
                                    const size_t vtx = base + vw.mVertexId;
                                    if (vtx < perVertex.size() && vw.mWeight > 0.0f)
                                        perVertex[vtx].emplace_back(boneIdx, vw.mWeight);
                                }
                            }
                            base += mesh->mNumVertices;
                        }
                        for (unsigned c = 0; c < node->mNumChildren; ++c) collect(node->mChildren[c]);
                    };
                    collect(scene->mRootNode);

                    result.skinning.resize(result.vertices.size());
                    for (size_t v = 0; v < result.vertices.size(); ++v)
                        result.skinning[v] = MakeSkinnedVertex(std::move(perVertex[v]));
                    SM_TRACE("Skinning extracted: %zu verts", result.skinning.size());
                }
```
**CRITICAL alignment note:** `base` must advance over meshes in the EXACT order `processNode` concatenated them into `result.vertices` (node pre-order, then `node->mMeshes` order). The `collect` lambda above mirrors `processNode`'s traversal (pre-order, same mesh order) so `base + mVertexId` lands on the right concatenated vertex. If `processNode` differs (re-read it), match its order exactly — a mismatch silently mis-weights vertices.

- [ ] **Step 3: Fill `MeshRequest.BoneData` in the drain (`GameThread.cpp`)**
In the completed-jobs drain, in the `if (!res.MeshUploaded)` block, after the `meshCmd.MeshRequest.SubMeshes = nullptr;` initializers add `meshCmd.MeshRequest.BoneData = nullptr;`. After the `Vertices` staging copy block, add a parallel bone-data staging copy:
```cpp
                        if (!res.skinning.empty() && res.skinning.size() == res.vertices.size())
                        {
                            meshCmd.MeshRequest.BoneData = static_cast<SkinnedVertex*>(GetStagingPool().Acquire(res.skinning.size() * sizeof(SkinnedVertex)));
                            std::memcpy(meshCmd.MeshRequest.BoneData, res.skinning.data(), res.skinning.size() * sizeof(SkinnedVertex));
                        }
```
In the ring-full retry cleanup (the `if (!m_AppContext->GRCommandRing.Push(meshCmd))` block that Returns Vertices/Indices/SubMeshes), add:
```cpp
                            if (meshCmd.MeshRequest.BoneData) GetStagingPool().Return(meshCmd.MeshRequest.BoneData);
```

- [ ] **Step 4: RenderThread passes + returns BoneData (`RenderThread.cpp`)**
In the RequestMesh case, add `cmd.MeshRequest.BoneData` as the last `AddMesh` arg:
```cpp
                    const auto meshHandle = m_Renderer->AddMesh(
                        std::string(cmd.MeshRequest.Key),
                        cmd.MeshRequest.Vertices, static_cast<uint32_t>(cmd.MeshRequest.VertexCount),
                        cmd.MeshRequest.Indices, static_cast<uint32_t>(cmd.MeshRequest.IndexCount),
                        cmd.MeshRequest.SubMeshes, static_cast<uint32_t>(cmd.MeshRequest.SubMeshCount),
                        cmd.MeshRequest.BoneData
                    );
```
And after the existing `GetStagingPool().Return(cmd.MeshRequest.SubMeshes)` line add:
```cpp
                    if (cmd.MeshRequest.BoneData) GetStagingPool().Return(cmd.MeshRequest.BoneData);
```

- [ ] **Step 5: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (RiggedSimple now uploads as a skinned mesh; nothing consumes the bone buffer yet → still renders static.)

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp src/engine/src/threading/RenderThread.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): extract per-vertex bone weights (top-4, Skeleton-indexed) + upload skinned mesh bone buffer"
```

---

### Task 4: PaletteFrame transport + GameThread skinning step + SkinTest flag

**Files:** Create `src/common/include/PaletteFrame.h`; Modify `src/common/include/ApplicationContext.h`, `src/engine/src/rendering/RenderStats.h`, `src/editor/src/panels/RenderStatsPanel.cpp`, `src/engine/src/threading/GameThread.cpp`.

- [ ] **Step 1: Create `src/common/include/PaletteFrame.h`**
```cpp
#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
#include "ECS.h"   // EntityId

// Per-tick bone-matrix palettes for all skinned entities, published GameThread->RenderThread via an
// atomic shared_ptr (parallel to LatestWorldSnapshot; immutable once published). Flat matrix array +
// per-entity ranges = one StructuredBuffer + a per-instance base offset on the GPU.
struct PaletteFrame {
    std::vector<glm::mat4> matrices;
    struct Range { EntityId entity; uint32_t offset; uint32_t count; };
    std::vector<Range> ranges;
};
```

- [ ] **Step 2: Publish atomic in `ApplicationContext.h`**
Add `#include "PaletteFrame.h"` near the snapshot includes. After `std::atomic<std::shared_ptr<const ECS>> LatestWorldSnapshot;` add:
```cpp
    // Game -> Render per-tick bone palettes (atomic shared_ptr, parallel to LatestWorldSnapshot).
    std::atomic<std::shared_ptr<const PaletteFrame>> LatestPaletteFrame;
```

- [ ] **Step 3: SkinTest flag (`RenderStats.h` + `RenderStatsPanel.cpp`)**
`RenderStats.h` `DebugDrawSettings`: after `bool ShowSkeleton = false;` add `bool SkinTest = false;`.
`RenderStatsPanel.cpp`: after `changed |= ImGui::Checkbox("Skeleton", &dd.ShowSkeleton);` add:
```cpp
    changed |= ImGui::Checkbox("Skin Test (wiggle)", &dd.SkinTest);
```

- [ ] **Step 4: GameThread skinning step (`GameThread.cpp`)**
Add includes: `#include "PaletteFrame.h"`, `#include "Skinning.h"`, `#include "animation/SkeletonStore.h"` (SkeletonStore already included from SP1), `#include "RenderStats.h"`, `#include <glm/gtc/matrix_transform.hpp>`.
Add a private method `void PublishPaletteFrame(GameState& state);` to `GameThread.h` (near `PublishSnapshot`). Define it in `GameThread.cpp`:
```cpp
void GameThread::PublishPaletteFrame(GameState& state) {
    auto frame = std::make_shared<PaletteFrame>();
    const bool wiggle = GetDebugDrawSettings().SkinTest;
    const float t = static_cast<float>(TimeNowSec());
    state.World.Each<SkeletonComponent>([&](EntityId e, const SkeletonComponent& sc) {
        const Skeleton* sk = SkeletonStore::Instance().Get(sc.SkeletonId);
        if (!sk || sk->bones.empty()) return;
        std::vector<glm::mat4> globals = ComputeBindPoseGlobals(*sk);
        if (wiggle && !globals.empty()) {
            // Perturb the last bone (a leaf) by a time-based rotation so deformation is visible.
            const size_t b = globals.size() - 1;
            globals[b] = globals[b] * glm::rotate(glm::mat4(1.0f), std::sin(t) * 0.8f, glm::vec3(0,0,1));
        }
        const std::vector<glm::mat4> palette = ComputeSkinningPalette(*sk, globals);
        const uint32_t offset = static_cast<uint32_t>(frame->matrices.size());
        frame->matrices.insert(frame->matrices.end(), palette.begin(), palette.end());
        frame->ranges.push_back(PaletteFrame::Range{ e, offset, static_cast<uint32_t>(palette.size()) });
    });
    m_AppContext->LatestPaletteFrame.store(std::move(frame), std::memory_order_release);
}
```
(If `Each<SkeletonComponent>` single-component form isn't available, use `Each<TransformComponent, SkeletonComponent>` and ignore the transform — check the `Each` overloads in `ECS.h` and match.)
Call it from `RunLoop` immediately before `PublishSnapshot(gameState, frameStats);` (so the palette reflects the same tick):
```cpp
			PublishPaletteFrame(gameState);
			PublishSnapshot(gameState, frameStats); // publish to SnapshotRing (S -> R)
```

- [ ] **Step 5: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean. (Palette computed + published; not consumed yet → no visual change.)

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/common/include/PaletteFrame.h src/common/include/ApplicationContext.h src/engine/src/rendering/RenderStats.h src/editor/src/panels/RenderStatsPanel.cpp src/engine/src/threading/GameThread.h src/engine/src/threading/GameThread.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): PaletteFrame transport + GameThread bind-pose skinning step + SkinTest debug flag"
```

---

### Task 5: Skinned PSO + skinning VS + palette SB + skinned draw path (GBufferFillPass)

**Files:** `src/engine/src/rendering/passes/GBufferFillPass.{h,cpp}`.

The big render task. Static path stays byte-identical; skinned entities take a new branch.

- [ ] **Step 1: InstanceData gains `PaletteOffset` (both C++ + HLSL)**
`GBufferFillPass.h` `MeshInstanceCPU`: change `uint32_t Flags; uint32_t _pad[3];` to:
```cpp
        uint32_t Flags; uint32_t PaletteOffset; uint32_t _pad[2];
```
In `GBUF_VS_HLSL` AND `GBUF_PS_HLSL`, change `uint Flags; uint3 _pad;` to `uint Flags; uint PaletteOffset; uint2 _pad;` (keep both struct copies identical). The static VS ignores `PaletteOffset`.

- [ ] **Step 2: Add the skinned VS string + members (`GBufferFillPass.{h,cpp}`)**
In `GBufferFillPass.h`, add members:
```cpp
    nvrhi::ShaderHandle m_SkinnedVS;
    nvrhi::GraphicsPipelineHandle m_SkinnedPipeline;
    nvrhi::InputLayoutHandle m_SkinnedInputLayout;
    nvrhi::BindingLayoutHandle m_SkinnedBindingLayout;
    nvrhi::BufferHandle m_PaletteBuffer;
    uint32_t m_PaletteCapacity = 0;                 // in mat4 elements
    std::shared_ptr<const PaletteFrame> m_LastPaletteFrame; // skip re-upload when unchanged
```
Add `#include "PaletteFrame.h"` + `#include "Skinning.h"` to `GBufferFillPass.cpp`. Add the skinned VS string near `GBUF_VS_HLSL`:
```cpp
static const char* GBUF_SKINNED_VS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4 BaseColor; uint Flags; uint PaletteOffset; uint2 _pad; };
cbuffer PerFrame : register(b0) { float4x4 uVP; };
StructuredBuffer<InstanceData> gInstances : register(t5);
StructuredBuffer<float4x4>     gBones     : register(t6);
struct VSIn  { float3 Position:POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; uint4 BoneIndices:BLENDINDICES; float4 BoneWeights:BLENDWEIGHT; uint InstanceID:SV_InstanceID; };
struct VSOut { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; };
VSOut main_vs(VSIn vin){
    InstanceData inst = gInstances[vin.InstanceID];
    uint off = inst.PaletteOffset;
    float4x4 skin =
        vin.BoneWeights.x * gBones[off + vin.BoneIndices.x] +
        vin.BoneWeights.y * gBones[off + vin.BoneIndices.y] +
        vin.BoneWeights.z * gBones[off + vin.BoneIndices.z] +
        vin.BoneWeights.w * gBones[off + vin.BoneIndices.w];
    float4 skinned = mul(skin, float4(vin.Position,1.0));
    float3 skinnedN = mul((float3x3)skin, vin.Normal);
    float4 wp = mul(inst.Model, skinned);
    VSOut o; o.PosH = mul(uVP, wp);
    o.Normal = mul((float3x3)inst.NormalMatrix, skinnedN);
    o.UV = vin.UV; o.WorldPos = wp.xyz; o.InstanceID = vin.InstanceID; return o;
}
)";
```
In `Initialize`, after compiling `m_VS`, compile the skinned VS:
```cpp
    m_SkinnedVS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, GBUF_SKINNED_VS_HLSL, strlen(GBUF_SKINNED_VS_HLSL), "main_vs", "vs_6_1");
```
(Match the exact `CreateShader` call shape used for `m_VS`.)

- [ ] **Step 3: Skinned input layout + binding layout (`GBufferFillPass.cpp` Initialize)**
After the static `m_InputLayout` creation, add the skinned layout (5 attrs: 3 from buffer 0 + 2 from buffer 1):
```cpp
    nvrhi::VertexAttributeDesc sattrs[5];
    sattrs[0] = attrs[0]; sattrs[1] = attrs[1]; sattrs[2] = attrs[2];
    sattrs[3].setName("BLENDINDICES").setFormat(nvrhi::Format::RGBA32_UINT)
        .setOffset(offsetof(SkinnedVertex, BoneIndices)).setBufferIndex(1).setElementStride(sizeof(SkinnedVertex));
    sattrs[4].setName("BLENDWEIGHT").setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(SkinnedVertex, BoneWeights)).setBufferIndex(1).setElementStride(sizeof(SkinnedVertex));
    m_SkinnedInputLayout = m_Device->createInputLayout(sattrs, 5, m_SkinnedVS);
```
After the static `m_BindingLayout` creation, add the skinned binding layout (adds `t6`):
```cpp
    nvrhi::BindingLayoutDesc slayoutDesc;
    slayoutDesc.visibility = nvrhi::ShaderType::All;
    slayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(2),
        nvrhi::BindingLayoutItem::Sampler(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6)
    };
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        slayoutDesc.setBindingOffsets(offsets);   // reuse the same `offsets` built for the static layout
    m_SkinnedBindingLayout = m_Device->createBindingLayout(slayoutDesc);
```
(If `offsets` is out of scope by here, rebuild the same `nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0)`.)

- [ ] **Step 4: Skinned pipeline (lazy, alongside `m_Pipeline`)**
In the `if (!m_Pipeline) { ... }` block, after `m_WireframePipeline` is created, add (reuse `pso`, swap VS/layouts; reset fillMode to Solid since wireframe set it last):
```cpp
        nvrhi::GraphicsPipelineDesc spso = pso;
        spso.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Solid;
        spso.VS = m_SkinnedVS;
        spso.inputLayout = m_SkinnedInputLayout;
        spso.bindingLayouts = { m_SkinnedBindingLayout };
        m_SkinnedPipeline = m_Device->createGraphicsPipeline(spso, fbi);
```

- [ ] **Step 5: Load + upload the palette (`GBufferFillPass.cpp` Render, before the draw loop)**
Near the top of `Render` (after `world` null-check, before the gather), load the palette frame and upload it, building an `EntityId→offset` map:
```cpp
    std::shared_ptr<const PaletteFrame> palette =
        m_Renderer->GetAppContext()->LatestPaletteFrame.load(std::memory_order_acquire);
    std::unordered_map<EntityId, uint32_t> paletteOffsetByEntity;
    if (palette && !palette->matrices.empty()) {
        for (const auto& rg : palette->ranges) paletteOffsetByEntity[rg.entity] = rg.offset;
        // Grow + upload only when the frame changed since last render.
        if (palette != m_LastPaletteFrame) {
            const uint32_t need = static_cast<uint32_t>(palette->matrices.size());
            if (need > m_PaletteCapacity) {
                nvrhi::BufferDesc pd;
                pd.debugName = "GBufferFillPass PaletteBuffer";
                pd.byteSize = sizeof(glm::mat4) * need;
                pd.structStride = sizeof(glm::mat4);
                pd.initialState = nvrhi::ResourceStates::CopyDest;
                pd.keepInitialState = true;
                m_PaletteBuffer = m_Device->createBuffer(pd);
                m_PaletteCapacity = need;
            }
            commandList->writeBuffer(m_PaletteBuffer, palette->matrices.data(), sizeof(glm::mat4) * need);
            m_LastPaletteFrame = palette;
        }
    }
```
(Add `#include <unordered_map>` if needed.)

- [ ] **Step 6: Skinned draw branch (`GBufferFillPass.cpp` per-draw loop)**
The cleanest minimal change: inside the per-instance fill loop, set `inst.PaletteOffset`; and at draw time, if the run's mesh `isSkinned` AND the palette has the entities, use the skinned pipeline + bind `t6` + bind the bone buffer at slot 1. Concretely:
(a) In the per-instance fill, after `inst.Flags = flags;` add:
```cpp
            auto poIt = paletteOffsetByEntity.find(entity);
            inst.PaletteOffset = (poIt != paletteOffsetByEntity.end()) ? poIt->second : 0u;
```
(b) Determine skinned-ness for the run once (after `meshResources` is fetched):
```cpp
        const bool runSkinned = meshResources.isSkinned && meshResources.boneBuffer && m_PaletteBuffer && (palette != nullptr);
```
(c) Where `state` is configured + the binding set built, branch on `runSkinned`. For the skinned case, build the binding set against `m_SkinnedBindingLayout` adding `StructuredBuffer_SRV(6, m_PaletteBuffer)`, set TWO vertex buffers, and select `m_SkinnedPipeline`. Apply this in BOTH the `subMeshes` branch and the non-submesh branch — factor the binding-set build to include the palette when `runSkinned`. Example for the binding set + state in the non-submesh branch:
```cpp
            nvrhi::BindingSetDesc bindingDesc;
            bindingDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
                nvrhi::BindingSetItem::Texture_SRV(2, materialResources.texture),
                nvrhi::BindingSetItem::Sampler(3, materialResources.sampler),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_InstanceBuffer)
            };
            if (runSkinned) bindingDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_PaletteBuffer));
            nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, runSkinned ? m_SkinnedBindingLayout : m_BindingLayout);

            state.pipeline = runSkinned ? m_SkinnedPipeline : (wireframe ? m_WireframePipeline : m_Pipeline);
            state.bindings = { bindingSet };
            if (runSkinned)
                state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0),
                                        nvrhi::VertexBufferBinding(meshResources.boneBuffer, 1, 0) };
            else
                state.vertexBuffers = { nvrhi::VertexBufferBinding(meshResources.vertexBuffer, 0, 0) };
            state.indexBuffer = nvrhi::IndexBufferBinding(meshResources.indexBuffer, nvrhi::Format::R32_UINT, 0);
```
**Read the existing `state.pipeline` assignment first** — the current code sets `state.pipeline` somewhere before/within the loop (find how `m_Pipeline`/`m_WireframePipeline` is selected; the var `wireframe` is from `GetDebugDrawSettings().Wireframe`). Mirror that selection, adding the `runSkinned ? m_SkinnedPipeline` case. Apply the same binding-set/vertex-buffer change to the `subMeshes` branch.

- [ ] **Step 7: Build full tree — GREEN**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean (skinned VS compiles via DXC; both PSOs create).

- [ ] **Step 8: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/GBufferFillPass.h src/engine/src/rendering/passes/GBufferFillPass.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(anim): skinned PSO + skinning VS + palette structured buffer; skinned entities deform in the G-buffer"
```

---

### Task 6: Full regression + manual smoke

**Files:** none (verification; fixups only if needed).

- [ ] **Step 1: Reconfigure + full clean build**
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets build, no errors / `LNK`.

- [ ] **Step 2: Run the suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skinning.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skeleton.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_assetkey.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_reloadpreserve.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_playermove.exe
```
Expected: each prints its pass line (`test_compserial` `NopeNotReal` warn + `test_reloadpreserve` `BadComp` warn expected).

- [ ] **Step 3: Manual smoke (human-owned; editor restart)**
With RiggedSimple: assign its (now skinned) mesh (MeshEditor) + its skeleton (SkeletonEditor) to an entity.
1. **At rest:** the mesh renders **identical** to before (bind-pose palette = identity) — proves the skinned path is correct.
2. **Toggle `Skin Test (wiggle)`** in RenderStats → the leaf bone rotates over time and the mesh **visibly deforms** (show the SP1 Skeleton overlay alongside to correlate).
3. **Static meshes** (existing `.obj`s) render unchanged (they take the static path; no skeleton/bone buffer).
4. Toggle off → back to rest.

- [ ] **Step 4: Commit fixups (only if needed).**

---

## Done criteria

- `Skinning.h` (`SkinnedVertex` + `MakeSkinnedVertex` top-4/normalize + `ComputeSkinningPalette`); `test_skinning` green (Task 1).
- MeshSystem optional parallel bone buffer (`isSkinned`/`boneBuffer`/`cpuSkinning`, hot-swap replay); ring + Renderer carry `boneData` (Task 2).
- Per-vertex weights extracted (top-4, Skeleton-indexed, aligned to the concatenated vertices) + skinned mesh uploaded (Task 3).
- `PaletteFrame` published per tick via `atomic<shared_ptr>`; GameThread computes `global*inverseBind` (+ `SkinTest` wiggle) (Task 4).
- Skinned PSO + skinning VS + palette SB (upload-skip-when-unchanged); skinned entities deform; **static path unchanged** (Task 5).
- RiggedSimple renders identical at rest; `SkinTest` visibly deforms it (smoke). Unit + regression suites green; full tree builds.
- No `ECS.h` change, no `GAME_API_VERSION` bump.

## Notes

- Bind-pose palette is identity by construction → SP2 correctness is "renders identical at rest"; the wiggle is the visible proof. SP3 keeps this whole pipeline and replaces `ComputeBindPoseGlobals` with clip-sampled `global = parent*localAnimated`, reusing `ComputeSkinningPalette` + the transport + the PSO unchanged.
- `PaletteFrame` mirrors `LatestWorldSnapshot` (atomic-`shared_ptr`, tick-rate, RenderThread reuses across frames; upload skipped when the pointer is unchanged). ≤1-tick palette/snapshot skew is harmless (lookup miss → static).
- Vertex-weight alignment to `result.vertices` is the one subtle correctness point — the `collect` traversal MUST match `processNode`'s concatenation order (Task 3, Step 2 note).
- `ShadowDepthPass` left static (skinned shadows = follow-up). Editor file-load path (`MeshLoader::ProcessMesh`) not skinned (startup path only). See [[project_animation]].
