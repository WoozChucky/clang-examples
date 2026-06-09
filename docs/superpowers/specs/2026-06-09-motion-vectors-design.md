# Motion-Vector Foundation Design (SP1 of Temporal/TAA)

**Status:** Design approved, pre-implementation.
**Branch:** `feat/motion-vectors`
**Part of:** the temporal-rendering effort. SP1 (this) = motion vectors. SP2 = TAA (consumes SP1). SP3 (future) = temporal GTAO (reuses SP1 + SP2's resolve). Each is its own spec→plan→build cycle.

## Goal

A per-pixel screen-space **velocity** G-buffer target — correct for static, dynamic-rigid, and **skinned** meshes — that downstream temporal passes (TAA, temporal GTAO) consume to reproject the previous frame. Verifiable standalone via a debug velocity view; no temporal consumer is built in SP1.

## Background / current state

- Deferred renderer. `m_GBuffer` (`Renderer.h`): Albedo RGBA8 (MRT0), Normal RGBA16F (MRT1), WorldPos RGBA16F (MRT2), shared D32 depth; built by `EnsureGBuffer(w,h,sharedDepth)`; `GBufferFillPass` fills it. `GetGBufferAlbedo/Normal/WorldPos/Framebuffer()` accessors.
- Camera: `m_ActiveCamera` (`CameraView{View,Projection,Position}`) resolved each frame in `Renderer.cpp` (~:334-344) — editor override or `WorldCameraComponent`. Passes read `GetActiveCamera()`.
- Skinning: `SkinningComputePass` runs first each frame, skins each skinned entity's verts into a shared per-frame **skinned VB** (UAV→VertexBuffer), with `m_OffsetByEntity[e]` = base vertex offset. GBuffer/shadow draw skinned entities via the static pipeline with `startVertexLocation = GetSkinnedVertexOffset(e)`.
- AA: `AAMode{Off,FXAA,SMAA}` resolves via an offscreen scene-color socket (consumers FxaaRenderPass/SmaaRenderPass). TAA will be a 4th consumer — **SP2**, not here.

## Architecture

### Velocity target
Add `m_GBuffer.Velocity` (**RG16_FLOAT**) as **MRT3** in `EnsureGBuffer` + the G-buffer framebuffer's color attachments; **cleared to (0,0)** each frame. Semantics: stores `prevUV − curUV` (where this pixel was last frame, in UV space) — the sign TAA wants for `historyUV = curUV + velocity`. Exposed via `GetGBufferVelocity()`.

### Velocity computation (GBuffer vertex shader)
Per vertex, compute current and previous clip positions from **UNJITTERED** matrices (jitter is SP2's; it must never pollute velocity), and output the screen-space delta, interpolated to the pixel:
- `curWorld  = curModel  * curPos`   (rigid: `curPos`=mesh vertex; skinned: `curPos`=current skinned VB vertex, as today)
- `prevWorld = prevModel * prevPos`  (rigid: `prevPos`=mesh vertex; skinned: `prevPos`=previous-frame skinned position, see below)
- `curClip = curViewProj * curWorld`; `prevClip = prevViewProj * prevWorld`
- `curUV  = curClip.xy/curClip.w * float2(0.5,-0.5) + 0.5`; `prevUV` likewise
- `velocity = prevUV − curUV` → MRT3 (interpolated `noperspective` is acceptable; standard is perspective-correct via the clip positions passed to the PS — pass `curClip`/`prevClip` to the PS and do the divide there for correctness, OR compute per-vertex UV and interpolate; **the plan picks perspective-correct PS-side divide** to avoid interpolation error on large triangles).

### Skinned prev-pose — prev-position via SRV (one pipeline, no stream/pipeline split)
Rather than a 2nd vertex stream (which would re-split the skinned vs rigid input layout that compute-skinning unified), the VS reads the **previous** skinned position from a **structured-buffer SRV**:
- `SkinningComputePass` **double-buffers** its skinned-VB output (ping-pong current/previous) and retains the **previous frame's** `m_OffsetByEntity`. New accessors: `GetPrevSkinnedVertexBuffer()`, `GetPrevSkinnedVertexOffset(e)`.
- Skinned GBuffer draws bind the previous skinned VB as a `StructuredBuffer<MeshVertex>` SRV and set `PrevSkinnedOffset` (+ `IsSkinned=1`) in the per-draw CB. The VS reads `prevPos = prevSkinnedVB[SV_VertexID + PrevSkinnedOffset].Position`.
- Rigid draws set `IsSkinned=0`; the VS uses `prevPos = curPos` and `prevModel`. A small VS branch on `IsSkinned`.
- An entity skinned this frame but absent last frame (no prev offset) → bind `IsSkinned=0` fallback for that draw → zero skinned velocity that frame (acceptable; no smear).

This keeps **one GBuffer pipeline + one input layout** (today's); the additions are: MRT3 output, a prev-skinned SRV binding, and CB fields.

### Per-draw CB additions (GBuffer)
Extend the GBuffer per-object/per-frame constants with: `prevViewProj` (mat4, per-frame), `prevModel` (mat4, per-draw), `curViewProj` unjittered (mat4, per-frame — if not already the value used), `IsSkinned` (int), `PrevSkinnedOffset` (uint). Keep 16-byte alignment; mirror the existing CB packing.

### Prev-data bookkeeping
- **Prev camera:** `Renderer` stores `m_PrevViewProj` (last frame's unjittered `Projection*View`), updated at end of frame after `m_ActiveCamera` resolve. First frame: `m_PrevViewProj = curViewProj` → zero velocity.
- **Prev model per entity:** a `std::unordered_map<EntityId, glm::mat4>` (owned by `GBufferFillPass` or Renderer) updated each frame after the draw loop (cur→prev). Entities new this frame: `prevModel = curModel` → zero velocity (no spawn smear). Entities gone: dropped from the map.
- **Prev skinned VB + offsets:** owned by `SkinningComputePass` (ping-pong + previous offset map), published via the new accessors.

### Debug visualization (SP1 smoke handle)
A `ShowVelocity` flag in `DebugDrawSettings` (RenderStats) → a debug full-screen pass (or a branch in an existing debug/post path) that samples `m_GBuffer.Velocity` and writes a visualization (e.g. `rg = velocity*scale + 0.5`, blue=0) to the scene. Editor toggle in the Render Stats panel. Static + still camera → flat/neutral; pan → uniform flow; animate the Fox → limb-localized flow; gives an unambiguous visual correctness check before any temporal consumer exists.

## Testing

- Velocity is GPU-side; correctness is the debug-view smoke. Extract any pure helper (e.g. a `ClipToVelocityUV` if cleanly factorable) into a header + unit test; otherwise no new unit test (mirrors how the AO/shadow shader math is smoke-validated).
- Existing suites stay green (`test_ecs`, `test_ssaomath`, etc.).
- **Smoke (manual GUI):** enable `ShowVelocity`. (1) still camera + static scene → near-zero everywhere. (2) pan/orbit camera → smooth uniform flow opposite camera motion. (3) animate the Fox (skinned) → velocity localized to moving limbs, body/background near-zero. (4) move a rigid entity (if any dynamic) → its velocity matches. (5) first frame after load → no spurious velocity. (6) no NVRHI validation errors (new MRT3 + prev-skinned SRV binding). (7) SSAO/shadows/AA unaffected.

## Scope

**In:** RG16F velocity MRT3 + framebuffer wiring + clear; GBuffer VS velocity (rigid via prevModel, skinned via prev-skinned SRV); GBuffer CB additions; `m_PrevViewProj`; per-entity prevModel map; `SkinningComputePass` skinned-VB double-buffer + prev offsets + accessors; `ShowVelocity` debug view + editor toggle.

**Out:** camera sub-pixel jitter; history ping-pong; reprojection/neighborhood-clamp resolve; TAA / any `AAMode` change; temporal GTAO; motion blur; the velocity buffer feeding anything other than the debug view (SP2 wires it into the resolve).

## Risks / notes

- **Skinned VB double-buffer** doubles the skinned-VB memory and requires the prev offset map to correspond to the prev VB; mismatched/absent entities fall back to zero skinned velocity (safe).
- **Unjittered invariant:** all velocity math uses unjittered matrices; SP2's jitter is applied only to the *rasterization* projection, never the velocity matrices. Establish this in SP1 (store unjittered prev/cur ViewProj) so SP2 drops in cleanly.
- **First-frame / new-entity / disocclusion:** no prev data → velocity 0 → TAA later treats as "use current" (no smear). Correct default.
- **Perspective-correct velocity:** interpolate clip positions and divide in the PS (not per-vertex UV lerp) to avoid large-triangle error.
- **MRT3 added to the shared G-buffer framebuffer:** every GBuffer consumer (SSAO reads Normal/WorldPos, Lighting reads all) must be unaffected — velocity is an additional target they ignore; verify the framebuffer attachment order doesn't disturb existing MRT indices (velocity is appended as MRT3).
- **Pipeline PSO:** adding MRT3 changes the GBuffer PSO's render-target count → the pipeline + framebuffer must agree; rebuild on resize as today.
