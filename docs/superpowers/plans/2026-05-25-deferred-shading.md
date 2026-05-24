# Deferred Shading Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the forward renderer to deferred shading (G-buffer geometry pass + full-screen lighting pass) while reproducing the current image 1:1.

**Architecture:** A new `GBufferFillPass` writes albedo/world-normal/world-position into a Renderer-owned 3-MRT G-buffer (+ shared depth). A new full-screen `LightingRenderPass` reads the G-buffer and computes the exact current lighting + fog, writing the scene color. `MeshRenderPass` is retired. Pass order becomes `Shadow → GBufferFill → Lighting → Primitive(grid) → Outline → Debug → UI`.

**Tech Stack:** C++23, NVRHI (DX12 + Vulkan), HLSL compiled at runtime via DXC from C++ string literals, GLM (depth [0,1], RH).

**Spec:** `docs/superpowers/specs/2026-05-25-deferred-shading-design.md`

**Build/verify:** Preset is `msvc-win64-vs2026-enterprise` (the CLAUDE.md `clang-…-community` preset is NOT installed). Engine build: `cmake --build out/build/msvc-win64-vs2026-enterprise --target Engine`. Editor build: `… --target editor`. **No unit tests** for renderer work (project norm); verification is build-clean + manual visual parity. The HLSL lives in C++ string literals → an Engine rebuild recompiles shaders at runtime; the running `editor.exe` does NOT hot-reload `Engine.dll`, so **restart the editor** to see changes.

**Branch:** `feat/atmospheric-fog` (current). Leave the pre-existing unstaged `src/engine/src/rendering/backends/RendererBackendDX12.cpp` change untouched; never stage it.

**Reuse note:** `MeshRenderPass` (`src/engine/src/rendering/passes/MeshRenderPass.{h,cpp}`) is the working template for the mesh VS, the lighting/shadow/fog PS math, the instance/batch/cull loop, the binding model, and the Vulkan binding offsets. Tasks below say exactly what to copy and what to change. Read it first.

---

## File structure

- `src/engine/src/rendering/Renderer.{h,cpp}` — owns the G-buffer (3 color textures + framebuffer), lifecycle (create-on-size-change, teardown with backend), accessors; changes pass registration order.
- `src/engine/src/rendering/passes/GBufferFillPass.{h,cpp}` — NEW. Geometry → G-buffer MRTs + depth. No lighting.
- `src/engine/src/rendering/passes/LightingRenderPass.{h,cpp}` — NEW. Full-screen; G-buffer + lights + shadow + fog → scene color.
- `src/engine/src/rendering/passes/MeshRenderPass.{h,cpp}` — REMOVED at the end.
- `src/engine/CMakeLists.txt` — add the two new passes; remove MeshRenderPass.

---

## Task 1: G-buffer resources owned by the Renderer

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h`
- Modify: `src/engine/src/rendering/Renderer.cpp`

This task only adds resources + lifecycle; nothing renders into them yet. Mirror the existing shadow-resource pattern (`CreateShadowResources`, the `m_Shadow*` members, teardown in `Shutdown`/`TeardownForSwap`).

- [ ] **Step 1: Add the G-buffer struct, members, and accessors to `Renderer.h`**

In the `private:` data section, near `m_ShadowView{};`, add:
```cpp
    // Deferred G-buffer (Renderer-owned intermediates; recreated on size change,
    // released with the backend). RT0 albedo(linear) / RT1 world-normal / RT2 world-pos.
    struct GBuffer {
        nvrhi::TextureHandle     Albedo;   // RGBA8_UNORM linear
        nvrhi::TextureHandle     Normal;   // RGBA16_FLOAT
        nvrhi::TextureHandle     WorldPos; // RGBA16_FLOAT
        nvrhi::FramebufferHandle Fb;       // RT0,1,2 + shared depth
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };
    GBuffer m_GBuffer{};

    // Builds m_GBuffer at the given size sharing the supplied depth texture.
    // No-op if already matching width/height/depth.
    void EnsureGBuffer(uint32_t width, uint32_t height, nvrhi::ITexture* sharedDepth);
    void ReleaseGBuffer();
```
In the public accessor group (near `GetShadowDepthTexture()`), add:
```cpp
    nvrhi::ITexture*     GetGBufferAlbedo()      const { return m_GBuffer.Albedo;   }
    nvrhi::ITexture*     GetGBufferNormal()      const { return m_GBuffer.Normal;   }
    nvrhi::ITexture*     GetGBufferWorldPos()    const { return m_GBuffer.WorldPos; }
    nvrhi::IFramebuffer* GetGBufferFramebuffer() const { return m_GBuffer.Fb;       }
```

- [ ] **Step 2: Implement `EnsureGBuffer` / `ReleaseGBuffer` in `Renderer.cpp`**

Add near `CreateShadowResources` (model the `TextureDesc` on that code and `SceneViewport.cpp`):
```cpp
void Renderer::ReleaseGBuffer()
{
    m_GBuffer = GBuffer{};
}

void Renderer::EnsureGBuffer(uint32_t width, uint32_t height, nvrhi::ITexture* sharedDepth)
{
    if (!sharedDepth || width == 0 || height == 0) return;
    if (m_GBuffer.Fb && m_GBuffer.Width == width && m_GBuffer.Height == height
        && m_GBuffer.Fb->getDesc().depthAttachment.texture == sharedDepth)
        return; // already current

    auto makeRT = [&](nvrhi::Format fmt, const char* name) {
        nvrhi::TextureDesc td;
        td.width = width; td.height = height;
        td.format = fmt;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.isRenderTarget = true;
        td.isShaderResource = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = name;
        td.clearValue = nvrhi::Color(0.f);
        td.useClearValue = true;
        return m_Device->createTexture(td);
    };

    m_GBuffer.Albedo   = makeRT(nvrhi::Format::RGBA8_UNORM,  "GBuffer.Albedo");
    m_GBuffer.Normal   = makeRT(nvrhi::Format::RGBA16_FLOAT, "GBuffer.Normal");
    m_GBuffer.WorldPos = makeRT(nvrhi::Format::RGBA16_FLOAT, "GBuffer.WorldPos");

    m_GBuffer.Fb = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
        .addColorAttachment(m_GBuffer.Albedo)
        .addColorAttachment(m_GBuffer.Normal)
        .addColorAttachment(m_GBuffer.WorldPos)
        .setDepthAttachment(sharedDepth));
    m_GBuffer.Width = width;
    m_GBuffer.Height = height;
}
```

- [ ] **Step 3: Call `EnsureGBuffer` each frame and release on teardown**

In `Renderer::Render`, after `sceneBuffer` is resolved (right after the `if (!sceneBuffer) sceneBuffer = frameBuffer;` line), add:
```cpp
            // Keep the G-buffer sized to the scene target + sharing its depth.
            {
                const auto& fbinfo = sceneBuffer->getFramebufferInfo();
                EnsureGBuffer(fbinfo.width, fbinfo.height,
                              sceneBuffer->getDesc().depthAttachment.texture);
            }
```
In `Renderer::Shutdown` and `Renderer::TeardownForSwap`, alongside the shadow-resource release, add `ReleaseGBuffer();`. (Find where `m_ShadowDepth`/`m_ShadowFb` are released and add it there.)

- [ ] **Step 4: Build the engine**

Run: `cmake --build out/build/msvc-win64-vs2026-enterprise --target Engine`
Expected: builds clean. No visual change (nothing reads/writes the G-buffer yet).

- [ ] **Step 5: Commit**
```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git commit -m "feat(deferred): Renderer-owned G-buffer resources + lifecycle"
```

---

## Task 2: GBufferFillPass — geometry into the G-buffer

**Files:**
- Create: `src/engine/src/rendering/passes/GBufferFillPass.h`
- Create: `src/engine/src/rendering/passes/GBufferFillPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/src/rendering/Renderer.cpp` (register the pass)

The fill pass reuses MeshRenderPass's vertex path, instance buffer, batching, and cull. Its PS writes G-buffer targets instead of a lit color. Registered **before** MeshRenderPass for this task so the visible image is still produced by MeshRenderPass (incremental: G-buffer fills but is unused). It renders into `Renderer::GetGBufferFramebuffer()`.

- [ ] **Step 1: Create `GBufferFillPass.h`**
Model on `MeshRenderPass.h`. Keep the `IRenderPass` overrides, the `PerFrameCB`-equivalent (only `glm::mat4 VP;` is needed), the `MeshInstanceCPU` struct (copy verbatim from MeshRenderPass.h, including the `static_assert`), and the instance buffer. Drop everything lighting/shadow/fog/point-light.
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Deferred geometry pass: writes albedo / world-normal / world-position into the
// Renderer's G-buffer MRTs + shared depth. No lighting.
class GBufferFillPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct GBufFrameCB { glm::mat4 VP; };
    struct MeshInstanceCPU { // copy from MeshRenderPass.h verbatim
        glm::mat4 Model; glm::mat4 NormalMatrix; glm::vec4 BaseColor;
        uint32_t Flags; uint32_t _pad[3];
    };
    static_assert(sizeof(MeshInstanceCPU) % 16 == 0, "MeshInstanceCPU must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_FrameCB;
    nvrhi::BufferHandle m_InstanceBuffer;
    uint32_t m_MaxInstances = 4096;
};
```

- [ ] **Step 2: Write the GBufferFill shaders (in `GBufferFillPass.cpp`)**
The VS is the mesh VS reduced to what the G-buffer needs (clip pos, world normal, world pos, uv, flags). The PS writes 3 targets. Note: `gInstances` stays at `t5`, texture `t2`, sampler `s3`, CB `b0` — matching the mesh-pass register scheme so the Vulkan offsets line up.
```cpp
static const char* GBUF_VS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4 BaseColor; uint Flags; uint3 _pad; };
cbuffer PerFrame : register(b0) { float4x4 uVP; };
StructuredBuffer<InstanceData> gInstances : register(t5);
struct VSIn  { float3 Position:POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; uint InstanceID:SV_InstanceID; };
struct VSOut { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; };
VSOut main_vs(VSIn vin){
    InstanceData inst = gInstances[vin.InstanceID];
    float4 wp = mul(inst.Model, float4(vin.Position,1.0));
    VSOut o; o.PosH = mul(uVP, wp);
    o.Normal = mul((float3x3)inst.NormalMatrix, vin.Normal);
    o.UV = vin.UV; o.WorldPos = wp.xyz; o.InstanceID = vin.InstanceID; return o;
}
)";

static const char* GBUF_PS_HLSL = R"(
struct InstanceData { float4x4 Model; float4x4 NormalMatrix; float4 BaseColor; uint Flags; uint3 _pad; };
Texture2D uTexture : register(t2);
SamplerState uSampler : register(s3);
StructuredBuffer<InstanceData> gInstances : register(t5);
static const uint OPT_SAMPLE_TEXTURE = 1u << 0;
struct PSIn { float4 PosH:SV_POSITION; float3 Normal:NORMAL; float2 UV:TEXCOORD0; float3 WorldPos:TEXCOORD1; uint InstanceID:TEXCOORD2; };
struct PSOut { float4 Albedo:SV_Target0; float4 Normal:SV_Target1; float4 WorldPos:SV_Target2; };
PSOut main_ps(PSIn i){
    InstanceData inst = gInstances[i.InstanceID];
    float3 albedo = ((inst.Flags & OPT_SAMPLE_TEXTURE) != 0)
        ? uTexture.Sample(uSampler, i.UV).rgb
        : inst.BaseColor.rgb;
    PSOut o;
    o.Albedo   = float4(albedo, 1.0);
    o.Normal   = float4(normalize(i.Normal), 0.0);
    o.WorldPos = float4(i.WorldPos, 1.0);
    return o;
}
)";
```
NOTE on sRGB: store albedo **linear**. RT0 is `RGBA8_UNORM` (a non-sRGB view), so `uTexture.Sample(...).rgb` (texture SRV is the same one MeshRenderPass binds) is written without sRGB encode here; the encode happens only when the lighting pass writes the SRGBA8 scene color. This matches the forward path.

- [ ] **Step 3: Implement `GBufferFillPass.cpp` (C++)**
- `Initialize`: create `m_VS`/`m_PS` via `m_Renderer->CreateShader(... "main_vs","vs_6_1")` / `"main_ps","ps_6_1"`. Build the input layout EXACTLY like MeshRenderPass.cpp:261-269 (POSITION/NORMAL/TEXCOORD from `MeshVertex`). Build a binding layout with **only**: `ConstantBuffer(0)`, `Texture_SRV(2)`, `Sampler(3)`, `StructuredBuffer_SRV(5)`, plus the same `VulkanBindingOffsets` block as MeshRenderPass.cpp:284-290. Create `m_FrameCB` (size `sizeof(GBufFrameCB)`) and `m_InstanceBuffer` (structured, `m_MaxInstances * sizeof(MeshInstanceCPU)`) like the mesh pass.
- `Render`:
  - Get the framebuffer from the Renderer: `nvrhi::IFramebuffer* gfb = m_Renderer->GetGBufferFramebuffer(); if (!gfb) return;` (ignore the `frameBuffer` argument).
  - Lazily create `m_Pipeline` against `gfb->getFramebufferInfo()`: `pso.VS/PS`, `inputLayout`, `bindingLayouts={m_BindingLayout}`, TriangleList, depthTest=true, depthWrite=true, cullMode=Back, and **blend disabled** on all three render targets (`rt.setBlendEnable(false)` for targets 0,1,2 via `pso.renderState.blendState.setRenderTarget(k, rt)`).
  - Clear the three color targets and the depth at the start:
    ```cpp
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 0, nvrhi::Color(0.f));
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 1, nvrhi::Color(0.f)); // normal=0 => sky mask
    nvrhi::utils::ClearColorAttachment(commandList, gfb, 2, nvrhi::Color(0.f));
    commandList->clearDepthStencilTexture(gfb->getDesc().depthAttachment.texture,
                                          nvrhi::AllSubresources, true, 1.0f, false, 0);
    ```
  - Fill `GBufFrameCB{ VP = P*V }` from `m_Renderer->GetActiveCamera()` and `writeBuffer(m_FrameCB,...)`.
  - **Copy the instance gather + frustum cull + batching + per-batch instance upload + draw loop from `MeshRenderPass.cpp` (the body from ~line 388 through the draw at ~line 597)**, with these changes: (a) drop all point-light/shadow/fog/PerFrame lighting fields — only `VP` is needed; (b) the per-batch binding set has only the 4 bindings above (CB0, tex2, samp3, instances5) bound to `gfb`; (c) `state.framebuffer = gfb`; viewport from `gfb->getFramebufferInfo()`.
- `Shutdown`: null the handles. `OnResize`: null `m_Pipeline` (so it rebuilds against a resized G-buffer FB).

- [ ] **Step 4: Register `GBufferFillPass.cpp` in CMake**
In `src/engine/CMakeLists.txt`, in the source list next to `src/rendering/passes/MeshRenderPass.cpp`, add:
```cmake
    src/rendering/passes/GBufferFillPass.cpp
```

- [ ] **Step 5: Register the pass in the Renderer (before MeshRenderPass)**
In `Renderer.cpp`, find where passes are added (`AddRenderPass(std::make_unique<MeshRenderPass>())`, both in `Init` and `InitForSwap`). Add `#include "passes/GBufferFillPass.h"` near the other pass includes, and insert **before** the MeshRenderPass line:
```cpp
    AddRenderPass(std::make_unique<GBufferFillPass>());
```

- [ ] **Step 6: Build + run**
Run: `cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Expected: builds clean. Launch the editor (restart if running). Scene looks **unchanged** (MeshRenderPass still produces the image; the G-buffer is filled but unused). Confirm no validation/runtime errors in the console.

- [ ] **Step 7: Commit**
```bash
git add src/engine/src/rendering/passes/GBufferFillPass.h src/engine/src/rendering/passes/GBufferFillPass.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp
git commit -m "feat(deferred): GBufferFillPass writes albedo/normal/worldpos MRT"
```

---

## Task 3: LightingRenderPass + switch the pipeline to deferred

**Files:**
- Create: `src/engine/src/rendering/passes/LightingRenderPass.h`
- Create: `src/engine/src/rendering/passes/LightingRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/src/rendering/Renderer.cpp` (pass order: drop MeshRenderPass, add Lighting, move Primitive after Lighting)

The lighting pass is the first full-screen pass in the codebase: a 3-vertex triangle expanded from `SV_VertexID`, no vertex buffer, no input layout. It reads the G-buffer SRVs + the point-light buffer + the shadow map, computes the **exact** current lighting+fog, and writes the scene color. Depth test/write OFF (it ignores depth; world pos comes from RT2). Rendering target = the scene framebuffer (`frameBuffer` arg), which is the editor offscreen target or the swapchain.

- [ ] **Step 1: Create `LightingRenderPass.h`**
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen deferred lighting: reads the Renderer's G-buffer, computes the
// directional + point + shadow lighting and fog, writes the scene color.
class LightingRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct DirectionalLight { glm::vec4 Direction; glm::vec4 Color; };
    struct LightFrameCB {
        glm::mat4 LightVP;
        DirectionalLight Dir;
        glm::vec4 CameraPos;   // xyz
        glm::vec4 Fog;         // rgb=color, w=density
        uint32_t  PointLightCount; float Ambient; int ShadowEnabled; float ShadowBias;
        int       FogEnabled;  int _pad[3];
    };
    struct PointLightCPU { // copy from MeshRenderPass.h verbatim
        glm::vec4 Position; glm::vec4 Color; float Intensity; float Range; float _pad[2];
    };

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::SamplerHandle m_GBufSampler;     // point/clamp
    nvrhi::BufferHandle m_FrameCB;
    nvrhi::BufferHandle m_PointLightBuffer;
    uint32_t m_MaxPointLights = 256;
};
```

- [ ] **Step 2: Write the lighting shaders (in `LightingRenderPass.cpp`)**
The VS makes a full-screen triangle. The PS ports the MeshRenderPass PS math verbatim — `ShadowFactor` (copy from MeshRenderPass.cpp:139-155), ambient `uAmbient*uDir.Color.rgb`, diffuse, point-light loop, then fog blend — but reads N / WorldPos / albedo from the G-buffer and applies the sky mask.
```cpp
static const char* LIGHT_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);     // (0,0)(2,0)(0,2)
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* LIGHT_PS_HLSL = R"(
struct DirectionalLight { float4 Direction; float4 Color; };
struct PointLight { float4 Position; float4 Color; float Intensity; float Range; float2 _pad; };
cbuffer PerFrame : register(b0) {
    float4x4 uLightVP;
    DirectionalLight uDir;
    float4 uCameraPos;
    float4 uFog;
    uint  uPointLightCount; float uAmbient; int uShadowEnabled; float uShadowBias;
    int   uFogEnabled; int3 _padL;
};
Texture2D uAlbedo   : register(t0);
Texture2D uNormal   : register(t1);
Texture2D uWorldPos : register(t2);
SamplerState uSamp  : register(s0);
StructuredBuffer<PointLight> gPointLights : register(t4);
Texture2D              uShadowMap  : register(t6);
SamplerComparisonState uShadowSamp : register(s7);
struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float ShadowFactor(float3 worldPos, float ndl){      // verbatim from MeshRenderPass
    if (uShadowEnabled == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos,1.0));
    float3 p = lp.xyz / lp.w;
    float2 uv = p.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x<0.0||uv.x>1.0||uv.y<0.0||uv.y>1.0) return 1.0;
    float bias = uShadowBias * (1.0 + (1.0-ndl)*2.0);
    float d = p.z - bias; float sum = 0.0; const float texel = 1.0/2048.0;
    [unroll] for(int y=-1;y<=1;++y)[unroll] for(int x=-1;x<=1;++x)
        sum += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv+float2(x,y)*texel, d);
    return sum/9.0;
}

float4 main_ps(PSIn i) : SV_Target {
    float3 N  = uNormal.Sample(uSamp, i.UV).xyz;
    float3 wp = uWorldPos.Sample(uSamp, i.UV).xyz;
    float3 albedo = uAlbedo.Sample(uSamp, i.UV).rgb;

    // Sky / no-geometry: cleared normal is (0,0,0).
    if (dot(N,N) < 0.5) {
        return float4(uFog.rgb, 1.0); // matches the scene clear (fog color)
    }
    N = normalize(N);
    float3 lightDir = normalize(uDir.Direction.xyz);
    float diffuse = max(dot(N, -lightDir), 0.0);

    float3 lighting = uAmbient * uDir.Color.rgb;
    lighting += diffuse * uDir.Color.rgb * ShadowFactor(wp, diffuse);
    [loop] for (uint idx=0; idx<uPointLightCount; ++idx){
        PointLight pl = gPointLights[idx];
        float3 L = pl.Position.xyz - wp; float dist = length(L);
        if (pl.Range > 0.0001){
            float3 Ldir = L / max(dist,1e-5);
            float NdotL = max(dot(N,Ldir),0.0);
            float falloff = saturate(1.0 - (dist/pl.Range)*(dist/pl.Range));
            lighting += pl.Color.rgb * (pl.Intensity * NdotL * falloff);
        }
    }
    float3 col = albedo * lighting;
    if (uFogEnabled != 0){
        float fogDist = length(wp - uCameraPos.xyz);
        float fogF = 1.0 - exp(-uFog.w * fogDist);
        col = lerp(col, uFog.rgb, fogF);
    }
    return float4(col, 1.0);
}
)";
```

- [ ] **Step 3: Implement `LightingRenderPass.cpp` (C++)**
- `Initialize`: create `m_VS`/`m_PS`. Create `m_GBufSampler` (linear or point, clamp address — point is fine for a 1:1 full-screen sample). Create `m_FrameCB` (`sizeof(LightFrameCB)`) and `m_PointLightBuffer` (structured, `m_MaxPointLights*sizeof(PointLightCPU)`) like the mesh pass. Build a binding layout with: `ConstantBuffer(0)`, `Texture_SRV(0)`, `Texture_SRV(1)`, `Texture_SRV(2)`, `Sampler(0)`, `StructuredBuffer_SRV(4)`, `Texture_SRV(6)`, `Sampler(7)`, plus the same `VulkanBindingOffsets` block as the mesh pass. No input layout.
- `Render`:
  - `if (!m_Renderer->GetGBufferFramebuffer()) return;`
  - Lazily build `m_Pipeline` against `frameBuffer->getFramebufferInfo()` (the scene target): `pso.VS/PS`, `bindingLayouts={m_BindingLayout}`, `inputLayout=nullptr`, TriangleList, `depthStencilState.depthTestEnable=false`, `depthWriteEnable=false`, `rasterState.cullMode=None`, blend disabled on RT0.
  - Gather lights from the ECS exactly like the mesh pass: directional (SunMarker, but matching current mesh-pass behavior = any directional is fine since only the sun exists) into `LightFrameCB.Dir`, point lights into `m_PointLightBuffer` + `PointLightCount`. (Copy from MeshRenderPass.cpp:365-386.)
  - Fill the rest of `LightFrameCB` from the Renderer: `CameraPos = vec4(cam.Position,1)`, `Fog = vec4(GetFrameFog().Color, GetFrameFog().Density)`, `FogEnabled = GetFogSettings().Enabled?1:0`, `Ambient = 0.1f` (the current hardcoded value — keep for 1:1), `ShadowEnabled/LightVP/ShadowBias` from `GetShadowView()` + `GetShadowSettings().Bias`. `writeBuffer` both buffers.
  - Binding set: `ConstantBuffer(0,m_FrameCB)`, `Texture_SRV(0, GetGBufferAlbedo())`, `Texture_SRV(1, GetGBufferNormal())`, `Texture_SRV(2, GetGBufferWorldPos())`, `Sampler(0,m_GBufSampler)`, `StructuredBuffer_SRV(4,m_PointLightBuffer)`, `Texture_SRV(6, GetShadowDepthTexture(), nvrhi::Format::R32_FLOAT)`, `Sampler(7, GetShadowSampler())`.
  - `state.framebuffer=frameBuffer; state.pipeline=m_Pipeline; state.bindings={set}; state.viewport...`; **no** vertex/index buffer. `commandList->setGraphicsState(state); nvrhi::DrawArguments a; a.vertexCount=3; commandList->draw(a);`
- `Shutdown`/`OnResize`: null handles / null `m_Pipeline`.

- [ ] **Step 4: Register in CMake**
In `src/engine/CMakeLists.txt`, next to the GBufferFillPass line, add:
```cmake
    src/rendering/passes/LightingRenderPass.cpp
```

- [ ] **Step 5: Switch the pass order in the Renderer**
In `Renderer.cpp` (BOTH `Init` and `InitForSwap`): add `#include "passes/LightingRenderPass.h"`. Change registration so the order is:
```
PrimitiveRenderPass   -> MOVE to after lighting
ShadowDepthPass
GBufferFillPass
LightingRenderPass    -> NEW, replaces MeshRenderPass
OutlineRenderPass
DebugRenderPass
UiRenderPass
```
Concretely: remove the `AddRenderPass(std::make_unique<MeshRenderPass>())` line; add `AddRenderPass(std::make_unique<LightingRenderPass>())` where MeshRenderPass was (after GBufferFill); and move the `PrimitiveRenderPass` registration to **after** the LightingRenderPass line. Leave `Shadow` before `GBufferFill`. Remove the now-unused `#include "passes/MeshRenderPass.h"` if present.

- [ ] **Step 6: Build + run + verify 1:1 (DX12)**
Run: `cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Launch (restart) the editor on the default backend. Verify the scene matches the pre-deferred look: lit meshes, shadows (3×3 PCF), fog over distance, the grid, outline on selection, debug gizmos, UI text — all identical. Scrub the day/night cycle; fog + shadows track as before. If anything differs, debug before committing (common suspects: sRGB on albedo RT, normal not normalized, sky mask threshold, fog distance using camera pos).

**Depth/grid check (important after the reorder):** the grid (`PrimitiveRenderPass`) now runs *after* lighting and must depth-test against the geometry depth written by `GBufferFillPass`. Confirm the grid is correctly occluded behind/under meshes (not painted on top of them) and there is no z-fighting. Only `GBufferFillPass` clears the scene depth now; `Primitive/Outline/Debug/UI` must not clear it (they don't today — verify none was added).

- [ ] **Step 7: Commit**
```bash
git add src/engine/src/rendering/passes/LightingRenderPass.h src/engine/src/rendering/passes/LightingRenderPass.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp
git commit -m "feat(deferred): full-screen LightingRenderPass; switch pipeline to deferred"
```

---

## Task 4: Retire MeshRenderPass

**Files:**
- Delete: `src/engine/src/rendering/passes/MeshRenderPass.h`, `src/engine/src/rendering/passes/MeshRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: any remaining includes of `MeshRenderPass.h`

- [ ] **Step 1: Remove the source from CMake**
In `src/engine/CMakeLists.txt`, delete the line `    src/rendering/passes/MeshRenderPass.cpp`.

- [ ] **Step 2: Delete the files and any stray includes**
```bash
git rm src/engine/src/rendering/passes/MeshRenderPass.h src/engine/src/rendering/passes/MeshRenderPass.cpp
```
Then grep for leftover references: `grep -rn "MeshRenderPass" src/`. Remove any remaining `#include "passes/MeshRenderPass.h"`. There should be none after Task 3.

- [ ] **Step 3: Build**
Run: `cmake --build out/build/msvc-win64-vs2026-enterprise --target editor`
Expected: builds clean (no missing symbol / include errors).

- [ ] **Step 4: Commit**
```bash
git add -A src/engine/src/rendering/passes/ src/engine/CMakeLists.txt
git commit -m "refactor(deferred): remove retired MeshRenderPass"
```

---

## Task 5: Two-backend parity + regression check

**Files:** none (verification; fix-only if issues found).

- [ ] **Step 1: Vulkan visual parity**
Launch the editor forcing Vulkan: run `…/bin/Debug/editor.exe --backend=vk` (the app parses `--backend=`). Verify the same scene matches the DX12 result and the pre-deferred look (lighting, shadows, fog, grid, outline, debug, UI). Then DX12: `editor.exe --backend=d3d12`. If a backend differs, debug (suspects: MRT format support, Vulkan binding offsets, depth/SRV state) before finishing.

- [ ] **Step 2: Backend hot-swap**
If the editor exposes runtime backend swap, toggle it and confirm the G-buffer + pipelines rebuild and the scene stays correct (exercises `TeardownForSwap`/`InitForSwap` + `ReleaseGBuffer`/`EnsureGBuffer`).

- [ ] **Step 3: Resize**
Resize the editor window and the scene viewport panel; confirm no crash and correct rendering (exercises the per-frame `EnsureGBuffer` size check + pass `OnResize` pipeline rebuilds).

- [ ] **Step 4: Unit-test regression**
Run:
```bash
cmake --build out/build/msvc-win64-vs2026-enterprise --target test_ecs
./out/build/msvc-win64-vs2026-enterprise/bin/Debug/test_ecs.exe
```
Expected: `All ECS tests passed.`

---

## Notes
- No `ECS.h`/`Game.h` changes → no `ecs.dll`/`Game.dll` API bump; just rebuild + restart the editor for engine changes.
- The legacy animated `red/green/blue` clear in `RenderThread.cpp` and the fog resolve in `Renderer::Render` are unchanged; the lighting pass outputs the fog color for sky pixels so the horizon stays seamless.
- Trigger to add a forward path later: any mesh that sets `OPT_UNLIT` or `BaseColor.a < 1` (none today).
