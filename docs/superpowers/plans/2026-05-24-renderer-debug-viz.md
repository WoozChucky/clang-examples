# Renderer Debug Visualization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add render-thread debug visualization — a debug-draw line API, gizmos (light/camera-frustum/selected-AABB) drawn by a new `DebugRenderPass`, and a mesh wireframe toggle — all controlled from the Render Stats panel.

**Architecture:** Pure geometry builders (`DebugDraw.h`) append line-list `DebugVertex`es to a reused `std::vector`; a `DebugRenderPass` (modeled on `OutlineRenderPass`) builds gizmos from the ECS snapshot each frame, uploads the vector to a GPU vertex buffer, and draws line-list (depth-test on / write off) through `Renderer::GetActiveCamera()`. Toggles live in a `GetDebugDrawSettings()` engine global (same pattern as `GetCullingSettings()`); `MeshRenderPass` gains a wireframe pipeline variant.

**Tech Stack:** C++23, NVRHI (DX12/Vulkan), inline HLSL via DXC, GLM (RH, depth `[0,1]`), CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-renderer-debug-viz-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump** (no `GameState`/export/ECS-component change). New engine sources → reconfigure.
- Commit author repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit `git log -1 --format='%an <%ae>'` must show the personal email.

---

### Task 1: `DebugDraw.h` geometry builders + `test_debugdraw` (TDD)

**Files:**
- Create: `src/engine/src/rendering/DebugDraw.h` (header-only)
- Create: `tests/test_debugdraw.cpp`
- Modify: `tests/CMakeLists.txt`

TDD: write test, build (fail — header missing), implement, build (pass).

- [ ] **Step 1: Write the header**

`src/engine/src/rendering/DebugDraw.h`:
```cpp
#pragma once
#include <vector>
#include <cmath>
#include <glm/glm.hpp>

// One vertex of debug line geometry. Line-list topology (pairs of verts = segments).
struct DebugVertex {
    glm::vec3 Position;
    glm::vec4 Color;
};

inline void DebugAppendLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b,
                            const glm::vec4& color) {
    out.push_back({a, color});
    out.push_back({b, color});
}

// 12 edges of the AABB [mn, mx] (24 verts).
inline void DebugAppendBox(std::vector<DebugVertex>& out, const glm::vec3& mn, const glm::vec3& mx,
                           const glm::vec4& color) {
    const glm::vec3 c[8] = {
        {mn.x,mn.y,mn.z},{mx.x,mn.y,mn.z},{mx.x,mx.y,mn.z},{mn.x,mx.y,mn.z},
        {mn.x,mn.y,mx.z},{mx.x,mn.y,mx.z},{mx.x,mx.y,mx.z},{mn.x,mx.y,mx.z},
    };
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& ed : e) DebugAppendLine(out, c[ed[0]], c[ed[1]], color);
}

// 3 axis-aligned circles (XY, XZ, YZ), `segments` line-segments each (3*segments*2 verts).
inline void DebugAppendSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius,
                              const glm::vec4& color, int segments = 24) {
    const float kTwoPi = 6.28318530718f;
    for (int axis = 0; axis < 3; ++axis) {
        for (int i = 0; i < segments; ++i) {
            const float t0 = kTwoPi * (float(i)     / float(segments));
            const float t1 = kTwoPi * (float(i + 1) / float(segments));
            const float c0 = std::cos(t0) * radius, s0 = std::sin(t0) * radius;
            const float c1 = std::cos(t1) * radius, s1 = std::sin(t1) * radius;
            glm::vec3 p0, p1;
            if (axis == 0)      { p0 = {c0, s0, 0.0f}; p1 = {c1, s1, 0.0f}; }
            else if (axis == 1) { p0 = {c0, 0.0f, s0}; p1 = {c1, 0.0f, s1}; }
            else                { p0 = {0.0f, c0, s0}; p1 = {0.0f, c1, s1}; }
            DebugAppendLine(out, center + p0, center + p1, color);
        }
    }
}

// Shaft from->to plus a 4-line arrowhead at `to`.
inline void DebugAppendArrow(std::vector<DebugVertex>& out, const glm::vec3& from, const glm::vec3& to,
                             const glm::vec4& color) {
    DebugAppendLine(out, from, to, color);
    glm::vec3 dir = to - from;
    const float len = glm::length(dir);
    if (len < 1e-5f) return;
    dir /= len;
    const glm::vec3 up = (std::abs(dir.y) < 0.99f) ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    const glm::vec3 right = glm::normalize(glm::cross(dir, up));
    const glm::vec3 u     = glm::normalize(glm::cross(right, dir));
    const float h = len * 0.2f;
    const glm::vec3 base = to - dir * h;
    DebugAppendLine(out, to, base + right * (h * 0.5f), color);
    DebugAppendLine(out, to, base - right * (h * 0.5f), color);
    DebugAppendLine(out, to, base + u     * (h * 0.5f), color);
    DebugAppendLine(out, to, base - u     * (h * 0.5f), color);
}

// 12 edges of the frustum = the unprojected NDC cube. z in {0,1} (GLM ZO depth).
inline void DebugAppendFrustum(std::vector<DebugVertex>& out, const glm::mat4& invViewProj,
                               const glm::vec4& color) {
    const glm::vec3 ndc[8] = {
        {-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0},
        {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},
    };
    glm::vec3 corners[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 w = invViewProj * glm::vec4(ndc[i], 1.0f);
        corners[i] = glm::vec3(w) / w.w;
    }
    const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (auto& ed : e) DebugAppendLine(out, corners[ed[0]], corners[ed[1]], color);
}
```

- [ ] **Step 2: Write the failing test**

`tests/test_debugdraw.cpp`:
```cpp
#include <cstdio>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

#include "DebugDraw.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static bool hasVert(const std::vector<DebugVertex>& v, glm::vec3 p) {
    for (auto& x : v) if (std::abs(x.Position.x-p.x)<1e-4f && std::abs(x.Position.y-p.y)<1e-4f
                          && std::abs(x.Position.z-p.z)<1e-4f) return true;
    return false;
}

static void T00_line()
{
    std::vector<DebugVertex> v;
    DebugAppendLine(v, {0,0,0}, {1,2,3}, {1,1,1,1});
    EXPECT(v.size() == 2);
    EXPECT(v[0].Position == glm::vec3(0,0,0));
    EXPECT(v[1].Position == glm::vec3(1,2,3));
    EXPECT(v[0].Color == glm::vec4(1,1,1,1));   // color copied onto every vertex
}

static void T01_box_24_verts_all_corners()
{
    std::vector<DebugVertex> v;
    DebugAppendBox(v, {-1,-1,-1}, {1,1,1}, {0,1,0,1});
    EXPECT(v.size() == 24);                       // 12 edges * 2
    EXPECT(hasVert(v, {-1,-1,-1}));
    EXPECT(hasVert(v, { 1, 1, 1}));
    EXPECT(hasVert(v, { 1,-1, 1}));
    EXPECT(v[0].Color == glm::vec4(0,1,0,1));
}

static void T02_sphere_segment_count()
{
    std::vector<DebugVertex> v;
    DebugAppendSphere(v, {0,0,0}, 2.0f, {1,0,0,1}, 8);
    EXPECT(v.size() == static_cast<size_t>(3 * 8 * 2)); // 3 circles * 8 segs * 2 verts
}

static void T03_frustum_identity_is_ndc_cube()
{
    std::vector<DebugVertex> v;
    DebugAppendFrustum(v, glm::mat4(1.0f), {1,1,0,1}); // identity: clip == NDC
    EXPECT(v.size() == 24);
    EXPECT(hasVert(v, {-1,-1,0}));   // near corner
    EXPECT(hasVert(v, { 1, 1,1}));   // far corner
}

int main()
{
    T00_line();
    T01_box_24_verts_all_corners();
    T02_sphere_segment_count();
    T03_frustum_identity_is_ndc_cube();

    if (g_Failures == 0) { std::printf("All debug draw tests passed.\n"); return 0; }
    std::printf("%d debug draw test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_debugdraw` CMake target**

In `tests/CMakeLists.txt`, after the last test block (e.g. `test_logformat`), append:
```cmake
add_executable(test_debugdraw
    test_debugdraw.cpp
)

target_link_libraries(test_debugdraw PRIVATE
    glm::glm
)

target_include_directories(test_debugdraw PRIVATE
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering
)

target_compile_definitions(test_debugdraw PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

set_target_properties(test_debugdraw PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 4: Reconfigure + build + run (expect PASS)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_debugdraw
./out/build/msvc-win64-vs2026-community/bin/Debug/test_debugdraw.exe
```
Expected: `All debug draw tests passed.` (TDD: build before the header exists to see the red phase.) If an assertion fails, fix the HEADER, not the test.

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/DebugDraw.h tests/test_debugdraw.cpp tests/CMakeLists.txt
git commit -m "feat: add DebugDraw.h line-geometry builders + test_debugdraw"
```

---

### Task 2: `DebugDrawSettings` global + Render Stats panel toggles

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h`
- Modify: `src/engine/src/rendering/RenderStats.cpp`
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`

No unit test (settings + UI glue); verified by build. Must land before Tasks 3/4 (they read `GetDebugDrawSettings()`).

- [ ] **Step 1: Add the settings struct + accessor (`RenderStats.h`)**

After the `struct CullingSettings { bool Enabled = true; };` line, add:
```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
};
```
After `ENGINE_API CullingSettings& GetCullingSettings();`, add:
```cpp
ENGINE_API DebugDrawSettings& GetDebugDrawSettings();
```

- [ ] **Step 2: Define it (`RenderStats.cpp`)**

After the `GetCullingSettings()` definition, add:
```cpp
DebugDrawSettings& GetDebugDrawSettings()
{
    static DebugDrawSettings s;
    return s;
}
```

- [ ] **Step 3: Add the checkboxes (`RenderStatsPanel.cpp`)**

In `DrawRenderStatsPanel`, after the existing batches `ImGui::Text(...)` lines and before `ImGui::End();`, add:
```cpp
    ImGui::Separator();
    ImGui::TextDisabled("Debug Draw");
    DebugDrawSettings& dd = GetDebugDrawSettings();
    ImGui::Checkbox("Light gizmos",     &dd.ShowLightGizmos);
    ImGui::Checkbox("Camera frustum",   &dd.ShowCameraFrustum);
    ImGui::Checkbox("Selected AABB",    &dd.ShowSelectedAABB);
    ImGui::Checkbox("Wireframe",        &dd.Wireframe);
```
(`GetDebugDrawSettings`/`DebugDrawSettings` come from the already-included `RenderStats.h`.)

- [ ] **Step 4: Build editor + runtime**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
```
Expected: clean (`runtime` links Engine, which now exports `GetDebugDrawSettings`).

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/rendering/RenderStats.h src/engine/src/rendering/RenderStats.cpp src/editor/src/rendering/imgui/RenderStatsPanel.cpp
git commit -m "feat: add DebugDrawSettings global + Render Stats panel toggles"
```

---

### Task 3: `DebugRenderPass` (gizmos) + register in the Renderer

**Files:**
- Create: `src/engine/src/rendering/passes/DebugRenderPass.h`
- Create: `src/engine/src/rendering/passes/DebugRenderPass.cpp`
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/engine/src/rendering/Renderer.cpp`

No unit test (GPU pass); verified by build + the GUI smoke. Builds on Tasks 1 + 2.

- [ ] **Step 1: Write `DebugRenderPass.h`**

`src/engine/src/rendering/passes/DebugRenderPass.h`:
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <vector>
#include <cstdint>

#include "IRenderPass.h"
#include "DebugDraw.h"

class Renderer;

// Draws editor debug gizmos (light gizmos, game-camera frustum, selected-entity AABB) as line lists,
// AFTER OutlineRenderPass, depth-tested (test on, write off, cull none). Gizmo selection is gated by
// GetDebugDrawSettings(); when all are off (the default, and always in runtime) the pass early-outs.
class DebugRenderPass : public IRenderPass
{
public:
    DebugRenderPass() = default;
    ~DebugRenderPass() override = default;

    bool Initialize(nvrhi::IDevice* device, Renderer* renderer) override;
    void Render(nvrhi::ICommandList* commandList,
                nvrhi::IFramebuffer* frameBuffer,
                SimulationSnapshot& snapshot,
                const ECS* world,
                double deltaTime,
                FrameAllocator* frameAllocator) override;
    void Shutdown() override;
    void OnResize(uint32_t width, uint32_t height) override;

private:
    struct DebugCB { glm::mat4 VP; };
    static_assert(sizeof(DebugCB) % 16 == 0, "DebugCB must be 16-byte aligned");

    void EnsureVertexCapacity(size_t vertexCount);

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;

    nvrhi::BufferHandle m_VertexBuffer;
    size_t m_VertexCapacity = 0;          // in vertices
    std::vector<DebugVertex> m_Verts;     // reused CPU scratch (cleared per frame, capacity retained)
};
```

- [ ] **Step 2: Write `DebugRenderPass.cpp`**

`src/engine/src/rendering/passes/DebugRenderPass.cpp`:
```cpp
#include "DebugRenderPass.h"

#include <nvrhi/utils.h>
#include <glm/gtc/matrix_inverse.hpp>

#include "Renderer.h"
#include "MeshSystem.h"
#include "RenderStats.h"
#include "ApplicationContext.h"
#include "ECS.h"
#include "TransformMath.h"
#include "Frustum.h"     // TransformAABB
#include "lib.h"

namespace {
const char* DEBUG_VS_HLSL = R"(
cbuffer DebugCB : register(b0) { float4x4 VP; };
struct VSIn  { float3 Position : POSITION; float4 Color : COLOR; };
struct VSOut { float4 PosH : SV_Position; float4 Color : COLOR; };
VSOut main_vs(VSIn i) { VSOut o; o.PosH = mul(VP, float4(i.Position, 1.0)); o.Color = i.Color; return o; }
)";
const char* DEBUG_PS_HLSL = R"(
struct VSOut { float4 PosH : SV_Position; float4 Color : COLOR; };
float4 main_ps(VSOut i) : SV_Target { return i.Color; }
)";
} // namespace

bool DebugRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(DebugCB), "DebugRenderPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer).setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, DEBUG_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  DEBUG_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

    nvrhi::VertexAttributeDesc attrs[2];
    attrs[0].setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(DebugVertex, Position)).setBufferIndex(0).setElementStride(sizeof(DebugVertex));
    attrs[1].setName("COLOR").setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(DebugVertex, Color)).setBufferIndex(0).setElementStride(sizeof(DebugVertex));
    m_InputLayout = m_Device->createInputLayout(attrs, 2, m_VS);

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

void DebugRenderPass::EnsureVertexCapacity(size_t vertexCount)
{
    if (vertexCount <= m_VertexCapacity && m_VertexBuffer)
        return;
    size_t cap = m_VertexCapacity ? m_VertexCapacity : 4096;
    while (cap < vertexCount) cap *= 2;
    nvrhi::BufferDesc desc;
    desc.byteSize = cap * sizeof(DebugVertex);
    desc.isVertexBuffer = true;
    desc.initialState = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    desc.debugName = "DebugRenderPass VB";
    m_VertexBuffer = m_Device->createBuffer(desc);
    m_VertexCapacity = cap;
}

void DebugRenderPass::Render(nvrhi::ICommandList* commandList,
                             nvrhi::IFramebuffer* frameBuffer,
                             SimulationSnapshot& /*snapshot*/,
                             const ECS* world,
                             double /*deltaTime*/,
                             FrameAllocator* /*frameAllocator*/)
{
    if (!world)
        return;

    const DebugDrawSettings& s = GetDebugDrawSettings();
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB)
        return;

    m_Verts.clear();

    if (s.ShowLightGizmos) {
        world->Each<TransformComponent, LightningComponent>(
            [&](EntityId, const TransformComponent& t, const LightningComponent& l) {
                const glm::vec4 col = l.Color;
                if (l.Type == LightningType::Point) {
                    const float r = (l.Range > 0.001f) ? l.Range : 1.0f;
                    DebugAppendSphere(m_Verts, t.Position, r, col);
                } else if (l.Type == LightningType::Directional) {
                    glm::vec3 d = glm::vec3(l.Direction);
                    if (glm::length(d) < 1e-5f) d = glm::vec3(0, -1, 0);
                    d = glm::normalize(d);
                    DebugAppendArrow(m_Verts, glm::vec3(0.0f), d * 2.0f, col);
                }
            });
    }

    if (s.ShowCameraFrustum) {
        if (const auto* cam = world->GetSingleton<WorldCameraComponent>()) {
            const glm::mat4 invVP = glm::inverse(cam->Projection * cam->View);
            DebugAppendFrustum(m_Verts, invVP, glm::vec4(0.4f, 0.8f, 1.0f, 1.0f));
        }
    }

    if (s.ShowSelectedAABB) {
        const EntityId sel = m_Renderer->GetAppContext()->SelectedEntity.load(std::memory_order_relaxed);
        if (sel != INVALID_ENTITY) {
            if (const auto* tc = world->GetComponent<TransformComponent>(sel)) {
                const glm::vec4 col(1.0f, 0.9f, 0.2f, 1.0f);
                const auto* mc = world->GetComponent<MeshComponent>(sel);
                bool drew = false;
                if (mc) {
                    const auto b = m_Renderer->GetMeshSystem()->GetMeshBounds(mc->MeshId);
                    if (b.valid) {
                        glm::vec3 wMin, wMax;
                        TransformAABB(ModelMatrix(*tc), b.min, b.max, wMin, wMax);
                        DebugAppendBox(m_Verts, wMin, wMax, col);
                        drew = true;
                    }
                }
                if (!drew) {
                    const glm::vec3 c = tc->Position, e(0.25f);
                    DebugAppendBox(m_Verts, c - e, c + e, col);
                }
            }
        }
    }

    if (m_Verts.empty())
        return;

    if (!m_Pipeline) {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::LineList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = false;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    EnsureVertexCapacity(m_Verts.size());

    glm::mat4 V(1.0f), P(1.0f);
    const CameraView& cam = m_Renderer->GetActiveCamera();
    V = cam.View; P = cam.Projection;
    DebugCB cb{}; cb.VP = P * V;
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));
    commandList->writeBuffer(m_VertexBuffer, m_Verts.data(), m_Verts.size() * sizeof(DebugVertex));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.bindings = { m_BindingSet };
    state.vertexBuffers = { nvrhi::VertexBufferBinding(m_VertexBuffer, 0, 0) };
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments args{};
    args.vertexCount = static_cast<uint32_t>(m_Verts.size());
    args.instanceCount = 1;
    commandList->draw(args);
}

void DebugRenderPass::OnResize(uint32_t, uint32_t) { m_Pipeline = nullptr; }

void DebugRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_CB = nullptr;
    m_VertexBuffer = nullptr;
    m_VertexCapacity = 0;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
}
```
VERIFY before finalizing (read the real headers — match the actual symbols, do not invent):
- `LightningComponent` fields (`Type`, `Direction` (vec4), `Color` (vec4), `Range`) + `LightningType::Point/Directional` + `WorldCameraComponent` (`View`/`Projection`) in `ECS.h` (same usage as `MeshRenderPass.cpp:335-356`).
- `MeshSystem::GetMeshBounds(meshId)` returns `{ valid, min, max }` (same as `ViewportPicker.cpp`).
- `ModelMatrix` (`TransformMath.h`) + `TransformAABB(mat4, min, max, outMin, outMax)` (`Frustum.h`) — same as `ViewportPicker.cpp`.
- `CameraView` + `Renderer::GetActiveCamera()` + `Renderer::GetAppContext()` + `Renderer::CreateShader`/`GetMeshSystem` (used by `OutlineRenderPass.cpp`). `INVALID_ENTITY` from `ECS.h`.
- The pipeline/CB/input-layout/binding idiom mirrors `OutlineRenderPass.cpp` exactly (copy its structure); only the topology (LineList), the vertex format (DebugVertex), and the per-frame `writeBuffer` to a growable vertex buffer differ. If `writeBuffer` to a `keepInitialState` vertex buffer isn't how this codebase uploads dynamic geometry, match the existing dynamic-upload pattern instead — but keep behavior identical.

- [ ] **Step 3: Add the source to the engine target**

In `src/engine/CMakeLists.txt`, in the engine source list, add next to `src/rendering/passes/OutlineRenderPass.cpp`:
```cmake
    src/rendering/passes/DebugRenderPass.cpp
```
(`DebugDraw.h` is header-only — no source entry.)

- [ ] **Step 4: Register the pass in `Renderer.cpp` (Init + InitForSwap)**

Add `#include "passes/DebugRenderPass.h"` next to the other pass includes. In BOTH `Renderer::Init` and `Renderer::InitForSwap`, immediately AFTER the `AddRenderPass(std::move(outlinePass));` block and BEFORE the `uiPass` block, insert:
```cpp
    auto debugPass = std::make_unique<DebugRenderPass>();
    if (!debugPass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize DebugRenderPass");
        return false;
    }
    AddRenderPass(std::move(debugPass));
```
(For `InitForSwap`, match its terser style: `if (!debugPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: DebugPass failed"); return false; }`.)

- [ ] **Step 5: Reconfigure, build engine + editor + runtime**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
```
Expected: clean.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/passes/DebugRenderPass.h src/engine/src/rendering/passes/DebugRenderPass.cpp src/engine/CMakeLists.txt src/engine/src/rendering/Renderer.cpp
git commit -m "feat: add DebugRenderPass (light/camera/AABB gizmos) + register after Outline"
```

---

### Task 4: Wireframe pipeline variant in `MeshRenderPass`

**Files:**
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.h`
- Modify: `src/engine/src/rendering/passes/MeshRenderPass.cpp`

No unit test (GPU state); verified by build + GUI smoke. `MeshRenderPass.cpp` already includes `RenderStats.h`.

- [ ] **Step 1: Add the wireframe pipeline member (`MeshRenderPass.h`)**

After the `nvrhi::GraphicsPipelineHandle m_Pipeline;` member, add:
```cpp
    nvrhi::GraphicsPipelineHandle m_WireframePipeline;
```

- [ ] **Step 2: Build the wireframe variant alongside the solid pipeline (`MeshRenderPass.cpp`)**

In `Render`, the `if (!m_Pipeline) { ... m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi); }` block ends at the `m_Pipeline = ...` line. Immediately AFTER that line (still inside the `if (!m_Pipeline)` block, before its closing `}`), add:
```cpp
        pso.renderState.rasterState.fillMode = nvrhi::RasterFillMode::Wireframe;
        m_WireframePipeline = m_Device->createGraphicsPipeline(pso, fbi);
```
(Reuses the same `pso`; only the fill mode differs. Confirm the enum is `nvrhi::RasterFillMode::Wireframe` in `nvrhi.h` — if this NVRHI version names it differently, use the wireframe enumerator it provides.)

- [ ] **Step 3: Select the pipeline per frame**

Find `state.pipeline = m_Pipeline;` (the `nvrhi::GraphicsState state;` setup). Replace it with:
```cpp
        state.pipeline = (GetDebugDrawSettings().Wireframe && m_WireframePipeline)
                             ? m_WireframePipeline : m_Pipeline;
```

- [ ] **Step 4: Reset the wireframe pipeline on teardown/resize**

In `Shutdown()` (sets `m_Pipeline = nullptr;`) and in `OnResize()` (sets `m_Pipeline = nullptr;`), add right after each:
```cpp
    m_WireframePipeline = nullptr;
```

- [ ] **Step 5: Build engine + editor + runtime + full regression**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking test_editorcam test_metrichistory test_transientstatus test_logformat test_debugdraw
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
```
Expected: all build clean; each test prints its `All ... passed.` line.

- [ ] **Step 6: Commit**

```bash
git add src/engine/src/rendering/passes/MeshRenderPass.h src/engine/src/rendering/passes/MeshRenderPass.cpp
git commit -m "feat: MeshRenderPass wireframe pipeline variant (DebugDrawSettings.Wireframe toggle)"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (editor, runtime, game, all test_*).
- [ ] All unit tests print their pass line (the 10 suites above, including `test_debugdraw`).
- [ ] **GUI smoke (user-run; surface to the user — do not self-approve):** Render Stats panel shows a
  "Debug Draw" section with 4 checkboxes. **Light gizmos:** point lights → wireframe spheres at their
  position sized to range; the sun → a direction arrow; colored by light color. **Camera frustum:**
  the game camera's frustum is drawn (visible while flying the editor camera in Edit mode). **Selected
  AABB:** selecting an entity draws its world bounding box (a small box for non-mesh entities). Gizmos
  are occluded by opaque meshes (depth-tested). **Wireframe:** meshes render as wireframe; toggle off →
  solid. All-off (default) = no debug geometry, zero cost. `runtime.exe` unaffected (no panel; settings
  default off).

## Notes / non-goals
- No `GAME_API_VERSION` bump (renderer/editor only).
- Render-thread/editor-only — no game-thread debug-draw ring (deferred).
- Lines only (no filled shapes); gizmos depth-tested (no x-ray mode); grid stays its own pass.
- Shader hot-reload + shadows are separate, later cycles (shadows is the queued next renderer feature).
