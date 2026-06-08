# SSAO Compute Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/ssao-compute`
**Depends on:** the SSAO pass (`SsaoRenderPass`, shipped 2026-05-29) + the compute scaffold from compute-skinning (merged `cc0d72a`: `createComputePipeline`/`ComputeState`/`dispatch`, the NVRHI flat-binding-unique-slots rule).

## Goal

Convert the SSAO pass from two full-screen pixel sub-passes to two **compute dispatches** (the engine's second compute pass), and add a **groupshared-memory (LDS) tile cache** to the blur — demonstrating the canonical "compute is better for blur" advantage and broadening the compute scaffold to **RWTexture2D UAV** image writes (skinning used buffer UAVs). Visual output stays identical; this is a GPU-architecture exercise + a foundation (HBAO/GTAO, separable/bilateral blur, half-res) that all want compute.

## Current state

`SsaoRenderPass` (`src/engine/src/rendering/passes/SsaoRenderPass.{h,cpp}`) is an ordered `IRenderPass` between GBufferFill and Lighting. Two full-screen pixel sub-passes (`draw(3)`, no VB):
- **AO sub-pass:** reads G-buffer Normal (t1) / WorldPos (t2) SRVs + a point sampler (s3) + `SsaoCB` (b0: 16-sample hemisphere kernel, RT size, Radius/Intensity/Power/Bias). Per-pixel rotation **tiled to a 4×4 grid** (the grain fix — the tile size MUST match the blur's NxN). Range-weighted view-Z occlusion (`occZ > sZ + bias`; RH view-Z negative → closer=larger), `ao = pow(1-occ·Intensity, Power)`. Writes raw AO to `m_SsaoRaw` (R8 render target).
- **Blur sub-pass:** `BLUR_PS` reads raw AO (t1) + sampler (s3), 4×4 box, writes `m_SsaoBlur` (R8). Lighting samples `m_SsaoBlur` at **t8** and multiplies the **ambient term only** by AO.

Textures `m_SsaoRaw`/`m_SsaoBlur` + their framebuffers are Renderer-owned (`GetSsaoRawTexture`/`GetSsaoRawFramebuffer`/`GetSsaoBlurTexture`/`GetSsaoBlurFramebuffer`), resize-tracked. The compute scaffold (createComputePipeline, etc.) exists from `SkinningComputePass`; this is the first compute pass to write **textures** (UAV images) rather than buffers.

## Architecture

`SsaoRenderPass` becomes a **compute pass** — two dispatches, no rasterization, no framebuffers. It stays an ordered `IRenderPass` in the same slot (GBufferFill → **SSAO** → Lighting); `Execute` records compute dispatches instead of `draw(3)`s.

```
[GBufferFill]  Normal(t1)/WorldPos(t2) G-buffer SRVs
[SsaoComputePass.Execute(cl)]
   set m_SsaoRaw  -> UnorderedAccess
   AO dispatch   ([numthreads(8,8,1)], ⌈w/8⌉×⌈h/8⌉): gNormal SRV, gWorldPos SRV, point sampler, SsaoCB  -> RWTexture2D rawAO
   set m_SsaoRaw  -> ShaderResource ; set m_SsaoBlur -> UnorderedAccess          (UAV→SRV barrier)
   Blur dispatch ([numthreads(8,8,1)], LDS tile+apron): rawAO SRV  -> RWTexture2D blurAO
   set m_SsaoBlur -> ShaderResource                                              (UAV→SRV barrier)
[Lighting]  samples blurAO (t8)  (unchanged)
```

Reuses the `SkinningComputePass` scaffold (pipeline/state/dispatch) + the **flat-binding-unique-slots rule** (globally-unique slot numbers across t/u/b/s within a pipeline; VulkanBindingOffsets 0).

## AO dispatch (port the AO pixel shader → compute)

`[numthreads(8,8,1)]`, dispatch `⌈width/8⌉ × ⌈height/8⌉`; one thread per pixel; bounds-guard threads ≥ render size (`if (px >= w || py >= h) return;`). Port the AO math **verbatim** from the AO PS:
- Read `gNormal`/`gWorldPos` G-buffer SRVs at the pixel.
- 16-tap hemisphere kernel (`SsaoCB`), rotated per-pixel **tiled to a 4×4 grid** (unchanged — keep the grain-fix tiling so the 4×4 blur cancels it cleanly).
- **Keep the point sampler:** the kernel reprojects each sample to arbitrary screen UVs and samples `gWorldPos` there → needs `Sample` (point), not `Load` at the current pixel.
- Range-weighted view-Z occlusion + `ao = pow(1-occ·Intensity, Power)`. Identical to today.
- Write `RWTexture2D<float> rawAO[px]`.

Output must be bit-for-bit equivalent in intent to the AO PS (visual parity).

## Blur dispatch — LDS tile cache (box fallback)

Thread per pixel, `[numthreads(8,8,1)]` groups. Steps:
1. **Cooperative tile load:** each group loads its 8×8 tile **+ a 2-px apron** (the 4×4 box reaches ±2) of raw AO into `groupshared float tile[(8+4)*(8+4)]` — each texel fetched once via `Load` (or `[]` index on the SRV at integer coords). Apron texels outside the image clamp to the edge pixel.
2. `GroupMemoryBarrierWithGroupSync()`.
3. Each thread averages its **4×4 box from LDS** (identical kernel/extent to `BLUR_PS` → visual parity) and writes `blurAO[px]`.

**Sequencing safeguard (per the design decision):** implement the **straight 4×4 box (direct texture reads, no LDS) FIRST** as its own task step, confirm visual parity, THEN layer the LDS tile cache as a separate step. If LDS misbehaves (parity break / validation error), revert that step and keep the working box port. The plan structures these as distinct, independently-committable steps.

## Texture + barrier changes

- **`m_SsaoRaw` / `m_SsaoBlur`:** add UAV usage to the Renderer-owned texture descs (`isUAV` / `.setIsUAV(true)` — verify the exact NVRHI `TextureDesc` field). Each is now compute-written (UAV) + read (SRV). Keep R8 format + render-size tracking + the existing SRV accessors. The AO/blur **framebuffers are removed** (no render targets) — drop `GetSsaoRawFramebuffer`/`GetSsaoBlurFramebuffer` and their creation; keep the texture + SRV getters.
- **Barriers:** `setTextureState(m_SsaoRaw, UnorderedAccess)` before the AO dispatch; `→ ShaderResource` after (before the blur reads it); `setTextureState(m_SsaoBlur, UnorderedAccess)` before the blur dispatch; `→ ShaderResource` after (Lighting reads it). Verify the NVRHI texture-state call name (`setTextureState`/`requireTextureState`) + that NVRHI's automatic barriers commit them at the next `setComputeState`/`setGraphicsState` (mirror how SkinningComputePass used `setBufferState` for the buffer UAV→VB transition). The AO→blur barrier MUST sit between the two dispatches.

## Binding slots (flat-unique)

NVRHI flat-binding rule — globally-unique slot numbers across t/u/b/s within each pipeline, VulkanBindingOffsets all 0:
- **AO pipeline:** `b0` SsaoCB, `t1` Normal SRV, `t2` WorldPos SRV, `s3` point sampler, `u4` rawAO UAV.
- **Blur pipeline:** `b0` SsaoCB (reused; only RT/inv-size fields used), `t1` rawAO SRV, `u2` blurAO UAV. (LDS is shader-internal — no binding.)

(`Texture_UAV` binding items — the new binding kind vs skinning's `StructuredBuffer_UAV`.)

## Testing

- Compute is GPU → not unit-testable directly. The AO math's pure helpers already have coverage in `tests/test_ssaomath.cpp` (`MakeHemisphereKernel`, etc.); they're unchanged (only the host shader moves PS→CS) — that suite stays green and continues to cover the kernel logic. No new unit test unless additional pure math is extracted.
- Smoke = visual parity (below) + no validation errors.
- Existing unit suites stay green (renderer-side change).

## Scope

**In:** SSAO pass → compute (AO + blur dispatches); `m_SsaoRaw`/`m_SsaoBlur` UAV usage + UAV→SRV barriers; drop the SSAO framebuffers; the LDS tile-cache blur (straight box first → LDS layered → box as fallback); RWTexture2D UAV binding; flat-unique slots; reuse the compute scaffold.

**Out (later):** HBAO/GTAO (shader-swap, same pass I/O); bilateral / edge-aware / separable blur; half-res AO + upsample; a shared reusable compute-pass helper (still YAGNI at 2 compute passes — noted); applying compute to other post-process (tonemap/bloom).

## Smoke test

1. Build (`msvc-win64-vs2026-community`), launch, play. SSAO on + some ambient (raise DayAmbient if needed): AO darkens creases/contact — **visually identical to before** (AO unchanged; blur 4×4 unchanged, just LDS-sourced).
2. Toggle SSAO off (Render Stats panel) → AO gone, scene otherwise unchanged.
3. No NVRHI validation-layer errors (two compute dispatches, RWTexture2D UAVs, UAV→SRV barriers between AO→blur and blur→lighting).
4. Static + skinned meshes and shadows unaffected.

## Risks / notes

- **First compute pass writing TEXTURES (UAV images)** — `Texture_UAV` bindings + `RWTexture2D` + texture UAV→SRV barriers are new (skinning used buffer UAVs). Budget for the validation layer; mirror the skinning pass's barrier discipline + the flat-binding rule.
- **LDS correctness** — apron load (out-of-bounds clamp), the `GroupMemoryBarrierWithGroupSync`, and LDS indexing are the bug-prone part; the box-first → LDS-second sequencing + visual-parity gate is the guard, with the straight box as the committed fallback.
- **Texture UAV format** — R8 (UNORM) as a `RWTexture2D<float>` UAV: confirm the backend supports a typed UAV on R8_UNORM (DX12 + Vulkan); if not, the fallback is R16F or R8 via the supported typed-UAV format (decide in the plan; prefer R8 if supported).
- **AO parity** — the PS→CS port must preserve the 4×4 rotation tiling + the reproject-sample (hence the retained point sampler); the visual-parity smoke is the guard.
- **No framebuffers** — removing the SSAO FBs must not break the Renderer's resize/recreate or the pass ordering; the pass stays ordered between GBuffer and Lighting but does compute.
- **Backends:** DX12 (smoke default) + Vulkan; the texture UAVs + barriers + LDS must work on whichever is smoked (DX12); Vulkan parity verified if/when smoked.
