# FXAA Post-Process Anti-Aliasing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add FXAA as a runtime-toggleable, engine_settings-persisted full-screen post-process resolve in the deferred renderer, applied to the 3D scene only (UI stays sharp), with zero overhead when disabled.

**Architecture:** A new `FxaaRenderPass` (an `IRenderPass`) is owned by `Renderer` as a separate member (NOT in `m_RenderPasses`). When FXAA is enabled, world passes render into a Renderer-owned offscreen scene-color SRV (`m_SceneColor`, sharing the existing scene depth); the FXAA pass then reads that SRV and writes the destination `sceneBuffer`; UI passes draw on top. Passes are split into `World`/`Overlay` stages via a new `IRenderPass::Stage()`. When disabled, world passes render straight into `sceneBuffer` exactly as today.

**Tech Stack:** C++23, NVRHI (DX12/Vulkan), DXC-compiled inline HLSL, nlohmann/json (settings), Dear ImGui (editor toggle).

---

## Reference patterns (read before starting)

- Full-screen pass with texture+sampler binding + Vulkan flat-binding offsets: `src/engine/src/rendering/passes/LightingRenderPass.cpp` (binding layout lines ~112-134, binding set ~256-268, pipeline ~170-187, full-screen triangle VS).
- Simpler full-screen pass (CB only): `src/engine/src/rendering/passes/SkyRenderPass.cpp`.
- Renderer-owned SRV render target with depth-keyed framebuffer cache: `Renderer::EnsureGBuffer` / `ReleaseGBuffer` (`src/engine/src/rendering/Renderer.cpp:454-502`).
- Engine-exported settings-singleton accessor: `GetShadowSettings()` (`RenderStats.h:38` / `RenderStats.cpp:21-25`).
- Settings load/save: `src/engine/src/utilities/SettingsManager.cpp` (Load renderer block ~60-72, Save ~89-93).
- Settings seed site (single, both exes): `Application::Init` (`src/engine/src/core/Application.cpp:10`).
- Engine-linked unit test target precedent: `test_alloc` in `tests/CMakeLists.txt:22-39`.

---

## File Structure

- `src/engine/src/rendering/IRenderPass.h` — add `RenderStage` enum + `virtual Stage()`.
- `src/engine/src/rendering/passes/UiRenderPass.h` — override `Stage()` → `Overlay`.
- `src/engine/src/rendering/RenderStats.h` / `RenderStats.cpp` — `AntiAliasingSettings` + accessor.
- `src/common/include/ApplicationContext.h` — `ApplicationSettings.fxaaEnabled`.
- `src/engine/src/utilities/SettingsManager.cpp` — load/save `renderer.fxaa`.
- `src/engine/src/core/Application.cpp` — seed `GetAntiAliasingSettings()` from loaded settings.
- `src/engine/src/rendering/Renderer.h` / `Renderer.cpp` — `m_SceneColor` target, `EnsureSceneColor`/`ReleaseSceneColor`, `GetSceneColorTexture`, owned `m_FxaaPass`, World/Overlay loop split + resolve, lifecycle.
- `src/engine/src/rendering/passes/FxaaRenderPass.h` / `FxaaRenderPass.cpp` — new pass (inline HLSL).
- `src/engine/CMakeLists.txt` — add `FxaaRenderPass.cpp`.
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — FXAA checkbox.
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — persist FXAA to engine_settings.json on change.
- `tests/test_settings.cpp` + `tests/CMakeLists.txt` — settings round-trip test.

**Task order is build-driven:** scaffolding (getter/target) lands before the pass that calls it; the pass lands before the Renderer wiring that owns it.

---

### Task 1: Pass-stage classification

**Files:**
- Modify: `src/engine/src/rendering/IRenderPass.h`
- Modify: `src/engine/src/rendering/passes/UiRenderPass.h:269` (add override after `OnResize`)

- [ ] **Step 1: Add the `RenderStage` enum + virtual to `IRenderPass`**

In `src/engine/src/rendering/IRenderPass.h`, inside `class IRenderPass`, after the `virtual ~IRenderPass() = default;` line, add:

```cpp
    // Which composition stage this pass belongs to. World passes render into the
    // scene-color target (which FXAA resolves); Overlay passes (UI) render on top
    // of the final target after the resolve so they stay un-antialiased.
    enum class RenderStage { World, Overlay };
    virtual RenderStage Stage() const { return RenderStage::World; }
```

- [ ] **Step 2: Override `Stage()` in `UiRenderPass`**

In `src/engine/src/rendering/passes/UiRenderPass.h`, in `class UiRenderPass`, immediately after the `void OnResize(uint32_t width, uint32_t height) override;` line (line ~269), add:

```cpp
    RenderStage Stage() const override { return RenderStage::Overlay; }
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected: builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/rendering/IRenderPass.h src/engine/src/rendering/passes/UiRenderPass.h
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): add IRenderPass::Stage (World/Overlay) for post-process split"
```

---

### Task 2: AntiAliasingSettings accessor

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h`
- Modify: `src/engine/src/rendering/RenderStats.cpp`

- [ ] **Step 1: Declare the struct + accessor**

In `src/engine/src/rendering/RenderStats.h`, after the `ShadowSettings` struct (line ~28), add:

```cpp
struct AntiAliasingSettings {
    bool FxaaEnabled = true;
};
```

Then in the accessor block at the bottom, after `ENGINE_API ShadowSettings& GetShadowSettings();`, add:

```cpp
ENGINE_API AntiAliasingSettings& GetAntiAliasingSettings();
```

- [ ] **Step 2: Define the accessor**

In `src/engine/src/rendering/RenderStats.cpp`, after the `GetShadowSettings()` definition, add:

```cpp
AntiAliasingSettings& GetAntiAliasingSettings()
{
    static AntiAliasingSettings s;
    return s;
}
```

- [ ] **Step 3: Build to verify**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected: builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/engine/src/rendering/RenderStats.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): add AntiAliasingSettings live toggle accessor"
```

---

### Task 3: Persist `fxaaEnabled` in engine_settings.json + seed at startup (TDD)

**Files:**
- Modify: `src/common/include/ApplicationContext.h:21`
- Modify: `src/engine/src/utilities/SettingsManager.cpp` (Load ~60-72, Save ~89-93)
- Modify: `src/engine/src/core/Application.cpp` (after line 17)
- Create: `tests/test_settings.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `tests/test_settings.cpp`:

```cpp
#include <cstdio>   // std::printf, std::fprintf, std::remove
#include "ApplicationContext.h"          // ApplicationSettings
#include "utilities/SettingsManager.h"   // SettingsManager::Load/Save
#include <nlohmann/json.hpp>
#include <fstream>

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static const char* kTmp = "test_settings_tmp.json";

// Saving fxaaEnabled=false then loading into a fresh (default-true) struct yields false.
static void T00_fxaa_false_roundtrip()
{
    ApplicationSettings in;
    in.fxaaEnabled = false;
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out; // default fxaaEnabled == true
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == false);
    std::remove(kTmp);
}

// Saving fxaaEnabled=true round-trips to true.
static void T01_fxaa_true_roundtrip()
{
    ApplicationSettings in;
    in.fxaaEnabled = true;
    EXPECT(SettingsManager::Save(kTmp, in));

    ApplicationSettings out;
    out.fxaaEnabled = false; // force off, expect Load to flip it back on
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == true);
    std::remove(kTmp);
}

// A settings file without renderer.fxaa leaves the caller default (true) untouched.
static void T02_missing_key_defaults_true()
{
    {
        nlohmann::json j;
        j["version"] = 1;
        j["renderer"]["backend"] = "directx12";
        std::ofstream(kTmp) << j.dump(2);
    }
    ApplicationSettings out; // default fxaaEnabled == true
    EXPECT(SettingsManager::Load(kTmp, &out));
    EXPECT(out.fxaaEnabled == true);
    std::remove(kTmp);
}

int main()
{
    T00_fxaa_false_roundtrip();
    T01_fxaa_true_roundtrip();
    T02_missing_key_defaults_true();
    if (g_Failures == 0) { std::printf("All settings tests passed.\n"); return 0; }
    std::printf("%d settings test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 2: Register the test target**

Append to `tests/CMakeLists.txt`:

```cmake
add_executable(test_settings
    test_settings.cpp
)

target_link_libraries(test_settings PRIVATE
    CommonHeaders
    Engine
    nlohmann_json::nlohmann_json
)

target_include_directories(test_settings PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/engine/src
)

set_target_properties(test_settings PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Configure + build, verify it FAILS to compile**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_settings
```
Expected: compile error — `ApplicationSettings` has no member `fxaaEnabled`.

- [ ] **Step 4: Add the field**

In `src/common/include/ApplicationContext.h`, in `struct ApplicationSettings` (line 17-22), after `bool vsyncEnabled = true;` add:

```cpp
    bool        fxaaEnabled   = true;
```

- [ ] **Step 5: Implement load + save**

In `src/engine/src/utilities/SettingsManager.cpp`, inside the `if (j.contains("renderer") && j["renderer"].is_object())` block, after the backend-parsing `if`/closing brace (after line ~72, still inside the renderer block), add:

```cpp
        if (jr.contains("fxaa") && jr["fxaa"].is_boolean()) {
            out->fxaaEnabled = jr["fxaa"].get<bool>();
        }
```

In the same file, in `Save`, after `j["renderer"]["backend"] = BackendToString(settings.Backend);` (line ~90), add:

```cpp
    j["renderer"]["fxaa"]      = settings.fxaaEnabled;
```

- [ ] **Step 6: Build + run the test, verify it PASSES**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_settings
./out/build/msvc-win64-vs2026-community/bin/Debug/test_settings.exe
```
Expected: `All settings tests passed.`

- [ ] **Step 7: Seed the live toggle at startup**

In `src/engine/src/core/Application.cpp`, add the include near the top (after line 4):

```cpp
#include "rendering/RenderStats.h"
```

Then in `Application::Init`, immediately after the CLI-override block (after line 17, before the `Backend == Invalid` check), add:

```cpp
    // Seed the live anti-aliasing toggle from the persisted setting (both exes
    // boot through here; the RenderThread reads GetAntiAliasingSettings()).
    GetAntiAliasingSettings().FxaaEnabled = m_AppContext->Settings.fxaaEnabled;
```

> Note: if `#include "rendering/RenderStats.h"` does not resolve from the engine `core` dir, use `#include "RenderStats.h"` (the rendering dir is on Engine's include path; `RenderStatsPanel.cpp` includes it bare). Pick whichever compiles.

- [ ] **Step 8: Build Engine + both exes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine editor runtime`
Expected: builds with no errors.

- [ ] **Step 9: Commit**

```bash
git add src/common/include/ApplicationContext.h src/engine/src/utilities/SettingsManager.cpp src/engine/src/core/Application.cpp tests/test_settings.cpp tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(settings): persist + seed fxaaEnabled in engine_settings.json"
```

---

### Task 4: Renderer scene-color target scaffolding

Adds the offscreen scene-color SRV + its lifecycle and the public getter the FXAA pass will read. No FXAA pass or loop change yet — this task only introduces (currently unused) plumbing so the next task compiles.

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h`
- Modify: `src/engine/src/rendering/Renderer.cpp`

- [ ] **Step 1: Add the getter + target member + lifecycle decls**

In `src/engine/src/rendering/Renderer.h`, add a getter next to `GetGBufferFramebuffer()` (after line 134):

```cpp
    // Offscreen scene-color SRV the world passes render into when FXAA is enabled;
    // the FXAA pass samples it and resolves into the final target. Null when FXAA
    // has never been enabled (lazily allocated).
    nvrhi::ITexture*     GetSceneColorTexture() const { return m_SceneColor.Color; }
```

In the `private:` section, after the `GBuffer m_GBuffer{};` member and its `EnsureGBuffer`/`ReleaseGBuffer` decls (after line 192), add:

```cpp
    // Offscreen scene-color render target for the FXAA path. Mirrors the G-buffer's
    // depth-keyed framebuffer cache (editor uses one stable depth; the swapchain
    // rotates depths across back-buffers). Lazily built only when FXAA is enabled.
    struct SceneColorTarget {
        nvrhi::TextureHandle     Color;  // matches the scene target's color format
        nvrhi::FramebufferHandle Fb;     // current frame's framebuffer
        std::vector<std::pair<nvrhi::ITexture*, nvrhi::FramebufferHandle>> FbCache;
        uint32_t      Width  = 0;
        uint32_t      Height = 0;
        nvrhi::Format Format = nvrhi::Format::UNKNOWN;
    };
    SceneColorTarget m_SceneColor{};

    // Builds m_SceneColor at the given size/format sharing the supplied depth.
    // No-op if already matching. Used only on the FXAA-enabled path.
    void EnsureSceneColor(uint32_t width, uint32_t height,
                          nvrhi::ITexture* sharedDepth, nvrhi::Format colorFormat);
    void ReleaseSceneColor();
```

- [ ] **Step 2: Add `EnsureSceneColor` / `ReleaseSceneColor` implementations**

In `src/engine/src/rendering/Renderer.cpp`, after `Renderer::EnsureGBuffer` (after line 502), add:

```cpp
void Renderer::ReleaseSceneColor()
{
    m_SceneColor = SceneColorTarget{};
}

void Renderer::EnsureSceneColor(uint32_t width, uint32_t height,
                                nvrhi::ITexture* sharedDepth, nvrhi::Format colorFormat)
{
    if (!sharedDepth || width == 0 || height == 0 || colorFormat == nvrhi::Format::UNKNOWN) return;

    // Recreate the color target only on size/format change; this invalidates every
    // cached framebuffer (they reference the old color texture).
    if (!m_SceneColor.Color || m_SceneColor.Width != width ||
        m_SceneColor.Height != height || m_SceneColor.Format != colorFormat)
    {
        nvrhi::TextureDesc td;
        td.width = width; td.height = height;
        td.format = colorFormat;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.isRenderTarget = true;
        td.isShaderResource = true;
        td.initialState = nvrhi::ResourceStates::ShaderResource;
        td.keepInitialState = true;
        td.debugName = "SceneColor";
        td.clearValue = nvrhi::Color(0.f);
        td.useClearValue = true;
        m_SceneColor.Color = m_Device->createTexture(td);
        m_SceneColor.FbCache.clear();
        m_SceneColor.Fb = nullptr;
        m_SceneColor.Width = width;
        m_SceneColor.Height = height;
        m_SceneColor.Format = colorFormat;
    }

    // Reuse a cached framebuffer for this depth texture, or build + cache one.
    for (auto& [depth, fb] : m_SceneColor.FbCache) {
        if (depth == sharedDepth) { m_SceneColor.Fb = fb; return; }
    }
    nvrhi::FramebufferHandle fb = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
        .addColorAttachment(m_SceneColor.Color)
        .setDepthAttachment(sharedDepth));
    m_SceneColor.FbCache.emplace_back(sharedDepth, fb);
    m_SceneColor.Fb = fb;
}
```

- [ ] **Step 3: Release scene color in `Shutdown` and `TeardownForSwap`**

In `Renderer::Shutdown`, after the `ReleaseGBuffer();` line (line 177), add:

```cpp
    ReleaseSceneColor();
```

In `Renderer::TeardownForSwap`, after the `ReleaseGBuffer();` line (line 527), add:

```cpp
    ReleaseSceneColor();
```

- [ ] **Step 4: Build Engine, verify it compiles**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected: builds with no errors (new plumbing unused for now).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): add Renderer scene-color SRV target + lifecycle"
```

---

### Task 5: FxaaRenderPass

**Files:**
- Create: `src/engine/src/rendering/passes/FxaaRenderPass.h`
- Create: `src/engine/src/rendering/passes/FxaaRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt:42` (after `UiRenderPass.cpp`)

- [ ] **Step 1: Create the header**

Create `src/engine/src/rendering/passes/FxaaRenderPass.h`:

```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include "IRenderPass.h"

class Renderer;

// Full-screen FXAA resolve. Reads the Renderer's scene-color SRV
// (Renderer::GetSceneColorTexture) and writes the destination framebuffer passed
// to Render(). Owned by Renderer and invoked between the World and Overlay pass
// loops; it is NOT stored in Renderer::m_RenderPasses (it needs a source SRV +
// a distinct destination FB, which the generic pass loop doesn't express).
class FxaaRenderPass : public IRenderPass {
public:
    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world,
                double deltaTime, FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;
private:
    struct FxaaFrameCB {
        glm::vec2 RcpFrame; // (1/width, 1/height)
        glm::vec2 _pad;
    };
    static_assert(sizeof(FxaaFrameCB) % 16 == 0, "FxaaFrameCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS, m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::SamplerHandle m_Sampler;
    nvrhi::BufferHandle m_FrameCB;
};
```

- [ ] **Step 2: Create the implementation**

Create `src/engine/src/rendering/passes/FxaaRenderPass.cpp`:

```cpp
#include "FxaaRenderPass.h"

#include "Renderer.h"
#include <nvrhi/utils.h>

// Full-screen FXAA 3.11 (luma-edge). VS emits a full-screen triangle from
// SV_VertexID; PS samples the scene color, detects high-contrast edges by luma
// range, and blends along the edge direction. Registers are globally unique
// across b/t/s (b0 CB, t1 scene, s2 sampler) so Vulkan flat-binding offsets
// don't collide, matching LightingRenderPass.
static const char* FXAA_VS_HLSL = R"(
struct VSOut { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut main_vs(uint vid : SV_VertexID){
    VSOut o;
    o.UV = float2((vid << 1) & 2, vid & 2);
    o.PosH = float4(o.UV * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* FXAA_PS_HLSL = R"(
cbuffer FxaaCB : register(b0) {
    float2 uRcpFrame;   // (1/width, 1/height)
    float2 _pad;
};
Texture2D    uScene : register(t1);
SamplerState uSamp  : register(s2);

struct PSIn { float4 PosH:SV_POSITION; float2 UV:TEXCOORD0; };

float FxaaLuma(float3 c){ return dot(c, float3(0.299, 0.587, 0.114)); }

float4 main_ps(PSIn i) : SV_Target {
    float2 uv = i.UV;
    float3 rgbM  = uScene.Sample(uSamp, uv).rgb;
    float3 rgbNW = uScene.Sample(uSamp, uv + float2(-1.0,-1.0) * uRcpFrame).rgb;
    float3 rgbNE = uScene.Sample(uSamp, uv + float2( 1.0,-1.0) * uRcpFrame).rgb;
    float3 rgbSW = uScene.Sample(uSamp, uv + float2(-1.0, 1.0) * uRcpFrame).rgb;
    float3 rgbSE = uScene.Sample(uSamp, uv + float2( 1.0, 1.0) * uRcpFrame).rgb;

    float lM  = FxaaLuma(rgbM);
    float lNW = FxaaLuma(rgbNW);
    float lNE = FxaaLuma(rgbNE);
    float lSW = FxaaLuma(rgbSW);
    float lSE = FxaaLuma(rgbSE);

    float lMin = min(lM, min(min(lNW,lNE), min(lSW,lSE)));
    float lMax = max(lM, max(max(lNW,lNE), max(lSW,lSE)));

    const float EDGE_THRESHOLD_MIN = 0.0312;
    const float EDGE_THRESHOLD     = 0.125;
    float range = lMax - lMin;
    if (range < max(EDGE_THRESHOLD_MIN, lMax * EDGE_THRESHOLD)) {
        return float4(rgbM, 1.0);          // flat region: no AA
    }

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    const float REDUCE_MUL = 1.0/8.0;
    const float REDUCE_MIN = 1.0/128.0;
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    const float SPAN_MAX = 8.0;
    dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * uRcpFrame;

    float3 rgbA = 0.5 * (
        uScene.Sample(uSamp, uv + dir * (1.0/3.0 - 0.5)).rgb +
        uScene.Sample(uSamp, uv + dir * (2.0/3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        uScene.Sample(uSamp, uv + dir * -0.5).rgb +
        uScene.Sample(uSamp, uv + dir *  0.5).rgb);

    float lB = FxaaLuma(rgbB);
    if (lB < lMin || lB > lMax) return float4(rgbA, 1.0);
    return float4(rgbB, 1.0);
}
)";

bool FxaaRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer)
        return false;

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, FXAA_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  FXAA_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    // Bilinear clamp sampler for the sub-pixel edge taps.
    {
        nvrhi::SamplerDesc sd;
        sd.setAllFilters(true); // linear
        sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_Sampler = m_Device->createSampler(sd);
    }

    // Binding layout: b0 (CB), t1 (scene color), s2 (sampler). Unique slots so the
    // Vulkan flat-binding offsets don't collide (same approach as the other passes).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::ConstantBuffer(0),
        nvrhi::BindingLayoutItem::Texture_SRV(1),
        nvrhi::BindingLayoutItem::Sampler(2)
    };
    nvrhi::VulkanBindingOffsets& offsets =
        nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0).setShaderResourceOffset(0).setSamplerOffset(0);

    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        layoutDesc.setBindingOffsets(offsets);
    }

    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    m_FrameCB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(FxaaFrameCB), "FxaaRenderPass FrameCB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    return true;
}

void FxaaRenderPass::Render(nvrhi::ICommandList* commandList,
                            nvrhi::IFramebuffer* frameBuffer,
                            SimulationSnapshot& /*snapshot*/,
                            const ECS* /*world*/,
                            double /*deltaTime*/,
                            FrameAllocator* /*frameAllocator*/)
{
    nvrhi::ITexture* scene = m_Renderer->GetSceneColorTexture();
    if (!scene)
        return; // nothing to resolve (FXAA off path never calls this)

    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        // No input layout: full-screen triangle from SV_VertexID.
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = false;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        nvrhi::BlendState::RenderTarget rt;
        rt.setBlendEnable(false)
          .setColorWriteMask(nvrhi::ColorMask::All);
        pso.renderState.blendState.setRenderTarget(0, rt);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    commandList->beginMarker("FxaaRenderPass");

    const auto fbi = frameBuffer->getFramebufferInfo();
    FxaaFrameCB cb{};
    cb.RcpFrame = glm::vec2(1.0f / static_cast<float>(fbi.width),
                            1.0f / static_cast<float>(fbi.height));
    commandList->writeBuffer(m_FrameCB, &cb, sizeof(cb));

    nvrhi::BindingSetDesc bindingDesc;
    bindingDesc.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameCB),
        nvrhi::BindingSetItem::Texture_SRV(1, scene),
        nvrhi::BindingSetItem::Sampler(2, m_Sampler)
    };
    nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingDesc, m_BindingLayout);

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.bindings = { bindingSet };
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments a;
    a.vertexCount = 3;
    commandList->draw(a);

    commandList->endMarker();
}

void FxaaRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingLayout = nullptr;
    m_Sampler = nullptr;
    m_FrameCB = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
    m_Device = nullptr;
    m_Renderer = nullptr;
}

void FxaaRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr;
}
```

- [ ] **Step 3: Add the source to the engine build**

In `src/engine/CMakeLists.txt`, after the line `src/rendering/passes/UiRenderPass.cpp` (line 42), add:

```cmake
    src/rendering/passes/FxaaRenderPass.cpp
```

- [ ] **Step 4: Configure + build Engine, verify it compiles**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: builds with no errors (pass compiled and references the now-existing `GetSceneColorTexture`, but not yet invoked).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/passes/FxaaRenderPass.h src/engine/src/rendering/passes/FxaaRenderPass.cpp src/engine/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): add FxaaRenderPass (full-screen FXAA 3.11 resolve)"
```

---

### Task 6: Renderer FXAA wiring (own the pass, split the loop, resolve)

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h`
- Modify: `src/engine/src/rendering/Renderer.cpp`

- [ ] **Step 1: Include the pass header + add the owned member**

In `src/engine/src/rendering/Renderer.h`, add the pass include near the existing includes (after `#include "MaterialSystem.h"`):

```cpp
#include "passes/FxaaRenderPass.h"
```

In the `private:` section, after the `ReleaseSceneColor();` declaration added in Task 4, add:

```cpp
    // FXAA resolve pass. Owned here (not in m_RenderPasses) and invoked between the
    // World and Overlay pass loops when FXAA is enabled.
    std::unique_ptr<FxaaRenderPass> m_FxaaPass;
```

- [ ] **Step 2: Create + initialize `m_FxaaPass` in `Init` and `InitForSwap`**

In `Renderer::Init`, after the UI pass is added (after line 140 `AddRenderPass(std::move(uiPass));`) and before `Engine::Registry().Register(&m_FrameAllocator);`, add:

```cpp
    // FXAA resolve pass: Renderer-owned, not part of m_RenderPasses.
    m_FxaaPass = std::make_unique<FxaaRenderPass>();
    if (!m_FxaaPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize FxaaRenderPass");
        return false;
    }
```

In `Renderer::InitForSwap`, after the UI pass is added (after line 618 `AddRenderPass(std::move(uiPass));`), add:

```cpp
    m_FxaaPass = std::make_unique<FxaaRenderPass>();
    if (!m_FxaaPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: FxaaPass failed"); return false; }
```

- [ ] **Step 3: Tear down `m_FxaaPass` in `Shutdown` and `TeardownForSwap`**

In `Renderer::Shutdown`, after the `m_RenderPasses.clear();` line (line 167), add:

```cpp
    if (m_FxaaPass) { m_FxaaPass->Shutdown(); m_FxaaPass.reset(); }
```

In `Renderer::TeardownForSwap`, after the `m_RenderPasses.clear();` line (line 517), add:

```cpp
    if (m_FxaaPass) { m_FxaaPass->Shutdown(); m_FxaaPass.reset(); }
```

- [ ] **Step 4: Forward resize to `m_FxaaPass`**

In `Renderer::Resize` (line 319), after the `for` loop that calls `pass->OnResize`, before the `if (m_Backend)` block, add:

```cpp
    if (m_FxaaPass) {
        m_FxaaPass->OnResize(width, height);
    }
```

- [ ] **Step 5: Include RenderStats in Renderer.cpp**

At the top of `src/engine/src/rendering/Renderer.cpp`, with the other includes, add (if not already present):

```cpp
#include "RenderStats.h"
```

- [ ] **Step 6: Split the render loop + insert the resolve**

In `src/engine/src/rendering/Renderer.cpp`, replace the block from the `m_FrameFog = ComputeFog(sunDir, fogComp);` line (line 252) through the end of the pass loop (line 283, the closing `}` of the `for` over `m_RenderPasses`) with:

```cpp
                m_FrameFog = ComputeFog(sunDir, fogComp);

                // FXAA path: world passes render into an offscreen scene-color SRV,
                // which the FXAA pass then resolves into sceneBuffer. UI draws on top,
                // un-AA'd. When disabled, worldTarget == sceneBuffer (today's path).
                bool fxaa = GetAntiAliasingSettings().FxaaEnabled && m_FxaaPass != nullptr;
                nvrhi::IFramebuffer* worldTarget = sceneBuffer;
                if (fxaa) {
                    const auto& sfbi = sceneBuffer->getFramebufferInfo();
                    nvrhi::ITexture* sharedDepth = sceneBuffer->getDesc().depthAttachment.texture;
                    EnsureSceneColor(sfbi.width, sfbi.height, sharedDepth, sfbi.colorFormats[0]);
                    if (m_SceneColor.Fb) worldTarget = m_SceneColor.Fb;
                    else fxaa = false; // allocation failed -> safe fallback to direct path
                }

                const auto sceneClear = fogComp.Enabled
                    ? nvrhi::Color(m_FrameFog.Color.r, m_FrameFog.Color.g, m_FrameFog.Color.b, 1.0f)
                    : nvrhi::Color(red, green, blue, 1.0f);
                nvrhi::utils::ClearColorAttachment(m_CommandList, worldTarget, 0, sceneClear);
                if (sceneBuffer != frameBuffer) {
                    // Offscreen scene: clear the swapchain (dark) so the present surface is clean
                    // behind the ImGui dockspace. Depth of the scene target is cleared in GBufferFillPass.
                    nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, nvrhi::Color(0.1f, 0.1f, 0.1f, 1.0f));
                }

                // Resolve the camera the world passes use this frame. Editor override (set by the
                // ImGui overlay last frame) wins when active; otherwise the game's WorldCameraComponent
                // from the snapshot. Runtime never sets EditorCameraActive -> always the game camera.
                {
                    CameraView active{}; // identity V/P, zero pos: matches the passes' old null fallback
                    if (m_AppContext && m_AppContext->EditorCameraActive.load(std::memory_order_relaxed)) {
                        active = m_AppContext->EditorCamera.load();
                    } else if (world) {
                        if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
                            active = { cam->View, cam->Projection, cam->Position };
                    }
                    m_ActiveCamera = active;
                }

                // World passes -> world target (offscreen scene color when FXAA on, else sceneBuffer).
                for (auto& pass : m_RenderPasses) {
                    ZoneScopedN("RenderPass Rec N");
                    if (pass && pass->Stage() == IRenderPass::RenderStage::World) {
                        pass->Render(m_CommandList, worldTarget, snapshot, world, deltaTime, &m_FrameAllocator);
                    }
                }

                // FXAA resolve: scene-color SRV -> sceneBuffer.
                if (fxaa) {
                    m_FxaaPass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                }

                // Overlay passes (UI) on top of sceneBuffer, after the resolve.
                for (auto& pass : m_RenderPasses) {
                    if (pass && pass->Stage() == IRenderPass::RenderStage::Overlay) {
                        pass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                    }
                }
```

> The original scene-clear and camera-resolve code (previously at lines 254-276) is folded into the replacement above — delete the originals so they are not duplicated. The `EnsureGBuffer(...)` call earlier (lines 226-231) stays unchanged; the G-buffer keeps sharing `sceneBuffer`'s depth, which is the same depth `m_SceneColor.Fb` uses.

- [ ] **Step 7: Build Engine + both exes**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine editor runtime`
Expected: builds with no errors.

- [ ] **Step 8: GUI smoke (manual)**

Launch the editor. With FXAA on (default), near-45° mesh silhouette edges under the iso camera should look smoother. Verify the scene is not broken (correct colors, depth, sky, UI text crisp). Run once on **DX12** and once on **Vulkan** (`editor.exe --backend=vulkan`) to catch flat-binding regressions. (Live toggle UI lands in Task 7; for now confirm default-on renders correctly.)

- [ ] **Step 9: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): wire FXAA resolve into Renderer (stage split + scene-color target)"
```

---

### Task 7: Editor toggle + engine_settings persistence

**Files:**
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp:324-325`

- [ ] **Step 1: Add the FXAA checkbox to the panel**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, after the Shadows block (after line 39, before `ImGui::End();`), add:

```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Anti-Aliasing");
    AntiAliasingSettings& aa = GetAntiAliasingSettings();
    changed |= ImGui::Checkbox("FXAA", &aa.FxaaEnabled);
```

- [ ] **Step 2: Persist FXAA to engine_settings.json on change**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, add the SettingsManager include with the other includes (after line 17 `#include "EditorPreferences.h"`):

```cpp
#include "utilities/SettingsManager.h"
```

Then replace the block at lines 324-325:

```cpp
        if (DrawRenderStatsPanel(&s_ShowRenderStatsPanel))
            EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
```

with:

```cpp
        if (DrawRenderStatsPanel(&s_ShowRenderStatsPanel)) {
            // Debug-draw/shadow toggles persist in editor_preferences.json.
            EditorPreferences::Save(EditorPreferences::DEFAULT_PREFERENCES_PATH, m_EditorCamera.GetState());
            // FXAA persists in engine_settings.json (engine tier). Only write when it
            // actually changed so unrelated panel edits don't rewrite engine settings.
            const bool fxaaNow = GetAntiAliasingSettings().FxaaEnabled;
            if (m_AppContext && fxaaNow != m_AppContext->Settings.fxaaEnabled) {
                m_AppContext->Settings.fxaaEnabled = fxaaNow;
                SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings);
            }
        }
```

- [ ] **Step 3: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: builds with no errors.

- [ ] **Step 4: GUI smoke (manual)**

Launch the editor → Render Stats panel → toggle **FXAA**. Edges should smooth/sharpen live (1-2 frame latency). Toggle off: frame should match pre-FXAA rendering (no artifacts, UI text crisp). Close + relaunch the editor: the toggle state should persist (verify `engine_settings.json` next to `editor.exe` has `"renderer": { ..., "fxaa": <bool> }`). Confirm `runtime.exe` honors the persisted default. Re-check on **Vulkan**.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): FXAA toggle in Render Stats, persisted to engine_settings.json"
```

---

## Final verification

- [ ] Run the unit test: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_settings.exe` → `All settings tests passed.`
- [ ] Run the existing ECS test to confirm no regression: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs` then `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` → `All ECS tests passed.`
- [ ] DX12 + Vulkan GUI smoke per Task 6/7.

## Self-review notes (vs. spec)

- Spec "seed in both mains" is implemented once in `Application::Init` (the single load site both exes route through) — strictly better than duplicating in two `main.cpp`s. No behavior gap.
- Spec mentioned a separate `.hlsl` file; the codebase inlines shader HLSL as string literals in each pass `.cpp` (Sky/Lighting), so FXAA follows that — no separate shader file, no CMake shader entry.
- Task order is build-driven: Task 4 adds `GetSceneColorTexture()` before Task 5's pass calls it; Task 5 defines `FxaaRenderPass` before Task 6's Renderer.h includes/owns it.
- All other spec items (RenderStage split, m_SceneColor SRV + lazy alloc + zero-overhead-off, FxaaRenderPass mirroring Sky/Lighting with Vulkan flat-binding, AntiAliasingSettings, engine_settings persistence, editor toggle, settings round-trip test) map to Tasks 1-7. No ECS.h / GAME_API_VERSION change.
