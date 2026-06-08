# SSAO Compute Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the SSAO pass's two full-screen pixel sub-passes into two compute dispatches (the engine's 2nd compute pass, first to write UAV *textures*), and add a groupshared-memory (LDS) tile cache to the blur — visual output unchanged.

**Architecture:** `SsaoRenderPass` stays an ordered IRenderPass (GBufferFill → SSAO → Lighting) but its Execute records compute dispatches: AO (G-buffer SRVs → raw-AO UAV) then blur (raw-AO SRV → blur-AO UAV, LDS-cached), with UAV→SRV barriers between. Reuses the SkinningComputePass compute scaffold + the NVRHI flat-binding-unique-slots rule.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset ONLY), NVRHI (DX12 default + Vulkan), DXC `cs_6_1`, the existing pass/Renderer architecture.

**Spec:** `docs/superpowers/specs/2026-06-08-ssao-compute-design.md`

---

## Conventions (apply to every task)

- **Build/test preset:** `msvc-win64-vs2026-community` ONLY. Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- **Commit author:** EVERY commit `--author="Nuno Silva <nuno.levezinho@live.com.pt>"`. NEVER the vinci-energies.net work email.
- **NEVER** `--no-verify`. **NEVER** `git add -A`/`git add .` — stage exact paths. Never stage `assets/world.json`, `assets/engine_settings.json`, `assets/editor_preferences.json`.
- **git from git-bash:** `git -C C:/dev/clang-examples ...`.
- Branch is already `feat/ssao-compute`.
- **Logging:** `SM_TRACE`/`SM_WARN`/`SM_ERROR`. Log degradation, never silent-skip.
- **No `ECS.h` change** → no component-restart caveat (normal editor relaunch after an Engine rebuild to smoke).
- **NVRHI flat-binding rule (recurring):** binding slot NUMBERS must be GLOBALLY UNIQUE across t/u/b/s within a pipeline (VulkanBindingOffsets all 0 → HLSL register# = Vulkan binding). This is why GBuffer uses t2/s3/t5/t6 and SkinningComputePass uses t0/t1/t2/u3/b4. Follow it.
- **NVRHI signatures are GROUND TRUTH.** Where this plan names a call (`createComputePipeline`, `ComputeState`, `setComputeState`, `dispatch`, `BindingLayoutItem::Texture_UAV`, `BindingSetItem::Texture_UAV`, `setTextureState`/`requireTextureState`, `TextureDesc::isUAV`), VERIFY against `third_party/NVRHI` headers + how `SkinningComputePass` (committed, the 1st compute pass) does the buffer-UAV equivalent. Mirror it.

---

## Sequencing rationale

T1 latently adds UAV usage to the SSAO textures (no behavior change — they stay render-targets too). T2 converts the pass to compute with a STRAIGHT 4×4 box blur (visual parity, proves compute-on-textures) + drops the now-unused framebuffers. T3 layers the LDS tile cache on the blur as a separate, independently-revertable step (the straight box from T2 is the committed fallback). T4 verifies.

---

## File Structure

**Modify:**
- `src/engine/src/rendering/Renderer.cpp` / `Renderer.h` — SSAO textures gain UAV usage; drop the SSAO framebuffers (T2).
- `src/engine/src/rendering/passes/SsaoRenderPass.cpp` / `.h` — rewrite the two sub-passes as compute dispatches; LDS blur.

No new files (the pass already exists; it changes from graphics to compute).

---

## Task 1: SSAO textures gain UAV usage (latent)

**Files:** Modify `src/engine/src/rendering/Renderer.cpp` (`EnsureSsao`)

- [ ] **Step 1: Add UAV usage to the SSAO texture descs**

In `Renderer::EnsureSsao` (~line 614-619), the `mk` lambda creates the R8_UNORM textures with `isRenderTarget = true; isShaderResource = true;`. Add UAV usage so compute can write them, keeping render-target+SRV so nothing breaks yet:
```cpp
        nvrhi::TextureDesc td; td.width = width; td.height = height; td.format = nvrhi::Format::R8_UNORM;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.isRenderTarget = true;          // (kept for now; dropped in T2 once compute owns the writes)
        td.isShaderResource = true;
        td.isUAV = true;                   // NEW: compute writes via RWTexture2D UAV
        td.debugName = name; td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
        td.clearValue = nvrhi::Color(1.f); td.useClearValue = true;
        return m_Device->createTexture(td);
```
VERIFY the exact NVRHI `TextureDesc` UAV field name (`isUAV` / `.setIsUAV(true)`). Confirm `R8_UNORM` supports a typed UAV on DX12 (it does for `RWTexture2D<float>`); if the backend rejects it (validation/createTexture failure), the fallback is `R16_FLOAT` — but try R8_UNORM first (preserves exact storage).

- [ ] **Step 2: Build + smoke (no behavior change)**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: links clean. SSAO still renders identically (the textures are still render-targets written by the pixel passes; UAV capability is latent). No validation errors at texture creation.

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/Renderer.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): SSAO raw/blur textures gain UAV usage (for compute SSAO)"
```

---

## Task 2: SsaoRenderPass → compute (AO + blur straight box); drop framebuffers

> The core conversion. Two compute dispatches replace the two pixel `draw(3)`s. Blur is a straight 4×4 box (LDS comes in T3). Read `SsaoRenderPass.cpp` (current AO_PS/BLUR_PS — the math to port) + `SkinningComputePass.{h,cpp}` (the compute pipeline/dispatch/barrier idiom) first.

**Files:** Modify `src/engine/src/rendering/passes/SsaoRenderPass.{h,cpp}`, `src/engine/src/rendering/Renderer.{h,cpp}`

- [ ] **Step 1: Write the AO + blur compute shaders**

Replace the `AO_PS`/`BLUR_PS`/`FULLSCREEN_VS` strings with compute shaders. AO CS (port AO_PS verbatim; thread→pixel→uv; KEEP the point sampler for reprojected `uWorldPos` samples; write a UAV):
```cpp
static const char* AO_CS = R"(
Texture2D       uNormal   : register(t1);
Texture2D       uWorldPos : register(t2);
SamplerState    uPt       : register(s3);
RWTexture2D<float> uOut    : register(u4);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
[numthreads(8,8,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    uint2 px = tid.xy;
    if (px.x >= (uint)uRtSize.x || px.y >= (uint)uRtSize.y) return;
    float2 uv = (float2(px) + 0.5) * uRtSize.zw;           // pixel-center UV
    float3 N = uNormal.Sample(uPt, uv).xyz;
    if (dot(N,N) < 0.5) { uOut[px] = 1.0; return; }         // sky/unwritten -> no occlusion
    N = normalize(N);
    float3 P = uWorldPos.Sample(uPt, uv).xyz;
    float2 fpx  = floor(uv * uRtSize.xy);
    float2 cell = fmod(fpx, 4.0);
    float  idx  = cell.y * 4.0 + cell.x;
    float  a    = frac(sin(idx * 12.9898) * 43758.5453) * 6.28318530718;
    float3 up = abs(N.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
    float3 T0 = normalize(cross(up, N));
    float3 T  = T0*cos(a) + cross(N,T0)*sin(a);
    float3 B  = cross(N, T);
    float radius = uParams.x; float bias = uParams.y;
    float occ = 0;
    [loop] for (int s = 0; s < 16; ++s) {
        float3 sp = P + (T*uKernel[s].x + B*uKernel[s].y + N*uKernel[s].z)*radius;
        float4 clip = mul(uViewProj, float4(sp,1));
        if (clip.w <= 1e-5) continue;
        float3 ndc = clip.xyz/clip.w;
        float2 uvs = ndc.xy*0.5+0.5; uvs.y = 1-uvs.y;
        if (any(uvs<0)||any(uvs>1)) continue;
        float3 Pocc = uWorldPos.Sample(uPt, uvs).xyz;
        float occZ = mul(uView, float4(Pocc,1)).z;
        float sZ   = mul(uView, float4(sp,1)).z;
        float range = saturate(1.0 - length(Pocc - P)/radius);
        if (occZ > sZ + bias) occ += range;
    }
    occ = (occ/16.0)*uParams.z;
    uOut[px] = pow(saturate(1.0 - occ), uParams.w);
}
)";
```
Blur CS (STRAIGHT box; reproduce the PS's point-sampled 4×4 box exactly). The PS sampled `uv + {-1.5,-0.5,0.5,1.5}*invSize` with a point-clamp sampler → texels at `px + {-1,0,+1,+2}` per axis (point sampling of `(px+0.5+off)/size` floors to those). Reproduce with clamped Load for exactness:
```cpp
static const char* BLUR_CS = R"(
Texture2D<float>   uAo  : register(t1);
RWTexture2D<float> uOut : register(u2);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
[numthreads(8,8,1)]
void main_cs(uint3 tid : SV_DispatchThreadID) {
    int2 px = int2(tid.xy);
    int W = (int)uRtSize.x, H = (int)uRtSize.y;
    if (px.x >= W || px.y >= H) return;
    const int offs[4] = { -1, 0, 1, 2 };           // matches the PS's point-sampled {-1.5,-0.5,0.5,1.5}
    float sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x) {
            int2 q = clamp(px + int2(offs[x], offs[y]), int2(0,0), int2(W-1,H-1));
            sum += uAo.Load(int3(q,0));
        }
    uOut[px] = sum / 16.0;
}
)";
```
(`Compose`/`FULLSCREEN_VS` are no longer needed — remove them. The shaders are now standalone CS strings.)

- [ ] **Step 2: Rebuild Initialize for compute pipelines**

In `SsaoRenderPass::Initialize`: replace the VS/PS compilation + graphics binding layouts with:
- `m_AoCS   = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, AO_CS,   strlen(AO_CS),   "main_cs", "cs_6_1");`
- `m_BlurCS = m_Renderer->CreateShader(nvrhi::ShaderType::Compute, BLUR_CS, strlen(BLUR_CS), "main_cs", "cs_6_1");`
- AO binding layout (visibility Compute; VulkanBindingOffsets all 0): `ConstantBuffer(0), Texture_SRV(1), Texture_SRV(2), Sampler(3), Texture_UAV(4)`.
- Blur binding layout: `ConstantBuffer(0), Texture_SRV(1), Texture_UAV(2)`.
- Keep `m_PointClamp` sampler (AO uses it) + `m_CB` (static CB) + `m_Kernel`.
- Header (`SsaoRenderPass.h`): replace `m_AoVS/m_AoPS/m_BlurVS/m_BlurPS` + `m_AoPipeline/m_BlurPipeline` (graphics) with `m_AoCS/m_BlurCS` (ShaderHandle), `m_AoPipeline/m_BlurPipeline` (now `nvrhi::ComputePipelineHandle`), `m_AoLayout/m_BlurLayout` (keep). Update Shutdown/OnResize to null the new handles.

- [ ] **Step 3: Rewrite Render() as compute dispatches**

Replace the graphics `makePipe`/`draw` body with compute. Get the textures (not framebuffers): `m_Renderer->GetSsaoRawTexture()` + `m_Renderer->GetSsaoTexture()` (the blur output — confirm the getter; it's `GetSsaoTexture()` returning `m_SsaoBlur`). Build the `SsaoCB` exactly as today (ViewProj/View/Kernel/Params/RtSize from the camera + settings + texture size). Then:
```cpp
    commandList->beginMarker("SsaoRenderPass");
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));

    nvrhi::ITexture* raw  = m_Renderer->GetSsaoRawTexture();
    nvrhi::ITexture* blur = m_Renderer->GetSsaoTexture();
    const uint32_t gx = (width + 7) / 8, gy = (height + 7) / 8;

    // AO dispatch: G-buffer -> raw AO (UAV).
    commandList->setTextureState(raw, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, gN),
            nvrhi::BindingSetItem::Texture_SRV(2, gP),
            nvrhi::BindingSetItem::Sampler(3, m_PointClamp),
            nvrhi::BindingSetItem::Texture_UAV(4, raw) };
        nvrhi::ComputeState st; st.pipeline = m_AoPipeline; st.bindings = { m_Device->createBindingSet(d, m_AoLayout) };
        commandList->setComputeState(st);
        commandList->dispatch(gx, gy, 1);
    }
    // Barrier: raw UAV-write -> SRV-read by the blur.
    commandList->setTextureState(raw,  nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(blur, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    // Blur dispatch: raw AO -> blurred AO (UAV).
    {
        nvrhi::BindingSetDesc d; d.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_CB),
            nvrhi::BindingSetItem::Texture_SRV(1, raw),
            nvrhi::BindingSetItem::Texture_UAV(2, blur) };
        nvrhi::ComputeState st; st.pipeline = m_BlurPipeline; st.bindings = { m_Device->createBindingSet(d, m_BlurLayout) };
        commandList->setComputeState(st);
        commandList->dispatch(gx, gy, 1);
    }
    // Barrier: blur UAV-write -> SRV-read by Lighting.
    commandList->setTextureState(blur, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->endMarker();
```
Lazy-create the compute pipelines on first Render (like the old graphics ones, but `createComputePipeline({ .CS=m_AoCS, .bindingLayouts={m_AoLayout} })`; no framebuffer needed). `width`/`height` come from the raw texture's desc (`raw->getDesc().width/height`). VERIFY `setTextureState` is the correct NVRHI call + that `setComputeState` commits the pending barriers (mirror SkinningComputePass's `setBufferState` usage). `gN`/`gP` are `GetGBufferNormal()`/`GetGBufferWorldPos()` as today.

- [ ] **Step 4: Drop the SSAO framebuffers (now unused)**

In `Renderer`: remove `m_SsaoRawFb`/`m_SsaoBlurFb` members + their creation in `EnsureSsao` (lines 623-624) + the nulling in resize/teardown (lines 215, 428-429, 701) + the getters `GetSsaoRawFramebuffer`/`GetSsaoBlurFramebuffer` (Renderer.h 147-148). Also drop `td.isRenderTarget = true;` from the SSAO texture desc (Task 1 kept it; now the textures are compute-written only — keep `isUAV` + `isShaderResource`). Keep `GetSsaoRawTexture`/`GetSsaoTexture`. Confirm no other caller references the dropped FBs/getters (grep).

- [ ] **Step 5: Build + smoke**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: links clean. (User smokes: SSAO visually IDENTICAL to before — AO + 4×4 box, just compute. Toggle SSAO off → unchanged. No validation errors.)

- [ ] **Step 6: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/SsaoRenderPass.h src/engine/src/rendering/passes/SsaoRenderPass.cpp src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): SSAO pass -> compute (AO + 4x4-box blur dispatches); drop SSAO framebuffers"
```

---

## Task 3: LDS tile-cache blur

> Layer groupshared memory onto the blur. Separate step so the T2 straight box stays the committed fallback if LDS misbehaves.

**Files:** Modify `src/engine/src/rendering/passes/SsaoRenderPass.cpp` (BLUR_CS only)

- [ ] **Step 1: Add the LDS tile cache to BLUR_CS**

Replace `BLUR_CS`'s direct `Load`s with a cooperative groupshared tile load. The box reads `px + {-1,0,+1,+2}` per axis, so each 8×8 group needs texels `[groupBase-1 .. groupBase+8+1]` per axis = an 11×11 region (apron lo=1, hi=2). Use a generous `groupshared float tile[12][12]` (12 = 8 + 1 + 2, rounded). Each thread loads one-or-more apron texels (cooperative load loop covering 12×12 from 64 threads), `GroupMemoryBarrierWithGroupSync()`, then blur from LDS:
```cpp
static const char* BLUR_CS = R"(
Texture2D<float>   uAo  : register(t1);
RWTexture2D<float> uOut : register(u2);
cbuffer SsaoCB : register(b0) {
    float4x4 uViewProj; float4x4 uView; float4 uKernel[16]; float4 uParams; float4 uRtSize;
};
#define TILE 8
#define APRON_LO 1
#define APRON_HI 2
#define SPAN (TILE + APRON_LO + APRON_HI)   // 11; tile array is SPAN-sized
groupshared float gTile[SPAN][SPAN];
[numthreads(8,8,1)]
void main_cs(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint3 dtid : SV_DispatchThreadID) {
    int W = (int)uRtSize.x, H = (int)uRtSize.y;
    int2 groupBase = int2(gid.xy) * TILE;          // top-left pixel of this group's tile
    // Cooperatively load SPAN*SPAN texels (origin = groupBase - APRON_LO) into LDS, each fetched once.
    for (int t = int(gtid.y) * TILE + int(gtid.x); t < SPAN*SPAN; t += TILE*TILE) {
        int lx = t % SPAN, ly = t / SPAN;
        int2 q = clamp(groupBase - APRON_LO + int2(lx, ly), int2(0,0), int2(W-1,H-1));
        gTile[ly][lx] = uAo.Load(int3(q,0));
    }
    GroupMemoryBarrierWithGroupSync();
    int2 px = int2(dtid.xy);
    if (px.x >= W || px.y >= H) return;
    // Local index of THIS pixel inside the tile = gtid + APRON_LO. Box offsets {-1,0,+1,+2}.
    const int offs[4] = { -1, 0, 1, 2 };
    int lx = int(gtid.x) + APRON_LO, ly = int(gtid.y) + APRON_LO;
    float sum = 0;
    [unroll] for (int y = 0; y < 4; ++y)
        [unroll] for (int x = 0; x < 4; ++x)
            sum += gTile[ly + offs[y]][lx + offs[x]];
    uOut[px] = sum / 16.0;
}
)";
```
Verify: the LDS read indices stay in `[0, SPAN)` — `lx` ranges `[APRON_LO, APRON_LO+TILE)` = `[1,9)`, `+offs` ∈ `{-1,0,1,2}` → `[0, 11)` ✓. The clamped edge-load reproduces the PS's clamp-address sampler at borders. Output must equal the T2 box (same texels, same average).

- [ ] **Step 2: Build + smoke (parity)**
```
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: links clean. (User smokes: SSAO STILL visually identical to T2 / pre-feature — the LDS blur produces the same texels. No validation errors. If parity breaks or validation complains, this step is independently revertable — the T2 straight box is the fallback.)

- [ ] **Step 3: Commit**
```
git -C C:/dev/clang-examples add src/engine/src/rendering/passes/SsaoRenderPass.cpp
git -C C:/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(render): SSAO blur uses groupshared (LDS) tile cache"
```

---

## Task 4: Full build, tests, manual smoke

**Files:** none (verification).

- [ ] **Step 1: Full build**
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets link clean.

- [ ] **Step 2: Unit suites (renderer-side change; confirm no breakage)**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ssaomath.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: each prints its pass line (`test_ssaomath` still covers the unchanged kernel math).

- [ ] **Step 3: Manual smoke (report results; do not auto-pass)**

1. Launch editor, play. SSAO ON + some ambient (raise DayAmbient in the Atmosphere panel if AO is hard to see): AO darkens creases/contact — **visually identical to before** (AO unchanged; blur 4×4 unchanged, now LDS-sourced).
2. Toggle SSAO off (Render Stats panel) → AO gone; scene otherwise unchanged.
3. No NVRHI validation-layer errors in the console (two compute dispatches, RWTexture2D UAVs, AO→blur + blur→lighting UAV→SRV barriers).
4. Static + skinned meshes and shadows unaffected; resize the window → SSAO still correct (texture rebuild path).

- [ ] **Step 4: Report** per step. On failure debug via systematic-debugging; commit fixes under the same author. Do not mark complete on failure.

---

## Self-Review (completed during planning)

**Spec coverage:** SSAO textures UAV (T1) ✓; pass→compute AO+blur dispatches (T2) ✓; UAV→SRV barriers AO→blur + blur→lighting (T2 Step 3) ✓; drop framebuffers (T2 Step 4) ✓; straight box first → LDS layered → box fallback (T2 box, T3 LDS) ✓; RWTexture2D UAV binding + flat-unique slots (T2) ✓; AO math ported verbatim incl. 4×4 rotation tiling + point sampler (T2 Step 1) ✓; test_ssaomath unchanged/green (T4) ✓; visual-parity smoke (T2/T3/T4) ✓.

**Type consistency:** `m_AoCS/m_BlurCS` (ShaderHandle) + `m_AoPipeline/m_BlurPipeline` (ComputePipelineHandle) + `m_AoLayout/m_BlurLayout` defined T2 Step 2, used T2 Step 3. Binding slots AO {b0,t1,t2,s3,u4} / Blur {b0,t1,u2} consistent across shader registers + layout items + set items. `GetSsaoRawTexture()`/`GetSsaoTexture()` are the surviving getters; FBs/`GetSsao*Framebuffer` removed (T2 Step 4). Box texel offsets {-1,0,+1,+2} match between T2 box and T3 LDS.

**Verify-at-implementation (flagged, real fallbacks):** `TextureDesc` UAV field name + R8_UNORM typed-UAV support (T1, fallback R16F); `setTextureState`/`requireTextureState` exact name + setComputeState barrier commit (T2 Step 3, mirror SkinningComputePass); `GetSsaoTexture()` vs a differently-named blur getter (T2 Step 3, grep); LDS index bounds (T3 Step 1, verified in-plan). Each has an in-task resolution.

---

## Execution note

T1 latent UAV add; T2 the core PS→CS conversion (visual parity, straight box) + FB removal; T3 layers LDS (independently revertable, box is the fallback); T4 verifies. NVRHI specifics (Texture_UAV, texture barriers, compute pipeline) verified against the headers + the committed SkinningComputePass. No `ECS.h` change → normal relaunch to smoke.
