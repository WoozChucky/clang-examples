# Editor Viewport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the 3D scene into an editor-owned offscreen render target shown in a dockable "Viewport" ImGui panel (Engine seam keeps `runtime.exe` full-screen), with a default dock layout and a correct per-panel camera/UI aspect.

**Architecture:** Add an `IOverlay::GetSceneFramebuffer(swapChainFb)` seam; `Renderer::Render` renders gameplay passes into that offscreen FB (editor) or the swapchain (runtime). A new editor `SceneViewport` owns the color+depth RT (formats matched to the swapchain so pass pipelines stay compatible). `ImGuiRenderer` shows the RT via `ImGui::Image`, publishes the panel size through a packed `atomic<uint64_t>` on `ApplicationContext`, and the `GameThread` feeds that to `ViewportComponent` for the camera aspect + UI ortho.

**Tech Stack:** C++23, NVRHI (DX12/VK), Dear ImGui 1.92.4 (docking + `imgui_internal.h` DockBuilder), custom ECS, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-editor-viewport-design.md`

**Conventions for every task:**
- Build preset: `msvc-win64-vs2026-community`. Build dir: `out/build/msvc-win64-vs2026-community`. Binaries: `out/build/msvc-win64-vs2026-community/bin/Debug/`.
- When a task **adds a new file to a target**, re-run `cmake --preset msvc-win64-vs2026-community` before building.
- No `GAME_API_VERSION` bump. `ecs`/`game` logic unchanged.
- This is rendering integration: there are **no new unit tests**. Each task's verification is a clean build + keeping `test_ecs`/`test_alloc`/`test_frustum` green. The GUI behaviour is confirmed by the user smoke after all tasks (a subagent cannot run the GUI — verify the build and report).
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — a plain `git commit` is correct. Never stage `.claude/`. Stage only the files each step names. After each commit run `git log -1 --format='%an <%ae>'` and confirm the personal email.

---

### Task 1: Cross-thread viewport size (ApplicationContext + GameThread)

Foundational and behaviour-preserving: the field starts at 0, so until the editor writes it (Task 4) the GameThread falls back to the window size — identical to today, and runtime never writes it.

**Files:**
- Modify: `src/common/include/ApplicationContext.h` (add a field to `struct ApplicationContext`, which starts at line 127; the existing atomics are around lines 135-140)
- Modify: `src/engine/src/threading/GameThread.cpp:378-380`

- [ ] **Step 1: Add the packed size field to `ApplicationContext`**

In `src/common/include/ApplicationContext.h`, immediately after the `std::atomic<bool> GameThreadPaused{false};` line (around line 140), add:
```cpp

    // Editor scene-viewport size, packed (width<<32 | height). 0 => use the OS window size.
    // Written by the editor overlay (RenderThread) each frame; read by the GameThread for the
    // camera aspect + UI ortho. The runtime never writes it, so it keeps the full-window aspect.
    std::atomic<uint64_t> SceneViewportSize{0};
```
(`std::atomic` is already used in this struct, so no new include is needed.)

- [ ] **Step 2: Feed it into the per-frame ViewportComponent update**

In `src/engine/src/threading/GameThread.cpp`, replace lines 378-379:
```cpp
			const uint32_t vw = m_AppContext->Settings.windowWidth;
			const uint32_t vh = m_AppContext->Settings.windowHeight;
```
with:
```cpp
			const uint64_t sv = m_AppContext->SceneViewportSize.load(std::memory_order_relaxed);
			const uint32_t vw = sv ? uint32_t(sv >> 32) : m_AppContext->Settings.windowWidth;
			const uint32_t vh = sv ? uint32_t(sv & 0xffffffffu) : m_AppContext->Settings.windowHeight;
```
This drives both `ViewportComponent` (3D camera aspect, line 380) and `UICameraComponent` (UI ortho, lines 381-382) — both should use the scene render-target size.

- [ ] **Step 3: Build engine + editor + runtime, run regression tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; `All ECS tests passed.` and `All allocator tests passed.` Behaviour is unchanged (field is 0 → window size).

- [ ] **Step 4: Commit**

```bash
git add src/common/include/ApplicationContext.h src/engine/src/threading/GameThread.cpp
git commit -m "feat: add cross-thread SceneViewportSize for camera/UI aspect"
```

---

### Task 2: `IOverlay::GetSceneFramebuffer` seam + `Renderer::Render`

Adds the seam and routes the passes through it. With no overlay implementing it yet (the base returns null), both exes render exactly as today (`sceneFb == swapFb`).

**Files:**
- Modify: `src/engine/include/IOverlay.h` (add a virtual after the `OnDeviceReset` declaration at line 26)
- Modify: `src/engine/src/rendering/Renderer.cpp:179-205`

- [ ] **Step 1: Add the seam to `IOverlay`**

In `src/engine/include/IOverlay.h`, after the `virtual bool OnDeviceReset(...) = 0;` line (line 26), add:
```cpp

    // Optional: the framebuffer the gameplay passes should render into (e.g. an editor's
    // offscreen viewport target). Return null to render directly to the swapchain — the default,
    // used by the stripped runtime. `swapChainFb` is supplied so an implementation can match its
    // offscreen color format + sample count to the swapchain (keeping pass pipelines compatible).
    virtual nvrhi::IFramebuffer* GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb) { return nullptr; }
```

- [ ] **Step 2: Route the passes through the seam in `Renderer::Render`**

In `src/engine/src/rendering/Renderer.cpp`, replace the `RenderPasses` block (lines 179-193):
```cpp
            {
                ZoneScopedN("RenderPasses");
                static glm::vec4 ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                const auto clearColor = nvrhi::Color(red, green, blue, ClearColor.a);

                nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, clearColor);

                // Render all passes
                for (auto& pass : m_RenderPasses) {
                    ZoneScopedN("RenderPass Rec N");
                    if (pass) {
                        pass->Render(m_CommandList, frameBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                    }
                }
            }
```
with:
```cpp
            // Editor renders the scene into an overlay-provided offscreen target; runtime (no
            // overlay, or overlay returns null) renders straight into the swapchain backbuffer.
            nvrhi::IFramebuffer* sceneBuffer = m_Overlay ? m_Overlay->GetSceneFramebuffer(frameBuffer) : nullptr;
            if (!sceneBuffer) sceneBuffer = frameBuffer;

            {
                ZoneScopedN("RenderPasses");
                const auto sceneClear = nvrhi::Color(red, green, blue, 1.0f);
                nvrhi::utils::ClearColorAttachment(m_CommandList, sceneBuffer, 0, sceneClear);
                if (sceneBuffer != frameBuffer) {
                    // Offscreen scene: clear the swapchain (dark) so the present surface is clean
                    // behind the ImGui dockspace. Depth of sceneBuffer is cleared inside MeshRenderPass.
                    nvrhi::utils::ClearColorAttachment(m_CommandList, frameBuffer, 0, nvrhi::Color(0.1f, 0.1f, 0.1f, 1.0f));
                }

                // Render all passes into the scene buffer
                for (auto& pass : m_RenderPasses) {
                    ZoneScopedN("RenderPass Rec N");
                    if (pass) {
                        pass->Render(m_CommandList, sceneBuffer, snapshot, world, deltaTime, &m_FrameAllocator);
                    }
                }
            }
```
The overlay composite call at line 205 (`m_Overlay->Render(frameBuffer, ...)`) stays unchanged — ImGui always renders into the swapchain `frameBuffer`.

- [ ] **Step 3: Build engine + editor + runtime, run regression tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target Engine
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; both tests pass. Editor + runtime still render full-screen as before (overlay's default `GetSceneFramebuffer` returns null → `sceneBuffer == frameBuffer`).

- [ ] **Step 4: Commit**

```bash
git add src/engine/include/IOverlay.h src/engine/src/rendering/Renderer.cpp
git commit -m "feat: add IOverlay scene-framebuffer seam; route passes through it"
```

---

### Task 3: `SceneViewport` (offscreen RT owner)

A self-contained class that owns the editor's offscreen color+depth target. Created here and compiled (unused until Task 4).

**Files:**
- Create: `src/editor/src/rendering/imgui/SceneViewport.h`
- Create: `src/editor/src/rendering/imgui/SceneViewport.cpp`
- Modify: `src/editor/CMakeLists.txt` (add the new source after `src/rendering/imgui/RenderStatsPanel.cpp`)

- [ ] **Step 1: Create `SceneViewport.h`**

```cpp
#pragma once
#include <cstdint>
#include <nvrhi/nvrhi.h>

// Owns the editor's offscreen scene render target (color + depth + framebuffer). The gameplay
// passes render into this; ImGui then samples the color texture in the Viewport panel. Color
// format + sample count are matched to the swapchain so the passes' pipelines stay compatible;
// depth is D32 (matches the swapchain depth). RenderThread-only; no locking.
class SceneViewport {
public:
    void Init(nvrhi::IDevice* device) { m_Device = device; }

    // (Re)create targets if size/format/samples changed. w,h are clamped to >= 1. Returns the
    // framebuffer to render the scene into (never null once a valid device is set).
    nvrhi::IFramebuffer* EnsureTargets(uint32_t w, uint32_t h,
                                       nvrhi::Format colorFormat, uint32_t sampleCount);

    nvrhi::ITexture* ColorTexture() const { return m_Color.Get(); }

    // Device-lost: drop all device-bound resources (rebuilt lazily by the next EnsureTargets).
    void Release();

private:
    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::TextureHandle m_Color;
    nvrhi::TextureHandle m_Depth;
    nvrhi::FramebufferHandle m_Fb;
    uint32_t m_W = 0;
    uint32_t m_H = 0;
    nvrhi::Format m_ColorFormat = nvrhi::Format::UNKNOWN;
    uint32_t m_Samples = 0;
};
```

- [ ] **Step 2: Create `SceneViewport.cpp`**

```cpp
#include "SceneViewport.h"

nvrhi::IFramebuffer* SceneViewport::EnsureTargets(uint32_t w, uint32_t h,
                                                  nvrhi::Format colorFormat, uint32_t sampleCount)
{
    if (!m_Device)
        return nullptr;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (m_Fb && w == m_W && h == m_H && colorFormat == m_ColorFormat && sampleCount == m_Samples)
        return m_Fb;

    m_W = w; m_H = h; m_ColorFormat = colorFormat; m_Samples = sampleCount;

    // Color: render target + shader resource (so ImGui can sample it). No keepInitialState, so
    // NVRHI tracks the RenderTarget -> ShaderResource transition between the scene and ImGui
    // command lists (same as MeshPreviewRenderer).
    nvrhi::TextureDesc colorDesc;
    colorDesc.width = m_W;
    colorDesc.height = m_H;
    colorDesc.format = colorFormat;
    colorDesc.dimension = nvrhi::TextureDimension::Texture2D;
    colorDesc.sampleCount = sampleCount;
    colorDesc.isRenderTarget = true;
    colorDesc.isShaderResource = true;
    colorDesc.debugName = "SceneViewportColor";
    colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    m_Color = m_Device->createTexture(colorDesc);

    nvrhi::TextureDesc depthDesc;
    depthDesc.width = m_W;
    depthDesc.height = m_H;
    depthDesc.format = nvrhi::Format::D32;
    depthDesc.dimension = nvrhi::TextureDimension::Texture2D;
    depthDesc.sampleCount = sampleCount;
    depthDesc.isRenderTarget = true;
    depthDesc.debugName = "SceneViewportDepth";
    depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
    m_Depth = m_Device->createTexture(depthDesc);

    m_Fb = m_Device->createFramebuffer(
        nvrhi::FramebufferDesc()
            .addColorAttachment(m_Color)
            .setDepthAttachment(m_Depth));

    return m_Fb;
}

void SceneViewport::Release()
{
    m_Fb = nullptr;
    m_Color = nullptr;
    m_Depth = nullptr;
    m_W = 0; m_H = 0;
    m_ColorFormat = nvrhi::Format::UNKNOWN;
    m_Samples = 0;
}
```

- [ ] **Step 3: Add the source to the editor target**

In `src/editor/CMakeLists.txt`, after the line `    src/rendering/imgui/RenderStatsPanel.cpp` add:
```cmake
    src/rendering/imgui/SceneViewport.cpp
```

- [ ] **Step 4: Reconfigure + build the editor**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean (the class compiles; it is not referenced yet).

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/SceneViewport.h src/editor/src/rendering/imgui/SceneViewport.cpp src/editor/CMakeLists.txt
git commit -m "feat: add SceneViewport offscreen render-target owner"
```

---

### Task 4: Wire `SceneViewport` into the editor overlay + Viewport panel

Implements the seam in the editor, renders the scene into the offscreen RT, shows it in a (floating, for now) "Viewport" window, and publishes the panel size. After this task the scene appears inside the Viewport window; runtime is untouched.

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h` (include, member, method decl)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (init/release lifecycle; `GetSceneFramebuffer`; Viewport window in `Render`)
- Modify: `src/editor/src/rendering/imgui/ImGuiOverlay.h` (override the seam)

- [ ] **Step 1: Declare the member + method in `ImGuiRenderer.h`**

Add the include near the existing imgui-layer includes (after `#include "imgui_nvrhi.h"` at line 8):
```cpp
#include "SceneViewport.h"
```
Add the public method declaration after `bool InitNvrhiForDevice(nvrhi::IDevice* device);` (line 34):
```cpp

    // Editor scene target: the gameplay passes render into this offscreen FB (sized to the
    // Viewport panel); the panel samples its color texture. Returns the FB to render into.
    nvrhi::IFramebuffer* GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb);
```
Add the members in the private member block (after `Renderer* m_Renderer = nullptr; ...` around line 55):
```cpp

    SceneViewport m_SceneViewport;          // offscreen scene render target
    uint32_t m_LastViewportW = 0;           // last Viewport panel content size (pixels)
    uint32_t m_LastViewportH = 0;
```

- [ ] **Step 2: Init / release the SceneViewport with the device lifecycle**

In `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`:

In `Init`, right after the MeshPreviewRenderer block (after line 199, before the function returns), add:
```cpp
    m_SceneViewport.Init(device);
```
In `InitNvrhiForDevice`, after the MeshPreviewRenderer block (after line 1633), add:
```cpp
    m_SceneViewport.Init(device);
```
In `Shutdown`, after the MeshPreviewRenderer reset (after line 1599), add:
```cpp
    m_SceneViewport.Release();
```
In `ShutdownNvrhiOnly`, after the MeshPreviewRenderer reset (after line 1610), add:
```cpp
    m_SceneViewport.Release();
```

- [ ] **Step 3: Implement `GetSceneFramebuffer`**

Add this definition in `ImGuiRenderer.cpp` (e.g. right before `void ImGuiRenderer::Shutdown()`):
```cpp
nvrhi::IFramebuffer* ImGuiRenderer::GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb)
{
    if (!swapChainFb)
        return nullptr;

    const nvrhi::FramebufferInfoEx& info = swapChainFb->getFramebufferInfo();
    const nvrhi::Format colorFormat = info.colorFormats[0];
    const uint32_t      sampleCount = info.sampleCount;

    // Frame 1 (before the Viewport panel has reported a size): default to the swapchain size.
    uint32_t w = m_LastViewportW ? m_LastViewportW : info.width;
    uint32_t h = m_LastViewportH ? m_LastViewportH : info.height;

    return m_SceneViewport.EnsureTargets(w, h, colorFormat, sampleCount);
}
```
(`getFramebufferInfo()` returns `nvrhi::FramebufferInfoEx`, which has `colorFormats`, `sampleCount`, `width`, `height`. If the exact type name differs in the vendored NVRHI, use `const auto& info = swapChainFb->getFramebufferInfo();`.)

- [ ] **Step 4: Draw the Viewport panel + publish its size**

In `ImGuiRenderer::Render`, after the dockspace + existing panels (after the `DrawRenderStatsPanel(...)` call at line 486), add:
```cpp
        // Scene viewport: shows the offscreen scene RT. Zero padding so the image fills the panel.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("Viewport"))
        {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            m_LastViewportW = static_cast<uint32_t>(avail.x > 1.0f ? avail.x : 1.0f);
            m_LastViewportH = static_cast<uint32_t>(avail.y > 1.0f ? avail.y : 1.0f);
            if (nvrhi::ITexture* sceneTex = m_SceneViewport.ColorTexture())
                ImGui::Image(reinterpret_cast<ImTextureID>(sceneTex), avail);
        }
        ImGui::End();
        ImGui::PopStyleVar();

        // Publish the panel size for the GameThread (camera aspect + UI ortho).
        if (m_AppContext)
            m_AppContext->SceneViewportSize.store(
                (static_cast<uint64_t>(m_LastViewportW) << 32) | static_cast<uint64_t>(m_LastViewportH),
                std::memory_order_relaxed);
```
(The Viewport will float over the existing docked panels until Task 5 adds the layout — that is expected and still proves the scene renders into the panel. `reinterpret_cast<ImTextureID>` mirrors the existing `ImGui::Image` call in this file.)

- [ ] **Step 5: Override the seam in `ImGuiOverlay.h`**

In `src/editor/src/rendering/imgui/ImGuiOverlay.h`, add after the `OnDeviceReset` override (line 20):
```cpp
    nvrhi::IFramebuffer* GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb) override {
        return m_Impl.GetSceneFramebuffer(swapChainFb);
    }
```

- [ ] **Step 6: Build editor + runtime, run regression tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all build clean; both tests pass. (GUI behaviour — scene drawn inside a floating "Viewport" window — is confirmed by the user smoke after Task 5.)

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/imgui/ImGuiOverlay.h
git commit -m "feat: render scene into editor SceneViewport panel; publish panel size"
```

---

### Task 5: Default dock layout + "View → Reset Layout"

Docks the Viewport in the center with the tool panels around it, and removes the passthrough flag. Auto-builds for fresh installs; a menu command lets existing `imgui.ini` users adopt it.

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (file-static layout helper; layout-state statics + menu item; dockspace call)

- [ ] **Step 1: Add the default-layout helper**

Near the top of `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (after the includes, in file scope), add:
```cpp
namespace {
void BuildDefaultDockLayout(ImGuiID dockId)
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.20f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25f, nullptr, &center);
    ImGuiID leftBottom  = ImGui::DockBuilderSplitNode(left,  ImGuiDir_Down, 0.5f, nullptr, &left);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.5f, nullptr, &right);

    ImGui::DockBuilderDockWindow("Mesh Manager",           left);
    ImGui::DockBuilderDockWindow("Material Manager",       left);
    ImGui::DockBuilderDockWindow("Hello, world!",          leftBottom);
    ImGui::DockBuilderDockWindow("ECS Inspector & Editor", right);
    ImGui::DockBuilderDockWindow("Render Stats",           rightBottom);
    ImGui::DockBuilderDockWindow("Memory",                 rightBottom);
    ImGui::DockBuilderDockWindow("Viewport",               center);

    ImGui::DockBuilderFinish(dockId);
}
} // namespace
```

- [ ] **Step 2: Add the layout-state statics + the View menu item**

In `ImGuiRenderer::Render`, immediately before `if (ImGui::BeginMainMenuBar())` (line 373), add:
```cpp
        static bool s_LayoutInitialized = false;
        static bool s_ResetLayout = false;
```
Inside the main menu bar, after the `Settings` menu's `ImGui::EndMenu();` (line 473) and before `ImGui::EndMainMenuBar();` (line 476), add:
```cpp
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Reset Layout")) { s_ResetLayout = true; }
                ImGui::EndMenu();
            }
```

- [ ] **Step 3: Capture the dock id, drop passthrough, build the layout once**

Replace the dockspace line (line 480):
```cpp
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
```
with:
```cpp
        const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (s_ResetLayout || !s_LayoutInitialized)
        {
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
            // Build when explicitly reset, or on first run when there is no saved split layout
            // (fresh install / no imgui.ini). An existing user layout is otherwise preserved.
            if (s_ResetLayout || node == nullptr || !node->IsSplitNode())
                BuildDefaultDockLayout(dockId);
            s_LayoutInitialized = true;
            s_ResetLayout = false;
        }
```
(`ImGuiDockNode`, `DockBuilder*`, and `IsSplitNode()` come from `imgui_internal.h`, already included at line 13. `DockSpaceOverViewport` returns `ImGuiID` in ImGui 1.92.4.)

- [ ] **Step 4: Build editor, run regression tests**

Run:
```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: builds clean; both tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: default editor dock layout with centered Viewport + Reset Layout menu"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (`Engine`, `ecs`, `game`, `editor`, `runtime`, `test_ecs`, `test_alloc`, `test_frustum`).
- [ ] `test_ecs`, `test_alloc`, `test_frustum` all print their `All ... passed.` line.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):**
  - Editor: the 3D scene is drawn **only** inside the **Viewport** panel; the salmon background appears only there. Tool panels dock around it.
  - Use **View → Reset Layout** once if your existing `imgui.ini` shows the Viewport floating — it should snap to the center with the panels arranged left/right.
  - Resize the Viewport panel: the scene resizes with it and is **not** stretched (correct aspect); the "Hello, Game!" UI text stays correctly proportioned too.
  - Drag/redock panels — no crash.
  - `runtime.exe`: scene renders full-screen exactly as before (no panel, no regression).

## Notes / non-goals
- No `GAME_API_VERSION` bump; no ECS component, `GameState`, or export-layout change (reuses `ViewportComponent`).
- 1-frame lag while actively resizing the Viewport panel (offscreen RT matches the new size next frame) — transient, standard.
- Not implemented (YAGNI): gating camera mouse-look to Viewport hover/focus, multiple viewports, render-scale/MSAA options.
