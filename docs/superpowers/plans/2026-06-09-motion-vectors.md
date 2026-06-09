# Motion-Vector Foundation Implementation Plan (SP1 of Temporal/TAA)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A per-pixel RG16F screen-space velocity G-buffer target, correct for static/rigid (Task 1) and skinned (Task 2) meshes, with a debug velocity view for standalone smoke. No temporal consumer (TAA = SP2).

**Architecture:** Append a velocity MRT3 to the G-buffer; the GBuffer VS computes `prevUV − curUV` from unjittered cur/prev clip positions (perspective-correct PS-side divide). Rigid prev = `prevModel*pos`; skinned prev = previous-frame skinned position read from a double-buffered skinned-VB SRV. Prev camera ViewProj + per-entity prev Model tracked across frames. One GBuffer pipeline/input-layout retained.

**Tech Stack:** C++23, NVRHI (DX12), HLSL vs/ps_6_1, GLM. Build/test preset `msvc-win64-vs2026-community`.

**Spec:** `docs/superpowers/specs/2026-06-09-motion-vectors-design.md`

**Commit identity:** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit ...`. Never `--no-verify`. Stage exact paths (never `git add -A`/`.`).

**Build:** `cmake --build --preset msvc-win64-vs2026-community --target editor` (+ `runtime`). GUI smoke is human-run — implementers do NOT launch the GUI.

**Reference anchors (read before editing):**
- `Renderer.cpp:563-606` (`EnsureGBuffer` — RT creation `makeRT`, framebuffer `addColorAttachment` x3 + depth). `Renderer.h:195-206` (`GBuffer` struct: Albedo/Normal/WorldPos/Fb/FbCache/Width/Height), `:142-145` (getters).
- `Renderer.cpp:334-344` (camera resolve into `m_ActiveCamera`).
- `GBufferFillPass.h:22-27` (`GBufFrameCB{mat4 VP}`, `MeshInstanceCPU{Model,NormalMatrix,BaseColor,Flags,_pad[3]}`).
- `GBufferFillPass.cpp`: `GBUF_VS_HLSL` (:17-30), `GBUF_PS_HLSL` (:32-51), pipeline build + 3 blend RTs (:130-154), clears (:158-162), per-frame CB write (:164-168), instance fill loop (:276-301), static draw (:326-386), skinned draw (:387-466).
- `SkinningComputePass.{h,cpp}`: `m_SkinnedVB`, `m_OffsetByEntity`, `GetSkinnedVertexBuffer()`, `GetSkinnedVertexOffset(e)`, `EnsureSkinnedCapacity`, `Execute` (output offsets at :184-206), `DestroyGpuResources`.
- `RenderStats.h` `DebugDrawSettings` (has ShowGrid/Wireframe/etc.); `RenderStatsPanel.cpp` debug-toggle section.
- `LightingRenderPass.cpp` shader (`main_ps`) + CB populate (~:230-254) + binding set (~:262-274) — for the debug velocity branch.

---

### Task 1: Velocity target + camera/rigid motion vectors + debug view

Add the RG16F velocity MRT3, make the GBuffer VS/PS write camera+rigid velocity (skinned falls back to `IsSkinned=0` → zero skinned velocity until Task 2), track prev ViewProj + per-entity prev Model, and add a `ShowVelocity` debug view to smoke it.

**Files:** `Renderer.h`, `Renderer.cpp`, `GBufferFillPass.h`, `GBufferFillPass.cpp`, `RenderStats.h`, `RenderStatsPanel.cpp`, `LightingRenderPass.cpp` (+`.h` CB), `RenderStatsPanel.cpp`.

- [ ] **Step 1: Velocity RT in the GBuffer struct.** `Renderer.h` `GBuffer` struct — add after `WorldPos`:
```cpp
        nvrhi::TextureHandle     Velocity; // RG16_FLOAT (prevUV - curUV)
```
Add getter near the others:
```cpp
    nvrhi::ITexture*     GetGBufferVelocity()   const { return m_GBuffer.Velocity; }
```

- [ ] **Step 2: Create + attach the velocity RT.** `Renderer.cpp` `EnsureGBuffer`: after the `WorldPos = makeRT(...)` line, add:
```cpp
        m_GBuffer.Velocity = makeRT(nvrhi::Format::RG16_FLOAT, "GBuffer.Velocity");
```
and in the framebuffer build (the `createFramebuffer(...)` chain), add a 4th color attachment after WorldPos:
```cpp
        .addColorAttachment(m_GBuffer.WorldPos)
        .addColorAttachment(m_GBuffer.Velocity)
```

- [ ] **Step 3: Prev ViewProj on the Renderer.** `Renderer.h` private members (near `m_ActiveCamera`): add `glm::mat4 m_PrevViewProj{1.0f};` and a getter `const glm::mat4& GetPrevViewProj() const { return m_PrevViewProj; }` + a setter the GBuffer pass calls at end-of-frame: `void SetPrevViewProj(const glm::mat4& vp) { m_PrevViewProj = vp; }`. (The GBuffer pass already computes `cb.VP`; it will store it as next frame's prev at the end of Render.)

- [ ] **Step 4: GBuffer CB + InstanceData additions.** `GBufferFillPass.h`:
```cpp
    struct GBufFrameCB { glm::mat4 VP; glm::mat4 PrevVP; };
    struct MeshInstanceCPU {
        glm::mat4 Model; glm::mat4 NormalMatrix; glm::mat4 PrevModel; glm::vec4 BaseColor;
        uint32_t Flags; uint32_t IsSkinned; uint32_t PrevSkinnedOffset; uint32_t _pad;
    };
    static_assert(sizeof(MeshInstanceCPU) % 16 == 0, "MeshInstanceCPU must be 16-byte aligned");
```
Add a member for the prev-model map: `std::unordered_map<uint32_t, glm::mat4> m_PrevModel;` (key = EntityId; include `<unordered_map>`). (EntityId is an integer id.)

- [ ] **Step 5: GBuffer VS — compute velocity.** Replace `GBUF_VS_HLSL` (`GBufferFillPass.cpp:17-30`) with:
```cpp
static const char* GBUF_VS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4x4 PrevModel; float4 BaseColor; uint Flags; uint IsSkinned; uint PrevSkinnedOffset; uint _pad; };
struct MeshVertex { float3 Position; float3 Normal; float2 UV; };
cbuffer PerFrame : register(b0) { float4x4 uVP; float4x4 uPrevVP; };
StructuredBuffer<InstanceData> gInstances : register(t5);
StructuredBuffer<MeshVertex>   gPrevSkinned : register(t6);   // previous-frame skinned verts (Task 2 binds; t6 unused when IsSkinned==0)
struct VSIn  { float3 Position:POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; uint VertexID:SV_VertexID; uint InstanceID:SV_InstanceID; };
struct VSOut { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; float4 CurClip:TEXCOORD3; float4 PrevClip:TEXCOORD4; };
VSOut main_vs(VSIn vin){
    InstanceData inst = gInstances[vin.InstanceID];
    float4 wp = mul(inst.Model, float4(vin.Position,1.0));
    float3 prevPos = (inst.IsSkinned != 0) ? gPrevSkinned[vin.VertexID + inst.PrevSkinnedOffset].Position : vin.Position;
    float4 prevWp = mul(inst.PrevModel, float4(prevPos,1.0));
    VSOut o;
    o.PosH    = mul(uVP, wp);
    o.CurClip = o.PosH;
    o.PrevClip = mul(uPrevVP, prevWp);
    o.Normal = mul((float3x3)inst.NormalMatrix, vin.Normal);
    o.UV = vin.UV; o.WorldPos = wp.xyz; o.InstanceID = vin.InstanceID; return o;
}
)";
```
(Note: `gPrevSkinned` at t6 is declared now but only bound/used in Task 2; with `IsSkinned==0` (Task 1) the branch never reads it. NVRHI requires the binding to exist in the layout — see Step 7 — bind a safe non-null SRV.)

- [ ] **Step 6: GBuffer PS — write velocity MRT3.** Replace `GBUF_PS_HLSL`'s `PSIn`/`PSOut`/`main_ps` so it carries the clip positions and writes SV_Target3:
```cpp
struct PSIn { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; float4 CurClip:TEXCOORD3; float4 PrevClip:TEXCOORD4; };
struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 WorldPos:SV_Target2; float2 Velocity:SV_Target3; };
```
and in `main_ps`, before `return o;`:
```cpp
    float2 curUV  = i.CurClip.xy  / i.CurClip.w  * float2(0.5,-0.5) + 0.5;
    float2 prevUV = i.PrevClip.xy / i.PrevClip.w * float2(0.5,-0.5) + 0.5;
    o.Velocity = prevUV - curUV;
```
(Keep the existing InstanceData struct in the PS in sync with the VS's — add the same `PrevModel`/`IsSkinned`/`PrevSkinnedOffset`/`_pad` fields so the struct layout matches `gInstances`.)

- [ ] **Step 7: Binding layout + PSO RT(3) + prev-skinned SRV (t6).** In `Initialize`, add `t6` to the binding layout:
```cpp
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),
```
In the pipeline build, add a 4th blend RT (mirror RT 0-2):
```cpp
        pso.renderState.blendState.setRenderTarget(3, rt);
```
The SV_VertexID input needs no input-layout entry (system value). The instance fill + draws must bind a non-null `gPrevSkinned` (t6) — in Task 1, bind the mesh's own vertex buffer as a structured SRV is not valid (it's not structured); instead bind the SkinningComputePass skinned VB if present, else a tiny dummy structured buffer. **Simplest for Task 1:** create a 1-element dummy `MeshVertex` structured buffer (`m_DummyPrevSkinned`) in `Initialize` and bind it at t6 everywhere; Task 2 swaps in the real prev skinned VB for skinned draws. Add `m_DummyPrevSkinned` member + create it (structStride=sizeof(MeshVertex), 1 element, ShaderResource state).

- [ ] **Step 8: Populate CB + InstanceData (prev VP + prev model; IsSkinned=0).** In `Render`, the per-frame CB:
```cpp
    GBufFrameCB cb{};
    const CameraView& cam = m_Renderer->GetActiveCamera();
    cb.VP = cam.Projection * cam.View;
    cb.PrevVP = m_Renderer->GetPrevViewProj();
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));
```
In the instance fill loop, set the new fields:
```cpp
            inst.PrevModel = m_PrevModel.count((uint32_t)entity) ? m_PrevModel[(uint32_t)entity] : M; // new entity -> zero velocity
            inst.IsSkinned = 0u;            // Task 2 sets 1 for skinned draws
            inst.PrevSkinnedOffset = 0u;
```
Add t6 to EVERY GBuffer binding set (4 sites: static sub/no-sub, skinned sub/no-sub) with the dummy:
```cpp
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_DummyPrevSkinned),
```
At the END of `Render` (after the run loop), update the prev-model map for next frame:
```cpp
    // Snapshot this frame's models as next frame's "prev"; drop entities no longer present.
    std::unordered_map<uint32_t, glm::mat4> next;
    world->Each<TransformComponent, MeshComponent>([&](EntityId e, const TransformComponent& t, const MeshComponent& m){
        if (m.Visible) next[(uint32_t)e] = ModelMatrix(t);
    });
    m_PrevModel.swap(next);
    m_Renderer->SetPrevViewProj(cb.VP);
```

- [ ] **Step 9: Debug velocity view (smoke handle).** `RenderStats.h` `DebugDrawSettings`: add `bool ShowVelocity = false;`. `RenderStatsPanel.cpp` (debug toggles section): add `changed |= ImGui::Checkbox("Show velocity", &GetDebugDrawSettings().ShowVelocity);`. In `LightingRenderPass`: add a velocity SRV at `t9` + a `uShowVelocity` CB int (extend the CB tail — keep 16-byte alignment, reuse a pad slot); in `main_ps` at the very top: `if (uShowVelocity != 0) { float2 v = uVelocity.Sample(uSamp, i.UV).xy; return float4(v*8.0 + 0.5, 0.5, 1.0); }`. Bind `m_Renderer->GetGBufferVelocity()` at t9 in the lighting binding set + add `Texture_SRV(9)` to the lighting binding layout; CPU sets `cb.ShowVelocity = GetDebugDrawSettings().ShowVelocity ? 1 : 0;`.

- [ ] **Step 10: Build editor + runtime.** Both clean. Grep that `MeshInstanceCPU` (C++) and the HLSL `InstanceData` have identical field order/count in BOTH the VS and PS shader strings.

- [ ] **Step 11: GUI smoke (human-run; implementer does NOT launch).** Report deferred. (Smoke: enable Show velocity → still camera/static = neutral (~0.5,0.5 blue); pan camera = smooth flow; move a rigid entity = its velocity; animating Fox shows ~0 velocity (skinned comes in Task 2); no validation errors.)

- [ ] **Step 12: Commit.**
```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/engine/src/rendering/passes/GBufferFillPass.h src/engine/src/rendering/passes/GBufferFillPass.cpp src/engine/src/rendering/RenderStats.h src/editor/src/panels/RenderStatsPanel.cpp src/engine/src/rendering/passes/LightingRenderPass.cpp src/engine/src/rendering/passes/LightingRenderPass.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(mv): velocity G-buffer target + camera/rigid motion vectors + debug view"
```

---

### Task 2: Skinned prev-pose via double-buffered skinned-VB SRV

Make `SkinningComputePass` keep the previous frame's skinned VB + offsets; bind it at t6 for skinned GBuffer draws and set `IsSkinned=1`/`PrevSkinnedOffset` so the VS reads the real prev skinned position. Now animating skinned meshes get correct velocity.

**Files:** `SkinningComputePass.{h,cpp}`, `GBufferFillPass.cpp`.

- [ ] **Step 1: Double-buffer the skinned VB + prev offsets.** `SkinningComputePass.h`: add members `nvrhi::BufferHandle m_PrevSkinnedVB;`, `uint32_t m_PrevSkinnedCapacity = 0;`, `std::unordered_map<EntityId,uint32_t> m_PrevOffsetByEntity;`, and accessors:
```cpp
    nvrhi::IBuffer* GetPrevSkinnedVertexBuffer() const { return m_PrevSkinnedVB; }
    int64_t GetPrevSkinnedVertexOffset(EntityId e) const {
        auto it = m_PrevOffsetByEntity.find(e);
        return (it != m_PrevOffsetByEntity.end()) ? (int64_t)it->second : -1;
    }
```

- [ ] **Step 2: Swap current↔prev at end of Execute.** In `SkinningComputePass::Execute`, BEFORE `m_OffsetByEntity.clear()` at the top, the current buffers ARE this frame's. Restructure: at the very END of `Execute` (after the UAV→VertexBuffer barrier), swap so this frame becomes next frame's prev:
```cpp
    std::swap(m_SkinnedVB, m_PrevSkinnedVB);
    std::swap(m_SkinnedCapacity, m_PrevSkinnedCapacity);
    m_PrevOffsetByEntity = m_OffsetByEntity;
```
Wait — the GBuffer pass (which reads the CURRENT skinned VB this frame) runs AFTER SkinningComputePass in the same frame. So the swap must NOT happen at end of skinning Execute (it'd hand the current VB to GBuffer as "prev"). Instead: keep `m_SkinnedVB` as current for this frame's GBuffer; expose `m_PrevSkinnedVB`/`m_PrevOffsetByEntity` = LAST frame's (populated at the END of the PREVIOUS Execute). So the swap belongs at the START of `Execute` (promote last frame's current → prev, then compute into the freed buffer). Implement at the TOP of Execute (after the early-out guards, before planning jobs):
```cpp
    std::swap(m_SkinnedVB, m_PrevSkinnedVB);            // last frame's current becomes this frame's prev
    std::swap(m_SkinnedCapacity, m_PrevSkinnedCapacity);
    m_PrevOffsetByEntity.swap(m_OffsetByEntity);         // last frame's offsets become prev; m_OffsetByEntity then cleared below
```
Then the existing `m_OffsetByEntity.clear()` + job planning compute into `m_SkinnedVB` (now the recycled buffer; `EnsureSkinnedCapacity` regrows if needed). Confirm the ordering in the file and place the swap so `m_OffsetByEntity` is swapped to prev BEFORE it's cleared.

- [ ] **Step 3: Null prev buffers in teardown.** `DestroyGpuResources`: add `m_PrevSkinnedVB = nullptr; m_PrevSkinnedCapacity = 0; m_PrevOffsetByEntity.clear();`.

- [ ] **Step 4: GBuffer binds prev skinned VB + sets skinned CB fields.** In `GBufferFillPass::Render` skinned path (the `else` branch, per-entity loop ~:399), for each skinned entity:
```cpp
                const int64_t prevOff = skinningPass ? skinningPass->GetPrevSkinnedVertexOffset(entity) : -1;
                nvrhi::IBuffer* prevSkinnedVB = skinningPass ? skinningPass->GetPrevSkinnedVertexBuffer() : nullptr;
                const bool havePrevSkinned = useSkinned && prevSkinnedVB && (prevOff >= 0);
                instances[i].IsSkinned = havePrevSkinned ? 1u : 0u;
                instances[i].PrevSkinnedOffset = havePrevSkinned ? (uint32_t)prevOff : 0u;
```
(Set these on the `instances[i]` BEFORE the `writeBuffer(m_InstanceBuffer, &instances[i], ...)` at :408.) Then in the skinned binding sets (both sub/no-sub), bind t6 to the real prev skinned VB when available else the dummy:
```cpp
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(6, havePrevSkinned ? prevSkinnedVB : (nvrhi::IBuffer*)m_DummyPrevSkinned),
```
(Static draws keep the dummy at t6 + `IsSkinned=0` from Task 1.)

- [ ] **Step 5: Build editor + runtime.** Clean.

- [ ] **Step 6: GUI smoke (human-run).** Report deferred. (Smoke: enable Show velocity, animate the Fox → velocity now localized to moving limbs (was ~0 in Task 1); still body/background ~0; camera pan still flows; no validation errors. Disable Show velocity → normal lighting unaffected.)

- [ ] **Step 7: Commit.**
```bash
git add src/engine/src/rendering/passes/SkinningComputePass.h src/engine/src/rendering/passes/SkinningComputePass.cpp src/engine/src/rendering/passes/GBufferFillPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(mv): skinned motion vectors via double-buffered skinned-VB prev-pose"
```

---

## Final review (after both tasks)

Whole-branch spec + quality review, then `superpowers:finishing-a-development-branch` → FF-merge to `main`. Confirm before pushing.

**Whole-branch smoke (user-run):** Show velocity on → static/still ≈ neutral; camera pan = smooth flow; rigid mover = correct; animating Fox = limb-localized flow (Task 2); first frame after load = no spurious velocity; toggle off = lighting unchanged; no NVRHI validation errors; SSAO/shadows/AA all still correct (shared G-buffer gained MRT3 only).

## Notes / gotchas

- **InstanceData C++↔HLSL parity (×2):** `MeshInstanceCPU` must match the `InstanceData` struct in BOTH the VS and PS shader strings field-for-field (Model, NormalMatrix, PrevModel, BaseColor, Flags, IsSkinned, PrevSkinnedOffset, _pad). A mismatch silently corrupts instancing. The `static_assert(%16==0)` guards size, not layout.
- **Swap timing (Task 2):** the prev skinned VB exposed during a frame must be LAST frame's, not this frame's — the swap is at the TOP of `Execute` (promote last frame's current to prev, recompute into the recycled buffer). Getting this backwards hands the current VB to the GBuffer as "prev" → zero/garbage skinned velocity. Re-read the Execute ordering before placing the swap.
- **t6 always bound:** NVRHI requires every layout slot bound each draw; static + Task-1 skinned bind the 1-element dummy `m_DummyPrevSkinned`; the VS only READS t6 when `IsSkinned!=0`, so the dummy is never dereferenced wrongly.
- **Unjittered:** SP1 has no jitter; `m_PrevViewProj`/`cb.VP` are the real matrices. SP2 must keep velocity on unjittered matrices when it adds projection jitter.
- **Velocity sign:** `prevUV - curUV` so TAA reads history at `curUV + velocity`. The debug view maps `v*8+0.5` (scale for visibility); expect mid-gray at rest.
- **RG16_FLOAT MRT3 + 4th blend RT + framebuffer 4th attachment** must all agree or the PSO/framebuffer validation fails. EnsureGBuffer rebuilds on resize (velocity RT recreated with the others).
- **EntityId key:** the prev-model map keys on the integer EntityId; cast consistently (`(uint32_t)entity`).
