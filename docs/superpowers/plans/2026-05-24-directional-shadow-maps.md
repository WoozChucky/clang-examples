# Directional Shadow Maps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Real-time directional (sun) shadows via a single dynamic shadow map: a depth pass from the light POV → `MeshRenderPass` samples it (3×3 PCF), tracking the day/night sun and gated off at night.

**Architecture:** Pure `ShadowMath.h` (light-VP fit + sun-up). Renderer owns the D32 shadow texture + depth-only FB + comparison sampler + a shared `ShadowView{LightVP,Enabled}`. A `ShadowDepthPass` (before Mesh) fits the light box to the visible-mesh AABB and renders depth; `MeshRenderPass` binds the map (t6/s4) and multiplies the directional term by a PCF shadow factor. Toggles in a `GetShadowSettings()` global.

**Tech Stack:** C++23, NVRHI (DX12/Vulkan), inline HLSL via DXC, GLM (RH, depth `[0,1]`), CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-directional-shadow-maps-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump.** New engine sources → reconfigure. `lib.h`/shared headers aside, most changes are engine-internal.
- Commit author repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit `git log -1 --format='%an <%ae>'` must show the personal email.
- **GPU-API verify discipline:** several steps touch NVRHI specifics (depth-as-SRV, comparison sampler, resource states, depth-only PSO). Where a step says VERIFY, read the real `nvrhi.h` / existing passes and match the actual API; keep behavior as designed. The clean full build is the validation.

---

### Task 1: `ShadowMath.h` (pure) + `test_shadowmath` (TDD)

**Files:**
- Create: `src/engine/src/rendering/ShadowMath.h` (header-only)
- Create: `tests/test_shadowmath.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the header**

`src/engine/src/rendering/ShadowMath.h`:
```cpp
#pragma once
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// The sun casts (is above the horizon) when its travel direction points downward (y < 0).
// eps trims the near-horizontal degenerate sliver (infinite shadows at the exact horizon).
inline bool IsSunUp(const glm::vec3& sunDir, float eps = 0.02f) {
    return sunDir.y < -eps;
}

// Orthographic light view-projection framing a sphere (scene bounds: center, radius) along the sun
// direction. sunDir = the direction the light travels (away from the sun). RH + ZO depth ([0,1]).
inline glm::mat4 ComputeLightViewProj(const glm::vec3& center, float radius, const glm::vec3& sunDir) {
    const glm::vec3 d = glm::normalize(sunDir);
    const float r = (radius > 1e-3f) ? radius : 1.0f;
    const glm::vec3 up = (std::abs(d.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::vec3 eye = center - d * (r * 2.0f);           // back off toward the sun
    const glm::mat4 view = glm::lookAtRH(eye, center, up);
    const glm::mat4 proj = glm::orthoRH_ZO(-r, r, -r, r, 0.0f, 4.0f * r);
    return proj * view;
}
```

- [ ] **Step 2: Write the failing test**

`tests/test_shadowmath.cpp`:
```cpp
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>

#include "ShadowMath.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static glm::vec3 proj_ndc(const glm::mat4& vp, const glm::vec3& p) {
    glm::vec4 c = vp * glm::vec4(p, 1.0f);
    return glm::vec3(c) / c.w;
}

static void T00_sun_up_gate()
{
    EXPECT(IsSunUp(glm::vec3(0, -1, 0)));               // straight down = noon
    EXPECT(IsSunUp(glm::normalize(glm::vec3(0, -0.5f, 0.5f)))); // descending, still up
    EXPECT(!IsSunUp(glm::vec3(0, 1, 0)));               // pointing up = sun below horizon
    EXPECT(!IsSunUp(glm::vec3(0, 0, 1)));               // horizontal = at horizon (gated)
}

static void T01_center_projects_to_ndc_origin()
{
    const glm::vec3 center(3, 1, -2);
    const glm::mat4 vp = ComputeLightViewProj(center, 5.0f, glm::vec3(0, -1, 0));
    const glm::vec3 n = proj_ndc(vp, center);
    EXPECT(std::abs(n.x) < 1e-3f);
    EXPECT(std::abs(n.y) < 1e-3f);
    EXPECT(n.z > 0.0f && n.z < 1.0f);                  // within ZO depth
}

static void T02_scene_fits_inside_light_frustum()
{
    const glm::vec3 center(0, 0, 0);
    const float r = 4.0f;
    const glm::mat4 vp = ComputeLightViewProj(center, r, glm::normalize(glm::vec3(0.3f, -1, 0.2f)));
    // The 8 corners of the bounding box [center-r, center+r] must land inside the light NDC.
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner(center.x + ((i&1)?r:-r), center.y + ((i&2)?r:-r), center.z + ((i&4)?r:-r));
        glm::vec3 n = proj_ndc(vp, corner);
        EXPECT(n.x >= -1.001f && n.x <= 1.001f);
        EXPECT(n.y >= -1.001f && n.y <= 1.001f);
        EXPECT(n.z >= -0.001f && n.z <= 1.001f);
    }
}

static void T03_straight_down_no_nan()
{
    const glm::mat4 vp = ComputeLightViewProj(glm::vec3(0), 2.0f, glm::vec3(0, -1, 0)); // up degenerate
    const glm::vec3 n = proj_ndc(vp, glm::vec3(0));
    EXPECT(!std::isnan(n.x) && !std::isnan(n.y) && !std::isnan(n.z));
}

int main()
{
    T00_sun_up_gate();
    T01_center_projects_to_ndc_origin();
    T02_scene_fits_inside_light_frustum();
    T03_straight_down_no_nan();

    if (g_Failures == 0) { std::printf("All shadow math tests passed.\n"); return 0; }
    std::printf("%d shadow math test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_shadowmath` CMake target**

In `tests/CMakeLists.txt`, after the last test block (e.g. `test_debugdraw`), append a target compiling only `test_shadowmath.cpp`, linking `glm::glm`, include dir `${CMAKE_SOURCE_DIR}/src/engine/src/rendering`, the three GLM defines (`GLM_FORCE_DEPTH_ZERO_TO_ONE`/`GLM_FORCE_RIGHT_HANDED`/`GLM_ENABLE_EXPERIMENTAL`), `RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"`, `FOLDER Tests` — copy the `test_debugdraw` block verbatim and rename to `test_shadowmath`.

- [ ] **Step 4: Reconfigure + build + run (expect PASS)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: `All shadow math tests passed.` (TDD: build before the header exists to see red.) Fix the HEADER if an assertion fails (not the test). If T02 fails because a corner sits exactly on a face and floating error pushes it just outside the ±1.001 tolerance, that's expected-tight — STOP and report rather than loosening beyond ~1.01.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/ShadowMath.h tests/test_shadowmath.cpp tests/CMakeLists.txt
git commit -m "feat: add ShadowMath.h (light view-proj fit + sun-up) + test_shadowmath"
```

---

### Task 2: Renderer-owned shadow resources + `ShadowSettings` + UI

**Files:**
- Modify: `src/engine/src/rendering/Renderer.h`
- Modify: `src/engine/src/rendering/Renderer.cpp`
- Modify: `src/engine/src/rendering/RenderStats.h`
- Modify: `src/engine/src/rendering/RenderStats.cpp`
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`

No unit test (GPU resources + UI); verified by build. Must precede Tasks 3/4 (they read these accessors + settings).

- [ ] **Step 1: `Renderer.h` — ShadowView, members, accessors**

Add (near the `CameraView m_ActiveCamera`/`GetActiveCamera` area):
```cpp
    struct ShadowView { glm::mat4 LightVP{1.0f}; int Enabled = 0; };

    // public accessors (next to GetActiveCamera):
    nvrhi::ITexture*     GetShadowDepthTexture() const { return m_ShadowDepth; }
    nvrhi::ISampler*     GetShadowSampler()      const { return m_ShadowSampler; }
    nvrhi::IFramebuffer* GetShadowFramebuffer()  const { return m_ShadowFb; }
    ShadowView&          GetShadowView()               { return m_ShadowView; }
    static constexpr uint32_t kShadowMapSize = 2048;
```
Private members (next to `m_ActiveCamera`):
```cpp
    nvrhi::TextureHandle     m_ShadowDepth;
    nvrhi::FramebufferHandle m_ShadowFb;
    nvrhi::SamplerHandle     m_ShadowSampler;
    ShadowView               m_ShadowView{};
```
Private helper declaration (next to `CreateDefaultMaterialResources`):
```cpp
    void CreateShadowResources();
```
(`glm::mat4` is available via the existing includes; if not, add `#include <glm/mat4x4.hpp>`.)

- [ ] **Step 2: `Renderer.cpp` — create + release the shadow resources**

Add the helper (model the texture/FB on `SceneViewport.cpp`, the sampler on `CreateDefaultMaterialResources`):
```cpp
void Renderer::CreateShadowResources()
{
    nvrhi::TextureDesc td;
    td.width  = kShadowMapSize;
    td.height = kShadowMapSize;
    td.format = nvrhi::Format::D32;
    td.dimension = nvrhi::TextureDimension::Texture2D;
    td.isRenderTarget = true;
    td.isShaderResource = true;            // sampled by MeshRenderPass
    td.initialState = nvrhi::ResourceStates::ShaderResource;
    td.keepInitialState = true;            // nvrhi auto-transitions DepthWrite<->ShaderResource per cmd list
    td.debugName = "ShadowDepthMap";
    m_ShadowDepth = m_Device->createTexture(td);

    m_ShadowFb = m_Device->createFramebuffer(
        nvrhi::FramebufferDesc().setDepthAttachment(m_ShadowDepth));

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true);
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sd.reductionType  = nvrhi::SamplerReductionType::Comparison;
    sd.comparisonFunc = nvrhi::ComparisonFunc::LessOrEqual;
    m_ShadowSampler = m_Device->createSampler(sd);
}
```
VERIFY against `nvrhi.h`: `SamplerReductionType::Comparison`, `ComparisonFunc::LessOrEqual`, `SamplerDesc.reductionType`/`.comparisonFunc` field names; that a `D32` texture with `isShaderResource=true` is bindable as `Texture_SRV` (NVRHI exposes the depth as a float SRV — if it needs an explicit SRV format, set it). If `setAllFilters(true)` conflicts with a comparison sampler on this NVRHI version, use the filter setup the API expects for comparison sampling.

Call `CreateShadowResources();` in **`Init`** and **`InitForSwap`** right after the device/backend are ready + the resource systems are set up (near where `CreateDefaultMaterialResources`/mesh+material systems initialize, BEFORE adding passes). In **`Shutdown`** and **`TeardownForSwap`**, release them (set `m_ShadowDepth = nullptr; m_ShadowFb = nullptr; m_ShadowSampler = nullptr;`) alongside the other GPU-resource teardown.

- [ ] **Step 3: `RenderStats.h` — ShadowSettings + accessor**

After the `DebugDrawSettings` block + its `GetDebugDrawSettings()` decl, add:
```cpp
struct ShadowSettings {
    bool  Enabled = true;     // shadows are a real render feature -> on by default (editor + runtime)
    float Bias    = 0.0015f;  // PS comparison-depth bias (slope-scaled)
};
```
and after `ENGINE_API DebugDrawSettings& GetDebugDrawSettings();`:
```cpp
ENGINE_API ShadowSettings& GetShadowSettings();
```

- [ ] **Step 4: `RenderStats.cpp` — define it**

After `GetDebugDrawSettings()`:
```cpp
ShadowSettings& GetShadowSettings()
{
    static ShadowSettings s;
    return s;
}
```

- [ ] **Step 5: `RenderStatsPanel.cpp` — UI**

Before the final `ImGui::End();`, add:
```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Shadows");
    ShadowSettings& sh = GetShadowSettings();
    ImGui::Checkbox("Shadows", &sh.Enabled);
    ImGui::SliderFloat("Shadow bias", &sh.Bias, 0.0f, 0.01f, "%.4f");
```

- [ ] **Step 6: Reconfigure, build editor + runtime**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
```
Expected: clean (resources created/destroyed but not yet used).

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/Renderer.h src/engine/src/rendering/Renderer.cpp src/engine/src/rendering/RenderStats.h src/engine/src/rendering/RenderStats.cpp src/editor/src/rendering/imgui/RenderStatsPanel.cpp
git commit -m "feat: Renderer shadow resources (D32 map + comparison sampler + ShadowView) + ShadowSettings UI"
```

---

### Task 3: `ShadowDepthPass` (depth render from the sun) + register

**Files:**
- Create: `src/engine/src/rendering/passes/ShadowDepthPass.h`
- Create: `src/engine/src/rendering/passes/ShadowDepthPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/src/rendering/Renderer.cpp`

No unit test (GPU pass); verified by build + smoke. Builds on Tasks 1+2.

- [ ] **Step 1: `ShadowDepthPass.h`**

`src/engine/src/rendering/passes/ShadowDepthPass.h`:
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include "IRenderPass.h"

class Renderer;

// Renders scene depth from the sun's POV into the Renderer's shadow map (depth-only, front-face cull),
// fitting an ortho light frustum to the visible-mesh AABB. Gated by GetShadowSettings().Enabled +
// IsSunUp; when gated off it sets Renderer.GetShadowView().Enabled = 0 and skips. Runs before MeshRenderPass.
class ShadowDepthPass : public IRenderPass
{
public:
    ShadowDepthPass() = default;
    ~ShadowDepthPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot, const ECS* world, double deltaTime,
                FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    struct ShadowCB { glm::mat4 LightVP; glm::mat4 Model; };
    static_assert(sizeof(ShadowCB) % 16 == 0, "ShadowCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;
    nvrhi::ShaderHandle m_VS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;
};
```

- [ ] **Step 2: `ShadowDepthPass.cpp`**

`src/engine/src/rendering/passes/ShadowDepthPass.cpp`:
```cpp
#include "ShadowDepthPass.h"

#include <nvrhi/utils.h>
#include <glm/glm.hpp>
#include <limits>

#include "Renderer.h"
#include "MeshSystem.h"
#include "RenderStats.h"
#include "ShadowMath.h"
#include "ECS.h"
#include "TransformMath.h"
#include "Frustum.h"      // TransformAABB
#include "lib.h"

namespace {
const char* SHADOW_VS_HLSL = R"(
cbuffer ShadowCB : register(b0) { float4x4 LightVP; float4x4 Model; };
struct VSIn  { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD; };
float4 main_vs(VSIn vin) : SV_Position { return mul(LightVP, mul(Model, float4(vin.Position, 1.0))); }
)";
} // namespace

bool ShadowDepthPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(ShadowCB), "ShadowDepthPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, SHADOW_VS_HLSL, 0, "main_vs", "vs_6_1");
    if (!m_VS) return false;

    nvrhi::VertexAttributeDesc attrs[3];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, px)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[1].setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(MeshVertex, nx)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    attrs[2].setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(MeshVertex, u)).setBufferIndex(0).setElementStride(sizeof(MeshVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 3, m_VS);

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
    if (m_Device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        layoutDesc.setBindingOffsets(nvrhi::VulkanBindingOffsets{}.setConstantBufferOffset(0));
    m_BindingLayout = m_Device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc bsd;
    bsd.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_CB) };
    m_BindingSet = m_Device->createBindingSet(bsd, m_BindingLayout);
    return true;
}

void ShadowDepthPass::Render(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* /*frameBuffer*/,
                             SimulationSnapshot& /*snapshot*/, const ECS* world, double /*dt*/,
                             FrameAllocator* /*fa*/)
{
    Renderer::ShadowView& sv = m_Renderer->GetShadowView();
    sv.Enabled = 0;
    if (!world || !GetShadowSettings().Enabled)
        return;

    // Sun direction (directional light).
    glm::vec3 sunDir(0.0f, -1.0f, 0.0f);
    bool haveSun = false;
    world->Each<TransformComponent, LightningComponent>(
        [&](EntityId, const TransformComponent&, const LightningComponent& l) {
            if (l.Type == LightningType::Directional) { sunDir = glm::vec3(l.Direction); haveSun = true; }
        });
    if (!haveSun || !IsSunUp(sunDir))
        return;

    // Fit to the visible-mesh world AABB.
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
    bool any = false;
    MeshSystem* ms = m_Renderer->GetMeshSystem();
    world->Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m) {
            if (!m.Visible) return;
            const auto b = ms->GetMeshBounds(m.MeshId);
            if (!b.valid) return;
            glm::vec3 wMin, wMax;
            TransformAABB(ModelMatrix(t), b.min, b.max, wMin, wMax);
            mn = glm::min(mn, wMin); mx = glm::max(mx, wMax); any = true;
        });
    if (!any)
        return;

    const glm::vec3 center = 0.5f * (mn + mx);
    const float radius = 0.5f * glm::length(mx - mn);
    const glm::mat4 lightVP = ComputeLightViewProj(center, radius, sunDir);
    sv.LightVP = lightVP;
    sv.Enabled = 1;

    nvrhi::IFramebuffer* shadowFb = m_Renderer->GetShadowFramebuffer();

    if (!m_Pipeline) {
        const auto fbi = shadowFb->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = nullptr;                                  // depth-only
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = true;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front; // peter-panning mitigation
        pso.renderState.rasterState.setFrontCounterClockwise(true);
        m_Pipeline = m_Device->createGraphicsPipeline(pso, shadowFb);
    }

    commandList->clearDepthStencilTexture(shadowFb->getDesc().depthAttachment.texture,
                                          nvrhi::AllSubresources, true, 1.0f, false, 0);

    world->Each<TransformComponent, MeshComponent>(
        [&](EntityId, const TransformComponent& t, const MeshComponent& m) {
            if (!m.Visible) return;
            if (!ms->IsValidMeshId(m.MeshId)) return;
            const auto res = ms->GetMeshResources(m.MeshId);
            if (!res.valid) return;

            ShadowCB cb{}; cb.LightVP = lightVP; cb.Model = ModelMatrix(t);
            commandList->writeBuffer(m_CB, &cb, sizeof(cb));

            nvrhi::GraphicsState state;
            state.pipeline = m_Pipeline;
            state.framebuffer = shadowFb;
            state.viewport.addViewportAndScissorRect(shadowFb->getFramebufferInfo().getViewport());
            state.bindings = { m_BindingSet };
            state.vertexBuffers = { nvrhi::VertexBufferBinding(res.vertexBuffer, 0, 0) };
            state.indexBuffer = nvrhi::IndexBufferBinding(res.indexBuffer, nvrhi::Format::R32_UINT, 0);
            commandList->setGraphicsState(state);

            if (res.subMeshes.size() > 0) {
                for (const auto& sm : res.subMeshes) {
                    nvrhi::DrawArguments a{}; a.vertexCount = sm.IndexCount; a.instanceCount = 1;
                    a.startIndexLocation = sm.IndexStart; commandList->drawIndexed(a);
                }
            } else {
                nvrhi::DrawArguments a{}; a.vertexCount = res.indexCount; a.instanceCount = 1;
                commandList->drawIndexed(a);
            }
        });
}

void ShadowDepthPass::OnResize(uint32_t, uint32_t) {}   // shadow map is fixed-size; pipeline keyed on it
void ShadowDepthPass::Shutdown()
{
    m_Pipeline = nullptr; m_BindingSet = nullptr; m_CB = nullptr;
    m_InputLayout = nullptr; m_BindingLayout = nullptr; m_VS = nullptr;
}
```
VERIFY: `MeshSystem::GetMeshResources`/`IsValidMeshId`/`GetMeshBounds` + the `res` fields (`vertexBuffer`,`indexBuffer`,`indexCount`,`subMeshes` with `IndexCount`/`IndexStart`) against `OutlineRenderPass.cpp` (which uses the same) + `MeshSystem.h`. `Renderer::ShadowView` is a nested type — reference it as `Renderer::ShadowView` (or adjust if it's declared elsewhere). `pso.PS = nullptr` for depth-only must be accepted by NVRHI; if not, supply a trivial empty PS. Pipeline is keyed on the fixed shadow FB (never resized), so `OnResize` is a no-op. Confirm `clearDepthStencilTexture` signature matches `MeshRenderPass.cpp:320`.

- [ ] **Step 3: Add the source to the engine target**

In `src/engine/CMakeLists.txt`, add next to `OutlineRenderPass.cpp`: `src/rendering/passes/ShadowDepthPass.cpp`.

- [ ] **Step 4: Register before MeshRenderPass (Init + InitForSwap)**

Add `#include "passes/ShadowDepthPass.h"` by the other pass includes. In BOTH `Init` and `InitForSwap`, AFTER the primitive pass `AddRenderPass(std::move(primitivePass));` and BEFORE the mesh pass block, insert:
```cpp
    auto shadowPass = std::make_unique<ShadowDepthPass>();
    if (!shadowPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize ShadowDepthPass");
        return false;
    }
    AddRenderPass(std::move(shadowPass));
```
(For `InitForSwap`, match its terser style.) New order: Primitive → Shadow → Mesh → Outline → Debug → Ui.

- [ ] **Step 5: Reconfigure, build engine + editor + runtime**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
```
Expected: clean (the shadow map renders; MeshRenderPass doesn't sample it yet — Task 4).

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/passes/ShadowDepthPass.h src/engine/src/rendering/passes/ShadowDepthPass.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp
git commit -m "feat: add ShadowDepthPass (sun-POV depth render, AABB-fit) + register before Mesh"
```

---

### Task 4: `MeshRenderPass` samples the shadow map (3×3 PCF)

**Files:**
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.h`
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp`

No unit test (GPU shading); verified by build + the GUI smoke. Builds on Tasks 2+3.

- [ ] **Step 1: Expand `PerFrameCB` (`MeshRenderPass.h`)**

Replace the `PerFrameCB` struct with (adds the light VP + shadow scalars; keep 16-byte alignment):
```cpp
    struct PerFrameCB
    {
        glm::mat4 P;
        glm::mat4 VP;
        glm::mat4 LightVP;            // shadow light view-projection
        DirectionalLight DirectionalLight;
        uint32_t PointLightCount = 0;
        float    Ambient = 0.0f;
        int      ShadowEnabled = 0;
        float    ShadowBias = 0.0f;   // 4 uints/floats after the two vec4s -> 16-byte aligned
    };
```

- [ ] **Step 2: Fill the new fields (`MeshRenderPass.cpp`, the PerFrameCB block ~362-376)**

After `perFrame.Ambient = 0.1f;` and before `writeBuffer(m_PerFrameCB, ...)`, add:
```cpp
        const Renderer::ShadowView& sv = m_Renderer->GetShadowView();
        perFrame.LightVP = sv.LightVP;
        perFrame.ShadowEnabled = sv.Enabled;
        perFrame.ShadowBias = GetShadowSettings().Bias;
```
(`RenderStats.h` is already included in this file; `Renderer.h` too.)

- [ ] **Step 3: Add the shadow SRV + comparison sampler to the binding layout (~232-241)**

Append to the `layoutDesc.bindings` list:
```cpp
        nvrhi::BindingLayoutItem::Texture_SRV(6),
        nvrhi::BindingLayoutItem::Sampler(4)
```
(The existing Vulkan `setBindingOffsets` with offsets 0 already covers these new slots — confirm.)

- [ ] **Step 4: Bind the shadow resources in the per-batch binding set (~523-531)**

Append to the `bindingDesc.bindings` list:
```cpp
        nvrhi::BindingSetItem::Texture_SRV(6, m_Renderer->GetShadowDepthTexture()),
        nvrhi::BindingSetItem::Sampler(4, m_Renderer->GetShadowSampler())
```

- [ ] **Step 5: HLSL — CB fields + PCF sampling (`MESH_PS_HLSL`)**

In the PS `cbuffer PerFrame : register(b0)`, replace its body to match the new C++ struct:
```hlsl
cbuffer PerFrame : register(b0)
{
    float4x4 uP;
    float4x4 uVP;
    float4x4 uLightVP;
    DirectionalLight uDirLight;
    uint  uPointLightCount;
    float uAmbient;
    int   uShadowEnabled;
    float uShadowBias;
};
```
Add the shadow resources + a PCF helper above `main_ps`:
```hlsl
Texture2D              uShadowMap  : register(t6);
SamplerComparisonState uShadowSamp : register(s4);

float ShadowFactor(float3 worldPos, float ndl)
{
    if (uShadowEnabled == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos, 1.0));
    float3 p = lp.xyz / lp.w;
    float2 uv = p.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;                                  // clip -> [0,1], D3D top-left
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0; // outside map = lit
    float bias = uShadowBias * (1.0 + (1.0 - ndl) * 2.0);
    float d = p.z - bias;
    float sum = 0.0;
    const float texel = 1.0 / 2048.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        sum += uShadowMap.SampleCmpLevelZero(uShadowSamp, uv + float2(x, y) * texel, d);
    return sum / 9.0;
}
```
Then in `main_ps`, change the directional term (line ~157) from:
```hlsl
    lighting += diffuse * uDirLight.Color.rgb;
```
to:
```hlsl
    float ndl = max(dot(N, -lightDir), 0.0);
    lighting += diffuse * uDirLight.Color.rgb * ShadowFactor(i.WorldPos, ndl);
```
(`diffuse` already equals that `max(dot(N,-lightDir),0)` — reuse it for `ndl` to avoid recomputing: `float ndl = diffuse;` is equivalent. Keep ambient + point lights unshadowed.) Mirror the same `cbuffer PerFrame` field additions in the **VS** HLSL block if the VS also declares `PerFrame` (only add the fields if its cbuffer must byte-match — the VS must use the identical b0 layout; update its `cbuffer PerFrame` to the same fields).

VERIFY: the VS `cbuffer PerFrame` layout must byte-match the PS one (same b0). Update BOTH to the new field list so the CB is consistent. `SampleCmpLevelZero` + `SamplerComparisonState` are the correct HLSL for a comparison sampler. `i.WorldPos` is `TEXCOORD1` in `PSIn` (already present).

- [ ] **Step 6: Build engine + editor + runtime + FULL regression**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking test_editorcam test_metrichistory test_transientstatus test_logformat test_debugdraw test_shadowmath
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_logformat.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_debugdraw.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_shadowmath.exe
```
Expected: all build clean; each test prints its `All ... passed.` line.

- [ ] **Step 7: Commit**

```bash
git add src/engine/src/rendering/passes/MeshRenderPass.h src/engine/src/rendering/passes/MeshRenderPass.cpp
git commit -m "feat: MeshRenderPass samples the shadow map (3x3 PCF) on the directional term"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (editor, runtime, game, all test_*).
- [ ] All 11 unit suites print their pass line (incl. `test_shadowmath`).
- [ ] **GUI smoke (user-run; surface to the user — do not self-approve):** a mesh on the ground casts a
  sun shadow that **moves as the day/night cycle rotates the sun**; the shadow disappears at night (sun
  below horizon) and returns at sunrise; the Render Stats "Shadows" checkbox toggles it; the "Shadow
  bias" slider trades acne (too low → surface speckle) vs peter-panning (too high → detached shadow);
  `runtime.exe` shows shadows too (default on). Point-light + ambient lighting are unaffected by shadows.

## Notes / non-goals
- No `GAME_API_VERSION` bump (renderer-internal).
- Single shadow map (no CSM); directional light only; per-mesh depth draws (instanced shadow batching
  deferred); fixed 2048²; 3×3 PCF; bias applied in the PS (live slider, no PSO rebuild).
- Shadows default ON in both editor + runtime (a real render feature, not debug).
