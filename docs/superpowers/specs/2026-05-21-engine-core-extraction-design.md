# Engine Core Extraction (Editor/Runtime Separation — Part A) — Design

**Date:** 2026-05-21
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (allocator-toolkit merged).

## Goal

Extract the editor's shared **runtime core** — the 3-thread model, the NVRHI `Renderer`
and gameplay render passes, the GPU resource systems, the `Game.dll` hot-reload, and the
.NET plugin host — out of the `editor` executable into the existing **`Engine` shared lib**,
and sever the one remaining ImGui dependency in that core (the `Renderer` → `ImGuiRenderer`
coupling) behind an injected `IOverlay` interface. After this part:

- **`Engine.dll`** holds the entire ImGui-free runtime core.
- **`editor.exe`** = `Engine` core + the ImGui tooling layer (the `rendering/imgui/` directory),
  attached by injecting an `ImGuiOverlay` into the renderer.
- The editor builds and runs identically at every step (no behavioral change, no feature loss).

This is **Part A** of the editor/runtime separation. **Part B** (a stripped `runtime.exe` that
links `Engine` with no overlay, plus deletion of the legacy single-threaded `runtime`/
`imgui_overlay`/`src/overlay`/`*_old.h` code) is a separate spec, built after A lands. A
delivers no new exe by itself — its deliverable is "the core lives in Engine, editor still
works." B becomes small once A is done.

## Background — the current shape (verified)

`Engine` is already a `SHARED` lib that `editor` links (it holds the allocator toolkit:
`MemoryCategory.cpp`, `AllocatorRegistry.cpp`; links only `CommonHeaders`). The editor's
`~28` `.cpp` files are ~79% ImGui-free core. ImGui-free and movable as-is: all three threads
(`PlatformThread`/`GameThread`/`RenderThread`), `core/Application`, `Renderer`'s backends,
all gameplay passes (`PrimitiveRenderPass`, `MeshRenderPass`, `UiRenderPass` — the last uses
freetype, not ImGui), `MeshSystem`/`MaterialSystem`/`ShaderCompiler`, the .NET plugin host,
`GameLibrary`, and the utilities. `ApplicationContext.h` (in `common`) is ImGui-free (it only
*names* a ring `ImGuiInputRing`; the type is `SpscRing<InputEvent>`). `Application` boot and
`PlatformThread` input are already ImGui-free (input is hand-rolled via rings, not
`imgui_impl_glfw`).

**The single coupling:** `Renderer` hard-owns ImGui.
- `Renderer.h:12` `#include "imgui/ImGuiRenderer.h"`, `Renderer.h:120`
  `std::unique_ptr<ImGuiRenderer> m_ImGuiRenderer;`
- Call sites in `Renderer.cpp`: construct+init in `Init` (`:77-78`), reset in `Shutdown`
  (`:115-117`), per-frame `m_ImGuiRenderer->Render(framebuffer, deltaTime, snapshot, world, secs)`
  (`:202`), and the two hot-swap hooks `ShutdownNvrhiOnly`/`InitNvrhiForDevice` (`:331`, `:399`).

`ImGuiRenderer`'s public surface maps 1:1 to an overlay interface:
`Init(nvrhi::IDevice*, ApplicationContext*, MeshSystem*, MaterialSystem*, Renderer*)`,
`Render(nvrhi::IFramebuffer*, double, SimulationSnapshot&, const ECS*, float)`, `Shutdown()`,
`ShutdownNvrhiOnly()`, `InitNvrhiForDevice(nvrhi::IDevice*)`.

## Scope

**In scope (Part A):**
- A new `IOverlay` interface in `Engine` (ImGui-free) + an overlay-factory injection path
  (editor `main` → `Application` → `RenderThread` → `Renderer`).
- Rewrite `Renderer` to own an optional `std::unique_ptr<IOverlay>` instead of `ImGuiRenderer`.
- A new editor-side `ImGuiOverlay` implementing `IOverlay` by wrapping the existing
  `ImGuiRenderer` (unchanged internally).
- Move the ImGui-free core `.cpp/.h` from `src/editor/src/**` into `src/engine/**`; update
  CMake (Engine gains the heavy link deps, PUBLIC includes, GLM/NOMINMAX defines, and the
  nethost/dxc/asset POST_BUILD copies); the editor relinks against `Engine` and keeps only
  the ImGui layer + its `main` + `ImGuiOverlay`.
- Decide + apply the DLL export strategy (`ENGINE_API`) for the Engine classes the editor's
  ImGui layer calls across the boundary.

**Out of scope (→ Part B):** the stripped `runtime.exe`; deleting the legacy
`runtime`/`imgui_overlay`/`src/overlay`/`*_old.h`/`build.bat`; renaming `ImGuiInputRing`.

**Explicitly unchanged:** `ecs.dll`, `Game.dll`, `GAME_API_VERSION`, the cross-thread
contracts in `ApplicationContext.h`, all gameplay/render behavior. This is a *move + one
interface seam*, not a rewrite.

## Design

### The `IOverlay` seam

New `src/engine/include/IOverlay.h` (ImGui-free; mirrors the `ImGuiRenderer` surface):

```cpp
#pragma once
#include <nvrhi/nvrhi.h>

struct ApplicationContext;
struct SimulationSnapshot;
class ECS;
class MeshSystem;
class MaterialSystem;
class Renderer;

// An optional, renderer-owned overlay drawn after the gameplay passes and before present.
// The editor supplies an ImGui-backed implementation; a stripped runtime supplies none.
class IOverlay {
public:
    virtual ~IOverlay() = default;
    virtual bool Init(nvrhi::IDevice* device, ApplicationContext* appCtx,
                      MeshSystem* meshSystem, MaterialSystem* materialSystem,
                      Renderer* renderer) = 0;
    virtual void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime,
                        SimulationSnapshot& snapshot, const ECS* world,
                        float gpuFrameTimeMs) = 0;
    virtual void Shutdown() = 0;
    virtual void OnDeviceLost() = 0;                       // hot-swap: drop device-bound state
    virtual bool OnDeviceReset(nvrhi::IDevice* device) = 0; // hot-swap: rebuild against new device
};
```

`Renderer` changes (now ImGui-free):
- Drop `#include "imgui/ImGuiRenderer.h"` and `m_ImGuiRenderer`; add `#include "IOverlay.h"`,
  a member `std::unique_ptr<IOverlay> m_Overlay;`, and `void SetOverlay(std::unique_ptr<IOverlay>);`.
- `Init`: after the resource systems are up and the device exists, `if (m_Overlay)
  m_Overlay->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem, this);`.
- `Render` (`:202`): `if (m_Overlay) m_Overlay->Render(framebuffer, deltaTime, snapshot, world, gpuMs);`.
- `Shutdown` (`:115-117`): `if (m_Overlay) m_Overlay->Shutdown();` (overlay reset before device teardown).
- Hot-swap (`:331`, `:399`): `if (m_Overlay) m_Overlay->OnDeviceLost();` /
  `if (m_Overlay && !m_Overlay->OnDeviceReset(newDevice)) return false;`.

### Injection path (factory)

`Engine` defines `using OverlayFactory = std::function<std::unique_ptr<IOverlay>()>;` (in
`IOverlay.h` or a small `OverlayFactory.h`). `Application` and `RenderThread` constructors gain
a trailing `OverlayFactory overlayFactory = {}` parameter. When `RenderThread` constructs the
`Renderer`, if the factory is set it calls `renderer.SetOverlay(overlayFactory())` **before**
`Renderer::Init` (so `Init` wires the overlay). The factory must run on the RenderThread (ImGui
context is created on that thread) — `RenderThread` already owns `Renderer` and runs the render
loop, so invoking the factory inside its setup satisfies this.

- **editor `main`**: passes `[]{ return std::make_unique<ImGuiOverlay>(); }`.
- **runtime `main` (Part B)**: passes nothing → `m_Overlay == nullptr` → every overlay call is
  skipped.

### `ImGuiOverlay` (editor, new — `src/editor/src/rendering/imgui/ImGuiOverlay.{h,cpp}`)

Thin adapter implementing `IOverlay` by owning + forwarding to an `ImGuiRenderer` (which is
unchanged):
```cpp
class ImGuiOverlay final : public IOverlay {
public:
    bool Init(nvrhi::IDevice* d, ApplicationContext* a, MeshSystem* m, MaterialSystem* mat, Renderer* r) override
        { return m_Impl.Init(d, a, m, mat, r); }
    void Render(nvrhi::IFramebuffer* fb, double dt, SimulationSnapshot& s, const ECS* w, float ms) override
        { m_Impl.Render(fb, dt, s, w, ms); }
    void Shutdown() override { m_Impl.Shutdown(); }
    void OnDeviceLost() override { m_Impl.ShutdownNvrhiOnly(); }
    bool OnDeviceReset(nvrhi::IDevice* d) override { return m_Impl.InitNvrhiForDevice(d); }
private:
    ImGuiRenderer m_Impl;
};
```

### What moves `editor → engine`

CORE → `src/engine/` (new subdirs mirroring the editor layout, e.g. `engine/src/threading`,
`engine/src/rendering`, `engine/src/rendering/passes`, `engine/src/rendering/backends`,
`engine/src/rendering/shader`, `engine/src/plugins`, `engine/src/utilities`, `engine/src/core`;
public headers under `engine/include` or kept beside sources with the dirs added to PUBLIC
includes):
- `core/Application.{h,cpp}`
- `threading/PlatformThread.{h,cpp}`, `GameThread.{h,cpp}`, `RenderThread.{h,cpp}`, `GameLibrary.{h,cpp}`
- `rendering/Renderer.{h,cpp}`, `RendererBackend.{h,cpp}`, `backends/RendererBackendDX12.{h,cpp}`,
  `backends/RendererBackendVulkan.{h,cpp}`, `MeshSystem.{h,cpp}`, `MaterialSystem.{h,cpp}`,
  `shader/ShaderCompiler.{h,cpp}`, `FrameAllocator.h`, `StagingBufferPool.h`, `IRenderPass.h`,
  `passes/PrimitiveRenderPass.{h,cpp}`, `passes/MeshRenderPass.{h,cpp}`, `passes/UiRenderPass.{h,cpp}`,
  `passes/MeshBatching.h`
- `plugins/DotNetPluginManager.{h,cpp}`, `DotNetPluginHost.{h,cpp}`
- `utilities/MeshLoader.{h,cpp}`, `MaterialLoader.{h,cpp}`, `WorldManager.{h,cpp}`, `SettingsManager.{h,cpp}`
- vendored: `tiny_obj_loader.{h,cpp}`, `stb_image.h`
- NEW: `IOverlay.h` (+ optional `OverlayFactory.h`)

STAY in `editor`:
- `rendering/imgui/` whole dir: `ImGuiRenderer.{h,cpp}`, `imgui_nvrhi.{h,cpp}`,
  `registered_font.{h,cpp}`, `MeshPreviewRenderer.{h,cpp}`, `MemoryPanel.{h,cpp}`
- NEW `rendering/imgui/ImGuiOverlay.{h,cpp}`
- `alloc.h` (editor-local global-new/delete tracker — must NOT compile into Engine)
- `main.cpp` (editor entry; supplies the ImGui overlay factory)

### CMake changes

`src/engine/CMakeLists.txt`:
- Add all moved `.cpp` to the `Engine` source list.
- `target_link_libraries(Engine PUBLIC ...)` gains: `ecs`, `GameHeaders`, `glm::glm`, `glfw`,
  `freetype`, `nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk`, `d3dcompiler dxgi dxcompiler`,
  `assimp::assimp`, `nlohmann_json::nlohmann_json`, `Tracy::TracyClient`, the Vulkan headers,
  and `nethost.lib` (+ `third_party/dotnet/include`). (`imgui`/`ImGuizmo` are **NOT** added —
  they stay editor-only.) Use PUBLIC where the editor must transitively see them (nvrhi, glm,
  ApplicationContext deps); PRIVATE otherwise.
- `target_include_directories(Engine PUBLIC ...)` gains the moved source dirs (the editor used
  flat unqualified includes like `#include "MeshSystem.h"`, so the dirs must be on the PUBLIC
  path).
- `target_compile_definitions(Engine PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL GLFW_INCLUDE_VULKAN)` and keep `PRIVATE
  ENGINE_EXPORTS NOMINMAX WIN32_LEAN_AND_MEAN` (NOMINMAX/lean also PUBLIC if headers need them).
- Move the POST_BUILD copies that the core needs (nethost.dll/hostfxr.dll, dxcompiler.dll, the
  `assets/` copy) so they land next to whichever exe ships — attach to `Engine` or replicate
  on each exe. Keep `add_dependencies(<exe> game)` on the exes (build-order for the
  hot-reloadable `Game.dll`), not on Engine; Engine must **not** link the `game` target (it
  loads `Game.dll` dynamically via `GameLibrary`).

`src/editor/CMakeLists.txt`:
- Source list shrinks to: `main.cpp`, the `rendering/imgui/` files, `ImGuiOverlay.{cpp}`,
  `MemoryPanel.cpp`, `alloc`-related TUs.
- `target_link_libraries(editor PRIVATE Engine ecs imgui ImGuizmo ...)` — keeps `imgui` +
  `ImGuizmo` (+ whatever the ImGui layer needs directly, e.g. nvrhi for `imgui_nvrhi`); drops
  everything now transitively provided by `Engine`.
- Keep the `ALLOC_TRACKER_ENABLED` knob editor-scoped.

### DLL export strategy (`ENGINE_API`)

The editor's ImGui layer calls into Engine classes across the DLL boundary, so those must be
exported. Mark with `ENGINE_API` the classes/methods the editor TUs reference: at minimum
`Application`, `Renderer`, `MeshSystem`, `MaterialSystem`, and the `IOverlay` interface (its
vtable is consumed by `ImGuiOverlay`). `ImGuiRenderer::Init` already takes `Renderer*`,
`MeshSystem*`, `MaterialSystem*`, so those three are definitely on the boundary;
`MeshPreviewRenderer` (editor) uses the device/shader path too. The exact set is determined in
A2 by what the editor TUs link-reference — the rule: **export an Engine class iff an editor TU
names it.** Classes used only within Engine (the three threads, the passes, backends,
`ShaderCompiler`, plugin host, `GameLibrary`, `WorldManager`, loaders) need no export. The root
already locks `MultiThreaded[Debug]DLL`, so STL types cross the boundary safely.

## Phasing (editor builds + runs at every commit)

- **A1 — cut the seam in place.** Add `IOverlay.h` (temporarily in the editor tree),
  `ImGuiOverlay.{h,cpp}`, the `SetOverlay` + optional-overlay member on `Renderer`, the factory
  param threaded `main → Application → RenderThread → Renderer`. `Renderer` no longer names
  `ImGuiRenderer` directly. **Nothing moves between targets yet**; editor builds + runs +
  passes a smoke (panels + gizmos + backend hot-swap all work). This isolates the riskiest
  change (the seam, incl. the hot-swap device hooks) with the smallest diff.
- **A2 — move the core to Engine.** Relocate the CORE files `editor → engine` in dependency
  order; move `IOverlay.h` into `engine/include`; update both CMakeLists (Engine gains
  deps/includes/defines/POST_BUILD; editor shrinks + links Engine); apply `ENGINE_API` to the
  editor-referenced classes. Build editor after each cohesive group. End state: editor = ImGui
  layer + `main` + `ImGuiOverlay`.
- **A3 — verify.** Full clean build (`ecs`, `Engine`, `game`, `editor`, `test_ecs`,
  `test_alloc`); editor smoke (load `assets/models`, ImGui panels/inspector/gizmos, **backend
  hot-swap** DX12↔Vulkan, model load, Memory panel). Confirm `Engine.dll` exports no ImGui
  symbols (e.g. `dumpbin /exports` shows no `ImGui*`).

## Build / ABI

No `GAME_API_VERSION` bump (no `GameState`/game-export/component-type change). `ecs.dll`/
`Game.dll` unchanged. `ApplicationContext`/`SimulationSnapshot`/`RendererCommand` now physically
cross the `Engine.dll` boundary via shared `common` headers — they are already POD/trivially
copyable; the constraint "editor + Engine recompiled together" already holds (one repo, one
build). `Engine::Registry()` cross-DLL singleton becomes intra-Engine for the moved Renderer
(register/unregister of `m_FrameAllocator` stays paired). Build preset
`msvc-win64-vs2026-community`. `test_ecs` + `test_alloc` must stay green (`test_alloc` already
includes from `src/editor/src/rendering/passes` + `src/editor/src/threading` — update those
include paths to the new `engine` locations).

## Testing

This is a structural move + one interface seam — there is **no new unit-testable logic**;
verification is build + behavioral parity:
- `ecs` + `Engine` + `game` + `editor` + `test_ecs` + `test_alloc` all build clean on the preset.
- `test_ecs` → `All ECS tests passed.`; `test_alloc` → `All allocator tests passed.` (after the
  include-path update for the moved headers).
- Editor smoke (manual; the GUI can't be unit-tested): launch, load `assets/models`, scene
  renders; ImGui panels/inspector/gizmos function; **backend hot-swap** works (the riskiest —
  validates the overlay `OnDeviceLost`/`OnDeviceReset` hooks); Memory panel still reads the
  pools. State explicitly that the smoke is the user's to run if the GUI can't be driven in the
  build environment.
- `dumpbin /exports Engine.dll | findstr ImGui` → no matches (proves ImGui left the core).

## Risks

- **Backend hot-swap overlay hooks (highest).** The DX12↔Vulkan swap rebuilds the renderer +
  the ImGui NVRHI backend; the overlay `OnDeviceLost`/`OnDeviceReset` must fire symmetrically
  where `ShutdownNvrhiOnly`/`InitNvrhiForDevice` did, or the editor crashes on swap. A1 keeps
  this in-place + smoke-tested before any files move, so a regression is isolated to the seam
  diff, not tangled with the relocation.
- **Export surface churn.** Getting `ENGINE_API` wrong shows up as link errors (unresolved
  external) when building the editor — loud and local, fixed by exporting the named class.
- **Include-path churn.** The editor's flat unqualified includes mean the moved dirs must be on
  Engine's PUBLIC include path; a missed dir is a compile error, fixed by adding the dir.
- **`alloc.h` leak tracker** overrides global `new`/`delete`; it must stay an editor-exe TU and
  never compile into `Engine` (would impose the override on every Engine consumer). Keep it +
  `ALLOC_TRACKER_ENABLED` editor-scoped.
- **Engine must not link the `game` target** (only `GameHeaders` + dynamic `Game.dll` load) —
  avoids an `Engine → game → ecs` tangle; `game` keeps linking `ecs` only.
- **Fat Engine link/POST_BUILD.** Engine gains many deps + the DLL/asset copies; a missed copy
  surfaces as a runtime "DLL not found" at editor launch, fixed by relocating the POST_BUILD step.
