# Compute Skinning Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/compute-skinning`
**Depends on:** the skeletal-animation pipeline (SP1–SP5, merged): `Skinning.h`, the GameThread palette computation, the `PaletteFrame` transport, the GBuffer skinned VS path, MeshSystem bone buffers.

## Goal

The engine's **first compute pass**: skin each skinned mesh's vertices **once** in a compute shader into a per-frame vertex buffer, then have the G-buffer **and** shadow passes read that buffer through their ordinary static pipelines. Payoff: **skinned meshes finally cast correctly-deformed shadows** (the shadow pass is currently static → bind-pose shadows), a single skinning implementation (no per-pass VS re-skin), and a reusable compute-pass scaffold for future GPU work (SSAO blur, culling, particles).

This is a proof-of-concept for compute work in the engine, chosen because the payoff is concrete and the palette plumbing already exists.

## Current state (what we're changing)

- **Skinning is done in the vertex shader.** `GBUF_SKINNED_VS_HLSL` (GBufferFillPass) computes `skin = Σ weightᵢ·gBones[paletteOffset + boneIdxᵢ]` per vertex, `pos' = mul(skin, pos)`, `n' = mul((float3x3)skin, normal)`, then `mul(Model, pos')`. The palette is `m_PaletteBuffer` (`StructuredBuffer<float4x4>` @ t6); the bone idx/weights ride a parallel vertex buffer (slot 1); per-instance `InstanceData.PaletteOffset` selects the entity's palette slice (sentinel `0xFFFFFFFF` = no palette → bind pose).
- **ShadowDepthPass is static:** `main_vs` is `mul(LightVP, mul(Model, pos))` with no skinning and no bone buffer → **skinned meshes cast bind-pose shadows** (the known follow-up bug).
- **No compute scaffolding exists** anywhere in the renderer — `ShaderCompiler` compiles VS/PS only; there is no `createComputePipeline`/`dispatch`/UAV path. This feature adds it.

## Architecture & data flow

The palette pipeline is **unchanged**. GameThread computes per-entity palettes (`global*inverseBind`) → `PaletteFrame` → RenderThread uploads to `m_PaletteBuffer` + per-entity palette offsets. We insert a compute stage that consumes the palette and produces posed geometry.

```
GameThread: palette (global*inverseBind) ──► PaletteFrame ──► RenderThread: m_PaletteBuffer (SRV) + per-entity paletteOffset
                                                                       │
RenderThread frame:                                                    ▼
  [SkinningComputePass]  per skinned entity: Dispatch(ceil(vtxCount/64))
       inputs (SRV): mesh positions/normals/uv, mesh bone idx/weights, m_PaletteBuffer (+ paletteOffset, outputOffset, vtxCount)
       output (UAV): skinnedVB[outputOffset + i] = { pos', n', uv }      (posed MESH-LOCAL)
       side table:  entity → outputVertexOffset
       barrier: UAV write ──► VertexBuffer read
  [ShadowDepthPass]   skinned entity: static VS, bind skinnedVB, mesh IB, drawIndexed(idx,1,0, baseVertex=outputOffset)  → skinned shadow
  [GBufferFillPass]   skinned entity: static VS, bind skinnedVB, mesh IB, drawIndexed(idx,1,0, baseVertex=outputOffset)
                      static meshes: unchanged (per-mesh instanced batch)
```

**Why the static path "just works" for skinned entities:** the compute shader outputs the *posed mesh-local* vertex (the old VS's `skinned` value, before `Model`). The static VSes already apply `Model`/`NormalMatrix` (G-buffer) and `Model` (shadow). So a skinned entity drawn through the static pipeline, reading pre-skinned mesh-local verts, produces exactly the same world-space result the old skinned VS did — and the shadow pass, applying the same `Model`, now deforms with the pose.

**Per-entity draws (a deliberate trade):** today skinned entities of the same mesh draw in one instanced call (each picks `PaletteOffset` in the VS). After pre-skinning, each entity has a distinct skinned-VB range, so each is its own draw (`instanceCount=1`, `baseVertex=outputOffset`). Static meshes keep their instanced batching. For the handful of skinned characters this engine targets, the trade (more draws, but skin-once + correct shadows + one code path) is the right one. Batched/instanced skinned draws are a scale follow-up.

## Compute infrastructure (the genuinely new part)

- **`ShaderCompiler`:** add a compute target profile (`cs_6_x` via DXC) alongside the existing VS/PS compilation.
- **NVRHI compute path:** `createComputePipeline` + a compute `BindingLayout` (SRVs: mesh positions/normals, bone idx/weights, palette; UAV: skinned VB) + `nvrhi::ComputeState` + `commandList->dispatch(...)`. Wrap the boilerplate in a small reusable helper so the next compute pass (SSAO blur, culling, particles) reuses the scaffold rather than re-deriving it.
- **Resource-state barrier:** transition the skinned VB from UAV (compute write) to VertexBuffer (raster read) between the compute pass and the first raster pass that reads it (NVRHI `setBufferState` / automatic-state tracking — match the engine's existing barrier convention).

## SkinningComputePass

A new pass (`src/engine/src/rendering/passes/SkinningComputePass.{h,cpp}`) owning the compute pipeline + shader, the per-frame skinned VB, and the entity→offset table.

- **Per skinned entity** (one that has a palette range this frame): `Dispatch(ceil(vertexCount / 64))`, threadgroup size 64, one thread per vertex.
- **Compute shader (HLSL string, like the existing VS strings):**
  ```
  for vertex i in [0, vertexCount):
    if paletteOffset == 0xFFFFFFFF: out = { pos, normal, uv }           // no palette → copy unskinned (bind pose)
    else:
      skin = w.x*gBones[paletteOffset+idx.x] + ... + w.w*gBones[paletteOffset+idx.w]
      out.pos    = mul(skin, float4(pos,1)).xyz
      out.normal = mul((float3x3)skin, normal)
      out.uv     = uv
    skinnedVB[outputOffset + i] = out
  ```
  Identical math to the current `GBUF_SKINNED_VS_HLSL`, just writing to a buffer instead of feeding the rasterizer.
- **Inputs:** mesh positions/normals/uv (the mesh `MeshVertex` VB as an SRV), the mesh bone buffer (idx/weights, SRV), `m_PaletteBuffer` (SRV); a small uniform/push block: `paletteOffset`, `outputOffset`, `vertexCount`.
- **Output:** the per-frame skinned VB range (UAV).
- The pass builds the `entity → outputVertexOffset` table while assigning ranges, and exposes it to the raster passes (shared via the Renderer, like other cross-pass state).

## Raster changes

- **GBufferFillPass — remove the VS skinning path.** Delete `GBUF_SKINNED_VS_HLSL`, `m_SkinnedPipeline`, `m_SkinnedBindingLayout`, the skinned input layout, the palette binding @ t6, the slot-1 bone-buffer vertex binding, and the `runSkinned` branch. Skinned entities route through the existing **static** pipeline: one draw per skinned entity with `baseVertex = entity outputOffset` into the skinned VB, `instanceCount = 1`, its `InstanceData[0]` supplying Model/NormalMatrix/BaseColor/Flags. The batch loop branches: **mesh-static → instanced draw (as today); mesh-skinned → per-entity static draws.** Remove `InstanceData.PaletteOffset` from the struct (no VS reads it anymore; the compute pass owns offsets) — replace with `_pad` to keep the struct layout/size stable, or shrink it (decide in the plan; keep 16-byte alignment).
- **ShadowDepthPass — skinned entities read the skinned VB.** For a skinned entity, bind the skinned VB + `baseVertex`, draw through the static depth VS; static meshes unchanged. This is the change that makes shadows track the pose.
- **MeshSystem** still owns the per-mesh static VB + IB + bone buffer (input to compute); no change to static-mesh rendering.

## Buffer usages

- **Mesh input buffers (skinned meshes only) need SRV access:** the `MeshVertex` VB and the bone buffer must be readable by the compute shader (structured/raw SRV view). Add the SRV usage flag for skinned meshes in `MeshSystem::AddMesh` (static meshes unaffected — no SRV needed).
- **Skinned VB needs UAV (compute write) + VertexBuffer (raster read):** created with both usages; per-frame; render-thread-owned; grow-on-demand sized to the frame's total skinned vertex count (sum over skinned entities). Layout = `MeshVertex` (pos/normal/uv), so the static input layouts bind it unchanged.
- The `entity → outputVertexOffset` table is rebuilt each frame alongside range assignment.

## Testing

- The compute pass runs on the GPU and is not unit-testable directly. Pin the **skin-math contract** instead: add a pure `SkinVertexCPU(...)` to `src/common/include/Skinning.h` (given the palette, 4 bone indices+weights, and a position/normal → skinned position/normal) + a unit test. The compute shader mirrors this exactly, giving a verified reference for the math.
- Smoke (visual): skinned shadow deforms with the animation (payoff); a skinned mesh's G-buffer output is visually identical to before (no regression from dropping the VS skinning); static meshes + their shadows unchanged; no validation-layer errors (UAV↔vertex-buffer barrier correct).
- Existing unit suites stay green (the change is renderer-side; `test_skinning` continues to cover `MakeSkinnedVertex`/`ComputeSkinningPalette`).

## Scope

**In:** compute infrastructure (`cs_6_x` profile + reusable compute-pipeline/dispatch helper); `SkinningComputePass` (per-entity dispatch, skinned-VB output, entity→offset table); skinned-VB consumed by **both** G-buffer and shadow via the static path; removal of the GBuffer VS skinning path; skinned-mesh input-buffer SRV usage; `SkinVertexCPU` + unit test.

**Out (later):** batched single-dispatch (scale to many skinned entities); vertex-pulling; async compute; applying the compute scaffold to other work (SSAO blur, GPU culling, particles); skinned-entity instancing of the compute output; render-side pose interpolation.

## Smoke test

1. Build (`msvc-win64-vs2026-community`), launch, enter play with a skinned Fox animating.
2. The skinned mesh renders **identically to before** in the G-buffer (no visual regression from removing the VS skinning path).
3. **Its shadow deforms with the animation** (previously a static bind-pose shadow) — the visible PoC payoff.
4. Static (non-skinned) meshes and their shadows are unchanged.
5. No NVRHI validation-layer errors (the compute UAV → vertex-buffer barrier is correct; backend = the default the editor launches with).

## Risks / notes

- **First compute pass = new NVRHI surface.** `createComputePipeline`, compute binding layouts, UAV creation, and the compute↔raster barrier are all new to this codebase; budget for getting the validation layer happy. Keep the helper minimal but correct.
- **Mesh-VB-as-SRV:** the mesh vertex/bone buffers were created as vertex buffers; adding SRV usage may require a buffer-desc flag (and possibly a structured/typed view). Confirm the NVRHI buffer can carry both VertexBuffer and ShaderResource usage on the target backend; if not, the compute pass reads from a structured-buffer copy of the bone/vertex data (decide in the plan — prefer dual-usage if the backend allows).
- **Per-entity draws** increase skinned draw count vs today's instanced skinned draw. Acceptable for few characters; documented as a scale follow-up. Static-mesh instancing is untouched.
- **Removing the working VS skinning path** is the riskiest edit (it's tested + correct today). The smoke step 2 (G-buffer visually identical) is the guard; the `SkinVertexCPU` reference pins the math so the compute output matches the old VS.
- **Backends:** the engine supports DX12 + Vulkan. Compute + UAV + the barrier must work on whichever the smoke runs (DX12 default); Vulkan parity verified if/when that backend is smoked (note any divergence).
