# Compute Skinning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The engine's first compute pass — skin each skinned mesh once in a compute shader into a per-frame vertex buffer that the G-buffer AND shadow passes read via their static pipelines, fixing bind-pose shadows and unifying skinning into one code path.

**Architecture:** A `SkinningComputePass` runs first each frame, dispatches one threadgroup-grid per skinned entity, and writes posed mesh-local vertices into a per-frame skinned VB (+ an `entity→outputVertexOffset` table). G-buffer and shadow draw skinned entities through the static pipeline with `baseVertex = outputOffset`. The palette pipeline (GameThread → `PaletteFrame` → palette SRV) is reused unchanged.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset ONLY), NVRHI (DX12 default + Vulkan), DXC (`cs_6_1`), the existing pass/`Renderer` architecture.

**Spec:** `docs/superpowers/specs/2026-06-08-compute-skinning-design.md`

---

## Conventions (apply to every task)

- **Build/test preset:** `msvc-win64-vs2026-community` ONLY. Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **Commit author:** EVERY commit `--author="Nuno Silva <nuno.levezinho@live.com.pt>"`. NEVER the vinci-energies.net work email.
- **NEVER** `--no-verify`. **NEVER** `git add -A`/`git add .` — stage exact paths. Never stage `assets/world.json`, `assets/engine_settings.json`, `assets/editor_preferences.json`.
- **git from git-bash:** `git -C C:/dev/clang-examples ...`.
- Branch is already `feat/compute-skinning`.
- **Logging:** `SM_TRACE`/`SM_WARN`/`SM_ERROR`. Log degradation, never silent-skip.
- **No `ECS.h` change** → no component-restart caveat (normal editor relaunch after an Engine rebuild to smoke).
- New `.cpp` files → add to `src/engine/CMakeLists.txt` (engine) explicitly.
- **NVRHI signatures are GROUND TRUTH.** Where this plan names an NVRHI call (`createComputePipeline`, `ComputeState`, `BindingSetItem::StructuredBuffer_UAV`, `commandList->dispatch`, `setBufferState`/`requireBufferState`), VERIFY the exact signature against `third_party/NVRHI` headers + how existing passes use the graphics equivalents (`createGraphicsPipeline`, `GraphicsState`, `setGraphicsState`). Mirror the engine's existing NVRHI idioms.

---

## Sequencing rationale (read before starting)

Tasks are ordered so **every task builds and runs**:
- T1–T2: isolated additions (CPU math + compute-shader compilation), no behavior change.
- T3: skinned-mesh input buffers become **dual-usage** (vertex buffer + structured SRV, trackable state) — old VS-skinning path still works.
- T4: the compute pass runs and writes the skinned VB, but **nothing reads it yet** (g-buffer still VS-skins, shadow still static). This isolates "does the first compute pass dispatch cleanly with no validation errors."
- T5: shadow reads it → **skinned shadows appear** (visible payoff).
- T6: g-buffer reads it + the VS skinning path is removed → one skinning path.
- T7: verify.

---

## File Structure

**Create:**
- `src/engine/src/rendering/passes/SkinningComputePass.h` / `.cpp` — the compute pass: pipeline, per-frame skinned VB, entity→offset table, per-entity dispatch.
- `tests/test_skinning` already exists — extend it (no new test target).

**Modify:**
- `src/common/include/Skinning.h` — `SkinVertexCPU` reference helper.
- `tests/test_skinning.cpp` — `SkinVertexCPU` test.
- `src/engine/src/rendering/Renderer.{h,cpp}` — own/orchestrate `SkinningComputePass`; expose a skinned-geometry accessor to other passes; run it before shadow/g-buffer.
- `src/engine/src/rendering/MeshSystem.cpp` — skinned-mesh input VB + bone buffer become structured-SRV-capable (dual-usage).
- `src/engine/src/rendering/MeshSystem.h` — `MeshResources` may need a flag/handle exposing the SRV-capable buffers (likely already has `vertexBuffer`/`boneBuffer`).
- `src/engine/src/rendering/passes/ShadowDepthPass.cpp` — skinned entities bind the skinned VB + `baseVertex`.
- `src/engine/src/rendering/passes/GBufferFillPass.cpp` — remove VS skinning; skinned entities draw static via skinned VB + `baseVertex`.
- `src/engine/CMakeLists.txt` — add `SkinningComputePass.cpp`.

---

## Task 1: `SkinVertexCPU` reference + test (TDD)

> Pins the skin-math contract the compute shader must mirror — the only unit-testable part of a GPU feature.

**Files:** Modify `src/common/include/Skinning.h`, `tests/test_skinning.cpp`

- [ ] **Step 1: Write the failing test in `tests/test_skinning.cpp`**

Mirror the file's existing test style. Add (and call from `main`):
```cpp
static void T_skin_vertex_cpu() {
    // Two-bone palette: bone0 = identity, bone1 = translate(+10 in x).
    std::vector<glm::mat4> palette = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(10,0,0)) };
    glm::vec3 pos(1,2,3), nrm(0,1,0);
    // Full weight on bone0 -> unchanged.
    glm::vec3 p0, n0; SkinVertexCPU(palette, glm::uvec4(0,0,0,0), glm::vec4(1,0,0,0), pos, nrm, p0, n0);
    EXPECT(veq(p0, pos) && veq(n0, nrm));
    // Full weight on bone1 -> +10 x; normal unaffected by pure translation.
    glm::vec3 p1, n1; SkinVertexCPU(palette, glm::uvec4(1,0,0,0), glm::vec4(1,0,0,0), pos, nrm, p1, n1);
    EXPECT(veq(p1, glm::vec3(11,2,3)) && veq(n1, nrm));
    // 50/50 blend of bone0/bone1 -> +5 x.
    glm::vec3 pm, nm; SkinVertexCPU(palette, glm::uvec4(0,1,0,0), glm::vec4(0.5f,0.5f,0,0), pos, nrm, pm, nm);
    EXPECT(veq(pm, glm::vec3(6,2,3)));
    // Empty palette / out-of-range index path: index >= palette size contributes identity (no crash).
    glm::vec3 pe, ne; SkinVertexCPU({}, glm::uvec4(0,0,0,0), glm::vec4(1,0,0,0), pos, nrm, pe, ne);
    EXPECT(veq(pe, pos));
}
```
Confirm `veq`/`EXPECT` exist in test_skinning.cpp (or use the file's actual vec-compare). Add `T_skin_vertex_cpu();` to `main`. Ensure `#include <glm/gtc/matrix_transform.hpp>` is present (for `translate`).

- [ ] **Step 2: Build — verify FAIL (SkinVertexCPU undefined)**
```
cmake --build --preset msvc-win64-vs2026-community --target test_skinning
```
Expected: compile error.

- [ ] **Step 3: Implement `SkinVertexCPU` in `src/common/include/Skinning.h`**

After `ComputeSkinningPalette`, add:
```cpp
// CPU reference for the per-vertex skin the compute shader performs (verified contract; the HLSL CS
// mirrors this exactly). palette is the resolved skinning palette (global*inverseBind, already
// rootTransform-prepended). An index >= palette.size() contributes the identity row (no crash; matches
// the sentinel/empty cases). outPos/outNormal are the posed MESH-LOCAL position/normal (before Model).
inline void SkinVertexCPU(const std::vector<glm::mat4>& palette,
                          const glm::uvec4& boneIndices, const glm::vec4& boneWeights,
                          const glm::vec3& pos, const glm::vec3& normal,
                          glm::vec3& outPos, glm::vec3& outNormal) {
    glm::mat4 skin(0.0f);
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        const uint32_t idx = boneIndices[i];
        const float    w   = boneWeights[i];
        if (w == 0.0f) continue;
        const glm::mat4 m = (idx < palette.size()) ? palette[idx] : glm::mat4(1.0f);
        skin += w * m;
        any = true;
    }
    if (!any) skin = glm::mat4(1.0f);
    outPos    = glm::vec3(skin * glm::vec4(pos, 1.0f));
    outNormal = glm::mat3(skin) * normal;
}
```
(Matches the HLSL: `skin = Σ wᵢ·palette[idxᵢ]`, `pos' = skin·pos`, `n' = (float3x3)skin·n`. The all-zero-weight guard mirrors the VS sentinel falling back to identity.)

- [ ] **Step 4: Build + run — verify PASS**
```
cmake --build --preset msvc-win64-vs2026-community --target test_skinning
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skinning.exe
```
Expected: `All skinning tests passed.`

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/common/include/Skinning.h tests/test_skinning.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): SkinVertexCPU reference + test (compute-skinning math contract)"
```

---

## Task 2: Compute-shader compilation support

> The renderer compiles VS/PS via `Renderer::CreateShader(ShaderType, hlsl, flags, entry, target)` → `CompileShader(targetName)` (DXC). Confirm a compute shader compiles end-to-end before building a pass around it.

**Files:** Modify `src/engine/src/rendering/shader/ShaderCompiler.cpp` (only if needed), `src/engine/src/rendering/Renderer.cpp` (only if `CreateShader` switches on ShaderType)

- [ ] **Step 1: Read the compile path**

Read `Renderer::CreateShader` and `CompileShader` in `src/engine/src/rendering/shader/ShaderCompiler.cpp`. Note: `CompileShader` takes `targetName` (e.g. `"vs_6_1"`); there is a `starts_with("vs_")` special-case (~line 149) and the GLSL path handles only `vs_`/`ps_`. For DX12 (DXC HLSL→DXIL), `cs_6_1` should compile with no special-casing.

- [ ] **Step 2: Ensure Compute is a valid path**

If `Renderer::CreateShader` maps `nvrhi::ShaderType` → something that excludes Compute, add the Compute case (`nvrhi::ShaderType::Compute`). For the DXC path, confirm passing `target = "cs_6_1"` works (no `vs_`-only assumptions block it). The Vulkan/SPIR-V GLSL branch (`CompileGlsl`, ~line 191-220) only handles `vs_`/`ps_` — add a `cs_` case there **only if the smoke runs on Vulkan**; DX12 (default) needs only the DXC path. Note in the commit which backends are covered.

- [ ] **Step 3: Smoke-compile a trivial CS (temporary)**

Temporarily, in `Renderer::InitForSwap` (or any pass Init), add a throwaway:
```cpp
auto testCS = CreateShader(nvrhi::ShaderType::Compute,
    "RWStructuredBuffer<uint> o:register(u0); [numthreads(1,1,1)] void main_cs(uint3 id:SV_DispatchThreadID){ o[0]=1; }",
    0, "main_cs", "cs_6_1");
SM_TRACE("compute test shader: %s", testCS ? "OK" : "NULL");
```
Build + launch the editor; confirm the log prints `compute test shader: OK` (non-null). Then REMOVE the throwaway and rebuild.

- [ ] **Step 4: Commit (only if files changed; if `cs_6_1` already worked with zero code change, skip to Task 3 and note it)**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/shader/ShaderCompiler.cpp src/engine/src/rendering/Renderer.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): compute shader (cs_6_1) compilation support"
```
If no code change was required (DXC compiled `cs_6_1` as-is), record that finding in the Task 3 commit instead and move on — do not create an empty commit.

---

## Task 3: Skinned-mesh input buffers → dual-usage (vertex buffer + structured SRV)

> The compute shader reads the mesh's positions/normals and bone idx/weights as SRVs. Make the skinned-mesh input buffers SRV-capable while keeping them usable as vertex buffers (so the old VS-skinning path keeps working until T6 removes it). Static meshes are unchanged.

**Files:** Modify `src/engine/src/rendering/MeshSystem.cpp` (and `.h` if `MeshResources` needs more)

- [ ] **Step 1: Make skinned-mesh VB + bone buffer SRV-capable**

In `MeshSystem::AddMesh`, the buffers are created with `isVertexBuffer=true` + `setPermanentBufferState(VertexBuffer)`. For **skinned meshes** (`entry.isSkinned`), change the **vertex buffer** and the **bone buffer** to ALSO be structured-SRV-capable and NOT permanent-state (so they can transition VertexBuffer↔ShaderResource):
- vertex buffer (skinned only): set `vbDesc.structStride = sizeof(MeshVertex);` and `vbDesc.canHaveTypedViews = true;` (verify the exact NVRHI field names for a structured-buffer SRV — likely `structStride` enables a `StructuredBuffer_SRV`); keep `isVertexBuffer = true`; replace `setPermanentBufferState(VertexBuffer)` with `setPermanentBufferState`-NOT-called (leave it trackable via `beginTrackingBufferState`), so the compute pass can `requireBufferState(ShaderResource)` and the raster can require `VertexBuffer`.
- bone buffer (already skinned-only): same — `structStride = sizeof(SkinnedVertex)`, SRV-capable, trackable state (the old VS binds it as a vertex buffer at slot 1; the compute reads it as SRV — dual usage).
- Static-mesh buffers: UNCHANGED (keep `isVertexBuffer` + permanent `VertexBuffer`).

VERIFY against NVRHI headers: the correct way to make a buffer carry both a vertex-buffer binding and a structured-buffer SRV (fields `structStride`, `canHaveRawViews`/`canHaveTypedViews`, `isVertexBuffer`), and that a non-permanent (tracked) buffer can transition between `VertexBuffer` and `ShaderResource`. Mirror how any existing engine StructuredBuffer SRV is created (e.g. the instance buffer or `m_PaletteBuffer` in GBufferFillPass).

- [ ] **Step 2: Mirror in `RecreateGpuResources`**

`MeshSystem::RecreateGpuResources` rebuilds buffers from CPU caches on backend hot-swap. Apply the SAME skinned-vs-static buffer-desc difference there (search the file for the recreate path; it mirrors AddMesh's buffer creation).

- [ ] **Step 3: Build the engine + smoke (no behavior change expected)**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Launch the editor with a skinned Fox; confirm it STILL renders + animates exactly as before (the old VS-skinning path still binds these buffers as vertex buffers; the SRV capability is latent). No validation errors.

- [ ] **Step 4: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/MeshSystem.cpp src/engine/src/rendering/MeshSystem.h
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): skinned-mesh input VB + bone buffer SRV-capable (dual-usage) for compute skinning"
```

---

## Task 4: `SkinningComputePass` — dispatch + skinned VB (unconsumed)

> The heart. Create the compute pass, run it before shadow/g-buffer, write the skinned VB + offset table. NOTHING reads it yet — this task's smoke is "the first compute dispatch runs with no validation errors."

**Files:** Create `SkinningComputePass.{h,cpp}`; Modify `Renderer.{h,cpp}`, `src/engine/CMakeLists.txt`

- [ ] **Step 1: Define the cross-pass skinned-geometry interface (Renderer.h)**

Add to `Renderer` a small accessor other passes use to find a skinned entity's pre-skinned geometry:
```cpp
struct SkinnedGeometry {
    nvrhi::IBuffer* skinnedVertexBuffer = nullptr;  // MeshVertex layout; all skinned entities' ranges
    // Returns this entity's first-vertex offset into skinnedVertexBuffer, or -1 if the entity was not
    // skinned this frame (no palette range). baseVertex for the raster draw.
    // Implemented by SkinningComputePass; queried by GBuffer/Shadow.
};
```
Concretely: have `SkinningComputePass` own the buffer + a `std::unordered_map<EntityId, uint32_t> m_OffsetByEntity` (rebuilt per frame) and expose:
```cpp
nvrhi::IBuffer* GetSkinnedVertexBuffer() const;
int64_t GetSkinnedVertexOffset(EntityId e) const; // -1 if not skinned this frame
```
`Renderer` holds the pass and forwards these (or exposes the pass pointer). Pick whichever matches how `Renderer` exposes other passes (it stores passes in `m_RenderPasses` / named members — read `Renderer.h`/`.cpp` and mirror).

- [ ] **Step 2: Create `SkinningComputePass.h`**

```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <unordered_map>
#include <cstdint>
#include "ECS.h"   // EntityId

class Renderer;
struct PaletteFrame;

// First compute pass in the engine. Skins each skinned entity's mesh vertices once (using the
// per-entity palette) into a per-frame skinned vertex buffer, which the G-buffer + shadow passes
// then read through their static pipelines. See docs/superpowers/specs/2026-06-08-compute-skinning-design.md.
class SkinningComputePass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer);
    // Records skinning dispatches for this frame. `palette` is the published PaletteFrame (entity ->
    // palette range) for the snapshot being rendered; null/empty => no skinned entities this frame.
    void Execute(nvrhi::ICommandList* cl, const class ECS* world, const PaletteFrame* palette);

    nvrhi::IBuffer* GetSkinnedVertexBuffer() const { return m_SkinnedVB; }
    int64_t GetSkinnedVertexOffset(EntityId e) const;  // -1 if not skinned this frame

    void DestroyGpuResources();   // backend hot-swap
    bool RecreateGpuResources(nvrhi::IDevice* device);

private:
    void EnsureCapacity(uint32_t totalVertices);   // grow m_SkinnedVB

    nvrhi::IDevice*            m_Device = nullptr;
    Renderer*                  m_Renderer = nullptr;
    nvrhi::ShaderHandle        m_CS;
    nvrhi::ComputePipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle        m_SkinnedVB;        // UAV (write) + VertexBuffer (read); MeshVertex layout
    nvrhi::BufferHandle        m_ParamsCB;         // per-dispatch constants (paletteOffset, outputOffset, vtxCount)
    uint32_t                   m_Capacity = 0;     // current skinnedVB capacity in vertices
    std::unordered_map<EntityId, uint32_t> m_OffsetByEntity;
};
```
(VERIFY `nvrhi::ComputePipelineHandle`, `ComputePipelineDesc`, `BindingSetItem::StructuredBuffer_UAV`/`_SRV`, `ConstantBuffer`, `commandList->dispatch`, `setComputeState`, `requireBufferState`/`setBufferState` against the NVRHI headers; the names above are the expected NVRHI API but confirm.)

- [ ] **Step 3: Implement `SkinningComputePass.cpp`**

The compute shader (HLSL string, mirrors `GBUF_SKINNED_VS_HLSL` math + `SkinVertexCPU`):
```cpp
static const char* SKIN_CS_HLSL = R"(
struct MeshVertex { float3 Position; float3 Normal; float2 UV; };
struct BoneVertex { uint4 BoneIndices; float4 BoneWeights; };
StructuredBuffer<MeshVertex> gIn      : register(t0);
StructuredBuffer<BoneVertex> gBoneIn  : register(t1);
StructuredBuffer<float4x4>   gPalette : register(t2);
RWStructuredBuffer<MeshVertex> gOut   : register(u0);
cbuffer Params : register(b0) { uint gPaletteOffset; uint gOutputOffset; uint gVertexCount; uint _pad; };
[numthreads(64,1,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= gVertexCount) return;
    MeshVertex v = gIn[i];
    MeshVertex o = v; // copies UV through
    if (gPaletteOffset == 0xFFFFFFFFu) { gOut[gOutputOffset + i] = o; return; } // no palette -> bind pose
    BoneVertex b = gBoneIn[i];
    float4x4 skin =
        b.BoneWeights.x * gPalette[gPaletteOffset + b.BoneIndices.x] +
        b.BoneWeights.y * gPalette[gPaletteOffset + b.BoneIndices.y] +
        b.BoneWeights.z * gPalette[gPaletteOffset + b.BoneIndices.z] +
        b.BoneWeights.w * gPalette[gPaletteOffset + b.BoneIndices.w];
    o.Position = mul(skin, float4(v.Position, 1.0)).xyz;
    o.Normal   = mul((float3x3)skin, v.Normal);
    gOut[gOutputOffset + i] = o;
}
)";
```
(NOTE: `MeshVertex` in the engine is `px,py,pz, nx,ny,nz, u,v` floats — confirm the HLSL struct field order/packing matches `MeshVertex`'s memory layout exactly, incl. any padding. The structured-buffer stride must equal `sizeof(MeshVertex)`.)

`Initialize`: `CreateShader(Compute, SKIN_CS_HLSL, 0, "main_cs", "cs_6_1")`; build a `BindingLayout` (t0 mesh SRV, t1 bone SRV, t2 palette SRV, u0 output UAV, b0 params CB); `createComputePipeline({ .CS = m_CS, .bindingLayouts = { m_BindingLayout } })`; create `m_ParamsCB` (volatile/constant). Create `m_SkinnedVB` lazily in `EnsureCapacity` with **both** `isVertexBuffer = true` and UAV-capable (`canHaveUAVs = true` + `structStride = sizeof(MeshVertex)`), initial capacity e.g. 0 (grown on first frame).

`Execute(cl, world, palette)`:
1. If `palette` null/empty → `m_OffsetByEntity.clear()`, return.
2. First pass over the palette's per-entity ranges (which carry `entity` + bone count) — but the SKINNED-VERTEX total is per *mesh vertexCount*, not palette size. So iterate skinned entities: for each entity with a palette range AND a skinned mesh, accumulate `outputOffset` by the mesh's `vertexCount`; record `m_OffsetByEntity[entity] = outputOffset`. Total = sum of vertexCounts. `EnsureCapacity(total)`.
3. `cl->requireBufferState(m_SkinnedVB, UAV/UnorderedAccess)`; for each skinned entity: write `m_ParamsCB = { paletteOffset (from the PaletteFrame range), outputOffset, mesh.vertexCount }`; bind a BindingSet { gIn = mesh VB SRV, gBoneIn = mesh bone-buffer SRV, gPalette = palette SRV, gOut = m_SkinnedVB UAV, Params = m_ParamsCB }; `requireBufferState` the mesh VB + bone buffer to `ShaderResource`; `setComputeState({pipeline, bindings})`; `cl->dispatch(ceil(vtxCount/64), 1, 1)`.
   - NOTE: `m_ParamsCB` is rewritten per dispatch → use a volatile/constant buffer the engine already uses for per-draw CBs (mirror ShadowDepthPass's `m_CB` `writeBuffer` per draw), or pass paletteOffset/outputOffset/vtxCount via a small per-entity constant. If a single CB rewritten per dispatch within one command list is unsafe (overwrites in-flight), use NVRHI's volatile constant buffer or a dynamic offset — VERIFY against how the engine reuses per-draw CBs (ShadowDepthPass writes `m_CB` per entity in the same list, so the pattern exists).
4. `cl->requireBufferState(m_SkinnedVB, ShaderResource? no — VertexBuffer)` so raster can bind it. (Barrier UAV → VertexBuffer.)
5. The palette SRV: this pass needs the same palette `StructuredBuffer` GBufferFillPass uploads from `PaletteFrame`. Today `m_PaletteBuffer` lives in GBufferFillPass. EXTRACT the palette upload to a shared owner (Renderer) so both the compute pass and (until T6) GBuffer can bind it — OR have the compute pass own the palette buffer + upload, and GBuffer reads it from the compute pass during T4/T5. Cleanest: **move the palette buffer + its per-frame upload into `SkinningComputePass`** (it's the natural owner now), expose `nvrhi::IBuffer* GetPaletteBuffer()`, and have GBuffer's (soon-removed) skinned path read it from there. Decide + do this in T4; it removes palette ownership from GBuffer ahead of T6.

`GetSkinnedVertexOffset(e)`: map lookup, -1 if absent.

`EnsureCapacity(total)`: if `total > m_Capacity`, recreate `m_SkinnedVB` at `total` (round up, e.g. *2 growth) with UAV + vertex-buffer usage. (Per-frame; render thread.)

- [ ] **Step 4: Wire into Renderer**

In `Renderer.h`/`.cpp`: construct + `Initialize` the `SkinningComputePass` alongside the other passes (mirror how `ShadowDepthPass`/`GBufferFillPass` are created in `InitForSwap`). In the per-frame render, call `m_SkinningPass->Execute(cl, world, paletteFrame)` **before** the shadow + g-buffer passes (the compute writes the buffer they'll later read). The `paletteFrame` is the same one the renderer already gets for skinning (find where `PaletteFrame`/`m_PaletteBuffer` is sourced — it's loaded from `ApplicationContext::LatestPaletteFrame` on the render thread; pass it in). Add `DestroyGpuResources`/`RecreateGpuResources` calls in the backend-swap paths next to the other passes.

- [ ] **Step 5: CMake + build + smoke**

Add `src/rendering/passes/SkinningComputePass.cpp` to `src/engine/CMakeLists.txt`. Build:
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Launch with a skinned Fox. Expected: **renders exactly as before** (g-buffer still VS-skins, shadow still static — nothing reads the skinned VB yet) AND **no NVRHI validation errors** from the new compute dispatch (check the console). This proves the first compute pass dispatches cleanly. If validation complains about buffer states/bindings, fix here (it's isolated).

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/SkinningComputePass.h src/engine/src/rendering/passes/SkinningComputePass.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/engine/src/rendering/passes/GBufferFillPass.cpp src/engine/CMakeLists.txt
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): SkinningComputePass — first compute pass, writes per-frame skinned VB (unconsumed)"
```
(GBufferFillPass.cpp is staged only if you moved the palette buffer out of it in Step 3.)

---

## Task 5: Shadow pass reads the skinned VB → skinned shadows

**Files:** Modify `src/engine/src/rendering/passes/ShadowDepthPass.cpp`

- [ ] **Step 1: Bind the skinned VB + baseVertex for skinned entities**

In `ShadowDepthPass`'s `Each<TransformComponent, MeshComponent>` draw lambda (~line 164-205), for each entity: query the skinning pass via the Renderer — `int64_t off = renderer->GetSkinningPass()->GetSkinnedVertexOffset(entity);` (or however T4 exposed it; the lambda needs the EntityId — change the `Each` capture to include the id: `[&](EntityId e, const TransformComponent& t, const MeshComponent& m)`). Then:
```cpp
            const int64_t skinnedOff = skinningPass ? skinningPass->GetSkinnedVertexOffset(e) : -1;
            const bool useSkinned = (skinnedOff >= 0) && skinningPass->GetSkinnedVertexBuffer();
            nvrhi::IBuffer* vb = useSkinned ? skinningPass->GetSkinnedVertexBuffer() : res.vertexBuffer;
            const int32_t baseVertex = useSkinned ? (int32_t)skinnedOff : 0;
            ...
            state.vertexBuffers = { nvrhi::VertexBufferBinding(vb, 0, 0) };
            state.indexBuffer = nvrhi::IndexBufferBinding(res.indexBuffer, nvrhi::Format::R32_UINT, 0);
            ...
            // in DrawArguments, set the vertex base:
            a.vertexCount = res.indexCount;     // (existing) index count
            a.instanceCount = 1;
            a.startVertexLocation = baseVertex; // baseVertex into the skinned VB (VERIFY the field name)
```
VERIFY the NVRHI `DrawArguments` field for base-vertex (`startVertexLocation` / `vertexOffset`/`baseVertex`) — match the existing struct. The mesh's index buffer is unchanged (indices are relative to the mesh's vertex range; `baseVertex` shifts them into the entity's skinned range). The `Model` CB stays (shadow VS applies it — correct, since the skinned VB is posed mesh-local).

- [ ] **Step 2: Build + smoke (the payoff)**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Launch, animate the Fox: **its shadow now deforms with the animation** (was bind-pose). Static meshes' shadows unchanged. No validation errors.

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/ShadowDepthPass.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): shadow pass reads compute-skinned VB -> skinned shadows"
```

---

## Task 6: G-buffer reads the skinned VB; remove the VS skinning path

**Files:** Modify `src/engine/src/rendering/passes/GBufferFillPass.cpp` (+ MeshSystem if dropping skinned-VB raster usage)

- [ ] **Step 1: Route skinned entities through the static pipeline**

In the GBuffer batch/draw loop: when a batch's mesh is skinned, emit **one draw per entity** (instanceCount 1) binding the skinned VB with `baseVertex = GetSkinnedVertexOffset(entity)`, through the **static** `m_Pipeline`; static meshes keep the existing instanced draw. The per-entity `InstanceData[0]` (Model/NormalMatrix/BaseColor/Flags) is built as today for that one instance. (The static VS reads `gInstances[InstanceID]` with InstanceID 0 for a 1-instance draw.)

- [ ] **Step 2: Remove the VS skinning path**

Delete `GBUF_SKINNED_VS_HLSL`, `m_SkinnedVS`, `m_SkinnedPipeline`, `m_SkinnedBindingLayout`, the skinned input layout, the `runSkinned` branch, the palette binding @ t6, the slot-1 bone-buffer vertex binding. The palette buffer was moved to `SkinningComputePass` in T4 — GBuffer no longer touches it. Remove `InstanceData.PaletteOffset` from BOTH HLSL `InstanceData` structs and the C++ `MeshInstanceCPU`/instance struct, replacing it with padding to keep the struct 16-byte aligned + matching size across HLSL/C++ (VERIFY the C++ struct + HLSL struct stay byte-identical — they MUST match the StructuredBuffer stride).

- [ ] **Step 3: Build + smoke (no regression)**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Launch: the skinned Fox's G-buffer output (lit result) is **visually identical to before**; shadow still deforms (T5); static meshes unchanged. No validation errors. (If the skinned mesh looks wrong, the compute output or the baseVertex/InstanceData stride is off — `SkinVertexCPU` pins the math; check stride + baseVertex.)

- [ ] **Step 4: (Optional tidy) Skinned-mesh input VB no longer needs vertex-buffer usage**

Skinned-mesh input VBs are now consumed ONLY as compute SRVs (no raster binds them as VBs). You MAY drop `isVertexBuffer` for skinned-mesh input VBs in MeshSystem (pure SRV). Low value + risk; OK to leave dual-usage. If you do it, re-smoke. (Skip unless trivial.)

- [ ] **Step 5: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/GBufferFillPass.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): g-buffer reads compute-skinned VB via static path; remove VS skinning"
```

---

## Task 7: Full build, tests, manual smoke

**Files:** none (verification).

- [ ] **Step 1: Full build**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets link clean.

- [ ] **Step 2: Unit suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_skinning.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_animator.exe
```
Expected: each prints its pass line. (`test_navagent` may be pre-existing RED — ignore.)

- [ ] **Step 3: Manual smoke (report results; do not auto-pass)**

1. Launch editor, play, skinned Fox animating.
2. G-buffer/lit output of the skinned mesh visually identical to pre-feature (no regression).
3. **Shadow deforms with the animation** (was bind-pose) — the PoC payoff.
4. Static meshes + their shadows unchanged.
5. No NVRHI validation-layer errors in the console (compute dispatch + UAV↔vertex-buffer barrier correct).
6. Multiple skinned entities (if present) each animate + shadow independently.

- [ ] **Step 4: Report** per step. On failure, debug via systematic-debugging; commit fixes under the same author. Do not mark complete on failure.

---

## Self-Review (completed during planning)

**Spec coverage:** compute infra — `cs_6_1` (T2) + compute pipeline/UAV/dispatch in SkinningComputePass (T4) ✓; SkinningComputePass per-entity dispatch + skinned VB + offset table (T4) ✓; skinned VB consumed by shadow (T5) + g-buffer (T6) via static path ✓; remove VS skinning (T6) ✓; skinned-mesh input SRV usage (T3) ✓; SkinVertexCPU + test (T1) ✓; smoke incl. skinned shadow + no-regression (T7) ✓. Out-of-scope (batched dispatch, vertex-pulling, async, other compute uses) absent.

**Type consistency:** `SkinVertexCPU` sig (T1) matches the CS math (T4). `SkinningComputePass::GetSkinnedVertexBuffer()/GetSkinnedVertexOffset()` defined T4, used T5/T6. `MeshVertex`/`SkinnedVertex` strides drive both the SRV structStride (T3) and the CS struct layout (T4) — flagged to verify byte-match. `InstanceData.PaletteOffset` removed in T6 from HLSL + C++ together (stride match flagged).

**Verify-at-implementation points (flagged inline, real fallbacks):** exact NVRHI signatures (compute pipeline/UAV/dispatch/barrier/DrawArguments base-vertex — T4/T5, "verify against headers + mirror graphics equivalents"); `MeshVertex` HLSL-struct vs C++ byte layout (T4); the per-dispatch CB reuse safety (T4 Step 3, mirror ShadowDepthPass's per-draw CB); whether `cs_6_1` needs any ShaderCompiler change (T2, throwaway smoke confirms); palette-buffer ownership move GBuffer→SkinningComputePass (T4 Step 3). Each has a concrete in-task resolution.

---

## Execution note

T1 pure TDD; T2 confirms compute compiles; T3 latent buffer-usage change (no behavior); T4 the first compute pass running UNCONSUMED (isolates compute/validation correctness); T5 delivers the visible payoff (skinned shadows); T6 unifies + removes VS skinning. NVRHI specifics (compute pipeline, UAV, barriers, base-vertex) are verified against the headers + existing graphics-pass idioms, not guessed. No `ECS.h` change → normal editor relaunch to smoke, no restart caveat.
