# Move the Ground Grid to the Debug Pass — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the opaque-plane ground grid (`PrimitiveRenderPass`) with a line-based grid drawn by `DebugRenderPass`, gated by a new editor-on/runtime-off `ShowGrid` flag, and delete `PrimitiveRenderPass`.

**Architecture:** A pure `DebugAppendGrid` builder emits a finite, camera-snapped line grid (Y=0) into the existing Debug line pass. A new `DebugDrawSettings::ShowGrid` flag (default off → runtime inert) gates it; the editor flips it on at startup. The old `PrimitiveRenderPass` (which only ever drew the grid) is removed from the core pass list and deleted. Fixes the skybox-horizon clobber (thin lines, no opaque slab) and removes the grid from the shipped runtime.

**Tech Stack:** C++23, NVRHI deferred renderer (`Engine.dll`), Dear ImGui editor overlay, GLM. Pure builder unit-tested via the existing `test_debugdraw` target.

**Spec:** `docs/superpowers/specs/2026-05-25-grid-debug-pass-design.md`

---

## Build & test reference (every task)

Build preset: **`msvc-win64-vs2026-community`** (enterprise preset is NOT installed — never use it). Binaries in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

```powershell
cmake --preset msvc-win64-vs2026-community                                   # reconfigure (after CMakeLists changes)
cmake --build --preset msvc-win64-vs2026-community --target <target>          # build
./out/build/msvc-win64-vs2026-community/bin/Debug/<test>.exe                   # run a test
```

**Commit identity (MANDATORY):** `git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "<msg>"`. Never the work email. Never `git add .claude/` — stage only files you changed.

No `ECS.h` / `GAME_API_VERSION` change in this plan; no editor restart semantics beyond a normal rebuild.

## File map

- `src/engine/src/rendering/DebugDraw.h` — add the pure `DebugAppendGrid` builder. (Task 1)
- `tests/test_debugdraw.cpp` — add grid tests. (Task 1)
- `src/engine/src/rendering/RenderStats.h` — add `DebugDrawSettings::ShowGrid`. (Task 2)
- `src/engine/src/rendering/passes/DebugRenderPass.cpp` — draw the grid when `ShowGrid`. (Task 2)
- `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` — enable the grid at editor startup. (Task 3)
- `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` — add the Grid checkbox. (Task 3)
- `src/engine/src/rendering/Renderer.cpp` — remove the two `PrimitiveRenderPass` registrations + its include. (Task 3)
- `src/engine/src/rendering/passes/PrimitiveRenderPass.{h,cpp}` — deleted. (Task 3)
- `src/engine/CMakeLists.txt` — drop the `PrimitiveRenderPass.cpp` source entry. (Task 3)

---

## Task 1: `DebugAppendGrid` builder + tests (TDD)

**Files:**
- Modify: `src/engine/src/rendering/DebugDraw.h` (append a builder after `DebugAppendFrustum`)
- Modify: `tests/test_debugdraw.cpp` (add tests + call them)

`DebugAppendGrid` is pure (appends `DebugVertex`s) and `test_debugdraw` compiles `DebugDraw.h` header-only (no engine link), so this is fully TDD-able in isolation.

- [ ] **Step 1: Write the failing tests in `tests/test_debugdraw.cpp`**

Add these helpers + tests before `main()` (the file already defines `hasVert` and the `EXPECT` macro):

```cpp
// Expected vertex count: lines per direction = 2*floor(halfExtent/step)+1, two
// directions, 2 verts per line.
static size_t gridExpectedVerts(float halfExtent, float step) {
    const int n = static_cast<int>(halfExtent / step);
    const int linesPerDir = 2 * n + 1;
    return static_cast<size_t>(2 * linesPerDir * 2);
}

// Return the color of the first vertex at position p (or a sentinel if absent).
static glm::vec4 colorAt(const std::vector<DebugVertex>& v, glm::vec3 p) {
    for (auto& x : v)
        if (std::abs(x.Position.x-p.x)<1e-4f && std::abs(x.Position.y-p.y)<1e-4f
            && std::abs(x.Position.z-p.z)<1e-4f) return x.Color;
    return glm::vec4(-1.0f);
}

static void T04_grid_vertex_count()
{
    std::vector<DebugVertex> v;
    DebugAppendGrid(v, {0,0,0}, 5.0f, 1.0f, {0.35f,0.35f,0.35f,1}, {1,0,0,1}, {0,0,1,1});
    EXPECT(v.size() == gridExpectedVerts(5.0f, 1.0f)); // 2 * (2*5+1) * 2 = 44
    // All grid verts are on Y=0.
    bool allY0 = true;
    for (auto& x : v) if (std::abs(x.Position.y) > 1e-5f) allY0 = false;
    EXPECT(allY0);
}

static void T05_grid_camera_snapped()
{
    std::vector<DebugVertex> v;
    // Camera at non-grid-aligned XZ; step 1 => snapped center = (round(3.4), round(-7.8)) = (3, -8).
    DebugAppendGrid(v, {3.4f, 2.0f, -7.8f}, 5.0f, 1.0f, {0.35f,0.35f,0.35f,1}, {1,0,0,1}, {0,0,1,1});
    // Patch spans X in [3-5, 3+5] = [-2, 8], Z in [-8-5, -8+5] = [-13, -3].
    // Min-X line (gx=-2) spans Z [-13,-3]: its endpoints exist.
    EXPECT(hasVert(v, {-2.0f, 0.0f, -3.0f}));
    EXPECT(hasVert(v, { 8.0f, 0.0f, -3.0f}));
    // Unsnapped raw min-X (3.4-5 = -1.6) must NOT be a line position.
    EXPECT(!hasVert(v, {-1.6f, 0.0f, -3.0f}));
}

static void T06_grid_axis_colors()
{
    const glm::vec4 grid{0.35f,0.35f,0.35f,1}, xCol{1,0,0,1}, zCol{0,0,1,1};
    std::vector<DebugVertex> v;
    DebugAppendGrid(v, {0,0,0}, 5.0f, 1.0f, grid, xCol, zCol);
    // World Z axis runs along X==0 -> that line uses axisZColor. Endpoint (0,0,5).
    EXPECT(colorAt(v, {0.0f, 0.0f, 5.0f}) == zCol);
    // World X axis runs along Z==0 -> that line uses axisXColor. Endpoint (5,0,0).
    EXPECT(colorAt(v, {5.0f, 0.0f, 0.0f}) == xCol);
    // An off-axis grid line (gx=2) uses gridColor. Endpoint (2,0,5).
    EXPECT(colorAt(v, {2.0f, 0.0f, 5.0f}) == grid);
}
```

In `main()`, add after `T03_frustum_identity_is_ndc_cube();`:

```cpp
    T04_grid_vertex_count();
    T05_grid_camera_snapped();
    T06_grid_axis_colors();
```

- [ ] **Step 2: Build + run to verify FAIL (builder undefined)**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_debugdraw
```
Expected: FAIL — `'DebugAppendGrid': identifier not found`.

- [ ] **Step 3: Add `DebugAppendGrid` to `src/engine/src/rendering/DebugDraw.h`**

Append at the end of the file (after `DebugAppendFrustum`):

```cpp
// Camera-snapped ground grid on Y=0. The patch is centered on the camera's XZ snapped
// to `step`, so a finite grid follows the camera (feels infinite, never reaches the
// true horizon). Lines where world X==0 / Z==0 fall inside the patch use the axis
// colors; all others use gridColor. Line-list: pairs of verts = segments.
inline void DebugAppendGrid(std::vector<DebugVertex>& out,
                            const glm::vec3& cameraPos,
                            float halfExtent, float step,
                            const glm::vec4& gridColor,
                            const glm::vec4& axisXColor,
                            const glm::vec4& axisZColor) {
    if (step < 1e-3f) step = 1e-3f;
    if (halfExtent < step) halfExtent = step;
    // Snap the patch center to the grid so lines appear stationary as the camera moves.
    const float cx = std::round(cameraPos.x / step) * step;
    const float cz = std::round(cameraPos.z / step) * step;
    const int   n  = static_cast<int>(halfExtent / step); // lines each side of center
    const float axisEps = 0.5f * step;
    // Lines parallel to Z (vary X): span the full Z extent. World Z axis is at X==0.
    for (int i = -n; i <= n; ++i) {
        const float gx = cx + static_cast<float>(i) * step;
        const glm::vec4 col = (std::abs(gx) < axisEps) ? axisZColor : gridColor;
        DebugAppendLine(out, {gx, 0.0f, cz - halfExtent}, {gx, 0.0f, cz + halfExtent}, col);
    }
    // Lines parallel to X (vary Z): span the full X extent. World X axis is at Z==0.
    for (int i = -n; i <= n; ++i) {
        const float gz = cz + static_cast<float>(i) * step;
        const glm::vec4 col = (std::abs(gz) < axisEps) ? axisXColor : gridColor;
        DebugAppendLine(out, {cx - halfExtent, 0.0f, gz}, {cx + halfExtent, 0.0f, gz}, col);
    }
}
```

(`std::round` / `std::abs` come from `<cmath>`, already included by `DebugDraw.h`.)

- [ ] **Step 4: Build + run to verify PASS**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target test_debugdraw
./out/build/msvc-win64-vs2026-community/bin/Debug/test_debugdraw.exe
```
Expected: `All debug draw tests passed.`

- [ ] **Step 5: Commit**

```powershell
git add src/engine/src/rendering/DebugDraw.h tests/test_debugdraw.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(debugdraw): add DebugAppendGrid camera-snapped line grid builder"
```

---

## Task 2: Draw the grid in `DebugRenderPass` + `ShowGrid` flag

**Files:**
- Modify: `src/engine/src/rendering/RenderStats.h` (add `ShowGrid` field)
- Modify: `src/engine/src/rendering/passes/DebugRenderPass.cpp` (gate + append grid)

No new unit test (this is engine/GPU wiring of the already-tested builder); verification is a clean `Engine` build. After this task the flag still defaults off, so behavior is unchanged until Task 3 turns it on in the editor.

- [ ] **Step 1: Add `ShowGrid` to `DebugDrawSettings` in `RenderStats.h`**

In `src/engine/src/rendering/RenderStats.h`, the `DebugDrawSettings` struct becomes:

```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
    bool ShowGrid          = false;
};
```

- [ ] **Step 2: Gate + append the grid in `DebugRenderPass::Render`**

In `src/engine/src/rendering/passes/DebugRenderPass.cpp`:

(a) Extend the early-return gate (currently
`if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB) return;`) to:

```cpp
    if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid)
        return;
```

(b) Right after `m_Verts.clear();` (and before the `if (s.ShowLightGizmos)` block), append the grid so gizmos draw over it:

```cpp
    if (s.ShowGrid) {
        const glm::vec3 camPos = m_Renderer->GetActiveCamera().Position;
        DebugAppendGrid(m_Verts, camPos, 50.0f, 1.0f,
                        glm::vec4(0.35f, 0.35f, 0.35f, 1.0f),  // grid grey
                        glm::vec4(1.0f, 0.2f, 0.2f, 1.0f),     // X axis red
                        glm::vec4(0.2f, 0.6f, 1.0f, 1.0f));    // Z axis blue
    }
```

`DebugRenderPass.cpp` already uses `m_Renderer->GetActiveCamera()` and the `DebugAppend*` builders (so `DebugDraw.h` is already included); no new includes needed. The line pipeline (depthTest on / write off / cull none) is unchanged.

- [ ] **Step 3: Build `Engine` to verify it compiles**

```powershell
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: builds clean.

- [ ] **Step 4: Commit**

```powershell
git add src/engine/src/rendering/RenderStats.h src/engine/src/rendering/passes/DebugRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(render): draw ground grid in DebugRenderPass behind ShowGrid flag"
```

---

## Task 3: Editor enables grid + toggle; delete `PrimitiveRenderPass`

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (enable grid at startup)
- Modify: `src/editor/src/rendering/imgui/RenderStatsPanel.cpp` (Grid checkbox)
- Modify: `src/engine/src/rendering/Renderer.cpp` (remove 2 registrations + include)
- Delete: `src/engine/src/rendering/passes/PrimitiveRenderPass.h` and `.cpp`
- Modify: `src/engine/CMakeLists.txt` (drop the source entry)

This is the cutover: turn the new grid on in the editor, expose the toggle, and delete the old pass. After Task 2 the old `PrimitiveRenderPass` still draws (and the new debug grid is off because the editor hasn't set the flag yet); this task swaps them.

- [ ] **Step 1: Enable the grid at editor startup in `ImGuiRenderer.cpp`**

Add the include near the other engine includes at the top of `src/editor/src/rendering/imgui/ImGuiRenderer.cpp` (it is not currently included):

```cpp
#include "RenderStats.h"
```

In `ImGuiRenderer::Init`, immediately after `m_Renderer = renderer;` (the existing first lines of the function set `m_AppContext`/`m_MeshSystem`/`m_MaterialSystem`/`m_Renderer`), add:

```cpp
    // Ground grid is an editor authoring aid — on by default in the editor, never in
    // the runtime (separate process, no overlay, leaves the flag at its default false).
    GetDebugDrawSettings().ShowGrid = true;
```

- [ ] **Step 2: Add the Grid checkbox in `RenderStatsPanel.cpp`**

In `src/editor/src/rendering/imgui/RenderStatsPanel.cpp`, in the Debug Draw section, after the `ImGui::Checkbox("Wireframe", &dd.Wireframe);` line, add:

```cpp
    ImGui::Checkbox("Grid", &dd.ShowGrid);
```

- [ ] **Step 3: Remove `PrimitiveRenderPass` from `Renderer.cpp`**

In `src/engine/src/rendering/Renderer.cpp`:

(a) Delete the include line `#include "passes/PrimitiveRenderPass.h"`.

(b) In `Renderer::Init`, delete this block (it sits between the sky pass's `AddRenderPass(std::move(skyPass));` and the outline pass):

```cpp
    auto primitivePass = std::make_unique<PrimitiveRenderPass>();
    if (!primitivePass->Initialize(m_Device, this)) {
        SM_ERROR("Failed to initialize PrimitiveRenderPass");
        return false;
    }
    AddRenderPass(std::move(primitivePass));
```

(c) In `Renderer::InitForSwap`, delete this block (same location):

```cpp
    auto primitivePass = std::make_unique<PrimitiveRenderPass>();
    if (!primitivePass->Initialize(m_Device, this)) { SM_ERROR("InitForSwap: PrimitivePass failed"); return false; }
    AddRenderPass(std::move(primitivePass));
```

- [ ] **Step 4: Delete the pass files + drop from CMake**

Delete the files:
```powershell
Remove-Item src/engine/src/rendering/passes/PrimitiveRenderPass.h, src/engine/src/rendering/passes/PrimitiveRenderPass.cpp
```

In `src/engine/CMakeLists.txt`, remove the source line:
```
    src/rendering/passes/PrimitiveRenderPass.cpp
```

- [ ] **Step 5: Reconfigure + build `editor`; confirm no `Primitive` stragglers**

```powershell
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
cmake --build --preset msvc-win64-vs2026-community --target test_debugdraw
./out/build/msvc-win64-vs2026-community/bin/Debug/test_debugdraw.exe
```
Expected: `editor` builds clean; `test_debugdraw` prints `All debug draw tests passed.`. Then grep the repo (`src/`) for `PrimitiveRenderPass` / `primitivePass` — there must be ZERO matches left (a missed registration site is a build break; if anything remains, remove it). Note: `PrimPerDrawCB`/`PrimitiveInstance` etc. lived only inside the deleted `.cpp`, so nothing else should reference them.

- [ ] **Step 6: Commit**

```powershell
git add src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/src/rendering/imgui/RenderStatsPanel.cpp src/engine/src/rendering/Renderer.cpp src/engine/CMakeLists.txt
git rm src/engine/src/rendering/passes/PrimitiveRenderPass.h src/engine/src/rendering/passes/PrimitiveRenderPass.cpp
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "feat(editor): grid via Debug pass (editor-on toggle); delete PrimitiveRenderPass"
```

---

## Done criteria

- `test_debugdraw` prints `All debug draw tests passed.` (now T00–T06); `Engine` + `editor` build clean.
- Zero `PrimitiveRenderPass` references remain in `src/`. New pass order: ShadowDepth → GBufferFill → Lighting → Sky → Outline → Debug → UI.
- User GUI smoke: editor shows the grid (toggle in Render Stats → Debug Draw → "Grid"); the sky horizon is no longer covered by a dark slab; meshes still occlude the grid. Runtime (`runtime.exe`) shows no grid.
