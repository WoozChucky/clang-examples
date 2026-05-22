# Selection Outline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw a colored mesh silhouette around the selected entity in the editor Viewport (inverted-hull, no stencil); runtime draws nothing.

**Architecture:** Publish the editor's selected `EntityId` to `ApplicationContext`; a new `OutlineRenderPass` (registered before `MeshRenderPass`) reads it and draws that entity's mesh enlarged along its normals in a flat color, so the normal mesh pass covers the center and leaves a rim.

**Tech Stack:** C++23, NVRHI (DX12/VK), inline HLSL via DXC, custom ECS, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-selection-outline-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- New `.cpp` added to a target → `cmake --preset msvc-win64-vs2026-community` before building.
- No `GAME_API_VERSION` bump; `game`/ECS layout unchanged.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.
- This is a GPU/shader feature — no unit tests. Per-task verification = clean build + existing `test_*` stay green; the visual result is the user's GUI smoke at the end.

---

### Task 1: Publish the selected entity to the engine

Foundation: the engine can read the editor's selection, and `Renderer` exposes its `ApplicationContext`. Behaviour-unchanged (nothing reads `SelectedEntity` yet).

**Files:**
- Modify: `src/common/include/ApplicationContext.h` (add an atomic to `struct ApplicationContext`)
- Modify: `src/engine/src/rendering/Renderer.h` (add a getter)
- Modify: `src/editor/src/rendering/imgui/EcsInspectorPanel.h` (add a getter)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (publish each frame)

- [ ] **Step 1: Add the atomic to `ApplicationContext`**
In `src/common/include/ApplicationContext.h`, after the existing editor atomics (e.g. `GameAcceptsKeyboard` / `HasImGuiConsumer`), add:
```cpp

    // Selected entity for the editor outline pass. The editor overlay (RenderThread) writes it;
    // OutlineRenderPass (also RenderThread, earlier in the frame) reads it -> 1-frame lag.
    // INVALID_ENTITY (0) = no outline. The runtime never writes it, so it draws no outline.
    std::atomic<uint64_t> SelectedEntity{INVALID_ENTITY};
```
(`ECS.h` is already included here, so `INVALID_ENTITY` is available; `EntityId` is `uint64_t`.)

- [ ] **Step 2: Add `Renderer::GetAppContext()`**
In `src/engine/src/rendering/Renderer.h`, next to `MeshSystem* GetMeshSystem() { return &m_MeshSystem; }` (line ~111), add:
```cpp
    ApplicationContext* GetAppContext() const { return m_AppContext; }
```
(`m_AppContext` is the existing private member set in the constructor.)

- [ ] **Step 3: Add `EcsInspectorPanel::GetSelectedEntity()`**
In `src/editor/src/rendering/imgui/EcsInspectorPanel.h`, in the `public:` section next to `SetSelectedEntity`, add:
```cpp
    EntityId GetSelectedEntity() const { return selectedEntity; }
```

- [ ] **Step 4: Publish from `ImGuiRenderer::Render`**
In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`, immediately AFTER the `m_EcsInspector.Draw(ctx);` call, add:
```cpp
        if (m_AppContext)
            m_AppContext->SelectedEntity.store(m_EcsInspector.GetSelectedEntity(), std::memory_order_relaxed);
```

- [ ] **Step 5: Build + regression**
```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all clean; both tests pass. Behaviour unchanged (nothing reads `SelectedEntity` yet).

- [ ] **Step 6: Commit**
```bash
git add src/common/include/ApplicationContext.h src/engine/src/rendering/Renderer.h src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: publish editor selected entity to ApplicationContext + Renderer::GetAppContext"
```

---

### Task 2: `OutlineRenderPass` (inverted-hull silhouette)

**Files:**
- Create: `src/engine/src/rendering/passes/OutlineRenderPass.h`
- Create: `src/engine/src/rendering/passes/OutlineRenderPass.cpp`
- Modify: `src/engine/src/rendering/Renderer.cpp` (register before MeshRenderPass)
- Modify: `src/engine/CMakeLists.txt` (add the source)

- [ ] **Step 1: Create `OutlineRenderPass.h`**
```cpp
#pragma once
#include <nvrhi/nvrhi.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <cstdint>

#include "IRenderPass.h"

class Renderer;

// Draws the selected entity's mesh enlarged along its normals in a flat color (inverted hull),
// BEFORE MeshRenderPass, so the normal mesh covers the center and a colored rim remains.
// Reads the selected entity from ApplicationContext (set by the editor overlay); no-op in runtime.
class OutlineRenderPass : public IRenderPass
{
public:
    OutlineRenderPass() = default;
    ~OutlineRenderPass() override = default;

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
    struct OutlineCB
    {
        glm::mat4 VP;
        glm::mat4 Model;
        float     OutlineWidth;
        float     _pad0[3];
        glm::vec4 OutlineColor;
    };
    static_assert(sizeof(OutlineCB) % 16 == 0, "OutlineCB must be 16-byte aligned");

    nvrhi::IDevice* m_Device = nullptr;
    Renderer* m_Renderer = nullptr;

    nvrhi::ShaderHandle m_VS;
    nvrhi::ShaderHandle m_PS;
    nvrhi::GraphicsPipelineHandle m_Pipeline;
    nvrhi::InputLayoutHandle m_InputLayout;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BufferHandle m_CB;
    nvrhi::BindingSetHandle m_BindingSet;
};
```

- [ ] **Step 2: Create `OutlineRenderPass.cpp`**
```cpp
#include "OutlineRenderPass.h"

#include <nvrhi/utils.h>

#include "Renderer.h"
#include "MeshSystem.h"
#include "ApplicationContext.h"
#include "ECS.h"
#include "TransformMath.h"
#include "lib.h"

namespace {
const char* OUTLINE_VS_HLSL = R"(
cbuffer OutlineCB : register(b0)
{
    float4x4 VP;
    float4x4 Model;
    float     OutlineWidth;
    float3    _pad0;
    float4    OutlineColor;
};
struct VSIn  { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD; };
struct VSOut { float4 PosH : SV_Position; };
VSOut main_vs(VSIn vin)
{
    VSOut o;
    float3 lp = vin.Position + normalize(vin.Normal) * OutlineWidth;
    float4 wp = mul(Model, float4(lp, 1.0));
    o.PosH = mul(VP, wp);
    return o;
}
)";

const char* OUTLINE_PS_HLSL = R"(
cbuffer OutlineCB : register(b0)
{
    float4x4 VP;
    float4x4 Model;
    float     OutlineWidth;
    float3    _pad0;
    float4    OutlineColor;
};
struct VSOut { float4 PosH : SV_Position; };
float4 main_ps(VSOut pin) : SV_Target { return OutlineColor; }
)";
} // namespace

bool OutlineRenderPass::Initialize(nvrhi::IDevice* device, Renderer* renderer)
{
    m_Device = device;
    m_Renderer = renderer;
    if (!m_Device || !m_Renderer || !renderer->GetMeshSystem())
        return false;

    m_CB = m_Device->createBuffer(
        nvrhi::utils::CreateStaticConstantBufferDesc(sizeof(OutlineCB), "OutlineRenderPass CB")
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true));

    m_VS = m_Renderer->CreateShader(nvrhi::ShaderType::Vertex, OUTLINE_VS_HLSL, 0, "main_vs", "vs_6_1");
    m_PS = m_Renderer->CreateShader(nvrhi::ShaderType::Pixel,  OUTLINE_PS_HLSL, 0, "main_ps", "ps_6_1");
    if (!m_VS || !m_PS)
        return false;

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

void OutlineRenderPass::Render(nvrhi::ICommandList* commandList,
                               nvrhi::IFramebuffer* frameBuffer,
                               SimulationSnapshot& /*snapshot*/,
                               const ECS* world,
                               double /*deltaTime*/,
                               FrameAllocator* /*frameAllocator*/)
{
    if (!world)
        return;

    const EntityId sel = m_Renderer->GetAppContext()->SelectedEntity.load(std::memory_order_relaxed);
    if (sel == INVALID_ENTITY)
        return;

    const auto* mc = world->GetComponent<MeshComponent>(sel);
    const auto* tc = world->GetComponent<TransformComponent>(sel);
    if (!mc || !tc || !mc->Visible)
        return;

    auto res = m_Renderer->GetMeshSystem()->GetMeshResources(mc->MeshId);
    if (!res.valid)
        return;

    glm::mat4 V(1.0f), P(1.0f);
    if (const auto* cam = world->GetSingleton<WorldCameraComponent>())
    {
        V = cam->View;
        P = cam->Projection;
    }

    if (!m_Pipeline)
    {
        const auto fbi = frameBuffer->getFramebufferInfo();
        nvrhi::GraphicsPipelineDesc pso;
        pso.VS = m_VS;
        pso.PS = m_PS;
        pso.inputLayout = m_InputLayout;
        pso.bindingLayouts = { m_BindingLayout };
        pso.primType = nvrhi::PrimitiveType::TriangleList;
        pso.renderState.depthStencilState.depthTestEnable = true;
        pso.renderState.depthStencilState.depthWriteEnable = true;
        pso.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Front; // render back of the hull -> rim
        pso.renderState.rasterState.setFrontCounterClockwise(true);          // match mesh winding
        // No blend: solid outline color.
        m_Pipeline = m_Device->createGraphicsPipeline(pso, fbi);
    }

    OutlineCB cb{};
    cb.VP = P * V;
    cb.Model = ModelMatrix(*tc);
    cb.OutlineWidth = 0.03f;
    cb.OutlineColor = glm::vec4(1.0f, 0.6f, 0.1f, 1.0f); // orange
    commandList->writeBuffer(m_CB, &cb, sizeof(cb));

    nvrhi::GraphicsState state;
    state.pipeline = m_Pipeline;
    state.framebuffer = frameBuffer;
    state.viewport.addViewportAndScissorRect(frameBuffer->getFramebufferInfo().getViewport());
    state.bindings = { m_BindingSet };
    state.vertexBuffers = { nvrhi::VertexBufferBinding(res.vertexBuffer, 0, 0) };
    state.indexBuffer = nvrhi::IndexBufferBinding(res.indexBuffer, nvrhi::Format::R32_UINT, 0);
    commandList->setGraphicsState(state);

    if (res.subMeshes.size() > 0)
    {
        for (const auto& sm : res.subMeshes)
        {
            nvrhi::DrawArguments args{};
            args.vertexCount = sm.IndexCount;
            args.instanceCount = 1;
            args.startIndexLocation = sm.IndexStart;
            args.startVertexLocation = 0;
            commandList->drawIndexed(args);
        }
    }
    else
    {
        nvrhi::DrawArguments args{};
        args.vertexCount = res.indexCount;
        args.instanceCount = 1;
        commandList->drawIndexed(args);
    }
}

void OutlineRenderPass::OnResize(uint32_t /*width*/, uint32_t /*height*/)
{
    m_Pipeline = nullptr; // rebuilt on next Render against the new framebuffer info
}

void OutlineRenderPass::Shutdown()
{
    m_Pipeline = nullptr;
    m_BindingSet = nullptr;
    m_CB = nullptr;
    m_InputLayout = nullptr;
    m_BindingLayout = nullptr;
    m_VS = nullptr;
    m_PS = nullptr;
}
```
(VERIFY against the codebase: `Renderer::CreateShader(ShaderType, const char*, <int>, entry, profile)` signature — copy MeshRenderPass's exact call form; `GetMeshResources` returns `{ vertexBuffer, indexBuffer, subMeshes (each `.IndexStart`/`.IndexCount`), indexCount, valid }`; `world->GetComponent<T>(id)` + `GetSingleton<WorldCameraComponent>()` exist; `MeshVertex` (`px..u,v`) is in `ApplicationContext.h`; `ModelMatrix` in `TransformMath.h`. `SubMesh` field names are `IndexStart`/`IndexCount` (confirm in MeshSystem.h — MeshRenderPass uses `subMesh.IndexStart`/`subMesh.IndexCount`). If the `SimulationSnapshot`/`FrameAllocator` params are unused, the `/*...*/` comments avoid -Wunused.)

- [ ] **Step 3: Register the pass before MeshRenderPass**
In `src/engine/src/rendering/Renderer.cpp`, `Renderer::Init()` registers Primitive → Mesh → Ui (lines ~85-104). Add `#include "passes/OutlineRenderPass.h"` at the top, and BETWEEN the Primitive `AddRenderPass(...)` and the Mesh pass construction, insert:
```cpp
    auto outlinePass = std::make_unique<OutlineRenderPass>();
    if (!outlinePass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize OutlineRenderPass");
        return false;
    }
    AddRenderPass(std::move(outlinePass));
```
So the order becomes Primitive → **Outline** → Mesh → Ui. If there is a second pass-list build site (e.g. `Renderer::InitForSwap`), register it there too in the same position (grep for the Mesh-pass `AddRenderPass` to find all sites).

- [ ] **Step 4: Add the source to the Engine target**
In `src/engine/CMakeLists.txt`, after `    src/rendering/passes/MeshRenderPass.cpp`, add:
```cmake
    src/rendering/passes/OutlineRenderPass.cpp
```

- [ ] **Step 5: Reconfigure + build + regression**
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; both tests pass.

- [ ] **Step 6: Commit**
```bash
git add src/engine/src/rendering/passes/OutlineRenderPass.h src/engine/src/rendering/passes/OutlineRenderPass.cpp src/engine/src/rendering/Renderer.cpp src/engine/CMakeLists.txt
git commit -m "feat: add OutlineRenderPass (inverted-hull selection silhouette)"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green.
- [ ] `test_ecs` / `test_alloc` / `test_frustum` / `test_input` / `test_picking` all print their `All ... passed.` lines.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):** select a mesh (click in the Viewport or via the Inspector list) → an orange silhouette hugs it; deselect (empty click) → outline disappears; pick a different mesh → outline moves; move the gizmo → the outline follows the mesh; the outline confirms which mesh got picked; `runtime.exe` renders with NO outline (identical to before).

## Notes / non-goals
- No `GAME_API_VERSION` bump; `SelectedEntity` lives in `ApplicationContext`, not `GameState`.
- Outline width is world/local (thicker when closer); hard-edged meshes can show small gaps; occlusion is approximate (drawn before other meshes). All accepted per the spec; a post-process constant-width edge-detect is the future upgrade.
- Color/width are hardcoded constants (orange, 0.03) — editor-configurable controls are out of scope.
