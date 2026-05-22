# ImGuiRenderer Panel Decomposition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the five inline ImGui windows from the 1913-line `ImGuiRenderer.cpp` into focused per-panel classes that own their state, leaving `ImGuiRenderer` a thin host.

**Architecture:** A per-frame `EditorContext` struct carries shared deps; each panel becomes a class with a `Draw(const EditorContext&)` method holding its own UI state; the host builds the context and calls panels in the current window order. Behavior-preserving relocation (no logic edits, identical window titles/IDs), one cleanup: dead gizmo code removed.

**Tech Stack:** C++23, Dear ImGui 1.92.4 + ImGuizmo, NVRHI, custom ECS, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-22-imgui-panel-decomposition-design.md`

---

## How to read this plan (relocation model)

This is a **verbatim relocation**, not a rewrite. For each panel the plan gives: (a) the new
header in full, (b) the new `.cpp` scaffolding in full (includes, `Draw` signature, state
members), (c) the **exact source line range** in `ImGuiRenderer.cpp` to move into `Draw`, and
(d) a **substitution table** of identifier renames to apply to the moved block. **Do not rewrite
the moved logic** — cut it verbatim and apply only the listed substitutions. Build errors after a
move reveal any identifier not in the table; resolve each by either routing it through
`EditorContext` (if it's a shared dep) or moving it to the panel as a member (if it's
panel-private state). Window titles, widget IDs, and control flow stay byte-identical.

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`.
- A new `.cpp` added to a target needs `cmake --preset msvc-win64-vs2026-community` before build.
- Editor-only. After each task: `cmake --build ... --target editor` is clean. The feature is
  UI — a subagent verifies the **build**; the user does the GUI smoke at the end.
- No `GAME_API_VERSION` bump; `Engine`/`ecs`/`game`/`runtime` untouched.
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain
  `git commit`. Stage only the files each step names; never stage `.claude/`. After each commit,
  `git log -1 --format='%an <%ae>'` must show the personal email.
- The line numbers below are from the current `ImGuiRenderer.cpp`. After each task the file
  shrinks, so **re-locate the next block by its `ImGui::Begin("<title>")` anchor**, not by the
  original number.

---

### Task 1: `EditorContext` + `EditorFileDialog`

Foundation: the shared-context struct and the extracted file dialog. The host starts building a
context and the file dialog moves out of `ImGuiRenderer`.

**Files:**
- Create: `src/editor/src/rendering/imgui/EditorContext.h`
- Create: `src/editor/src/rendering/imgui/EditorFileDialog.h`
- Create: `src/editor/src/rendering/imgui/EditorFileDialog.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (move `OpenFileDialog` body out; build a `ctx` local at the top of `Render`)
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h` (remove the `OpenFileDialog` decl)
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create `EditorContext.h`**

```cpp
#pragma once
#include <cstdint>
#include <memory>

class ECS;
class MeshSystem;
class MaterialSystem;
class MeshPreviewRenderer;
struct ApplicationContext;
struct SimulationSnapshot;
struct ImDrawList;

// Per-frame dependencies shared by the editor panels. Built once by ImGuiRenderer::Render and
// passed by const& to each panel's Draw. The viewport gizmo fields are filled AFTER the Viewport
// window draws (so the inspector's gizmo can map to the panel) — see ImGuiRenderer::Render.
struct EditorContext {
    ApplicationContext*        App            = nullptr;
    MeshSystem*                MeshSys        = nullptr;
    MaterialSystem*            MatSys         = nullptr;
    MeshPreviewRenderer*       Preview        = nullptr;
    const ECS*                 World          = nullptr;
    std::shared_ptr<const ECS> WorldSnapshot;
    SimulationSnapshot*        Snapshot       = nullptr;
    float                      GpuFrameTimeMs = 0.0f;
    ImDrawList*                ViewportDrawList = nullptr;
    float                      ViewportMinX = 0.0f, ViewportMinY = 0.0f;
    std::uint32_t              ViewportW = 0, ViewportH = 0;
};
```

- [ ] **Step 2: Create `EditorFileDialog.h`**

```cpp
#pragma once
#include <cstddef>

// Native Windows "open file" dialog. Returns true if a file was chosen (path written to outPath).
namespace EditorFileDialog {
    bool Open(char* outPath, size_t outPathSize, const char* filter);
}
```

- [ ] **Step 3: Create `EditorFileDialog.cpp`** — move the body of `ImGuiRenderer::OpenFileDialog`

Cut the entire body of `bool ImGuiRenderer::OpenFileDialog(char* outPath, size_t outPathSize, const char* filter)` (currently `ImGuiRenderer.cpp:1822`–end) into this file, unchanged except the function header. Carry over the Windows includes it needs (`<windows.h>`, `<commdlg.h>`, `<shobjidl.h>` — copy whichever the original TU used for it):

```cpp
#include "EditorFileDialog.h"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

namespace EditorFileDialog {
bool Open(char* outPath, size_t outPathSize, const char* filter)
{
    // <<< paste the verbatim body of the former ImGuiRenderer::OpenFileDialog here >>>
}
} // namespace EditorFileDialog
```

- [ ] **Step 4: Remove `OpenFileDialog` from `ImGuiRenderer`**

Delete the `bool OpenFileDialog(...);` declaration from `ImGuiRenderer.h` and the definition from
`ImGuiRenderer.cpp`. Its current call sites are inside the Mesh/Material panels (still inline at
this point) — update those calls from `OpenFileDialog(` to `EditorFileDialog::Open(` and add
`#include "EditorFileDialog.h"` to `ImGuiRenderer.cpp`. (They move out in Tasks 4/5; routing them
through the free function now keeps the build green.)

- [ ] **Step 5: Build a `ctx` at the top of `Render` and add the include**

Add `#include "EditorContext.h"` to `ImGuiRenderer.cpp`. In `Render`, right after
`std::shared_ptr<const ECS> worldSnapshot = std::atomic_load(&m_AppContext->LatestWorldSnapshot);`
(currently line ~393), construct the context (it is not consumed yet — panels adopt it in later
tasks; this verifies it compiles and is ready):
```cpp
        EditorContext ctx;
        ctx.App = m_AppContext;
        ctx.MeshSys = m_MeshSystem;
        ctx.MatSys = m_MaterialSystem;
        ctx.Preview = m_MeshPreviewRenderer.get();
        ctx.World = world;
        ctx.WorldSnapshot = worldSnapshot;
        ctx.Snapshot = &snapshot;
        ctx.GpuFrameTimeMs = gpuFrameTimeMs;
        (void)ctx; // consumed by panels in later tasks
```

- [ ] **Step 6: Add the new sources to CMake**

In `src/editor/CMakeLists.txt`, after `    src/rendering/imgui/SceneViewport.cpp` add:
```cmake
    src/rendering/imgui/EditorFileDialog.cpp
```

- [ ] **Step 7: Reconfigure, build, commit**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: clean build.
```bash
git add src/editor/src/rendering/imgui/EditorContext.h src/editor/src/rendering/imgui/EditorFileDialog.h src/editor/src/rendering/imgui/EditorFileDialog.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: add EditorContext + extract EditorFileDialog from ImGuiRenderer"
```

---

### Task 2: `StatsPanel` (the "Hello, world!" window)

**Files:**
- Create: `src/editor/src/rendering/imgui/StatsPanel.h`
- Create: `src/editor/src/rendering/imgui/StatsPanel.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.{h,cpp}`
- Modify: `src/editor/CMakeLists.txt`

- [ ] **Step 1: Create `StatsPanel.h`**

```cpp
#pragma once
struct EditorContext;

// Draws the "Hello, world!" window: renderer/GPU/TPS readouts, GameThreadSettings editor,
// and frame-time stats.
class StatsPanel {
public:
    void Draw(const EditorContext& ctx);
};
```

- [ ] **Step 2: Create `StatsPanel.cpp`** — relocate the block

Move `ImGuiRenderer.cpp` lines **565–633** (the `ImGui::Begin("Hello, world!")` … `ImGui::End();`
block, including the leading comment) verbatim into `StatsPanel::Draw`. Scaffolding:
```cpp
#include "StatsPanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include "ApplicationContext.h" // GameThreadSettings/FrameTimeStats + GameThreadConfig seqlock

void StatsPanel::Draw(const EditorContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    // <<< paste lines 565-633 here, applying the substitution table below >>>
}
```
**Substitution table** (apply to the moved block):
| in source | replace with |
|---|---|
| `gpuFrameTimeMs` | `ctx.GpuFrameTimeMs` |
| `snapshot.` | `ctx.Snapshot->` |
| `m_AppContext` | `ctx.App` |

(`io` is declared locally in `Draw` above. Any other `m_*` referenced here is shared state →
route via `ctx`; there should be none beyond `m_AppContext`.)

- [ ] **Step 3: Host wiring**

In `ImGuiRenderer.h` add `#include "StatsPanel.h"` and a member `StatsPanel m_StatsPanel;`. In
`ImGuiRenderer.cpp` add `#include "StatsPanel.h"`, delete the moved 565–633 block, and in its
place call `m_StatsPanel.Draw(ctx);`. Drop the now-unused `(void)ctx;` line from Task 1 once a
panel consumes `ctx`.

- [ ] **Step 4: CMake + build + commit**

Add `    src/rendering/imgui/StatsPanel.cpp` to `src/editor/CMakeLists.txt` (after
`EditorFileDialog.cpp`).
```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: clean.
```bash
git add src/editor/src/rendering/imgui/StatsPanel.h src/editor/src/rendering/imgui/StatsPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract StatsPanel from ImGuiRenderer"
```

---

### Task 3: `MaterialManagerPanel` (the "Material Manager" window)

**Files:** create `MaterialManagerPanel.{h,cpp}`; modify `ImGuiRenderer.{h,cpp}`, `CMakeLists.txt`.

- [ ] **Step 1: Create `MaterialManagerPanel.h`**

```cpp
#pragma once
struct EditorContext;

// Draws the "Material Manager" window: material list, preview, and load-from-file.
class MaterialManagerPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    int m_SelectedMaterial = -1; // adopt the former function-local selection static(s)
};
```

- [ ] **Step 2: Create `MaterialManagerPanel.cpp`** — relocate the block

Move the `ImGui::Begin("Material Manager")` … matching `ImGui::End();` block (currently lines
**1526–1634**) verbatim into `Draw`. Scaffolding:
```cpp
#include "MaterialManagerPanel.h"
#include "EditorContext.h"
#include "EditorFileDialog.h"

#include <imgui.h>
#include "MaterialSystem.h"
#include "MaterialLoader.h" // match the includes the inline block used
#include "ApplicationContext.h"

void MaterialManagerPanel::Draw(const EditorContext& ctx)
{
    // <<< paste lines 1526-1634 here, applying the substitution table below >>>
}
```
**Substitution table:**
| in source | replace with |
|---|---|
| `m_MaterialSystem` | `ctx.MatSys` |
| `m_AppContext` | `ctx.App` |
| `OpenFileDialog(` | `EditorFileDialog::Open(` |
| any `static <T> selectedMaterial...` | the `m_SelectedMaterial` member (rename to match the actual static) |

(If the block declares its selection as a function-local `static`, delete the `static` and use
the member instead; if it uses a differently-named/typed static, rename the member to match its
type/name.)

- [ ] **Step 3: Host wiring** — `#include "MaterialManagerPanel.h"` + member
`MaterialManagerPanel m_MaterialManager;` in the header; in `Render`, delete the moved block and
call `m_MaterialManager.Draw(ctx);` in the same position.

- [ ] **Step 4: CMake + build + commit**

Add `    src/rendering/imgui/MaterialManagerPanel.cpp` to CMake; reconfigure; build editor (clean).
```bash
git add src/editor/src/rendering/imgui/MaterialManagerPanel.h src/editor/src/rendering/imgui/MaterialManagerPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract MaterialManagerPanel from ImGuiRenderer"
```

---

### Task 4: `MeshManagerPanel` (the "Mesh Manager" window)

**Files:** create `MeshManagerPanel.{h,cpp}`; modify `ImGuiRenderer.{h,cpp}`, `CMakeLists.txt`.
This panel owns the mesh-preview camera state (the current `m_MeshPreviewState` struct/member).

- [ ] **Step 1: Create `MeshManagerPanel.h`**

Copy the `MeshPreviewState` struct definition (currently nested in `ImGuiRenderer.h`,
`struct MeshPreviewState { float cameraDistance; ... };`) into this panel and hold it as a member:
```cpp
#pragma once
struct EditorContext;

// Draws the "Mesh Manager" window: mesh list, 3D preview (orbit camera), and load-from-file.
class MeshManagerPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    struct PreviewState {
        float cameraDistance = 3.0f;
        float cameraYaw = 0.0f;
        float cameraPitch = 0.3f;
        bool  isDragging = false;
        float lastMouseX = 0.0f;
        float lastMouseY = 0.0f;
    };
    PreviewState m_Preview;
    int m_SelectedMesh = -1; // adopt the former selection static(s)
};
```
(Match the exact field set of the existing `MeshPreviewState` — copy it verbatim; the values above
mirror `ImGuiRenderer.h`.)

- [ ] **Step 2: Create `MeshManagerPanel.cpp`** — relocate the block

Move `ImGui::Begin("Mesh Manager")` … matching `ImGui::End();` (currently **1302–1523**) verbatim.
```cpp
#include "MeshManagerPanel.h"
#include "EditorContext.h"
#include "EditorFileDialog.h"

#include <imgui.h>
#include "MeshSystem.h"
#include "MeshPreviewRenderer.h"
#include "MeshLoader.h"          // match the includes the inline block used
#include "ApplicationContext.h"

void MeshManagerPanel::Draw(const EditorContext& ctx)
{
    // <<< paste lines 1302-1523 here, applying the substitution table below >>>
}
```
**Substitution table:**
| in source | replace with |
|---|---|
| `m_MeshSystem` | `ctx.MeshSys` |
| `m_MeshPreviewRenderer` | `ctx.Preview` |
| `m_MeshPreviewState` | `m_Preview` |
| `m_AppContext` | `ctx.App` |
| `OpenFileDialog(` | `EditorFileDialog::Open(` |
| any `static ... selectedMesh...` | the `m_SelectedMesh` member (rename to match the real static) |

- [ ] **Step 3: Host wiring** — `#include "MeshManagerPanel.h"` + member
`MeshManagerPanel m_MeshManager;`; in `Render` delete the moved block, call
`m_MeshManager.Draw(ctx);`. Remove the now-unused `MeshPreviewState m_MeshPreviewState;` member
and its `struct MeshPreviewState { ... };` from `ImGuiRenderer.h` (it now lives in the panel).

- [ ] **Step 4: CMake + build + commit**

Add `    src/rendering/imgui/MeshManagerPanel.cpp`; reconfigure; build editor (clean).
```bash
git add src/editor/src/rendering/imgui/MeshManagerPanel.h src/editor/src/rendering/imgui/MeshManagerPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract MeshManagerPanel from ImGuiRenderer"
```

---

### Task 5: `GizmoController` (and drop the dead gizmo window)

**Files:** create `GizmoController.{h,cpp}`; modify `ImGuiRenderer.{h,cpp}`, `CMakeLists.txt`.

- [ ] **Step 1: Create `GizmoController.h`**

```cpp
#pragma once
#include <ImGuizmo.h>
struct EditorContext;

// Owns the transform-gizmo settings and drives ImGuizmo for the selected entity, mapped to the
// scene Viewport panel (rect/drawlist come from EditorContext).
class GizmoController {
public:
    // Inline operation/mode/snap controls (the radio buttons + snap inputs).
    void DrawControls();
    // Manipulate `matrix` (column-major float[16]) with the current camera, targeting the
    // Viewport panel. No-op if the Viewport is hidden (ctx.ViewportDrawList == nullptr).
    void EditTransform(float* cameraView, float* cameraProjection, float* matrix,
                       const EditorContext& ctx);
private:
    ImGuizmo::OPERATION m_Operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE      m_Mode      = ImGuizmo::LOCAL;
    bool                m_UseSnap   = false;
    float               m_Snap[3]   = { 1.0f, 1.0f, 1.0f };
};
```

- [ ] **Step 2: Create `GizmoController.cpp`**

`EditTransform` body is the current `ImGuiRenderer::EditTransform` (lines **342–355**), moved with
substitutions. `DrawControls` is the inline gizmo radio/snap UI currently inside the inspector
(the `RadioButton("Translate##GZ" ...)` … snap `InputFloat` block, currently ~lines **896–928**),
moved with the same substitutions.
```cpp
#include "GizmoController.h"
#include "EditorContext.h"
#include <imgui.h>

void GizmoController::DrawControls()
{
    // <<< paste the inline gizmo controls block (~896-928) here, applying substitutions >>>
}

void GizmoController::EditTransform(float* cameraView, float* cameraProjection, float* matrix,
                                   const EditorContext& ctx)
{
    // <<< paste the body of the former ImGuiRenderer::EditTransform (342-355), applying substitutions >>>
}
```
**Substitution table:**
| in source | replace with |
|---|---|
| `m_GizmoOperation` | `m_Operation` |
| `m_GizmoMode` | `m_Mode` |
| `m_GizmoUseSnap` | `m_UseSnap` |
| `m_GizmoSnap` | `m_Snap` |
| `m_ViewportDrawList` | `ctx.ViewportDrawList` |
| `m_ViewportImageMinX` / `m_ViewportImageMinY` | `ctx.ViewportMinX` / `ctx.ViewportMinY` |
| `m_LastViewportW` / `m_LastViewportH` | `ctx.ViewportW` / `ctx.ViewportH` |

(The former `EditTransform` guard `if (!m_ViewportDrawList || m_LastViewportW < 1 ...)` becomes
`if (!ctx.ViewportDrawList || ctx.ViewportW < 1 || ctx.ViewportH < 1) return;`.)

- [ ] **Step 3: Delete the dead gizmo code from `ImGuiRenderer`**

Remove from `ImGuiRenderer.cpp`: the gizmo file-statics block (lines **234–239**), the
`TransformStart` definition (243–331), the `TransformEnd` definition (333–340), and the
`EditTransform` definition (342–355). Remove the matching declarations
(`TransformStart`/`TransformEnd`/`EditTransform`) from `ImGuiRenderer.h`. These (the "Gizmo"
`ViewManipulate` window) are unused (`m_GizmoUseWindow` is false and `TransformStart` is never
called) — confirm with a grep for `TransformStart(` / `TransformEnd(` (only definitions exist)
before deleting.

- [ ] **Step 4: Wire the controller into the still-inline inspector**

Add `#include "GizmoController.h"` + member `GizmoController m_Gizmo;` to `ImGuiRenderer`. In the
still-inline ECS Inspector block, replace the inline gizmo radio/snap UI (the block moved in
Step 2, ~896–928) with `m_Gizmo.DrawControls();`, and replace the
`EditTransform(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection), glm::value_ptr(M));`
call with `m_Gizmo.EditTransform(glm::value_ptr(cameraView), glm::value_ptr(cameraProjection), glm::value_ptr(M), ctx);`.
The `ctx.Viewport*` fields must be populated before this runs — they are (the Viewport window
draws earlier in `Render`; if not yet wired into `ctx`, also do Step 5).

- [ ] **Step 5: Populate the gizmo fields on `ctx` from the Viewport draw**

In `Render`, where the Viewport window currently records `m_ViewportImageMinX/Y`,
`m_LastViewportW/H`, `m_ViewportDrawList`, also copy them into the context immediately after that
block:
```cpp
        ctx.ViewportDrawList = m_ViewportDrawList;
        ctx.ViewportMinX = m_ViewportImageMinX;
        ctx.ViewportMinY = m_ViewportImageMinY;
        ctx.ViewportW = m_LastViewportW;
        ctx.ViewportH = m_LastViewportH;
```
(The host keeps owning the Viewport draw + those members; it just publishes them to `ctx`.)

- [ ] **Step 6: CMake + build + commit**

Add `    src/rendering/imgui/GizmoController.cpp`; reconfigure; build editor (clean).
```bash
git add src/editor/src/rendering/imgui/GizmoController.h src/editor/src/rendering/imgui/GizmoController.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract GizmoController; drop dead gizmo window"
```

---

### Task 6: `EcsInspectorPanel` (the "ECS Inspector & Editor" window)

The largest move (~665 lines). It uses the `GizmoController` from Task 5.

**Files:** create `EcsInspectorPanel.{h,cpp}`; modify `ImGuiRenderer.{h,cpp}`, `CMakeLists.txt`.

- [ ] **Step 1: Create `EcsInspectorPanel.h`**

```cpp
#pragma once
#include "ECS.h"            // EntityId, INVALID_ENTITY, TransformComponent (edit working copy)
#include "GizmoController.h"
struct EditorContext;

// Draws the "ECS Inspector & Editor" window: entity list, create/select/delete, component
// add/remove, and per-component editors (Transform incl. gizmo, Lightning, Mesh, Material, Text,
// Sun marker). Mutations are issued as ECSCommands via EditorContext::App.
class EcsInspectorPanel {
public:
    void Draw(const EditorContext& ctx);
private:
    EntityId         m_Selected   = INVALID_ENTITY;
    EntityId         m_LastEdited = INVALID_ENTITY;
    TransformComponent m_EditTransform{}; // working copy used by the Transform editor
    GizmoController  m_Gizmo;
};
```
(Match the actual types/names of the inspector's function-local statics — `selectedEntity`,
`lastEditedEntity`, and the local `editTransform` working copy — when declaring these members.)

- [ ] **Step 2: Create `EcsInspectorPanel.cpp`** — relocate the block

Move `ImGui::Begin("ECS Inspector & Editor")` … matching `ImGui::End();` (currently **636–1299**)
verbatim into `Draw`. Scaffolding:
```cpp
#include "EcsInspectorPanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include "ApplicationContext.h"
#include "ECSCommands.h"
// (carry over any other includes the inline inspector block relied on, e.g. component headers)

void EcsInspectorPanel::Draw(const EditorContext& ctx)
{
    // <<< paste lines 636-1299 here, applying the substitution table below >>>
}
```
**Substitution table:**
| in source | replace with |
|---|---|
| `worldSnapshot` | `ctx.WorldSnapshot` |
| `world` | `ctx.World` |
| `snapshot.` | `ctx.Snapshot->` |
| `m_AppContext` | `ctx.App` |
| `static EntityId selectedEntity = INVALID_ENTITY;` (the decl) | *(delete — use the member)* |
| `selectedEntity` | `m_Selected` |
| `lastEditedEntity` (and its `static` decl) | `m_LastEdited` (delete the `static`) |
| the local `editTransform` working copy | `m_EditTransform` (if it was a local that must persist across frames; if it is rebuilt each frame from the component, leave it as a local) |
| the `m_Gizmo.DrawControls()` / `m_Gizmo.EditTransform(...)` calls added in Task 5 | keep, but `m_Gizmo` is now this panel's member (the host's `m_Gizmo` is removed in Step 3) |

(Whether `editTransform` becomes a member or stays a frame-local depends on the original: if it
only persists via `lastEditedEntity` reseeding, keep it local and only `m_LastEdited`/`m_Selected`
are members. Preserve the original persistence semantics exactly.)

- [ ] **Step 3: Host wiring** — `#include "EcsInspectorPanel.h"` + member
`EcsInspectorPanel m_EcsInspector;`. In `Render`, delete the moved block and call
`m_EcsInspector.Draw(ctx);` in the same position. **Remove** the host's `GizmoController m_Gizmo;`
member added in Task 5 (the gizmo now lives inside the inspector). Remove the now-unused
`#include "GizmoController.h"` from `ImGuiRenderer.{h,cpp}` if nothing else there uses it.

- [ ] **Step 4: CMake + build + commit**

Add `    src/rendering/imgui/EcsInspectorPanel.cpp`; reconfigure; build editor (clean).
```bash
git add src/editor/src/rendering/imgui/EcsInspectorPanel.h src/editor/src/rendering/imgui/EcsInspectorPanel.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract EcsInspectorPanel from ImGuiRenderer"
```

---

### Task 7: `MainMenuBar` (File / Edit / About / Settings / View)

**Files:** create `MainMenuBar.{h,cpp}`; modify `ImGuiRenderer.{h,cpp}`, `CMakeLists.txt`.

- [ ] **Step 1: Create `MainMenuBar.h`**

```cpp
#pragma once
#include "lib.h"   // RendererAPI
#include <string>
struct EditorContext;

// Draws the main menu bar. Returns true if "View -> Reset Layout" was clicked this frame, so the
// host rebuilds the dock layout.
class MainMenuBar {
public:
    bool Draw(const EditorContext& ctx);
private:
    RendererAPI m_PendingBackend = RendererAPI::Invalid;
    bool        m_PendingBackendInitialized = false;
    std::string m_SettingsSaveError;
};
```
(Match the exact type of `RendererAPI` and the `m_Pending*`/`m_SettingsSaveError` fields as they
exist in `ImGuiRenderer.h` today.)

- [ ] **Step 2: Create `MainMenuBar.cpp`** — relocate the block

Move the `if (ImGui::BeginMainMenuBar()) { … ImGui::EndMainMenuBar(); }` block (currently
**401–536**) into `Draw`. Replace the `View -> Reset Layout` handler so that instead of setting
the host's `s_ResetLayout` static, it records a local result and returns it.
```cpp
#include "MainMenuBar.h"
#include "EditorContext.h"

#include <imgui.h>
#include "ApplicationContext.h"
#include "utilities/SettingsManager.h"
#include "WorldManager.h"   // SaveWorldSnapshot (match the inline block's includes)

bool MainMenuBar::Draw(const EditorContext& ctx)
{
    bool resetLayoutRequested = false;
    // <<< paste lines 401-536 here, applying the substitution table below >>>
    return resetLayoutRequested;
}
```
**Substitution table:**
| in source | replace with |
|---|---|
| `m_AppContext` | `ctx.App` |
| `worldSnapshot` | `ctx.WorldSnapshot` |
| `m_PendingBackend` / `m_PendingBackendInitialized` / `m_SettingsSaveError` | same names (now members of `MainMenuBar`) |
| `if (ImGui::MenuItem("Reset Layout")) { s_ResetLayout = true; }` | `if (ImGui::MenuItem("Reset Layout")) { resetLayoutRequested = true; }` |

- [ ] **Step 3: Host wiring**

Add `#include "MainMenuBar.h"` + member `MainMenuBar m_MenuBar;`. Remove the
`m_PendingBackend`/`m_PendingBackendInitialized`/`m_SettingsSaveError` members from
`ImGuiRenderer.h` (now in `MainMenuBar`). In `Render`, replace the inline menu-bar block with:
```cpp
        if (m_MenuBar.Draw(ctx)) s_ResetLayout = true;
```
keeping the existing `static bool s_LayoutInitialized/s_ResetLayout` + the dockspace/layout-build
block exactly as-is (the host still owns the layout build).

- [ ] **Step 4: CMake + build + commit**

Add `    src/rendering/imgui/MainMenuBar.cpp`; reconfigure; build editor (clean).
```bash
git add src/editor/src/rendering/imgui/MainMenuBar.h src/editor/src/rendering/imgui/MainMenuBar.cpp src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt
git commit -m "refactor: extract MainMenuBar from ImGuiRenderer"
```

---

### Task 8: Final host slim + verification

**Files:** modify `src/editor/src/rendering/imgui/ImGuiRenderer.{h,cpp}` (cleanup only).

- [ ] **Step 1: Remove dead includes/members**

Audit `ImGuiRenderer.h`/`.cpp` for now-unused `#include`s (e.g. `ImGuizmo.h`, `MeshLoader.h`,
`MaterialLoader.h`, component headers only the moved panels used) and remove them. Confirm the
remaining members are only: `m_fonts`, `m_ImGuiNvrhi`, `m_MeshPreviewRenderer`, `m_AppContext`,
`m_MeshSystem`, `m_MaterialSystem`, `m_Renderer`, `m_SceneViewport`, the Viewport rect/drawlist
members, and the panel instances (`m_MenuBar`, `m_StatsPanel`, `m_EcsInspector`, `m_MeshManager`,
`m_MaterialManager`). Remove anything orphaned.

- [ ] **Step 2: Confirm `Render` is orchestration only**

`Render` should now be: process input + io setup; fonts; `NewFrame`; load `worldSnapshot`; build
`ctx`; `ImGuizmo::BeginFrame`; `if (m_MenuBar.Draw(ctx)) s_ResetLayout = true;`; dockspace +
layout build; Viewport window draw (+ publish rect/drawlist to `ctx`); then, in the original
window order: `m_StatsPanel.Draw(ctx)`, `DrawMemoryPanel(...)`, `DrawRenderStatsPanel(...)`,
`m_EcsInspector.Draw(ctx)`, `m_MeshManager.Draw(ctx)`, `m_MaterialManager.Draw(ctx)`; `PopFont`;
`ImGui::Render()`; `m_ImGuiNvrhi->render`. No inline panel bodies remain.

- [ ] **Step 3: Build + regression + commit**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs
cmake --build out/build/msvc-win64-vs2026-community --target test_alloc
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
```
Expected: all clean; both tests pass. `wc -l ImGuiRenderer.cpp` should be roughly ~600.
```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "refactor: slim ImGuiRenderer to a host after panel extraction"
```

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green.
- [ ] `test_ecs` / `test_alloc` / `test_frustum` print their `All ... passed.` lines.
- [ ] **GUI smoke (user-run, surface to the user — do not self-approve):** every panel behaves
  exactly as before — ECS Inspector (select/create/delete entity, add/remove components, edit
  Transform **with the gizmo dragging in the Viewport**), Mesh Manager (list + 3D preview orbit +
  load from file), Material Manager (list + preview + load), Stats panel + GameThreadSettings
  controls, the menu bar incl. **View → Reset Layout**, the Viewport, and `runtime.exe` unchanged.

## Notes / non-goals
- Pure relocation: identical behavior, window titles, and widget IDs (so the saved `imgui.ini`
  dock layout is unaffected). Only intentional removal: the dead `TransformStart`/`TransformEnd`/
  "Gizmo" `ViewManipulate` window.
- No `GAME_API_VERSION` bump; no ECS/engine/runtime change.
- Each task leaves the editor building and behaving identically; panels are extracted one at a
  time so a regression is isolated to a single small commit.
