# Renderer Backend Hot-Swap (Phase B) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **MODEL REQUIREMENT:** This is a high-complexity feature (GPU device lifetime, threading, ImGui re-init, no-rollback failure handling). The controller MUST dispatch every implementer and reviewer subagent with **Opus 4.7** (`model: opus`). Do NOT downgrade any subagent to a cheaper model, even for tasks that look mechanical.

**Goal:** Swap the renderer backend (DirectX 12 ↔ Vulkan) at runtime when the user clicks `Settings → Renderer Backend → Apply`, with no editor restart.

**Architecture:** `Apply` pushes a `SwapBackend` command into the Platform→Render ring. RenderThread, at command-drain time, runs a blocking state machine: pause GameThread at a tick boundary, `waitForIdle`, tear down all GPU resources (ImGui NVRHI backend, render passes, MaterialSystem, MeshSystem, swapchain, device, backend), construct the new backend, then recreate everything by replaying CPU-side caches so `MeshId`/`MaterialId` stay numerically stable. No rollback: any fatal step shows a MessageBox and exits the process (settings file already holds the new backend, so a restart applies it).

**Tech Stack:** C++23, CMake (preset `msvc-win64-vs2026-community`), MSVC/VS2026, NVRHI (DX12 + Vulkan backends already present), Dear ImGui, GLFW, Tracy.

**Spec:** `docs/superpowers/specs/2026-05-20-renderer-hot-swap-design.md` — read first.

> **Testing note:** Per spec §"Testing", no automated tests. Each task ends with a build check; Task 9 runs the manual smoke matrix. `test_ecs.exe` must stay green (no ECS surface is touched).

---

## File Structure

**Modified:**
- `src/common/include/ApplicationContext.h` — `RendererCommandType::SwapBackend` + union member; `SwapInProgress` / `GameThreadPaused` atomics.
- `src/editor/src/rendering/MeshSystem.{h,cpp}` — CPU cache + `DestroyGpuResources` / `RecreateGpuResources`.
- `src/editor/src/rendering/MaterialSystem.{h,cpp}` — CPU cache + `DestroyGpuResources` / `RecreateGpuResources`.
- `src/editor/src/rendering/imgui/ImGuiRenderer.{h,cpp}` — `ShutdownNvrhiOnly` / `InitNvrhiForDevice`; Apply pushes SwapBackend; remove restart banner.
- `src/editor/src/rendering/Renderer.{h,cpp}` — `SwapBackend`, `CurrentApi`, `TeardownForSwap`, `InitForSwap`, extracted `CreateDefaultMaterialResources` helper.
- `src/editor/src/threading/GameThread.cpp` — pause cooperation at tick top.
- `src/editor/src/threading/RenderThread.cpp` — handle SwapBackend command; fatal-exit on failure.

**No new files.** The swap logic lives in `Renderer` (orchestration) and the two resource systems (recreation), matching the existing ownership model.

---

## Build / run commands (reused throughout)

- Configure (only if CMake files change): `cmake --preset msvc-win64-vs2026-community`
- Build editor: `cmake --build --preset msvc-win64-vs2026-community --target editor`
- Build all in-scope: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs`
- Run editor: `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`
- ECS tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`

`runtime.exe` is broken on `main` (pre-existing, unrelated). Do not build it.

---

## Task 1: CPU-side cache + GPU teardown/recreate in MeshSystem

Retain vertex/index/submesh data on every `AddMesh`, and add two methods that destroy and rebuild only the GPU buffers while preserving slot indices (so `MeshId` is stable across a swap).

**Files:**
- Modify: `src/editor/src/rendering/MeshSystem.h`
- Modify: `src/editor/src/rendering/MeshSystem.cpp`

- [ ] **Step 1: Add CPU cache fields + new method declarations to `MeshSystem.h`**

In the `MeshEntry` struct (currently lines 58-66), add CPU-side retained data:

```cpp
    struct MeshEntry {
        nvrhi::BufferHandle vertexBuffer;
        nvrhi::BufferHandle indexBuffer;
        std::vector<SubMesh> subMeshes;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};

        // CPU-side copies retained for backend hot-swap replay.
        std::vector<MeshVertex> cpuVertices;
        std::vector<uint32_t>   cpuIndices;
    };
```

In the `public:` section (after `Shutdown();`), declare:

```cpp
    // Hot-swap support: release GPU buffers but keep entries + CPU caches.
    void DestroyGpuResources();
    // Hot-swap support: rebuild GPU buffers from CPU caches against m_Device.
    // m_Device must already point to the new device. Preserves slot indices.
    // Returns false if any mesh failed to rebuild (that slot is left null).
    bool RecreateGpuResources();
```

- [ ] **Step 2: Retain CPU data in `AddMesh`**

In `MeshSystem.cpp`, inside `AddMesh`, after the bounding-box loop and before creating the command list (after line 94, before line 96 `// Create command list for upload`), store the CPU copies into `entry`:

```cpp
    // Retain CPU-side copies for hot-swap replay.
    entry.cpuVertices.assign(vertices, vertices + vertexCount);
    entry.cpuIndices.assign(indices, indices + indexCount);
```

(`entry.subMeshes` is already populated from the `subMeshes` argument at lines 80-84, so it doubles as the CPU cache — no extra copy needed.)

- [ ] **Step 3: Implement `DestroyGpuResources` + `RecreateGpuResources`**

Add at the end of `MeshSystem.cpp` (before nothing — append):

```cpp
void MeshSystem::DestroyGpuResources()
{
    // Release GPU buffers but keep m_Meshes entries (and their CPU caches).
    for (auto& entry : m_Meshes)
    {
        entry.vertexBuffer = nullptr;
        entry.indexBuffer = nullptr;
    }
    // Keep m_Device as-is; caller updates it before RecreateGpuResources().
}

bool MeshSystem::RecreateGpuResources()
{
    if (!m_Device)
    {
        SM_ERROR("MeshSystem::RecreateGpuResources: no device");
        return false;
    }

    bool allOk = true;
    for (size_t i = 0; i < m_Meshes.size(); ++i)
    {
        MeshEntry& entry = m_Meshes[i];
        if (entry.cpuVertices.empty() || entry.cpuIndices.empty())
        {
            SM_WARN("MeshSystem::RecreateGpuResources: mesh %zu has no CPU cache; skipping", i);
            allOk = false;
            continue;
        }

        auto cl = m_Device->createCommandList(
            nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
        cl->open();

        nvrhi::BufferDesc vbDesc;
        vbDesc.debugName = "MeshSystem VB " + std::to_string(i);
        vbDesc.byteSize = sizeof(MeshVertex) * entry.cpuVertices.size();
        vbDesc.isVertexBuffer = true;
        vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.vertexBuffer = m_Device->createBuffer(vbDesc);
        if (!entry.vertexBuffer)
        {
            SM_ERROR("MeshSystem::RecreateGpuResources: mesh %zu vertex buffer failed", i);
            cl->close();
            allOk = false;
            continue;
        }
        cl->beginTrackingBufferState(entry.vertexBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.vertexBuffer, entry.cpuVertices.data(), vbDesc.byteSize);
        cl->setPermanentBufferState(entry.vertexBuffer, nvrhi::ResourceStates::VertexBuffer);

        nvrhi::BufferDesc ibDesc;
        ibDesc.debugName = "MeshSystem IB " + std::to_string(i);
        ibDesc.byteSize = sizeof(uint32_t) * entry.cpuIndices.size();
        ibDesc.isIndexBuffer = true;
        ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
        entry.indexBuffer = m_Device->createBuffer(ibDesc);
        if (!entry.indexBuffer)
        {
            SM_ERROR("MeshSystem::RecreateGpuResources: mesh %zu index buffer failed", i);
            cl->close();
            entry.vertexBuffer = nullptr;
            allOk = false;
            continue;
        }
        cl->beginTrackingBufferState(entry.indexBuffer, nvrhi::ResourceStates::CopyDest);
        cl->writeBuffer(entry.indexBuffer, entry.cpuIndices.data(), ibDesc.byteSize);
        cl->setPermanentBufferState(entry.indexBuffer, nvrhi::ResourceStates::IndexBuffer);

        cl->close();
        m_Device->executeCommandList(cl);
    }

    SM_TRACE("MeshSystem::RecreateGpuResources: rebuilt %zu meshes", m_Meshes.size());
    return allOk;
}
```

Note: `m_Device` is set by the caller (Renderer) via a new setter added in Task 5, or by re-calling `Initialize`. To avoid wiping `m_Meshes`, do NOT call `Initialize` on swap — Task 5 adds a lightweight `SetDevice`. Add this declaration to `MeshSystem.h` public section:

```cpp
    void SetDevice(nvrhi::IDevice* device) { m_Device = device; }
```

- [ ] **Step 4: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. New methods compile; nothing calls them yet.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/MeshSystem.h src/editor/src/rendering/MeshSystem.cpp
git commit -m "MeshSystem: retain CPU cache + GPU teardown/recreate for hot-swap"
```

---

## Task 2: CPU-side cache + GPU teardown/recreate in MaterialSystem

Same pattern for textures. Note: materials whose texture is the shared `m_MissingMaterial` (no real texture uploaded) must re-point at the *new* missing texture after swap, not recreate one.

**Files:**
- Modify: `src/editor/src/rendering/MaterialSystem.h`
- Modify: `src/editor/src/rendering/MaterialSystem.cpp`

- [ ] **Step 1: Add CPU cache fields + declarations to `MaterialSystem.h`**

Extend `MaterialEntry` (lines 43-46):

```cpp
    struct MaterialEntry {
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;

        // CPU-side copy retained for hot-swap. If usesMissingTexture is true,
        // this entry points at the shared missing texture and has no pixels.
        std::vector<uint32_t> cpuPixels;
        uint32_t width = 0;
        uint32_t height = 0;
        bool usesMissingTexture = false;
    };
```

Add to `public:` (after `Shutdown();`):

```cpp
    void SetDevice(nvrhi::IDevice* device) { m_Device = device; }

    // Hot-swap support: release GPU textures but keep entries + CPU caches.
    void DestroyGpuResources();
    // Hot-swap support: rebuild textures from CPU caches against m_Device.
    // newMissing / newSampler replace the previous default resources.
    // Returns false if any material failed (that slot falls back to missing).
    bool RecreateGpuResources(const nvrhi::TextureHandle& newMissing,
                              const nvrhi::SamplerHandle& newSampler);
```

- [ ] **Step 2: Populate CPU cache in `Initialize` + `AddMaterial`**

In `MaterialSystem.cpp` `Initialize`, the default material at index 0 uses the missing texture. Mark it (replace lines 13-16):

```cpp
    MaterialEntry defaultMaterial{};
    defaultMaterial.texture = m_MissingMaterial;
    defaultMaterial.sampler = m_DefaultSampler;
    defaultMaterial.usesMissingTexture = true;
    m_Materials.push_back(defaultMaterial);
```

In `AddMaterial`, the no-texture branch (lines 32-40) also uses the missing texture — mark it:

```cpp
    if (!textureRgba8 || texWidth == 0 || texHeight == 0)
    {
        entry.texture = m_MissingMaterial;
        entry.usesMissingTexture = true;
        const uint32_t materialId = static_cast<uint32_t>(m_Materials.size());
        m_Materials.push_back(entry);
        SM_TRACE("MaterialSystem::AddMaterial: Created material %u with default white texture", materialId);
        return MaterialHandle{ materialId };
    }
```

In the real-texture path, after computing `td` and before/around the upload, store the CPU copy. Insert right after `entry.sampler = m_DefaultSampler;` (line 30) is not enough — we need width/height/pixels. Add immediately after line 40's closing brace (i.e., at the start of the real-texture path, before creating the command list at line 42):

```cpp
    // Retain CPU-side copy for hot-swap replay.
    entry.width = texWidth;
    entry.height = texHeight;
    entry.cpuPixels.assign(textureRgba8, textureRgba8 + (static_cast<size_t>(texWidth) * texHeight));
    entry.usesMissingTexture = false;
```

- [ ] **Step 3: Implement teardown + recreate**

Append to `MaterialSystem.cpp`:

```cpp
void MaterialSystem::DestroyGpuResources()
{
    for (auto& entry : m_Materials)
    {
        entry.texture = nullptr;
        // sampler is re-pointed in RecreateGpuResources to the new default
        entry.sampler = nullptr;
    }
    m_MissingMaterial = nullptr;
    m_DefaultSampler = nullptr;
}

bool MaterialSystem::RecreateGpuResources(const nvrhi::TextureHandle& newMissing,
                                          const nvrhi::SamplerHandle& newSampler)
{
    if (!m_Device)
    {
        SM_ERROR("MaterialSystem::RecreateGpuResources: no device");
        return false;
    }

    m_MissingMaterial = newMissing;
    m_DefaultSampler = newSampler;

    bool allOk = true;
    for (size_t i = 0; i < m_Materials.size(); ++i)
    {
        MaterialEntry& entry = m_Materials[i];
        entry.sampler = m_DefaultSampler;

        if (entry.usesMissingTexture)
        {
            entry.texture = m_MissingMaterial;
            continue;
        }

        if (entry.cpuPixels.empty() || entry.width == 0 || entry.height == 0)
        {
            SM_WARN("MaterialSystem::RecreateGpuResources: material %zu has no CPU cache; using missing", i);
            entry.texture = m_MissingMaterial;
            allOk = false;
            continue;
        }

        nvrhi::TextureDesc td;
        td.debugName = "MaterialSystem Texture " + std::to_string(i);
        td.width = entry.width;
        td.height = entry.height;
        td.depth = 1;
        td.arraySize = 1;
        td.mipLevels = 1;
        td.sampleCount = 1;
        td.dimension = nvrhi::TextureDimension::Texture2D;
        td.format = nvrhi::Format::RGBA8_UNORM;
        td.isShaderResource = true;
        entry.texture = m_Device->createTexture(td);
        if (!entry.texture)
        {
            SM_ERROR("MaterialSystem::RecreateGpuResources: material %zu texture failed; using missing", i);
            entry.texture = m_MissingMaterial;
            allOk = false;
            continue;
        }

        auto cl = m_Device->createCommandList(
            nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Graphics));
        cl->open();
        cl->beginTrackingTextureState(entry.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
        cl->writeTexture(entry.texture, 0, 0, entry.cpuPixels.data(), entry.width * 4);
        cl->setPermanentTextureState(entry.texture, nvrhi::ResourceStates::ShaderResource);
        cl->commitBarriers();
        cl->close();
        m_Device->executeCommandList(cl);
    }

    SM_TRACE("MaterialSystem::RecreateGpuResources: rebuilt %zu materials", m_Materials.size());
    return allOk;
}
```

- [ ] **Step 4: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/MaterialSystem.h src/editor/src/rendering/MaterialSystem.cpp
git commit -m "MaterialSystem: retain CPU cache + GPU teardown/recreate for hot-swap"
```

---

## Task 3: SwapBackend command + thread-pause atomics

**Files:**
- Modify: `src/common/include/ApplicationContext.h`

- [ ] **Step 1: Add the command type**

Extend `RendererCommandType` (lines 64-72) — add `SwapBackend = 7`:

```cpp
enum class RendererCommandType : uint8_t {
    Invalid = 0,
    ToggleVSync = 1,
    TogglePause = 2,
    Resize = 3,
    RequestMesh = 4,
    RequestMaterial = 5,
    RequestModel = 6,
    SwapBackend = 7
};
```

- [ ] **Step 2: Add the union member**

In `struct RendererCommand`'s union (after `MaterialRequest`, before the closing `};` at line 96), add:

```cpp
        struct {
            RendererAPI TargetApi;
        } SwapBackend;
```

(`RendererAPI` is defined in `lib.h`, which is already transitively included — confirm the build; add `#include "lib.h"` if needed.)

- [ ] **Step 3: Add coordination atomics to `ApplicationContext`**

After `std::atomic<bool> ShutdownRequested{false};` (line 132), add:

```cpp
    // Renderer hot-swap coordination (Phase B).
    // RenderThread sets SwapInProgress; GameThread acks via GameThreadPaused.
    std::atomic<bool> SwapInProgress{false};
    std::atomic<bool> GameThreadPaused{false};
```

- [ ] **Step 4: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```

Expected: clean build of all targets (this header is included broadly).

- [ ] **Step 5: Commit**

```bash
git add src/common/include/ApplicationContext.h
git commit -m "Add SwapBackend command + hot-swap coordination atomics"
```

---

## Task 4: GameThread pause cooperation

**Files:**
- Modify: `src/editor/src/threading/GameThread.cpp`

- [ ] **Step 1: Pause at the top of the tick loop**

In `GameThread::RunLoop`, the `while (Running())` loop begins at line 135. Insert the pause check as the very first thing inside the loop, BEFORE the reload-flag drain (before line 138's `if (m_ReloadPending...`):

```cpp
	while (Running()) {
		// Renderer hot-swap: pause here while RenderThread rebuilds the device.
		if (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
			ZoneScopedN("Game:SwapPause");
			m_AppContext->GameThreadPaused.store(true, std::memory_order_release);
			while (m_AppContext->SwapInProgress.load(std::memory_order_acquire)
			       && Running()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			m_AppContext->GameThreadPaused.store(false, std::memory_order_release);
			// Reset frame pacing so the resumed tick doesn't see a huge dt.
			nextFrameTime = Clock::now();
			lastFrameTime = nextFrameTime;
		}

		// Drain reload flag BEFORE input/commands/game logic ...
```

(The `nextFrameTime`/`lastFrameTime` locals are in scope from lines 120-121. Resetting them prevents a multi-hundred-ms `actualDt` spike on the first resumed tick, which would otherwise make the rotating text jump.)

- [ ] **Step 2: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. Behavior unchanged (nothing sets `SwapInProgress` yet).

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/threading/GameThread.cpp
git commit -m "GameThread: pause at tick boundary during renderer hot-swap"
```

---

## Task 6: Renderer swap orchestration

This is the core. Extract the default missing-texture/sampler creation into a helper, then add `SwapBackend` + `TeardownForSwap` + `InitForSwap`. ImGui re-init is delegated to `ImGuiRenderer` — Task 5 (already done by the time this runs) added `ShutdownNvrhiOnly` / `InitNvrhiForDevice`, which this task calls.

**Files:**
- Modify: `src/editor/src/rendering/Renderer.h`
- Modify: `src/editor/src/rendering/Renderer.cpp`

- [ ] **Step 1: Declare new methods + helper in `Renderer.h`**

In `public:` after `void ToggleVSync();` add:

```cpp
    // Phase B hot-swap. Runs synchronously on the RenderThread. Returns false
    // on a fatal, unrecoverable failure (caller should MessageBox + exit).
    bool SwapBackend(RendererAPI newApi);
    RendererAPI CurrentApi() const { return m_Backend ? m_Backend->GetAPI() : RendererAPI::Invalid; }
```

In `private:` add:

```cpp
    void TeardownForSwap();
    bool InitForSwap(RendererAPI newApi);
    // Creates the default magenta missing texture + default sampler.
    void CreateDefaultMaterialResources(nvrhi::TextureHandle& outMissing,
                                        nvrhi::SamplerHandle& outSampler);
```

- [ ] **Step 2: Extract `CreateDefaultMaterialResources` and call it from `Init`**

In `Renderer.cpp`, move the body that builds `missingMaterialTexture` and `defaultSampler` (lines 67-116) into the new helper. Add at the end of the file:

```cpp
void Renderer::CreateDefaultMaterialResources(nvrhi::TextureHandle& outMissing,
                                              nvrhi::SamplerHandle& outSampler)
{
    constexpr uint32_t texSize = 256;
    constexpr uint32_t magenta = 0xFFFF00FFu;
    constexpr uint32_t black   = 0xFF000000u;

    nvrhi::TextureDesc td;
    td.debugName = "Renderer DefaultMissingTexture";
    td.width = texSize;
    td.height = texSize;
    td.depth = 1;
    td.arraySize = 1;
    td.mipLevels = 1;
    td.sampleCount = 1;
    td.dimension = nvrhi::TextureDimension::Texture2D;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isShaderResource = true;
    outMissing = m_Device->createTexture(td);

    static uint32_t pixels[texSize * texSize];
    constexpr uint32_t checkerSize = 16;
    for (uint32_t y = 0; y < texSize; ++y) {
        for (uint32_t x = 0; x < texSize; ++x) {
            const bool checkerX = (x / checkerSize) % 2 == 0;
            const bool checkerY = (y / checkerSize) % 2 == 0;
            pixels[y * texSize + x] = (checkerX == checkerY) ? magenta : black;
        }
    }

    const auto cl = m_Device->createCommandList();
    cl->open();
    cl->beginTrackingTextureState(outMissing, nvrhi::AllSubresources, nvrhi::ResourceStates::Common);
    cl->writeTexture(outMissing, 0, 0, pixels, texSize * sizeof(uint32_t));
    cl->setPermanentTextureState(outMissing, nvrhi::ResourceStates::ShaderResource);
    cl->commitBarriers();
    cl->close();
    m_Device->executeCommandList(cl);

    nvrhi::SamplerDesc sd;
    sd.setAllFilters(true);
    sd.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    outSampler = m_Device->createSampler(sd);
}
```

Then replace `Init`'s inline block (the `{ ... }` spanning lines 66-121) with:

```cpp
    {
        nvrhi::TextureHandle missingMaterialTexture;
        nvrhi::SamplerHandle defaultSampler;
        CreateDefaultMaterialResources(missingMaterialTexture, defaultSampler);

        m_MeshSystem.Initialize(m_Device);
        m_MaterialSystem.Initialize(m_Device, missingMaterialTexture, defaultSampler);
    }
```

Keep the rest of `Init` (ImGuiRenderer creation, render passes) unchanged.

- [ ] **Step 3: Implement `TeardownForSwap`, `InitForSwap`, `SwapBackend`**

Append to `Renderer.cpp`. `TeardownForSwap` mirrors `Shutdown` but KEEPS `m_MeshSystem`/`m_MaterialSystem` entries (only releases their GPU resources) and KEEPS `m_ImGuiRenderer` alive (only its NVRHI backend is torn down):

```cpp
void Renderer::TeardownForSwap()
{
    // ImGui: drop only the NVRHI backend + device-bound preview resources;
    // keep the ImGui context (dock layout, fonts loaded from disk).
    if (m_ImGuiRenderer) {
        m_ImGuiRenderer->ShutdownNvrhiOnly();
    }

    m_GpuTimer.Cleanup();

    for (auto& pass : m_RenderPasses) {
        if (pass) pass->Shutdown();
    }
    m_RenderPasses.clear();

    // Release GPU resources but keep CPU caches + entry slots.
    m_MaterialSystem.DestroyGpuResources();
    m_MeshSystem.DestroyGpuResources();

    m_CommandList = nullptr;
    m_Device = nullptr;

    if (m_Backend) {
        m_Backend->Shutdown(SHUTDOWN_TIMEOUT);
        delete m_Backend;
        m_Backend = nullptr;
    }
}

bool Renderer::InitForSwap(RendererAPI newApi)
{
    switch (newApi) {
        case RendererAPI::DirectX12:
            m_Backend = new RendererBackendDX12(m_BackendSettings, m_Window);
            break;
        case RendererAPI::Vulkan:
            m_Backend = new RendererBackendVulkan(m_BackendSettings, m_Window);
            break;
        default:
            SM_ERROR("InitForSwap: unsupported API %d", static_cast<int>(newApi));
            return false;
    }

    if (!m_Backend->Init()) { SM_ERROR("InitForSwap: backend Init failed"); return false; }

    m_Device = m_Backend->CreateDevice();
    if (!m_Device) { SM_ERROR("InitForSwap: CreateDevice failed"); return false; }

    m_Backend->CreateSwapChain(m_BackendSettings.backBufferWidth, m_BackendSettings.backBufferHeight);

    m_CommandList = m_Device->createCommandList();
    if (!m_CommandList) { SM_ERROR("InitForSwap: createCommandList failed"); return false; }

    m_GpuTimer.Init(m_Device, 256);

    // Rebuild default material resources + replay caches.
    nvrhi::TextureHandle missingTex;
    nvrhi::SamplerHandle defaultSampler;
    CreateDefaultMaterialResources(missingTex, defaultSampler);

    m_MeshSystem.SetDevice(m_Device);
    m_MeshSystem.RecreateGpuResources();          // per-mesh failures are non-fatal
    m_MaterialSystem.SetDevice(m_Device);
    m_MaterialSystem.RecreateGpuResources(missingTex, defaultSampler);

    // ImGui NVRHI backend against the new device.
    if (!m_ImGuiRenderer || !m_ImGuiRenderer->InitNvrhiForDevice(m_Device)) {
        SM_ERROR("InitForSwap: ImGui re-init failed");
        return false;
    }

    // Recreate render passes.
    auto primitivePass = std::make_unique<PrimitiveRenderPass>();
    if (!primitivePass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: PrimitivePass failed"); return false; }
    AddRenderPass(std::move(primitivePass));

    auto meshPass = std::make_unique<MeshRenderPass>();
    if (!meshPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: MeshPass failed"); return false; }
    AddRenderPass(std::move(meshPass));

    auto uiPass = std::make_unique<UiRenderPass>();
    if (!uiPass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: UiPass failed"); return false; }
    AddRenderPass(std::move(uiPass));

    // Frame index reset so the first post-swap frame is treated like a warm-up
    // (Render() skips frame 0; see m_FrameIndex guard).
    m_FrameIndex = 0;

    SM_TRACE("InitForSwap: now running API %d", static_cast<int>(m_Backend->GetAPI()));
    return true;
}

bool Renderer::SwapBackend(RendererAPI newApi)
{
    if (m_Backend && m_Backend->GetAPI() == newApi) {
        SM_TRACE("SwapBackend: already running API %d; no-op", static_cast<int>(newApi));
        return true;
    }

    SM_TRACE("SwapBackend: %d -> %d begin",
             static_cast<int>(m_Backend ? m_Backend->GetAPI() : RendererAPI::Invalid),
             static_cast<int>(newApi));

    if (m_Device) {
        m_Device->waitForIdle();
    }

    TeardownForSwap();

    if (!InitForSwap(newApi)) {
        SM_ERROR("SwapBackend: InitForSwap failed (fatal)");
        return false;
    }

    SM_TRACE("SwapBackend: complete");
    return true;
}
```

Note on `m_FrameIndex`: `Render()` only draws when `m_FrameIndex > 0` (line 208), incrementing each call. Resetting to 0 gives the new swapchain one warm-up iteration, matching first-boot behavior.

- [ ] **Step 4: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. (Requires Task 6's ImGui methods to exist.)

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/Renderer.h src/editor/src/rendering/Renderer.cpp
git commit -m "Renderer: SwapBackend orchestration (teardown + rebuild)"
```

---

## Task 5: ImGui NVRHI-only teardown + re-init

> **Runs before Task 6** (Renderer calls these methods), so it is numbered first.

`m_ImGuiNvrhi` and `m_MeshPreviewRenderer` are device-bound; the ImGui *context* and fonts are not. Split the lifecycle so a swap keeps the context but rebuilds the GPU backend.

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

- [ ] **Step 1: Declare the two methods in `ImGuiRenderer.h`**

In `public:` after `void Shutdown();`:

```cpp
    // Hot-swap: tear down only the device-bound NVRHI backend + preview
    // renderer; keep the ImGui context, dock layout, and loaded fonts.
    void ShutdownNvrhiOnly();
    // Hot-swap: recreate the NVRHI backend + preview renderer against a new
    // device, and re-upload the font atlas. Returns false on failure.
    bool InitNvrhiForDevice(nvrhi::IDevice* device);
```

`ImGuiRenderer` needs the `Renderer*` to rebuild `MeshPreviewRenderer`. Add a private member to retain it. After `MaterialSystem* m_MaterialSystem = nullptr;` add:

```cpp
    Renderer* m_Renderer = nullptr;  // retained for hot-swap preview re-init
```

- [ ] **Step 2: Retain `Renderer*` in `Init`**

In `ImGuiRenderer::Init` (line 142), after `m_MaterialSystem = materialSystem;` add:

```cpp
    m_Renderer = renderer;
```

- [ ] **Step 3: Implement the two methods**

Add near `ImGuiRenderer::Shutdown` (around line 1622) in `ImGuiRenderer.cpp`:

```cpp
void ImGuiRenderer::ShutdownNvrhiOnly() {
    // Device-bound resources only. ImGui context + fonts stay alive.
    if (m_MeshPreviewRenderer) {
        m_MeshPreviewRenderer.reset();
    }
    if (m_ImGuiNvrhi) {
        m_ImGuiNvrhi.reset();
    }
}

bool ImGuiRenderer::InitNvrhiForDevice(nvrhi::IDevice* device) {
    if (!device) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: null device");
        return false;
    }

    m_ImGuiNvrhi = std::make_unique<ImGui_NVRHI>();
    if (!m_ImGuiNvrhi->init(device)) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: ImGui_NVRHI init failed");
        return false;
    }

    m_MeshPreviewRenderer = std::make_unique<MeshPreviewRenderer>();
    if (!m_MeshPreviewRenderer->Initialize(device, m_Renderer, 256, 256)) {
        SM_ERROR("ImGuiRenderer::InitNvrhiForDevice: MeshPreviewRenderer init failed");
        m_MeshPreviewRenderer.reset();
        // Non-fatal: preview just won't render. Continue.
    }

    // Re-upload the font atlas against the new device.
    m_ImGuiNvrhi->updateFontTexture();

    return true;
}
```

> **Check `ImGui_NVRHI::init` return type.** In `ImGuiRenderer::Init` (line 188) it is called as `m_ImGuiNvrhi->init(device);` without checking a return. Open `imgui_nvrhi.h` to confirm whether `init` returns `bool`. If it returns `void`, drop the `if (!...)` wrapper and just call it, then `return true;`. Adjust the code above accordingly — do not invent a bool return that doesn't exist.

- [ ] **Step 4: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "ImGuiRenderer: NVRHI-only teardown + re-init for hot-swap"
```

---

## Task 7: RenderThread handles the SwapBackend command

**Files:**
- Modify: `src/editor/src/threading/RenderThread.cpp`

- [ ] **Step 1: Add the SwapBackend case to the PR command drain**

In `RenderThread::RunLoop`, the PR command loop is at lines 40-53. Add a `SwapBackend` case. It must run the full swap then clear the pause flag. Insert before the `default:` case:

```cpp
                case RendererCommandType::SwapBackend: {
                    if (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
                        SM_WARN("RenderThread: swap already in progress; dropping duplicate");
                        break;
                    }
                    const RendererAPI target = cmd.SwapBackend.TargetApi;
                    SM_TRACE("RenderThread: SwapBackend -> %d requested", static_cast<int>(target));

                    // 1. Signal GameThread to pause and wait for its ack (5 s timeout).
                    m_AppContext->SwapInProgress.store(true, std::memory_order_release);
                    {
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (!m_AppContext->GameThreadPaused.load(std::memory_order_acquire)) {
                            if (std::chrono::steady_clock::now() > deadline) {
                                SM_ERROR("RenderThread: GameThread did not pause in time");
                                MessageBoxA(nullptr,
                                    "GameThread did not pause for renderer swap; forcing exit.",
                                    "Editor — swap failure", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                                ExitProcess(static_cast<UINT>(-1));
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }

                    // 2. Perform the swap.
                    const bool ok = m_Renderer->SwapBackend(target);

                    // 3. Release GameThread regardless (so it can exit cleanly even on failure).
                    m_AppContext->SwapInProgress.store(false, std::memory_order_release);

                    if (!ok) {
                        SM_ERROR("RenderThread: SwapBackend failed (fatal)");
                        MessageBoxA(nullptr,
                            "Failed to initialize the selected renderer backend.\n"
                            "The editor cannot continue. Restart and choose a different\n"
                            "backend via editor_settings.json or --backend=...",
                            "Editor — swap failure", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
                        ExitProcess(static_cast<UINT>(-1));
                    }
                    SM_TRACE("RenderThread: SwapBackend complete");
                    break;
                }
```

- [ ] **Step 2: Ensure required headers**

At the top of `RenderThread.cpp`, confirm `<windows.h>` (for `MessageBoxA`/`ExitProcess`), `<chrono>`, and `<thread>` are included. Add any that are missing:

```cpp
#include <windows.h>
#include <chrono>
#include <thread>
```

- [ ] **Step 3: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/editor/src/threading/RenderThread.cpp
git commit -m "RenderThread: execute backend swap on SwapBackend command"
```

---

## Task 8: Apply triggers the swap; remove the restart banner

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h`

- [ ] **Step 1: Change the Apply path to push SwapBackend**

In `ImGuiRenderer.cpp`, find the Settings menu Apply block (search for `Apply##SettingsBackendApply`). Replace the success branch so it pushes a swap command instead of setting `m_RestartRequired`:

```cpp
                if (ImGui::Button("Apply##SettingsBackendApply"))
                {
                    const RendererAPI previous = m_AppContext->Settings.Backend;
                    m_AppContext->Settings.Backend = m_PendingBackend;
                    if (SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH,
                                              m_AppContext->Settings))
                    {
                        m_SettingsSaveError.clear();
                        RendererCommand swapCmd{};
                        swapCmd.Type = RendererCommandType::SwapBackend;
                        swapCmd.SwapBackend.TargetApi = m_PendingBackend;
                        if (!m_AppContext->PRCommandRing.Push(swapCmd)) {
                            m_SettingsSaveError = "Renderer busy; could not start swap. Restart to apply.";
                        }
                    }
                    else
                    {
                        m_AppContext->Settings.Backend = previous;
                        m_SettingsSaveError = "Failed to save editor_settings.json";
                    }
                }
```

- [ ] **Step 2: Remove the restart banner block**

Delete the restart banner block added in Phase A (the `if (m_RestartRequired) { ... }` block submitted near the end of `Render()` — search for `##RestartBanner`). Remove the whole block including the `PushFont`/`PopFont` wrapper added for it.

- [ ] **Step 3: Remove the `m_RestartRequired` member**

In `ImGuiRenderer.h`, delete the `bool m_RestartRequired = false;` member (added in Phase A). Leave `m_PendingBackend`, `m_PendingBackendInitialized`, and `m_SettingsSaveError` — those are still used by the Settings menu.

- [ ] **Step 4: Confirm the dockspace offset no longer references `m_RestartRequired`**

The Phase B fix offset the dockspace by banner height when `m_RestartRequired` was true (search for `dockOffsetY`). Since the banner is gone, simplify: set `dockOffsetY` to `0.0f` unconditionally, OR revert that block to the original `DockSpaceOverViewport` call. Use the simpler revert:

```cpp
        // Dockspace covering the full work area (no banner offset in Phase B).
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
```

Replace the entire manual `##DockSpaceHost` block with this single line. (This reverts the Task-7/Phase-A dockspace workaround, which only existed to make room for the banner.)

- [ ] **Step 5: Build**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build. No references to `m_RestartRequired` remain (grep to confirm: `grep -rn m_RestartRequired src/editor` should return nothing).

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h
git commit -m "Settings Apply performs live backend swap; remove restart banner"
```

---

## Task 9: Smoke matrix

**Files:** none (manual verification). Some steps need a human at the editor; the controller dispatches what it can and clearly flags interactive rows for the user.

- [ ] **Step 1: Build everything in scope + ECS tests**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```

Expected: clean build; `All ECS tests passed`.

- [ ] **Step 2: Interactive smoke (user-driven)**

Launch the editor. Run each row; record PASS/FAIL:

1. Start under DX12 (default). `Settings → Renderer Backend → vulkan → Apply`. Expect: brief freeze, then renders under Vulkan; window responsive; log shows `SwapBackend: ... complete`.
2. Swap back Vulkan → DX12. Symmetric.
3. With the default scene (rotating text + lights), swap. Text still rotates after; day/night cycle continues; no duplicate entities.
4. Load a mesh + texture (via the existing mesh/material UI), then swap. Mesh still visible, texture not garbled; inspector `MeshId`/`MaterialId` unchanged.
5. Hold `W` during Apply. After swap, camera still moves forward (GameState input survived).
6. Swap, then `File → Save` world, then restart editor. World loads under the persisted backend, no duplicates.
7. Rapid double-Apply (click Apply twice fast). Second swap dropped with WARN; no crash.
8. ImGui dock layout (inspector right, managers left) retained after swap.

- [ ] **Step 3: Driver-absent path (optional, if a machine without one backend is available)**

Force a backend that can't init. Expect MessageBox + clean exit; the settings file already holds the target so restart shows the Phase A guard.

- [ ] **Step 4: Memory sanity**

Swap back and forth ~10 times. Watch Task Manager — no unbounded growth (CPU caches retained once; GPU resources freed each swap).

- [ ] **Step 5: Commit any fixes**

If smoke surfaces fixes, commit them as a small follow-up. If all green, no commit.

---

## Task 10: Wrap up + PR

- [ ] **Step 1: Confirm clean tree + branch**

```
git status
git log --oneline origin/main..HEAD
```

- [ ] **Step 2: Push + open PR**

```
git push -u origin renderer-hot-swap
gh pr create --title "Renderer backend hot-swap (Phase B): live DX12<->Vulkan" --body "$(cat <<'EOF'
## Summary

Phase B: swap the renderer backend at runtime via Settings → Apply, no restart.

- MeshSystem / MaterialSystem retain CPU-side caches and gain
  DestroyGpuResources / RecreateGpuResources; handles stay numerically stable.
- Renderer::SwapBackend tears down (ImGui NVRHI, passes, systems' GPU
  resources, swapchain, device, backend) and rebuilds against the new device,
  replaying the caches.
- GameThread pauses at a tick boundary; PlatformThread keeps pumping input.
- No rollback: any fatal step shows a MessageBox and exits (settings file
  already holds the new backend, so a restart applies it).
- Apply now performs the live swap; the Phase A restart banner is removed and
  the dockspace workaround reverted.

Spec: docs/superpowers/specs/2026-05-20-renderer-hot-swap-design.md
Plan: docs/superpowers/plans/2026-05-20-renderer-hot-swap.md

## Test plan

- [x] Build ecs+editor+game+test_ecs clean; test_ecs passes
- [ ] (manual) DX12<->Vulkan swap, empty + populated scenes, no duplicates
- [ ] (manual) handles stable; textures/meshes intact post-swap
- [ ] (manual) held-key + dock layout survive swap
- [ ] (manual) rapid double-Apply safe; driver-absent path exits cleanly
EOF
)"
```

- [ ] **Step 3: Hand the PR URL back to the user.**

---

## Out-of-scope reminders (do not implement here)

- Rollback on failure (kill-on-fail chosen).
- Async / non-blocking swap (blocking chosen).
- DirectX 11 backend.
- Re-load-from-disk asset path.
- TAA / render-target history preservation across swap.
