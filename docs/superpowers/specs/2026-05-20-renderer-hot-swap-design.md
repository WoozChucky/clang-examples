# Renderer Backend Hot-Swap (Phase B)

**Date:** 2026-05-20
**Status:** Draft — pending user review
**Scope:** Swap the renderer backend (DirectX 12 ↔ Vulkan) at runtime, without restarting the editor. Builds on Phase A (`docs/superpowers/specs/2026-05-20-renderer-backend-selection-design.md`), which added settings persistence, the CLI override, and the `Settings` menu.

## Motivation

Phase A persists the chosen backend but requires an editor restart to apply it. Phase B makes the `Settings → Renderer Backend → Apply` action perform the switch live, so a developer comparing DX12 and Vulkan output never leaves the editor. The restart path remains the fallback for changes a runtime swap cannot handle (e.g. a `GameState` layout change that needs a rebuild).

## Decisions (from brainstorming)

1. **Trigger:** the `Apply` button performs the swap directly. No restart banner in the normal path.
2. **Failure policy:** no rollback. If the new backend fails to come up, the editor logs fatal, shows a MessageBox, and exits. The settings file already holds the new value, so a restart applies it via the Phase A path.
3. **UX:** blocking. The editor freezes ~100-500 ms during the swap. No progress modal.
4. **Resource recreation:** add a CPU-side cache to `MeshSystem` and `MaterialSystem`; replay it against the new device. One code path for all assets (v1).
5. **Handle stability:** `MeshId` / `MaterialId` stay numerically stable across the swap (replay in original order). ECS components need no patching.
6. **Thread coordination:** GameThread pauses at a tick boundary; PlatformThread keeps pumping window/input events. RenderThread performs the swap.

## Architecture

```
ImGui Settings menu → Apply
  └─ ImGuiRenderer pushes a SwapBackend command into ApplicationContext::PRCommandRing
       (new RendererCommandType::SwapBackend carrying the target RendererAPI)

RenderThread main loop:
  Drain PR commands. On SwapBackend, defer all other work and run the swap
  state machine below.

Swap state machine (single linear pass, RenderThread-local):
  A. Set ApplicationContext::SwapInProgress = true
  B. Wait for GameThread to ack pause (ApplicationContext::GameThreadPaused == true),
     with a 5 s timeout
  C. device->waitForIdle()
  D. Tear down, in order:
       - ImGui_NVRHI backend (font texture, pipelines)
       - Render passes (Mesh, UI, MeshPreview, etc.)
       - MaterialSystem GPU resources
       - MeshSystem GPU resources
       - Swapchain
       - nvrhi::IDevice
       - RendererBackend (DX12 or Vulkan)
  E. Construct new RendererBackend(api) → Init() → CreateDevice() → CreateSwapChain()
  F. Re-init MeshSystem with new device → replay CPU cache in order (handles stay stable)
  G. Re-init MaterialSystem with new device → replay CPU cache in order
  H. Re-create render passes against the new device
  I. Re-init ImGui_NVRHI → re-upload font atlas
  J. (ApplicationSettings.Backend was already set to the target by the Apply path)
  K. Clear SwapInProgress; GameThread resumes
  L. Resume normal frame production

PlatformThread: untouched. Continues pumping window/input events into the ring;
                events buffer during the swap and drain when GameThread resumes.
                Held-key state lives in GameState, so keys are not lost.
```

### Threading invariants

- GameThread remains the single writer to the ECS — during the swap it is paused, not racing.
- RenderThread is the sole actor that touches GPU resources during the swap.
- PlatformThread never blocks; the Windows event pump stays alive, so the window does not enter a "not responding" state.

## Components

### `ApplicationContext.h`

```cpp
enum class RendererCommandType {
    ToggleVSync, Resize, RequestMesh, RequestMaterial,
    SwapBackend  // NEW
};

struct RendererCommand {
    RendererCommandType Type;
    // ... existing fields ...
    struct { RendererAPI TargetApi; } SwapBackend;  // NEW
};

// In ApplicationContext, alongside ShutdownRequested:
std::atomic<bool> SwapInProgress{false};    // set by RenderThread, observed by GameThread
std::atomic<bool> GameThreadPaused{false};  // ack from GameThread
```

### `MeshSystem.{h,cpp}`

Adds a CPU-side cache and a teardown/recreate surface.

```cpp
struct MeshEntry {
    // ... existing GPU fields (vertexBuffer, indexBuffer, counts, bounds) ...
    std::vector<MeshVertex> CpuVertices;   // NEW — retained for swap replay
    std::vector<uint32_t>   CpuIndices;    // NEW
    std::vector<SubMesh>    CpuSubMeshes;  // NEW
};

void DestroyGpuResources();   // nulls vertex/index BufferHandles; keeps m_Meshes + CPU caches
void RecreateGpuResources();  // replays m_Meshes in order; re-creates buffers from CPU caches
```

`AddMesh` retains CPU copies. `RecreateGpuResources` preserves slot indices so `MeshId` stays stable. A per-mesh recreation failure leaves that slot with a null buffer (`GetMeshResources` returns `valid=false`); the mesh render pass already skips invalid meshes.

### `MaterialSystem.{h,cpp}`

```cpp
struct MaterialEntry {
    // ... existing GPU fields (texture, sampler) ...
    std::vector<uint32_t> CpuTexturePixels;  // NEW — RGBA8 retained
    uint32_t              Width = 0;          // NEW
    uint32_t              Height = 0;         // NEW
};

void DestroyGpuResources();
void RecreateGpuResources(const nvrhi::TextureHandle& missingMaterial,
                          const nvrhi::SamplerHandle& defaultSampler);
```

A per-material recreation failure leaves a null texture; the render pass falls back to `MissingMaterial`.

### `Renderer.{h,cpp}`

Owns the swap orchestration.

```cpp
bool SwapBackend(RendererAPI newApi);  // runs the full state machine; returns false on fatal failure
RendererAPI CurrentApi() const;

// private:
void TeardownForSwap();        // ImGui + passes + MaterialSystem + MeshSystem GPU + swapchain + device
bool InitForSwap(RendererAPI); // new backend → device → swapchain → recreate everything
```

`ImGui_NVRHI` is owned by `ImGuiRenderer`, so `Renderer::SwapBackend` calls into `ImGuiRenderer` for the ImGui portion (below) rather than reaching into its internals.

### `ImGuiRenderer.{h,cpp}`

```cpp
void ShutdownNvrhiOnly();                       // tears down m_ImGuiNvrhi + font texture; keeps ImGui context
bool InitNvrhiForDevice(nvrhi::IDevice* device); // re-creates m_ImGuiNvrhi against the new device
```

ImGui's CPU context (dock layout, window positions, fonts loaded from disk) survives the swap. Only the NVRHI backend (GPU resources) is destroyed and recreated.

The Phase A `Settings → Apply` path changes from "save + raise restart banner" to "save + push `SwapBackend` command":

```cpp
m_AppContext->Settings.Backend = m_PendingBackend;
if (SettingsManager::Save(SettingsManager::DEFAULT_SETTINGS_PATH, m_AppContext->Settings)) {
    RendererCommand swapCmd{};
    swapCmd.Type = RendererCommandType::SwapBackend;
    swapCmd.SwapBackend.TargetApi = m_PendingBackend;
    if (!m_AppContext->PRCommandRing.Push(swapCmd)) {
        m_SettingsSaveError = "Renderer busy; could not start swap. Restart to apply.";
    }
} else {
    // revert + error text (unchanged from Phase A)
}
```

The Phase A restart banner (`m_RestartRequired` member + banner block in `Render()`) is removed — the swap replaces it. If a future need arises for a restart-only path, it can be reintroduced; it is not needed for backend swap.

### `RenderThread.{h,cpp}`

The PR command drain gains a `SwapBackend` case that calls `m_Renderer->SwapBackend(cmd.SwapBackend.TargetApi)` and, on a false return, shows the MessageBox and exits.

### `GameThread.{h,cpp}`

At the top of each tick:

```cpp
if (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
    m_AppContext->GameThreadPaused.store(true, std::memory_order_release);
    while (m_AppContext->SwapInProgress.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    m_AppContext->GameThreadPaused.store(false, std::memory_order_release);
}
```

## Error handling

| Failure point | Behavior |
|---|---|
| `SwapBackend` called with the current API | No-op, log INFO, return true. |
| `device->waitForIdle()` errors | Fatal. MessageBox "GPU did not drain cleanly; forcing exit". `ExitProcess(-1)`. |
| New `RendererBackend::Init` / `CreateDevice` / `CreateSwapChain` fails | Fatal. MessageBox "Failed to initialize <api>. Editor cannot continue." `ExitProcess(-1)`. No rollback. |
| `MeshSystem::RecreateGpuResources` fails on one mesh | Log error; leave that slot null (`valid=false`); render pass skips it. Continue. |
| `MaterialSystem::RecreateGpuResources` fails on one material | Log error; leave null texture; render pass falls back to `MissingMaterial`. Continue. |
| `ImGuiRenderer::InitNvrhiForDevice` fails | Fatal. MessageBox "ImGui backend re-init failed." `ExitProcess(-1)`. |
| GameThread does not ack pause within 5 s | Fatal. MessageBox "GameThread did not pause; possible deadlock." `ExitProcess(-1)`. |
| PR ring full when Apply pushes the command | Inline red error near the combo; no swap. Settings file already saved → restart applies. |
| Swap requested while one is in progress | RenderThread drops the duplicate command with a WARN. |

### Invariants

- `SwapInProgress == true` ⇒ RenderThread is the only thread touching GPU resources.
- `m_Meshes` / `m_Materials` vector sizes never shrink across a swap ⇒ handle stability holds.
- `ApplicationSettings.Backend` reflects the swap target (set before the command is pushed). A fatal swap failure therefore leaves the file pointing at the new backend, which a restart applies cleanly.
- The ECS world is untouched throughout (GameThread paused; RenderThread never writes ECS).

## Testing

No automated tests. The swap touches GPU device lifetime, ImGui, and threading; failures are environmental (drivers) or visual (rendering correctness), not unit-testable. `test_ecs.exe` is unaffected.

Manual smoke matrix:

| Case | Expected |
|---|---|
| DX12 → Vulkan, empty scene | ~100-500 ms freeze, renders under Vulkan, window responsive after. |
| Vulkan → DX12, empty scene | Symmetric. |
| Swap with meshes loaded | Meshes still visible, same positions; inspector `MeshId` unchanged. |
| Swap with textures loaded | Textures correct, not garbled / missing. |
| Swap with rotating-text + lights scene | Text still rotates; day/night cycle continues. |
| Swap mid-camera-move (hold W during Apply) | Camera continues forward after swap (GameState input survived). |
| Swap → save world → restart | World loads under persisted backend, no duplicates (Phase A path). |
| Rapid double-Apply | Second swap dropped with WARN; no crash. |
| Swap to a backend with no driver | MessageBox + clean exit; restart shows the same guard until JSON/CLI fixes it. |
| ImGui dock layout after swap | Docked panels retain positions (ImGui CPU context survived). |
| Memory after several swaps | No unbounded growth; eyeball Task Manager. |

Add `SM_TRACE` timing around the swap (total + per-phase ms) to confirm the latency budget and catch regressions.

## Out of scope (Phase B v1)

- Rollback on failure (kill-on-fail chosen).
- Async / non-blocking swap (blocking chosen).
- DirectX 11 backend (still unimplemented).
- Re-load-from-disk asset path (CPU cache used instead).
- Preserving GPU-side TAA / render-target history across the swap (scene re-renders fresh).
- Swapping `ecs.dll` / `Game.dll` (covered by the hot-reload feature, orthogonal).
