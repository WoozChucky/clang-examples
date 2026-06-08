# Animation SP2 — Static bind-pose GPU skinning — Design

**Date:** 2026-06-08
**Branch:** `feat/anim-skinning`
**Status:** Approved (brainstorm) — ready for implementation plan

## Context

Sub-project 2 of the animation effort (SP1 import+viz → **SP2 skinning** → SP3 clip playback → SP4+
blend/IK; see [[project_animation]]). SP1 shipped (merged): `Skeleton` asset + `SkeletonStore`
singleton + `SkeletonComponent` builtin + glTF bone extraction (`AiToGlm` transpose, topo-ordered
bones, `inverseBind` captured) + bind-pose debug-line viz + a `SkeletonEditor` inspector picker.

SP2 makes a rigged mesh actually **deform on the GPU** via skeletal skinning in the deferred
G-buffer pass, driven by a per-entity bone-matrix palette. The bind-pose palette is mathematically
identity (`global_bind * inverseBind = I`), so a skinned mesh renders *identical* to unskinned —
that is the correctness baseline. A throwaway debug bone-wiggle provides visible proof that the
skinned path deforms vertices. SP3 replaces the static pose with animation-clip sampling.

### As-built facts (verified)
- `MeshVertex { float px,py,pz, nx,ny,nz, u,v }` (`ApplicationContext.h:67`), 32 bytes, no bone data.
- **Shaders are inline HLSL string literals** compiled per-`CreateShader` (DXC) — `GBUF_VS_HLSL`
  (`GBufferFillPass.cpp:16-29`), `GBUF_PS_HLSL` (`:31-50`). `ShaderCompiler` has **no `-D`/permutation
  and no `#include`** — a skinned VS is a separate source string + separate PSO.
- Per-entity data flows via **GPU instancing + a `StructuredBuffer` at `t5`** (`InstanceData{Model,
  NormalMatrix, BaseColor, Flags}` indexed by `SV_InstanceID`); `b0` PerFrame CB holds only `VP`.
  Pipeline + input layout (`GBufferFillPass.cpp:66-73`, 3 attrs, `bufferIndex(0)`, stride
  `sizeof(MeshVertex)`) + binding layout (`:76-92`) + per-draw loop (`:230-360`, fills `m_InstanceBuffer`,
  one `BindingSetDesc` per submesh, `drawIndexed(instanceCount=…)`). The input layout is **duplicated
  in `ShadowDepthPass.cpp:42-49`**.
- `MeshEntry` (`MeshSystem.h:87-100`): `key, vertexBuffer, indexBuffer, subMeshes, counts, bounds,
  cpuVertices, cpuIndices`. No skinned flag. `AddMesh` takes `const MeshVertex*` only; CPU copies
  retained for hot-swap (`RecreateGpuResources`).
- **Bone weights are NOT read today.** The async startup load (`assets/models` scan) runs in
  `GameThread::WorkerThreadFunc` (the `processMesh` lambda builds pos/normal/uv); SP1's skeleton
  extraction is in that same function. `aiMesh->mBones[b]->mWeights[w]` give `{mVertexId, mWeight}`.
  (The editor file-load path `MeshLoader::ProcessMesh` is separate; SP2 targets the startup path that
  loads RiggedSimple — the editor path can gain weights later, out of scope.)
- Cross-thread: ECS snapshot via `std::atomic<std::shared_ptr<const ECS>> LatestWorldSnapshot`
  (`ApplicationContext.h:236`; publish `GameThread.cpp` PublishSnapshot, load `RenderThread.cpp:186`).
  `Seqlock<T>` requires trivially-copyable `T` → **cannot** carry a variable-length matrix palette.
- Skinned-ness detection precedent: `DebugRenderPass.cpp:215` does `Each<TransformComponent,
  SkeletonComponent>` + `SkeletonStore::Instance().Get(...)`.
- `MeshRequest` (the Game→Render ring upload struct, `ApplicationContext.h:93-101`) carries
  `MeshVertex* Vertices` — a skinned format affects this marshalling.

## Goal

A rigged glTF entity deforms via GPU skeletal skinning in the G-buffer, using a per-entity
bone-matrix palette. SP2 computes the bind-pose palette (identity baseline) and adds a debug
bone-wiggle for visible proof. Static (non-skinned) rendering is untouched.

## Non-goals (later sub-projects / follow-ups)

- No `AnimationClip`, keyframe sampling, or `AnimationComponent` (SP3). The debug wiggle is a
  throwaway placeholder for real sampling.
- No skinned **shadows** — `ShadowDepthPass` stays static for SP2 (bind-pose shadow ≈ correct;
  skinned shadow is a follow-up).
- No CPU skinning fallback; no >4 influences per vertex (top-4 + renormalize).
- No weights via the editor file-load path (`MeshLoader::ProcessMesh`) — startup path only.
- No new ECS component and **no `ECS.h`/X-macro change** (skinned-ness is a mesh property +
  `SkeletonComponent` reuse; palette is engine-side). No `GAME_API_VERSION` bump.
- No autobind of skeleton↔mesh (manual pickers from SP1 suffice for testing; autobind is a later nicety).

## Components

### 1. Skinned vertex data — parallel bone buffer
- New `struct SkinnedVertex { glm::uvec4 BoneIndices; glm::vec4 BoneWeights; }` (common header next to
  `MeshVertex`). 32 bytes; bound at `bufferIndex(1)` only for the skinned PSO. `MeshVertex` is
  **unchanged** — static meshes, `ShadowDepthPass`, and the static input layout pay nothing.
- **Extraction** (in `WorkerThreadFunc`, together with SP1's skeleton extraction so bone indexing is
  consistent): build a `std::vector<SkinnedVertex>` parallel to the mesh's vertices. For each
  `aiMesh->mBones[b]` (mapped by `mName` → the Skeleton's bone index — the SAME name→index map used to
  build the Skeleton), for each weight `{mVertexId, mWeight}`, accumulate into that vertex's influence
  list; keep the **top 4** by weight, renormalize to sum 1 (a vertex with 0 bones → index 0, weight 0).
  Carried in `ModelLoadResult` (`bool hasSkinning; std::vector<SkinnedVertex> skinning;`).
  - NOTE on `aiProcess_JoinIdenticalVertices`: the mesh's `mNumVertices` is post-join; `mWeights`
    reference the same post-join vertex ids, so weights stay aligned. (Verify; if join reorders,
    extraction still keys by `mVertexId` so it remains correct.)
- **Ring:** add `SkinnedVertex* BoneData` (null for static) to `MeshRequest`; GameThread acquires a
  staging copy like it does for `Vertices`; RenderThread passes it to `AddMesh`.
- **MeshSystem:** `MeshEntry` gains `bool isSkinned = false;`, `nvrhi::BufferHandle boneBuffer;`,
  `std::vector<SkinnedVertex> cpuSkinning;` (hot-swap replay). `AddMesh` extended with optional
  `const SkinnedVertex* boneData` (null → static, as today). `MeshResources` (the render-side view)
  gains `bool isSkinned; nvrhi::IBuffer* boneBuffer;`. `RecreateGpuResources` replays `cpuSkinning`
  into a new `boneBuffer` when `isSkinned`.

### 2. Per-entity bone palette + cross-thread transport
- `struct PaletteFrame { std::vector<glm::mat4> matrices; struct Range { EntityId entity; uint32_t
  offset; uint32_t count; }; std::vector<Range> ranges; }` — a flat matrix array + per-entity ranges
  into it (the instanced-architecture fit: one structured buffer, per-instance base offset).
- Published as `std::atomic<std::shared_ptr<const PaletteFrame>> LatestPaletteFrame;` in
  `ApplicationContext` (parallel to `LatestWorldSnapshot`; immutable once published; `shared_ptr`
  because it carries a `std::vector` — Seqlock can't).
- **Skinning step (GameThread, inline in `RunLoop`, after game update, immediately before publishing
  the snapshot so it reflects the same tick):** build a fresh `PaletteFrame`. For each entity with a
  `SkeletonComponent` whose `SkeletonStore::Get` resolves: compute `globals = ComputeBindPoseGlobals(sk)`;
  `palette[b] = globals[b] * sk.bones[b].inverseBind`; append the `count` matrices to `matrices`,
  record a `Range{entity, offset, count}`. **Debug wiggle:** if `GetDebugDrawSettings().SkinTest` (new
  flag) is set, before the multiply, multiply one chosen bone's `global` (e.g. the last/leaf bone) by a
  time-based rotation (`rotate(sin(t)*angle, axis)`) — `t` from the tick timestamp. Atomic-store the
  frame.
  - Engine-side, inline (not an ECS `ISystem` / not in `Game.dll`): skinning is an engine service and
    must read the post-update ECS + `SkeletonStore` on the GameThread.
- **RenderThread:** atomic-load `LatestPaletteFrame` alongside the ECS snapshot. Upload
  `frame->matrices` into a palette `StructuredBuffer` (`t6`; grow on demand; same Vulkan
  register-offset handling as `t5`). Build a transient `EntityId → offset` lookup from `ranges` for the
  draw loop.

### 3. Skinned PSO + skinning vertex shader (GBufferFillPass)
- New inline VS `GBUF_SKINNED_VS_HLSL`: `VSIn` adds `uint4 BoneIndices:BLENDINDICES; float4
  BoneWeights:BLENDWEIGHT;`. `InstanceData` adds `uint PaletteOffset;` (+ adjust padding). Declare
  `StructuredBuffer<float4x4> gBones : register(t6);`. Compute
  `float4x4 skin = w.x*gBones[off+i.x] + w.y*gBones[off+i.y] + w.z*gBones[off+i.z] + w.w*gBones[off+i.w];`
  then `pos = mul(skin, float4(Position,1)); nrm = mul((float3x3)skin, Normal);` BEFORE the existing
  `inst.Model` multiply. PS (`GBUF_PS_HLSL`) **unchanged**.
- New `m_SkinnedPipeline` (`GraphicsPipelineHandle`, lazily created like `m_WireframePipeline`):
  skinned VS + an input layout = the 3 MeshVertex attrs (`bufferIndex(0)`) + `BLENDINDICES`
  (`RGBA32_UINT`) + `BLENDWEIGHT` (`RGBA32_FLOAT`) at `bufferIndex(1)`; binding layout adds the `t6`
  palette `StructuredBuffer_SRV`. Same depth/cull/blend state as the static pipeline.
- **Draw loop:** partition the gathered entities into static vs skinned (skinned = mesh
  `isSkinned` AND entity has `SkeletonComponent` AND the palette lookup has its entity). Skinned batch:
  fill instances with `PaletteOffset` from the lookup, bind `vertexBuffer`@0 + `boneBuffer`@1 + palette
  SB@t6, use `m_SkinnedPipeline`, `drawIndexed`. Static batch: **exactly as today**, untouched.

### 4. Debug flag
`DebugDrawSettings` (`RenderStats.h`) gains `bool SkinTest = false;` + a RenderStatsPanel checkbox
("Skin Test (wiggle bone)"). Read by the GameThread skinning step. Off by default + runtime → bind-pose
identity (no visible change).

## Data flow

glTF in `assets/models` → `WorkerThreadFunc`: build `MeshVertex` VB **+** `SkinnedVertex` bone data
(indices mapped to the Skeleton's bone order) **+** `Skeleton` → `ModelLoadResult` → mesh+bone buffers
to `MeshSystem` (`isSkinned`) via the ring; `Skeleton` to `SkeletonStore`. Per tick → GameThread
skinning step computes per-entity `palette = global*inverseBind` (+ optional wiggle) → `PaletteFrame`
atomic-published. Per frame → RenderThread loads the frame, uploads the palette SB, and the skinned
PSO blends bones in the VS → deformed vertices into the G-buffer (rest of deferred pipeline unchanged).

## Error handling / edge cases

- **`SkeletonComponent` on a non-skinned mesh** (no `boneBuffer`) → static path (can't skin without
  weights) + `SM_WARN` once per mesh.
- **Skinned mesh, no `SkeletonComponent` / no palette range** → static path (renders via `inst.Model`
  only; undeformed). No crash.
- **`LatestPaletteFrame` null** (first frames before the first skinning step) → all skinned entities
  fall back to the static path that frame.
- **Vertex with >4 influences** → top-4 by weight, renormalized (extraction). **0 influences** →
  bone 0, weights 0 (the VS `skin` becomes a zero matrix → guard: if total weight ~0, fall back to
  identity in the VS, OR ensure extraction writes weight (1,0,0,0) on bone 0 for unweighted verts).
  Plan picks: extraction guarantees weights sum to 1 (unweighted → `(1,0,0,0)` on bone 0), so the VS
  needs no special-case.
- **Palette buffer overflow** (matrices exceed the SB capacity) → grow the SB; if a hard cap is hit,
  clamp + `SM_WARN`.
- **Bone count per skeleton vs a shader max** — palette is a `StructuredBuffer` (dynamically sized),
  so no fixed shader array cap; only the SB capacity matters.

## Testing

- **Unit (`tests/test_skinning.cpp`, new):**
  - Weight extraction helper (factor it into a pure function `BuildSkinnedVertices(...)` or test the
    top-4/normalize logic on a synthetic influence set): >4 influences → top-4 kept, weights sum to 1;
    unweighted vertex → `(1,0,0,0)`.
  - Bind-pose palette: for the synthetic 2-bone `Skeleton` (from SP1's test shape), `palette[b] =
    ComputeBindPoseGlobals(sk)[b] * inverseBind[b]` ≈ identity when `inverseBind = inverse(globalBind)`
    (construct the skeleton so this holds) — proves the SP2 palette math + `inverseBind` usage.
- **Build-verified:** skinned VS compiles (DXC), `m_SkinnedPipeline` creates, input layout valid.
- **Manual smoke (human-owned):** assign RiggedSimple's (now skinned) mesh + its skeleton to an
  entity. (a) At rest the mesh renders **identical** to before (bind-pose identity). (b) Toggle
  `SkinTest` → the wiggled bone visibly deforms the mesh (and the SP1 debug skeleton can be shown
  alongside). (c) Static meshes (existing `.obj`s) render unchanged. (d) Toggle off → back to rest.
- **Regression:** `test_skeleton`, `test_skinning`, `test_assetkey`, `test_ecs`, `test_worldserial`,
  `test_compserial`, `test_reloadpreserve`, `test_playermove` green; full tree builds.

## Done criteria

- `SkinnedVertex` + extraction (top-4, normalized, bone indices aligned to the Skeleton); `MeshEntry`
  `isSkinned`/`boneBuffer`; `MeshRequest`/`AddMesh` carry optional bone data; hot-swap replays it.
- `PaletteFrame` published per tick via `atomic<shared_ptr>`; GameThread skinning step computes
  `global*inverseBind` (+ `SkinTest` wiggle); RenderThread uploads the palette SB.
- Skinned PSO + skinning VS in GBufferFillPass; skinned entities deform; **static path unchanged**.
- RiggedSimple renders identical at rest; `SkinTest` visibly deforms it (manual smoke).
- Unit tests (extraction + bind-pose identity) green; full tree builds; the suite stays green.
- No `ECS.h` change, no `GAME_API_VERSION` bump. (`ApplicationContext.h` + rendering + GameThread
  change ⇒ rebuild Engine + editor + game; ECS untouched.)

## Notes

- Bind-pose palette is identity by construction, so SP2's correctness is "renders identical at rest";
  the wiggle is the visible proof. SP3 keeps the entire SP2 pipeline and only replaces
  `ComputeBindPoseGlobals` with clip-sampled globals + a `global = parent*localAnimated` walk.
- `PaletteFrame` mirrors the `LatestWorldSnapshot` atomic-`shared_ptr` publish exactly — the same
  load-before-use ordering on the RenderThread applies.
- Flat palette + per-instance `PaletteOffset` reuses the existing instanced `StructuredBuffer`
  architecture (no per-entity buffers, no new CB).
- `ShadowDepthPass` left static (its duplicated input layout is untouched); skinned shadows are a
  deliberate follow-up.
- Editor file-load skinning (`MeshLoader::ProcessMesh`) deferred — only the startup async path
  (which loads RiggedSimple) extracts weights in SP2.
