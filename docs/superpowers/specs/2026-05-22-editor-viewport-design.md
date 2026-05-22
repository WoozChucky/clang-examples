# Editor Viewport (offscreen RT in a dockable panel) — Design

**Date:** 2026-05-22
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main`.

## Goal

Render the 3D scene into an **editor-owned offscreen color+depth render target** and display it
via `ImGui::Image` inside a **dockable "Viewport" window**, so the world is drawn only inside
that panel — with the other tool panels docked around it, like Unity/Unreal/Godot. A default
dock layout places the Viewport in the center out of the box. `runtime.exe` (no ImGui overlay)
keeps rendering full-screen directly to the swapchain — **unchanged**.

This replaces today's behaviour (scene rendered full-window to the swapchain with opaque panels
floating on top, the scene wastefully drawn behind them and stretched to the window aspect).

## Background (verified)

- **Frame loop** — `src/engine/src/rendering/Renderer.cpp:152-226` `Renderer::Render`:
  acquires the swapchain FB (`m_Backend->GetCurrentFrameBuffer()`, `:171`), clears its color
  (`ClearColorAttachment(... red,green,blue ...)`, `:184`), runs every `IRenderPass` into that
  **same** FB (`:187-192`), executes the command list (`:198-200`), then
  `if (m_Overlay) m_Overlay->Render(frameBuffer, ...)` (`:205`), then `Present` (`:209`).
  The dynamic salmon clear color is passed in from `RenderThread.cpp:192-197`.
- **Pass pipeline compatibility** — passes build their pipeline lazily against the **first**
  framebuffer's `FramebufferInfo` (`MeshRenderPass.cpp:301-324`: `m_Device->createGraphicsPipeline(pso, fbi)`),
  and `OnResize` nulls it (`:622-625`). NVRHI pipeline compatibility depends on **formats +
  sample count, not size**. So an offscreen RT whose color format + depth format + sample count
  match the swapchain keeps all three passes' pipelines valid in both exes.
- **Depth clear** — `MeshRenderPass.cpp:329` clears the depth of *whatever* FB it is handed
  (`frameBuffer->getDesc().depthAttachment.texture`). So the offscreen path needs **no extra
  depth-clear plumbing** — as long as the offscreen FB has a depth attachment, the existing pass
  clears it.
- **Offscreen-RT→ImGui pattern already exists** — `src/editor/src/rendering/imgui/MeshPreviewRenderer.{h,cpp}`:
  creates a color texture (`isRenderTarget=true, isShaderResource=true`) + depth texture +
  framebuffer (`CreateRenderTargets`, `:170-214`), renders into it, returns the `nvrhi::ITexture*`,
  and ImGui samples it via `ImGui::Image((ImTextureID)(nvrhi::ITexture*)tex, ...)`. The NVRHI
  ImGui backend (`imgui_nvrhi.h`) auto-caches binding sets per texture (`bindingsCache`), and
  NVRHI's automatic state tracking inserts the RenderTarget→ShaderResource barrier between the
  scene command list and the ImGui command list. `Resize` recreates the targets (`:386-403`).
  This proves the whole RT-sampling path; the Viewport reuses this approach (with formats matched
  to the swapchain instead of MeshPreviewRenderer's fixed RGBA8/D24S8).
- **Dockspace** — `ImGuiRenderer.cpp:480`:
  `ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);`
  No DockBuilder layout exists; the docked panels in the running editor come from a saved
  `imgui.ini`. The main menu bar is at `:373-...` (File / Edit / About / Settings).
- **Panel window titles** (for docking by name): `"Mesh Manager"` (`:1228`),
  `"Material Manager"` (`:1452`), `"ECS Inspector & Editor"` (`:562`), `"Hello, world!"`
  (`:491`, the renderer/frame-time panel), `"Render Stats"` (`RenderStatsPanel.cpp:10`),
  `"Memory"` (`MemoryPanel.cpp:34`). `"Gizmo"` (`:279`) is an overlay window, not docked.
- **Camera aspect** — `src/game/src/game.cpp` `FreeLookCameraSystem` (`~:128`):
  `aspect = vp->Width/vp->Height` from the `ViewportComponent` singleton
  (`ECS.h:137` `struct ViewportComponent { uint32_t Width=1920, Height=1080; };`), set on the
  GameThread (`GameThread.cpp` init `~:73-74` and per-frame `~:380`) from the window size
  (`ApplicationContext::Settings.windowWidth/Height`). Projection is
  `perspectiveRH_ZO(fov, aspect, 0.1, 1000)`. A confined viewport must feed the **panel** size
  here, or the scene stretches.
- **IOverlay** — `src/engine/include/IOverlay.h`: `Render`, `Init`, `Shutdown`, `OnDeviceLost`,
  `OnDeviceReset`. The editor's `ImGuiOverlay` adapts these to `ImGuiRenderer`.

## Scope

**In scope:** one new `IOverlay` virtual (`GetSceneFramebuffer`); the `Renderer::Render` seam;
a new editor `SceneViewport` class (owns the offscreen RT); `ImGuiOverlay`/`ImGuiRenderer`
wiring (implement the seam, draw the Viewport window, default dock layout + Reset Layout menu,
publish the panel size); a packed `std::atomic<uint64_t>` viewport-size field on
`ApplicationContext`; the `GameThread` read that feeds `ViewportComponent`; editor CMake.

**Out of scope / unchanged:** `runtime.exe` behaviour (full-screen, no overlay → seam returns
null), `ecs`/`game` logic, `GAME_API_VERSION` (no `GameState`/export/component change — reuses
`ViewportComponent`). Not doing (YAGNI): gating camera mouse-look to viewport hover/focus,
multiple viewports, render-scale/MSAA options.

## Design

### 1. The seam — `IOverlay::GetSceneFramebuffer`

```cpp
// IOverlay.h — default returns null so non-overlay consumers (runtime) are unaffected.
virtual nvrhi::IFramebuffer* GetSceneFramebuffer(nvrhi::IFramebuffer* swapChainFb) { return nullptr; }
```
Passing `swapChainFb` lets the overlay read the swapchain's color format + sample count
(`swapChainFb->getFramebufferInfo()`) so its offscreen RT matches — solving the
frame-1 "what format?" question without querying the backend directly.

### 2. `Renderer::Render` change (the only core edit)

Replace the single-clear + render-into-`frameBuffer` block (`:181-192`, `:205`) with:
```cpp
nvrhi::IFramebuffer* swapFb  = m_Backend->GetCurrentFrameBuffer();
nvrhi::IFramebuffer* sceneFb = m_Overlay ? m_Overlay->GetSceneFramebuffer(swapFb) : nullptr;
if (!sceneFb) sceneFb = swapFb;

const auto sceneClear = nvrhi::Color(red, green, blue, 1.0f);
nvrhi::utils::ClearColorAttachment(m_CommandList, sceneFb, 0, sceneClear);
if (sceneFb != swapFb)   // editor: also clear the present surface (dark) behind ImGui
    nvrhi::utils::ClearColorAttachment(m_CommandList, swapFb, 0, nvrhi::Color(0.1f,0.1f,0.1f,1.0f));

for (auto& pass : m_RenderPasses)
    if (pass) pass->Render(m_CommandList, sceneFb, snapshot, world, deltaTime, &m_FrameAllocator);
// ... execute command list (unchanged) ...
if (m_Overlay) m_Overlay->Render(swapFb, deltaTime, snapshot, world, secs);
```
Runtime: `sceneFb == swapFb` → exactly today's behaviour (one salmon clear, passes into the
swapchain, no overlay). Editor: scene clears+renders into the offscreen RT (depth cleared by
`MeshRenderPass`), the swapchain gets a dark clear, ImGui composites into the swapchain.

### 3. Editor — `SceneViewport` (new, owns the RT)

`src/editor/src/rendering/imgui/SceneViewport.{h,cpp}`:
```cpp
class SceneViewport {
public:
    void Init(nvrhi::IDevice* device);
    // (Re)create color+depth+FB if size or format changed; clamps w,h >= 1. colorFormat &
    // sampleCount come from the swapchain FBI; depth = D32 (matches the swapchain depth).
    nvrhi::IFramebuffer* EnsureTargets(uint32_t w, uint32_t h,
                                       nvrhi::Format colorFormat, uint32_t sampleCount);
    nvrhi::ITexture* ColorTexture() const;     // for ImGui::Image
    void Release();                            // device-lost: drop color/depth/fb
private:
    nvrhi::IDevice* m_Device = nullptr;
    nvrhi::TextureHandle m_Color, m_Depth;
    nvrhi::FramebufferHandle m_Fb;
    uint32_t m_W = 0, m_H = 0; nvrhi::Format m_ColorFormat = nvrhi::Format::UNKNOWN; uint32_t m_Samples = 0;
};
```
Texture descs mirror `MeshPreviewRenderer`'s working state-handling (color
`isRenderTarget=true, isShaderResource=true`; depth `isRenderTarget=true`), but the **color
format + sample count come from the swapchain** and **depth is D32** so pass pipelines stay
compatible. No `keepInitialState` on the color (NVRHI tracks it for the RT→SRV barrier).

### 4. `ImGuiOverlay` / `ImGuiRenderer` wiring

- `ImGuiOverlay::GetSceneFramebuffer(swapFb)` → `m_Renderer->GetSceneFramebuffer(swapFb)`.
- `ImGuiRenderer::GetSceneFramebuffer(swapFb)` (new): read `colorFormat`/`sampleCount` from
  `swapFb->getFramebufferInfo()`; call `m_SceneViewport.EnsureTargets(m_LastViewportW,
  m_LastViewportH, colorFormat, sampleCount)` where the last size defaults to the swapchain
  size on frame 1; return the FB. `m_SceneViewport` is a member; `Init` it where the device is
  known (alongside the existing NVRHI init).
- `ImGuiRenderer::Render`: add a **`"Viewport"`** window drawn with zero window padding:
  ```cpp
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
  if (ImGui::Begin("Viewport")) {
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      m_LastViewportW = (uint32_t)std::max(1.0f, avail.x);
      m_LastViewportH = (uint32_t)std::max(1.0f, avail.y);
      if (auto* tex = m_SceneViewport.ColorTexture())
          ImGui::Image((ImTextureID)tex, avail);
  }
  ImGui::End();
  ImGui::PopStyleVar();
  // publish for the camera aspect (render thread -> game thread)
  m_AppContext->SceneViewportSize.store(((uint64_t)m_LastViewportW << 32) | m_LastViewportH,
                                        std::memory_order_relaxed);
  ```
  (The recorded size is used by *next* frame's `EnsureTargets` — a 1-frame resize lag, standard.)
- Drop `ImGuiDockNodeFlags_PassthruCentralNode` at `:480` (the Viewport window now occupies the
  center; passthrough is no longer needed).
- Forward `ImGuiOverlay::OnDeviceLost` → `m_SceneViewport.Release()` (recreated lazily by the
  next `GetSceneFramebuffer` after `OnDeviceReset` sets the new device).

### 5. Default dock layout + Reset Layout

`ImGuiRenderer.cpp`, using the dock id returned by `DockSpaceOverViewport`:
```cpp
const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
static bool s_LayoutInit = false;
if (!s_LayoutInit) {                 // run once per session
    s_LayoutInit = true;
    if (s_ResetLayout || ImGui::DockBuilderGetNode(dockId) == nullptr || !ImGui::DockBuilderGetNode(dockId)->IsSplitNode()) {
        s_ResetLayout = false;
        BuildDefaultLayout(dockId);  // see below
    }
}
```
`BuildDefaultLayout`:
```cpp
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
```
New users (no `imgui.ini`) get this automatically (the dockspace node starts unsplit). Existing
users have a saved layout (which lacks "Viewport", so it floats) — they pick **"View → Reset
Layout"** (new menu, added to the main menu bar at `:373`, sets `s_ResetLayout=true` and clears
`s_LayoutInit` so the rebuild runs next frame) to adopt the centered layout. The menu also lets
anyone restore the default at any time.

**Implementation notes:** the `ImGui::DockBuilder*` functions are declared in
`<imgui_internal.h>` — that header must be included in `ImGuiRenderer.cpp`. Confirm the bundled
ImGui version's `DockSpaceOverViewport` returns an `ImGuiID` (modern signature); if it returns
`void` in this version, get the id explicitly instead (e.g. keep the existing
`DockSpaceOverViewport(...)` call for the host/dockspace and obtain the id via the documented
overload / `ImGui::GetID`), then use that id for the DockBuilder calls. The plan must pin the
exact call against the actual ImGui version in `third_party`.

### 6. Cross-thread aspect

Add to `src/common/include/ApplicationContext.h`:
```cpp
#include <atomic>
// Packed (width<<32 | height) of the editor scene viewport. 0 => use the OS window size
// (the runtime never writes this, so it keeps the full-window aspect). Written by the editor
// overlay on the RenderThread, read by the GameThread for the camera aspect.
std::atomic<uint64_t> SceneViewportSize{0};
```
`GameThread.cpp` where it sets `ViewportComponent` (init `~:73-74` and per-frame `~:380`):
```cpp
const uint64_t sv = m_AppContext->SceneViewportSize.load(std::memory_order_relaxed);
const uint32_t vw = sv ? uint32_t(sv >> 32) : m_AppContext->Settings.windowWidth;
const uint32_t vh = sv ? uint32_t(sv & 0xffffffff) : m_AppContext->Settings.windowHeight;
gameState.World.ModifySingleton<ViewportComponent>([&](ViewportComponent& v){ v.Width = vw; v.Height = vh; });
```
Runtime: `SceneViewportSize` stays 0 → window aspect (current behaviour). Editor: panel aspect.

## Data flow (editor, per frame, RenderThread)

1. `Renderer::Render` asks the overlay for the scene FB → `SceneViewport.EnsureTargets(last
   panel size, swapchain color format/samples)` → offscreen FB.
2. Clear offscreen color (salmon) + swapchain (dark); passes render into the offscreen FB
   (depth cleared by `MeshRenderPass`); command list executed.
3. `overlay->Render(swapFb)`: ImGui builds the dockspace (default layout on first run), draws
   the Viewport window `ImGui::Image(offscreen color)`, records the panel content size, and
   publishes it to `SceneViewportSize`. ImGui renders into the swapchain. Present.
4. Next frame, GameThread reads `SceneViewportSize` → `ViewportComponent` → camera aspect.

Result: the scene is confined to the Viewport panel, correctly proportioned, with tool panels
docked around it. 1-frame lag only while actively resizing the panel (transient stretch).

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump. Rendering integration →
no new unit test (optional: a 2-line size pack/unpack check). Verification:
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum` stay green.
- **GUI smoke (user):** editor opens with the Viewport centered (or via View → Reset Layout for
  an existing `imgui.ini`); the 3D scene is drawn **only** inside the Viewport panel; resizing
  the panel resizes the scene with a correct (non-stretched) aspect; dragging/redocking panels
  works; the salmon background shows only inside the Viewport. `runtime.exe` renders the scene
  full-screen exactly as before (no panel, no regression).

## Risks

- **Offscreen format ≠ swapchain → pipeline incompatibility** → mitigated by matching color
  format + sample count from `swapChainFb` FBI and using D32 depth.
- **RT→SRV barrier** between the scene command list and the ImGui command list → handled by
  NVRHI automatic state tracking (proven by `MeshPreviewRenderer`); the color texture must not
  pin a fixed state via `keepInitialState`.
- **Existing `imgui.ini` hides the Viewport / ignores the default layout** → mitigated by the
  View → Reset Layout command (and auto-build for fresh installs).
- **Panel size 0 / collapsed** → `EnsureTargets` clamps to ≥1; the Image is only drawn when a
  color texture exists.
- **Device lost** → `SceneViewport.Release()` on `OnDeviceLost`; lazy rebuild on next frame.
