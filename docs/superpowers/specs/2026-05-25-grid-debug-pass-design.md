# Move the Ground Grid to the Debug Pass — Design

**Date:** 2026-05-25
**Status:** Approved (design)

## Problem

The editor ground grid is drawn by `PrimitiveRenderPass`
(`src/engine/src/rendering/passes/PrimitiveRenderPass.cpp`) as a **giant
4000×4000 opaque plane** at Y=0 (`S = 2000.0f`, two triangles) with a procedural
grid + axis shader. Two problems:

1. **It ships in the runtime player.** `PrimitiveRenderPass` is registered in the
   engine-core pass list in both `Renderer::Init` and `Renderer::InitForSwap`, which
   `editor.exe` and `runtime.exe` both use. The grid is an editor authoring aid and
   should not appear in the shipped runtime.
2. **It clobbers the skybox horizon.** The pixel shader returns
   `float4(color * fade, 1.0)` — alpha is always 1.0 and the pipeline has **no
   blending** (opaque). Beyond the grid's distance fade, `color * fade → ~black`, so
   the plane writes an **opaque dark slab** across its whole footprint. Pass order is
   `… Lighting → Sky → Primitive …`, so the plane depth-tests in front of the
   far-plane sky and overwrites the skybox below the horizon.

## Goal

Replace the opaque-plane grid with a **line-based grid drawn by `DebugRenderPass`**,
gated by a new `ShowGrid` debug flag that is on in the editor and off in the runtime,
and **delete `PrimitiveRenderPass` entirely** (it only ever drew the grid). This
removes the grid from the runtime and eliminates the horizon clobber (thin lines, no
opaque fill; a finite camera-snapped patch never reaches the horizon).

## Non-Goals

- No soft distance fade. The chosen scope is a crisp finite line grid; the Debug
  pipeline stays opaque (no per-vertex alpha blending). A hard edge at the grid extent
  is acceptable (raise the extent if it feels close).
- No new per-grid editor tunables beyond an on/off checkbox (extent/step are fixed
  constants for now — YAGNI).
- No `ECS.h` / `GAME_API_VERSION` changes.

## Architecture

### 1. Flag — `DebugDrawSettings` (`src/engine/src/rendering/RenderStats.h`)

Add a field, defaulting **off** (matching the other debug flags, so the runtime —
which has no editor panel to flip it — draws nothing):

```cpp
struct DebugDrawSettings {
    bool ShowLightGizmos   = false;
    bool ShowCameraFrustum = false;
    bool ShowSelectedAABB  = false;
    bool Wireframe         = false;
    bool ShowGrid          = false;
};
```

### 2. Builder — `DebugDraw.h` (`src/engine/src/rendering/DebugDraw.h`)

Add a pure inline builder that appends line segments (same pattern as the existing
`DebugAppendBox`/`DebugAppendSphere`/etc., built on `DebugAppendLine`):

```cpp
// Camera-snapped ground grid on Y=0. The grid is centered on the camera's XZ,
// snapped to `step`, so a finite patch follows the camera (feels infinite and never
// reaches the true horizon). World axis lines (X==0 / Z==0) are drawn in axis colors
// when they fall inside the patch; all other lines use gridColor.
inline void DebugAppendGrid(std::vector<DebugVertex>& out,
                            const glm::vec3& cameraPos,
                            float halfExtent, float step,
                            const glm::vec4& gridColor,
                            const glm::vec4& axisXColor,
                            const glm::vec4& axisZColor);
```

Behavior:
- `step = max(step, 1e-3f)`; `halfExtent = max(halfExtent, step)`.
- Snap the patch center to the grid: `cx = round(cameraPos.x / step) * step`,
  `cz = round(cameraPos.z / step) * step`.
- Patch spans `[cx - halfExtent, cx + halfExtent] × [cz - halfExtent, cz + halfExtent]`.
- For each grid line coordinate `gx` from `cx - halfExtent` to `cx + halfExtent` in
  `step` increments, emit a line from `(gx, 0, cz - halfExtent)` to
  `(gx, 0, cz + halfExtent)`. The color is `axisZColor` if `gx == 0` (the world Z
  axis runs along X==0), else `gridColor`. (Use an exact-zero compare after snapping;
  since coordinates are integer multiples of `step` from a snapped center, the world
  zero line is hit exactly only when `cx` is a multiple of `step` — which it is.)
- Symmetrically for lines along the other axis: for each `gz`, emit
  `(cx - halfExtent, 0, gz)` → `(cx + halfExtent, 0, gz)`, colored `axisXColor` if
  `gz == 0` (world X axis runs along Z==0), else `gridColor`.
- Use a small epsilon compare for the axis test (`fabs(coord) < 0.5f * step`) so
  floating-point exactness isn't required.

Vertex budget at defaults (`halfExtent 50`, `step 1`): ~101 lines per direction ×
2 directions × 2 verts ≈ **404 verts** — negligible.

### 3. Debug pass — `DebugRenderPass::Render` (`passes/DebugRenderPass.cpp`)

- Extend the early-return gate to include the grid:
  `if (!s.ShowLightGizmos && !s.ShowCameraFrustum && !s.ShowSelectedAABB && !s.ShowGrid) return;`
- When `s.ShowGrid`, append the grid first (so gizmos draw over it):
  ```cpp
  if (s.ShowGrid) {
      const glm::vec3 camPos = m_Renderer->GetActiveCamera().Position;
      DebugAppendGrid(m_Verts, camPos, 50.0f, 1.0f,
                      glm::vec4(0.35f, 0.35f, 0.35f, 1.0f),  // grid grey
                      glm::vec4(1.0f, 0.2f, 0.2f, 1.0f),     // X axis red
                      glm::vec4(0.2f, 0.6f, 1.0f, 1.0f));    // Z axis blue
  }
  ```
- No pipeline change: the Debug pass is already `LineList`, `depthTestEnable = true`,
  `depthWriteEnable = false`, `cullMode = None`. Grid lines depth-test against the
  G-buffer (meshes occlude them) and draw over sky only where a line is — sky/fog
  show through the gaps, and the finite ±50 patch never reaches the horizon.

`GetActiveCamera()` is already used by this pass (for the VP CB), so no new wiring.

### 4. Editor enables the grid — `ImGuiRenderer::Init` (`imgui/ImGuiRenderer.cpp`)

The editor turns the grid on once at startup (RenderThread); the runtime never does:

```cpp
// Ground grid is an editor authoring aid — on by default in the editor, never in
// the runtime (which has no overlay and leaves the flag at its default false).
GetDebugDrawSettings().ShowGrid = true;
```

Add the include if needed (`#include "RenderStats.h"`). Place it after the renderer
is wired in `Init`.

### 5. Editor toggle — `RenderStatsPanel.cpp`

In the Debug Draw section, add alongside the existing checkboxes:

```cpp
    ImGui::Checkbox("Grid", &dd.ShowGrid);
```

### 6. Delete `PrimitiveRenderPass`

- Delete `src/engine/src/rendering/passes/PrimitiveRenderPass.h` and `.cpp`.
- Remove its `#include` and the two registration blocks in `Renderer.cpp`
  (`Renderer::Init` ~line 122 and `Renderer::InitForSwap` ~line 616).
- Remove its source entry from `src/engine/CMakeLists.txt`.

New active pass order: **ShadowDepth → GBufferFill → Lighting → Sky → Outline →
Debug → UI**.

## Testing

`DebugAppendGrid` is pure (appends to a vector) → extend the existing `test_debugdraw`
target (`tests/test_debugdraw.cpp`):

1. **Vertex count:** for `halfExtent`/`step`, the number of appended verts equals the
   expected `2 lines-per-direction-set × verts` — compute lines = `(2*halfExtent/step) + 1`
   per direction, total verts = `2 * lines * 2`. Assert the vector grows by exactly that.
2. **Camera snap:** with a camera at a non-grid-aligned XZ (e.g. `x = 3.4, z = -7.8`,
   `step = 1`), assert the produced line coordinates are integer multiples of `step`
   (snapped center), i.e. the min X line is at `round(3.4) - halfExtent`.
3. **Axis color override:** when the world origin is inside the patch, assert at least
   one segment carries `axisXColor` and one carries `axisZColor`, and that a clearly
   off-axis line carries `gridColor`.

## Build impact

No `ECS.h` or `GAME_API_VERSION` change. Rebuild `Engine` + `editor`; extend and run
`test_debugdraw`. `runtime` is unaffected at the source level but no longer renders the
grid (flag stays false). No editor restart semantics beyond a normal rebuild.

## Risks / Notes

- **Hard grid edge.** Without fade, lines stop abruptly at `halfExtent`. Mitigation:
  the default 50-unit camera-snapped patch keeps the edge in the periphery; raise
  `halfExtent` if it reads poorly.
- **Axis exactness.** The axis-color test uses an epsilon (`fabs(coord) < 0.5*step`)
  rather than `== 0` to avoid float-compare fragility.
- **Pass-removal completeness.** After deleting `PrimitiveRenderPass`, grep the repo
  for any lingering reference (`Primitive`), and confirm both `Init` and `InitForSwap`
  registration sites are removed (a missed one is a build break, not silent).
