# Engine Core Extraction (Editor/Runtime Separation — Part A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the ImGui-free runtime core out of `editor` into the `Engine` shared lib and sever the `Renderer`→ImGui coupling behind an injected `IOverlay`, so the editor becomes `Engine core + ImGui overlay` and still builds/runs identically.

**Architecture:** Phase A1 introduces an `IOverlay` interface + an overlay factory threaded `main → Application → RenderThread → Renderer`, replacing the hard `ImGuiRenderer` member — *nothing moves between targets yet*. Phase A2 relocates the ~22 core files `editor → engine` via `git mv`, rewires both CMakeLists (Engine gains the heavy deps + PUBLIC includes + defines + POST_BUILD; editor slims to the ImGui layer), and exports the editor-referenced Engine classes with `ENGINE_API`. Phase A3 verifies a full clean build, the editor smoke (incl. backend hot-swap), and that `Engine.dll` exports no ImGui symbols.

**Tech Stack:** C++23, CMake (preset `msvc-win64-vs2026-community`), MSVC DLL boundary (`ENGINE_API` dllexport/dllimport), NVRHI, GLFW, ImGui/ImGuizmo (editor-only), nethost.

**Spec:** `docs/superpowers/specs/2026-05-21-engine-core-extraction-design.md`

**Branch:** `engine-core-extraction` (off `main`, already checked out). Do NOT merge or offer to merge.

**Build:** `cmake --build --preset msvc-win64-vs2026-community --target <t>`. Tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe` (`All ECS tests passed.`), `test_alloc.exe` (`All allocator tests passed.`).

**Global rules:** No `GAME_API_VERSION` bump. `Engine` must NOT link the `game` target (Game.dll loads dynamically via `GameLibrary`). `imgui`/`ImGuizmo` stay editor-only. `alloc.h` + `ALLOC_TRACKER_ENABLED` stay editor-scoped. This is a structural move + one seam — verification is build + behavioral parity, NOT new unit tests.

---

## Phase A1 — Cut the ImGui seam in place (nothing moves; editor keeps ImGui)

### Task 1: Add the `IOverlay` interface + `OverlayFactory`

**Files:**
- Create: `src/editor/src/rendering/IOverlay.h`

- [ ] **Step 1: Create the interface**

Create `src/editor/src/rendering/IOverlay.h`:

```cpp
#pragma once
#include <functional>
#include <memory>
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
    virtual void OnDeviceLost() = 0;                        // hot-swap: drop device-bound state
    virtual bool OnDeviceReset(nvrhi::IDevice* device) = 0; // hot-swap: rebuild against new device
};

// Factory the host supplies so the renderer can create its overlay on the RenderThread.
using OverlayFactory = std::function<std::unique_ptr<IOverlay>()>;
```

- [ ] **Step 2: Build to confirm it compiles (header-only, no consumers yet)**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: clean (the header is unused so far; this just confirms no syntax error).

- [ ] **Step 3: Commit**

```bash
git add src/editor/src/rendering/IOverlay.h
git commit -m "Add IOverlay interface + OverlayFactory for renderer overlay injection"
```

### Task 2: Add the `ImGuiOverlay` adapter (wraps the unchanged `ImGuiRenderer`)

**Files:**
- Create: `src/editor/src/rendering/imgui/ImGuiOverlay.h`, `src/editor/src/rendering/imgui/ImGuiOverlay.cpp`
- Modify: `src/editor/CMakeLists.txt` (add `ImGuiOverlay.cpp` to the source list)

- [ ] **Step 1: Create the adapter header**

Create `src/editor/src/rendering/imgui/ImGuiOverlay.h`:

```cpp
#pragma once
#include "IOverlay.h"
#include "imgui/ImGuiRenderer.h"

// Editor overlay: implements the engine's IOverlay by forwarding to ImGuiRenderer.
class ImGuiOverlay final : public IOverlay {
public:
    bool Init(nvrhi::IDevice* device, ApplicationContext* appCtx,
              MeshSystem* meshSystem, MaterialSystem* materialSystem,
              Renderer* renderer) override {
        return m_Impl.Init(device, appCtx, meshSystem, materialSystem, renderer);
    }
    void Render(nvrhi::IFramebuffer* framebuffer, double deltaTime,
                SimulationSnapshot& snapshot, const ECS* world,
                float gpuFrameTimeMs) override {
        m_Impl.Render(framebuffer, deltaTime, snapshot, world, gpuFrameTimeMs);
    }
    void Shutdown() override { m_Impl.Shutdown(); }
    void OnDeviceLost() override { m_Impl.ShutdownNvrhiOnly(); }
    bool OnDeviceReset(nvrhi::IDevice* device) override { return m_Impl.InitNvrhiForDevice(device); }
private:
    ImGuiRenderer m_Impl;
};
```

- [ ] **Step 2: Create the adapter .cpp (anchor TU so it's in the build)**

Create `src/editor/src/rendering/imgui/ImGuiOverlay.cpp`:

```cpp
#include "imgui/ImGuiOverlay.h"
// All behavior is header-inline forwarding to ImGuiRenderer; this TU anchors the vtable.
```

- [ ] **Step 3: Add it to the editor source list**

In `src/editor/CMakeLists.txt`, add `src/rendering/imgui/ImGuiOverlay.cpp` to the `add_executable(editor ...)` source list (next to the other `src/rendering/imgui/*.cpp` entries).

- [ ] **Step 4: Build**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: clean (the adapter compiles but is not yet wired in).

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiOverlay.h src/editor/src/rendering/imgui/ImGuiOverlay.cpp src/editor/CMakeLists.txt
git commit -m "Add ImGuiOverlay adapter wrapping ImGuiRenderer"
```

### Task 3: Rewire `Renderer` to the optional overlay + thread the factory end-to-end

This task is atomic: after it, the editor drives ImGui through the injected overlay (no ImGui loss at any commit). It touches `Renderer`, `RenderThread`, `Application`, and `main`.

**Files:**
- Modify: `src/editor/src/rendering/Renderer.h`
- Modify: `src/editor/src/rendering/Renderer.cpp` (5 call sites)
- Modify: `src/editor/src/threading/RenderThread.h`, `RenderThread.cpp`
- Modify: `src/editor/src/core/Application.h`, `Application.cpp`
- Modify: `src/editor/src/main.cpp`

- [ ] **Step 1: `Renderer.h` — replace the ImGui member with an optional overlay**

In `src/editor/src/rendering/Renderer.h`:
- Remove `#include "imgui/ImGuiRenderer.h"` (line 12) and add `#include "IOverlay.h"` in its place.
- Replace the member `std::unique_ptr<ImGuiRenderer> m_ImGuiRenderer;` (line 120) with:
  ```cpp
  std::unique_ptr<IOverlay> m_Overlay;  // optional; editor injects ImGui, runtime injects none
  ```
- Add a public setter in the `public:` section (e.g. after `void RemoveRenderPass(...)`, line 99):
  ```cpp
  // Inject an optional overlay BEFORE Init(); the renderer Inits/Renders/tears it down.
  void SetOverlay(std::unique_ptr<IOverlay> overlay) { m_Overlay = std::move(overlay); }
  ```

- [ ] **Step 2: `Renderer.cpp` — update the 5 call sites**

In `src/editor/src/rendering/Renderer.cpp`:

(a) `Init` (lines 77-81) — replace:
```cpp
    m_ImGuiRenderer = std::make_unique<ImGuiRenderer>();
    if (!m_ImGuiRenderer->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem, this)) {
        SM_ERROR("Failed to initialize ImGuiRenderer");
        return false;
    }
```
with:
```cpp
    if (m_Overlay && !m_Overlay->Init(m_Device, m_AppContext, &m_MeshSystem, &m_MaterialSystem, this)) {
        SM_ERROR("Failed to initialize renderer overlay");
        return false;
    }
```

(b) `Shutdown` (lines 115-118) — replace:
```cpp
    if (m_ImGuiRenderer) {
        m_ImGuiRenderer.reset();
        m_ImGuiRenderer = nullptr;
    }
```
with:
```cpp
    if (m_Overlay) {
        m_Overlay->Shutdown();
        m_Overlay.reset();
    }
```

(c) `Render` (line 202) — replace:
```cpp
            m_ImGuiRenderer->Render(frameBuffer, deltaTime, snapshot, world, secs);
```
with:
```cpp
            if (m_Overlay) m_Overlay->Render(frameBuffer, deltaTime, snapshot, world, secs);
```

(d) `TeardownForSwap` (lines 330-332) — replace:
```cpp
    if (m_ImGuiRenderer) {
        m_ImGuiRenderer->ShutdownNvrhiOnly();
    }
```
with:
```cpp
    if (m_Overlay) {
        m_Overlay->OnDeviceLost();
    }
```

(e) `InitForSwap` (lines 399-402) — replace:
```cpp
    if (!m_ImGuiRenderer || !m_ImGuiRenderer->InitNvrhiForDevice(m_Device)) {
        SM_ERROR("InitForSwap: ImGui re-init failed");
        return false;
    }
```
with:
```cpp
    if (m_Overlay && !m_Overlay->OnDeviceReset(m_Device)) {
        SM_ERROR("InitForSwap: overlay device reset failed");
        return false;
    }
```

- [ ] **Step 3: `RenderThread` — accept + apply the factory**

In `src/editor/src/threading/RenderThread.h`:
- Add `#include "IOverlay.h"` near the top (after `#include "Renderer.h"`).
- Change the constructor declaration (line 11) to:
  ```cpp
  explicit RenderThread(const std::shared_ptr<ApplicationContext> &appContext, GLFWwindow* window, RendererAPI api, OverlayFactory overlayFactory = {});
  ```
- Add a member (after `RendererAPI m_API = ...`, line 27):
  ```cpp
  OverlayFactory m_OverlayFactory;
  ```

In `src/editor/src/threading/RenderThread.cpp`:
- Update the constructor (lines 16-19) to store the factory:
  ```cpp
  RenderThread::RenderThread(const std::shared_ptr<ApplicationContext> &appContext, GLFWwindow* window, RendererAPI api, OverlayFactory overlayFactory)
      : m_AppContext(appContext), m_Window(window), m_Running(true), m_API(api), m_OverlayFactory(std::move(overlayFactory))
  {
  }
  ```
- In `Initialize` (lines 218-226), set the overlay BEFORE `Init`:
  ```cpp
  bool RenderThread::Initialize()
  {
      m_Renderer = std::make_unique<Renderer>(m_Window, m_AppContext.get());
      if (m_OverlayFactory) {
          m_Renderer->SetOverlay(m_OverlayFactory());
      }
      if (!m_Renderer->Init(m_API)) {
          SM_ERROR("RenderThread: Initialize failed");
          return false;
      }
      return true;
  }
  ```

- [ ] **Step 4: `Application` — pass the factory through**

In `src/editor/src/core/Application.h`:
- Change `Init` (line 15) to:
  ```cpp
  bool Init(std::optional<RendererAPI> backendOverride = std::nullopt, OverlayFactory overlayFactory = {});
  ```
  (`OverlayFactory` is visible via `RenderThread.h` → `IOverlay.h`, already included through `Application.h`'s includes.)

In `src/editor/src/core/Application.cpp`:
- Change the signature (line 6) to match:
  ```cpp
  bool Application::Init(std::optional<RendererAPI> backendOverride, OverlayFactory overlayFactory) {
  ```
- Pass the factory into the `RenderThread` construction (lines 34-37):
  ```cpp
      m_RenderThread = std::make_unique<RenderThread>(
          m_AppContext,
          m_PlatformThread->GetWindow(),
          m_AppContext->Settings.Backend,
          std::move(overlayFactory));
  ```

- [ ] **Step 5: `main.cpp` — supply the ImGui overlay factory**

In `src/editor/src/main.cpp`:
- Add the include (after line 18 `#include "Application.h"`):
  ```cpp
  #include "rendering/imgui/ImGuiOverlay.h"
  ```
- Change the `app.Init(cli.override_)` call (line 107) to:
  ```cpp
      if (!app.Init(cli.override_, []{ return std::make_unique<ImGuiOverlay>(); })) {
  ```

- [ ] **Step 6: Build the editor**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
Expected: clean compile/link. (`Renderer` no longer names `ImGuiRenderer`; ImGui reaches it only via the injected `ImGuiOverlay`.)

- [ ] **Step 7: Smoke (report honestly)**

Launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`. Confirm: ImGui panels/inspector render; **backend hot-swap DX12↔Vulkan works** (this validates `OnDeviceLost`/`OnDeviceReset`); model load + Memory panel work. If the GUI can't be driven in this environment, report build success and leave the smoke to the user — do NOT claim runtime success unobserved. The backend-hot-swap path is the highest-risk item; call it out explicitly.

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/rendering/Renderer.h src/editor/src/rendering/Renderer.cpp src/editor/src/threading/RenderThread.h src/editor/src/threading/RenderThread.cpp src/editor/src/core/Application.h src/editor/src/core/Application.cpp src/editor/src/main.cpp
git commit -m "Drive ImGui through injected IOverlay; Renderer is ImGui-free"
```

---

## Phase A2 — Move the core `editor → engine`

After A1, `Renderer` and the whole core are ImGui-free. Now relocate them into `Engine`. The move is done as one relocation + CMake rewrite + an `ENGINE_API` export pass; build errors are resolved by the defined procedure in Task 6.

### Task 4: Relocate core source files into the engine tree (`git mv`)

**Files:** moves only (no content edits except include fixes in Task 6).

- [ ] **Step 1: Create engine subdirs and move the files**

From the repo root, run (preserves history):
```bash
mkdir -p src/engine/src/core src/engine/src/threading src/engine/src/rendering/backends src/engine/src/rendering/passes src/engine/src/rendering/shader src/engine/src/plugins src/engine/src/utilities src/engine/include

git mv src/editor/src/rendering/IOverlay.h            src/engine/include/IOverlay.h
git mv src/editor/src/core/Application.h              src/engine/src/core/Application.h
git mv src/editor/src/core/Application.cpp            src/engine/src/core/Application.cpp
git mv src/editor/src/threading/PlatformThread.h     src/engine/src/threading/PlatformThread.h
git mv src/editor/src/threading/PlatformThread.cpp   src/engine/src/threading/PlatformThread.cpp
git mv src/editor/src/threading/GameThread.h         src/engine/src/threading/GameThread.h
git mv src/editor/src/threading/GameThread.cpp       src/engine/src/threading/GameThread.cpp
git mv src/editor/src/threading/RenderThread.h       src/engine/src/threading/RenderThread.h
git mv src/editor/src/threading/RenderThread.cpp     src/engine/src/threading/RenderThread.cpp
git mv src/editor/src/threading/GameLibrary.h        src/engine/src/threading/GameLibrary.h
git mv src/editor/src/threading/GameLibrary.cpp      src/engine/src/threading/GameLibrary.cpp
git mv src/editor/src/rendering/Renderer.h           src/engine/src/rendering/Renderer.h
git mv src/editor/src/rendering/Renderer.cpp         src/engine/src/rendering/Renderer.cpp
git mv src/editor/src/rendering/RendererBackend.h    src/engine/src/rendering/RendererBackend.h
git mv src/editor/src/rendering/RendererBackend.cpp  src/engine/src/rendering/RendererBackend.cpp
git mv src/editor/src/rendering/backends/RendererBackendDX12.h    src/engine/src/rendering/backends/RendererBackendDX12.h
git mv src/editor/src/rendering/backends/RendererBackendDX12.cpp  src/engine/src/rendering/backends/RendererBackendDX12.cpp
git mv src/editor/src/rendering/backends/RendererBackendVulkan.h    src/engine/src/rendering/backends/RendererBackendVulkan.h
git mv src/editor/src/rendering/backends/RendererBackendVulkan.cpp  src/engine/src/rendering/backends/RendererBackendVulkan.cpp
git mv src/editor/src/rendering/MeshSystem.h         src/engine/src/rendering/MeshSystem.h
git mv src/editor/src/rendering/MeshSystem.cpp       src/engine/src/rendering/MeshSystem.cpp
git mv src/editor/src/rendering/MaterialSystem.h     src/engine/src/rendering/MaterialSystem.h
git mv src/editor/src/rendering/MaterialSystem.cpp   src/engine/src/rendering/MaterialSystem.cpp
git mv src/editor/src/rendering/FrameAllocator.h     src/engine/src/rendering/FrameAllocator.h
git mv src/editor/src/rendering/StagingBufferPool.h  src/engine/src/rendering/StagingBufferPool.h
git mv src/editor/src/rendering/IRenderPass.h        src/engine/src/rendering/IRenderPass.h
git mv src/editor/src/rendering/shader/ShaderCompiler.h    src/engine/src/rendering/shader/ShaderCompiler.h
git mv src/editor/src/rendering/shader/ShaderCompiler.cpp  src/engine/src/rendering/shader/ShaderCompiler.cpp
git mv src/editor/src/rendering/passes/PrimitiveRenderPass.h    src/engine/src/rendering/passes/PrimitiveRenderPass.h
git mv src/editor/src/rendering/passes/PrimitiveRenderPass.cpp  src/engine/src/rendering/passes/PrimitiveRenderPass.cpp
git mv src/editor/src/rendering/passes/MeshRenderPass.h    src/engine/src/rendering/passes/MeshRenderPass.h
git mv src/editor/src/rendering/passes/MeshRenderPass.cpp  src/engine/src/rendering/passes/MeshRenderPass.cpp
git mv src/editor/src/rendering/passes/UiRenderPass.h    src/engine/src/rendering/passes/UiRenderPass.h
git mv src/editor/src/rendering/passes/UiRenderPass.cpp  src/engine/src/rendering/passes/UiRenderPass.cpp
git mv src/editor/src/rendering/passes/MeshBatching.h   src/engine/src/rendering/passes/MeshBatching.h
git mv src/editor/src/plugins/DotNetPluginManager.h    src/engine/src/plugins/DotNetPluginManager.h
git mv src/editor/src/plugins/DotNetPluginManager.cpp  src/engine/src/plugins/DotNetPluginManager.cpp
git mv src/editor/src/plugins/DotNetPluginHost.h    src/engine/src/plugins/DotNetPluginHost.h
git mv src/editor/src/plugins/DotNetPluginHost.cpp  src/engine/src/plugins/DotNetPluginHost.cpp
git mv src/editor/src/utilities/MeshLoader.h    src/engine/src/utilities/MeshLoader.h
git mv src/editor/src/utilities/MeshLoader.cpp  src/engine/src/utilities/MeshLoader.cpp
git mv src/editor/src/utilities/MaterialLoader.h    src/engine/src/utilities/MaterialLoader.h
git mv src/editor/src/utilities/MaterialLoader.cpp  src/engine/src/utilities/MaterialLoader.cpp
git mv src/editor/src/utilities/WorldManager.h    src/engine/src/utilities/WorldManager.h
git mv src/editor/src/utilities/WorldManager.cpp  src/engine/src/utilities/WorldManager.cpp
git mv src/editor/src/utilities/SettingsManager.h    src/engine/src/utilities/SettingsManager.h
git mv src/editor/src/utilities/SettingsManager.cpp  src/engine/src/utilities/SettingsManager.cpp
git mv src/editor/src/tiny_obj_loader.h    src/engine/src/tiny_obj_loader.h
git mv src/editor/src/tiny_obj_loader.cpp  src/engine/src/tiny_obj_loader.cpp
git mv src/editor/src/stb_image.h          src/engine/src/stb_image.h
```

Note: any other helper header still referenced by the core (e.g. `Timing.h`, `Systems.h` if they live under editor) must move too — find them with a build error in Task 6 and `git mv` into the matching engine dir. Do NOT move `rendering/imgui/*`, `alloc.h`, `main.cpp`, or `MemoryCategory.cpp`/`AllocatorRegistry.cpp` (already engine).

- [ ] **Step 2: Commit the raw move (build will be red until Task 5/6 — that's expected)**

```bash
git add -A
git commit -m "Relocate runtime core sources from editor into engine tree (git mv)"
```

### Task 5: Rewrite both CMakeLists for the new layout

**Files:**
- Modify: `src/engine/CMakeLists.txt`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Rewrite `src/engine/CMakeLists.txt`**

Replace its contents with (sources = the moved core + the existing allocator TUs; deps = the editor's old core deps minus imgui/ImGuizmo):

```cmake
add_library(Engine SHARED
    src/MemoryCategory.cpp
    src/AllocatorRegistry.cpp
    src/core/Application.cpp
    src/threading/PlatformThread.cpp
    src/threading/GameThread.cpp
    src/threading/RenderThread.cpp
    src/threading/GameLibrary.cpp
    src/rendering/Renderer.cpp
    src/rendering/RendererBackend.cpp
    src/rendering/backends/RendererBackendDX12.cpp
    src/rendering/backends/RendererBackendVulkan.cpp
    src/rendering/MeshSystem.cpp
    src/rendering/MaterialSystem.cpp
    src/rendering/shader/ShaderCompiler.cpp
    src/rendering/passes/PrimitiveRenderPass.cpp
    src/rendering/passes/MeshRenderPass.cpp
    src/rendering/passes/UiRenderPass.cpp
    src/plugins/DotNetPluginManager.cpp
    src/plugins/DotNetPluginHost.cpp
    src/utilities/MeshLoader.cpp
    src/utilities/MaterialLoader.cpp
    src/utilities/WorldManager.cpp
    src/utilities/SettingsManager.cpp
    src/tiny_obj_loader.cpp
)

target_include_directories(Engine PUBLIC
    include
    src
    src/core
    src/threading
    src/rendering
    src/plugins
    src/utilities
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/include
)

target_compile_definitions(Engine PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
    GLFW_INCLUDE_VULKAN
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_compile_definitions(Engine PRIVATE ENGINE_EXPORTS)

target_link_libraries(Engine PUBLIC
    CommonHeaders GameHeaders ecs glm::glm glfw freetype Tracy::TracyClient
    nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk d3dcompiler dxgi dxcompiler
    assimp::assimp nlohmann_json::nlohmann_json
    ${CMAKE_SOURCE_DIR}/third_party/dotnet/lib/nethost.lib
)
if (TARGET Vulkan-Headers)
    target_link_libraries(Engine PUBLIC Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(Engine PUBLIC Vulkan::Headers)
endif()

set_target_properties(Engine PROPERTIES
    OUTPUT_NAME Engine
    DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Libraries
)

if(MSVC)
    set_property(TARGET Engine PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
                 $<$<CONFIG:Debug>:Embedded>)
    target_compile_options(Engine PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Build the hot-reloadable Game.dll before Engine consumers run (Engine loads it dynamically).
add_dependencies(Engine game)

# Copy the .NET host + DXC runtime DLLs next to the output for whichever exe runs.
add_custom_command(TARGET Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/dotnet/bin/nethost.dll ${RUNTIME_DIR}/nethost.dll)
add_custom_command(TARGET Engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/third_party/dxc-prebuilt/bin/x64/dxcompiler.dll ${RUNTIME_DIR}/dxcompiler.dll)
```

Note: the DLL-copy source paths above mirror the editor's existing POST_BUILD copies — verify them against the OLD `src/editor/CMakeLists.txt` POST_BUILD block (it copied nethost/hostfxr/dxcompiler) and copy ALL of those same files (incl. `hostfxr.dll` and any others) here, adjusting paths to whatever the editor used. The `assets/` copy can stay on the editor (editor runs from `RUNTIME_DIR`); it will move to the runtime exe in Part B.

- [ ] **Step 2: Rewrite `src/editor/CMakeLists.txt`**

Slim the editor to the ImGui layer + `main`, linking `Engine`. Replace the `add_executable` source list with only the editor-only TUs and adjust links/includes:

```cmake
add_executable(editor
    src/main.cpp
    src/rendering/imgui/ImGuiRenderer.cpp
    src/rendering/imgui/imgui_nvrhi.cpp
    src/rendering/imgui/registered_font.cpp
    src/rendering/imgui/MeshPreviewRenderer.cpp
    src/rendering/imgui/MemoryPanel.cpp
    src/rendering/imgui/ImGuiOverlay.cpp
)

target_include_directories(editor PRIVATE
    src
    src/rendering
)

target_compile_definitions(editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RIGHT_HANDED GLM_ENABLE_EXPERIMENTAL GLFW_INCLUDE_VULKAN)

# Allocation tracker knob stays editor-scoped (unchanged from before).
set(ENABLE_ALLOC_TRACKER "AUTO" CACHE STRING "Enable allocation tracker (ON/OFF/AUTO)")
set_property(CACHE ENABLE_ALLOC_TRACKER PROPERTY STRINGS "AUTO" "ON" "OFF")
if(ENABLE_ALLOC_TRACKER STREQUAL "ON")
    target_compile_definitions(editor PRIVATE ALLOC_TRACKER_ENABLED)
endif()

target_link_libraries(editor PRIVATE
    Engine ecs CommonHeaders GameHeaders glm::glm glfw
    nvrhi nvrhi_d3d12 nvrhi_d3d11 nvrhi_vk d3dcompiler dxgi dxcompiler
    imgui ImGuizmo freetype Tracy::TracyClient)
if (TARGET Vulkan-Headers)
    target_link_libraries(editor PRIVATE Vulkan-Headers)
elseif (TARGET Vulkan::Headers)
    target_link_libraries(editor PRIVATE Vulkan::Headers)
endif()

set_target_properties(editor PROPERTIES
    OUTPUT_NAME editor
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}")

if(MSVC)
    target_compile_options(editor PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Editor still triggers the Game.dll build + ships assets.
add_dependencies(editor game)
add_custom_command(TARGET editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets)
```

Note: the editor still links `nvrhi`/`glfw`/`dxc`/`freetype` directly because its ImGui layer (`imgui_nvrhi`, `MeshPreviewRenderer`, `registered_font`) uses them. These are also PUBLIC on Engine, so the duplication is harmless; keep them explicit for clarity. Preserve any editor specifics from the OLD CMakeLists not shown here (e.g. exact warning flags) by diffing against git.

- [ ] **Step 3: Commit the CMake rewrite**

```bash
git add src/engine/CMakeLists.txt src/editor/CMakeLists.txt
git commit -m "Rewrite engine/editor CMake for core relocation (engine fat lib; editor = ImGui layer)"
```

### Task 6: Build green — apply `ENGINE_API` exports + fix include/move fallout

This is the iterative DLL-boundary completion. Work the loop until the editor links.

- [ ] **Step 1: Build Engine first**

Run: `cmake --build --preset msvc-win64-vs2026-community --target Engine`
Expected eventually: `Engine.dll` builds. Likely first failures + fixes:
- **Missing header (a core file includes something still under `editor/` or a moved sibling by an old relative path).** Fix by `git mv`-ing the missing header into the matching `src/engine/` dir and/or correcting the include. Re-run.
- The `Renderer`/systems compile against `ApplicationContext.h` (common) + nvrhi — already on Engine's deps.

- [ ] **Step 2: Mark editor-referenced Engine classes with `ENGINE_API`**

`ENGINE_API` is defined in `src/engine/include/Engine.h` (existing). Add `#include "Engine.h"` (or the header that defines `ENGINE_API`) and the macro to the class declarations the editor's ImGui TUs name across the DLL boundary — start with this predicted set and extend per the linker (Step 3):
- `class ENGINE_API Renderer` (Renderer.h)
- `class ENGINE_API MeshSystem` (MeshSystem.h)
- `class ENGINE_API MaterialSystem` (MaterialSystem.h)
- `class ENGINE_API Application` (Application.h)
- `IOverlay` is abstract (vtable only) — no `ENGINE_API` needed (the editor implements it; no Engine-side symbols to import). If the linker disagrees, add it.

Example (Renderer.h): `class ENGINE_API Renderer final {`.

- [ ] **Step 3: Build the editor; resolve unresolved externals by exporting the named class**

Run: `cmake --build --preset msvc-win64-vs2026-community --target editor`
For each `LNK2019 unresolved external symbol ... <Class>::<method>` referencing an Engine class, add `ENGINE_API` to that class declaration in its Engine header and rebuild Engine + editor. Repeat until the editor links. (This is the defined completion procedure for the move — the export set is exactly "Engine classes named by an editor TU.")

Expected end state: `editor.exe` links clean.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "Export editor-referenced Engine classes (ENGINE_API); fix relocation include fallout"
```

### Task 7: Fix `test_alloc` include paths for moved headers

**Files:**
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Update the include dirs**

In `tests/CMakeLists.txt`, the `test_alloc` `target_include_directories` references the OLD editor paths:
```cmake
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering/passes
    ${CMAKE_SOURCE_DIR}/src/editor/src/threading
```
Change them to the new engine locations:
```cmake
    ${CMAKE_SOURCE_DIR}/src/engine/src/rendering/passes
    ${CMAKE_SOURCE_DIR}/src/engine/src/threading
```
(`test_alloc` includes `MeshBatching.h` (passes) and `StagingBufferPool.h` (rendering). Confirm `StagingBufferPool.h`'s new dir `src/engine/src/rendering` is on the list too; add it if `test_alloc` includes it.)

- [ ] **Step 2: Build + run both test suites**

Run: `cmake --build --preset msvc-win64-vs2026-community --target test_alloc` then `./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe`
Expected: `All allocator tests passed.`
Run: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs` then `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`
Expected: `All ECS tests passed.`

- [ ] **Step 3: Commit**

```bash
git add tests/CMakeLists.txt
git commit -m "Point test_alloc include dirs at relocated engine headers"
```

---

## Phase A3 — Verify

### Task 8: Full build, smoke, and ImGui-free Engine check

- [ ] **Step 1: Clean full build**

Run, in order:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
cmake --build --preset msvc-win64-vs2026-community --target test_alloc
```
Expected: all clean.

- [ ] **Step 2: Tests green**

Run `test_ecs.exe` (`All ECS tests passed.`) and `test_alloc.exe` (`All allocator tests passed.`).

- [ ] **Step 3: Confirm Engine.dll exports no ImGui**

Run (from a VS dev shell or with dumpbin on PATH):
```
dumpbin /exports out/build/msvc-win64-vs2026-community/bin/Debug/Engine.dll | findstr /i imgui
```
Expected: no matches (ImGui left the core).

- [ ] **Step 4: Editor smoke (manual; report honestly)**

Launch the editor; verify: scene renders; ImGui panels/inspector/gizmos work; load `assets/models`; **backend hot-swap DX12↔Vulkan**; Memory panel reads the pools. If the GUI can't be driven here, report the build + dumpbin results and leave the visual smoke to the user. Do not claim unobserved runtime success.

- [ ] **Step 5: Commit (if any final fixups were needed)**

```bash
git add -A
git commit -m "Engine core extraction complete: editor = Engine core + ImGui overlay"
```

---

## Self-Review

**Spec coverage:**
- `IOverlay` interface (ImGui-free, mirrors ImGuiRenderer surface) → Task 1. ✓
- `ImGuiOverlay` adapter wrapping unchanged `ImGuiRenderer` → Task 2. ✓
- Renderer rewired to optional overlay (5 call sites incl. hot-swap hooks) → Task 3 Steps 1-2. ✓
- Factory injection `main → Application → RenderThread → Renderer` → Task 3 Steps 3-5. ✓
- Move core editor→engine (the spec's file list) → Task 4. ✓
- Engine CMake gains deps/includes/defines/POST_BUILD; editor slims + links Engine; imgui/ImGuizmo editor-only; Engine not linking `game` target → Task 5. ✓
- `ENGINE_API` export rule ("export iff an editor TU names it") → Task 6. ✓
- `alloc.h`/`ALLOC_TRACKER_ENABLED` editor-scoped → Task 5 Step 2 (kept in editor list + knob). ✓
- test_alloc include-path fix → Task 7. ✓
- A1→A2→A3 phasing, editor builds throughout → phase structure. ✓
- Verify: full build, tests green, dumpbin no ImGui, smoke incl. hot-swap → Task 8. ✓
- No GAME_API_VERSION bump → global rules + unchanged ecs/game. ✓

**Placeholder scan:** A1 has exact code. A2's non-determinism (the exact moved-helper set in Task 4 Step 1 note, and the exact ENGINE_API set in Task 6) is inherent to a DLL-boundary relocation and is handled by an explicit, bounded procedure (move the header the build names; export the class the linker names), not a vague "handle errors." Acceptable.

**Type consistency:** `IOverlay` methods (`Init/Render/Shutdown/OnDeviceLost/OnDeviceReset`) are spelled identically in Task 1 (interface), Task 2 (ImGuiOverlay overrides), and Task 3 (Renderer call sites). `OverlayFactory` and `SetOverlay` match across Renderer/RenderThread/Application/main. The `ImGuiRenderer` methods forwarded to (`Init/Render/Shutdown/ShutdownNvrhiOnly/InitNvrhiForDevice`) match its real signatures (verified). ✓
