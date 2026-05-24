# Performance & Simulation Panels (retire the Hello panel) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `perf-sim-panels`).

## Goal

Retire the demo "Hello, world!" panel (`StatsPanel`) by splitting its contents into two focused,
better-UX editor panels:
- a read-only **Performance** panel with live rolling **graphs** + readouts for FPS, CPU frame time,
  GPU time, and TPS;
- a **Simulation** panel holding the game-thread settings controls relocated from the Hello panel.

The existing **Render Stats** and **Memory** panels are unchanged. Editor-only; no runtime/engine
threading change.

## Background (verified)

- **The Hello panel** = `StatsPanel` (`src/editor/src/rendering/imgui/StatsPanel.{h,cpp}`), a class
  with `void Draw(const EditorContext&)`, drawn unconditionally as `m_StatsPanel.Draw(ctx)`
  (`ImGuiRenderer.cpp:413`), window title `"Hello, world!"`. It currently shows, in one window:
  - **Read-only metrics:** renderer ms/FPS (`io.Framerate`), GPU ms (`ctx.GpuFrameTimeMs`), Game TPS
    (`ctx.Snapshot->ActualTPS`/`TargetTPS`), and frame-time **Min/Max/Avg + SampleCount**
    (`ctx.Snapshot->FrameStats`).
  - **Game-thread settings controls** (write to `ctx.App->GameThreadConfig`, a
    `Seqlock<GameThreadSettings>`): Target TPS preset buttons (60/120/144/165) + a slider
    (60–240 Hz); Spin Threshold slider (0–2000 µs); Enable Frame Time Tracking checkbox.
  - A **dead "Reset Stats" button** (empty body — no-op).
- **`GameThreadSettings`** (`ApplicationContext.h:24-28`): `double TargetTPS`,
  `uint32_t SpinThresholdMicros`, `bool EnableFrameTimeTracking`. Accessed via
  `Seqlock<GameThreadSettings> GameThreadConfig` — `.load()` / `.store()`.
- **Metric sources, all reachable render-side per frame:**
  - FPS / CPU frame time: `ImGui::GetIO().Framerate` and `io.DeltaTime` (render thread).
  - GPU time: `EditorContext.GpuFrameTimeMs` (set from the `Renderer` `GpuTimer`).
  - TPS: `EditorContext.Snapshot->ActualTPS` + `->TargetTPS` (published by the GameThread each tick).
- **No rolling history exists** — `FrameTimeStats` holds only Min/Max/Avg aggregates, and there is
  **no `ImGui::PlotLines`/`PlotHistogram` use anywhere** in the editor. A per-frame ring buffer must
  be added (render-side, in the panel).
- **Panel registration** (`ImGuiRenderer`): class panels are members drawn unconditionally
  (`m_StatsPanel`, `m_EcsInspector`, `m_MeshManager`, `m_MaterialManager`); `Memory` and
  `Render Stats` are free functions with a `bool* open`. Default dock layout
  (`ImGuiRenderer.cpp` `BuildDefaultDockLayout`) docks `"Hello, world!"` into `leftBottom` via
  `ImGui::DockBuilderDockWindow("Hello, world!", leftBottom)`.
- **Render Stats** (`RenderStatsPanel.cpp`): frustum-cull toggle + mesh/instance/batch counts —
  out of scope here, unchanged. **Memory** panel — unchanged.
- Editor sources are listed explicitly in `src/editor/CMakeLists.txt` (includes
  `src/rendering/imgui/StatsPanel.cpp`). Tests are explicit targets in `tests/CMakeLists.txt`
  (e.g. `test_picking`, `test_editorcam`) linking `glm::glm`, output to `RUNTIME_DIR`, `FOLDER Tests`.

## Scope

**In scope:**
1. A pure, unit-tested `MetricHistory` ring helper (push + windowed min/max/avg).
2. A new `PerformancePanel` class: 4 stacked sparklines (FPS, CPU ms, GPU ms, TPS) + per-metric
   `current · min/max/avg` readouts, fed by `MetricHistory` sampled each `Draw`.
3. A new `SimulationPanel` class: the relocated game-thread controls (Target TPS presets + slider,
   Spin Threshold slider, Frame-Time-Tracking toggle) over `GameThreadConfig`.
4. Delete `StatsPanel.{h,cpp}` + all its registration; drop the dead "Reset Stats" button.
5. Re-wire `ImGuiRenderer` + the default dock layout (`"Performance"` + `"Simulation"` replace
   `"Hello, world!"`).
6. CMake: add the two panel sources + `MetricHistory` to the editor target; add a `test_metrichistory`
   target.

**Out of scope / non-goals:** changes to Render Stats / Memory; per-panel show/hide menu items
(new panels are always-drawn like the other class panels); combined/overlaid multi-series graphs
(one series per plot); graphing the game-thread `FrameStats` aggregates as a series (TPS already
covers sim pacing; the aggregates remain available but the graphs sample render-side per frame);
persisting graph history or settings; any `ApplicationContext`/engine/runtime change. **No
`GAME_API_VERSION` bump.**

## Design

### 1. `MetricHistory` (pure, unit-tested)

New `src/editor/src/rendering/MetricHistory.h` (header-only is fine; pure C++/std, no ImGui/glm):
- Fixed-capacity rolling ring of `float` samples (capacity a ctor arg or template/`constexpr`,
  default 240 ≈ 4 s at 60 fps).
- `void Push(float v)` — append, overwriting the oldest when full.
- Accessors for plotting + readouts: contiguous sample access for `ImGui::PlotLines`
  (either expose a `const float*` + count with the ring laid out for plotting, or a `Latest()`
  ordered copy), plus `Min()`, `Max()`, `Avg()`, `Last()`, `Count()` over the current window.
- Deterministic, no globals → unit-testable.

Because `ImGui::PlotLines` wants a flat array with a `values_offset`, the simplest robust approach:
store samples in a `std::array<float, N>` with a write index and pass
`PlotLines(label, data, N, writeIndex, ...)` (ImGui handles the offset wrap). `Min/Max/Avg` iterate
the filled portion.

### 2. `PerformancePanel` (class)

New `src/editor/src/rendering/imgui/PerformancePanel.{h,cpp}`:
- Members: one `MetricHistory` per metric (FPS, CPU ms, GPU ms, TPS).
- `void Draw(const EditorContext& ctx)`:
  - `ImGui::Begin("Performance")`.
  - Sample once per call: `fps = io.Framerate`; `cpuMs = io.DeltaTime * 1000.0f`;
    `gpuMs = ctx.GpuFrameTimeMs`; `tps = (float)ctx.Snapshot->ActualTPS` (guard null `Snapshot`).
    `Push` each into its history.
  - For each metric: a label line `"<name>  <current>   min/max/avg"` then
    `ImGui::PlotLines("##<name>", ...)` with an auto/fixed scale (FPS & TPS auto-min0; ms graphs
    auto). Show TPS target alongside (`ctx.Snapshot->TargetTPS`).
  - `ImGui::End()`.
- No state beyond the histories; no engine interaction.

### 3. `SimulationPanel` (class)

New `src/editor/src/rendering/imgui/SimulationPanel.{h,cpp}`:
- `void Draw(const EditorContext& ctx)`:
  - `ImGui::Begin("Simulation")`.
  - `GameThreadSettings s = ctx.App->GameThreadConfig.load();` track a `changed` flag.
  - Target TPS: preset buttons 60/120/144/165 + `SliderFloat` 60–240 Hz (same logic as the Hello
    panel today).
  - Spin Threshold: `SliderInt` 0–2000 µs + the explanatory `TextWrapped`.
  - `Checkbox("Enable Frame Time Tracking", &s.EnableFrameTimeTracking)`.
  - If `changed`: `ctx.App->GameThreadConfig.store(s);`.
  - `ImGui::End()`.
- This is a straight move of the existing, working controls — same seqlock round-trip, no behavior
  change. The dead "Reset Stats" button is **not** carried over.

### 4. Wiring (`ImGuiRenderer`)

- `ImGuiRenderer.h`: replace `StatsPanel m_StatsPanel;` with `PerformancePanel m_Performance;` and
  add `SimulationPanel m_Simulation;`; swap the `#include "StatsPanel.h"` for the two new headers.
- `ImGuiRenderer.cpp`: replace the `m_StatsPanel.Draw(ctx);` call with `m_Performance.Draw(ctx);`
  and `m_Simulation.Draw(ctx);`.
- `BuildDefaultDockLayout`: replace
  `DockBuilderDockWindow("Hello, world!", leftBottom)` with
  `DockBuilderDockWindow("Performance", leftBottom)` and
  `DockBuilderDockWindow("Simulation", leftBottom)` (both into `leftBottom`, tabbed where Hello was).
- Delete `StatsPanel.h` + `StatsPanel.cpp`; remove `src/rendering/imgui/StatsPanel.cpp` from
  `src/editor/CMakeLists.txt`; add the two panel `.cpp`s. `MetricHistory.h` is header-only (no
  `.cpp`, no CMake source entry — it is `#include`d by the panel + the test).

### 5. CMake

- `src/editor/CMakeLists.txt`: drop `StatsPanel.cpp`; add `src/rendering/imgui/PerformancePanel.cpp`
  and `src/rendering/imgui/SimulationPanel.cpp`.
- `tests/CMakeLists.txt`: new `test_metrichistory` target compiling only `tests/test_metrichistory.cpp`
  (header-only `MetricHistory`, pure float math — no `glm` link needed); include dir
  `${CMAKE_SOURCE_DIR}/src/editor/src/rendering`; `RUNTIME_OUTPUT_DIRECTORY RUNTIME_DIR`;
  `FOLDER Tests`.

## Data flow

Each editor frame (RenderThread), `ImGuiRenderer::Render` builds `EditorContext ctx` (already has
`Snapshot`, `GpuFrameTimeMs`, `App`). `PerformancePanel::Draw` reads `io`/`ctx`, pushes one sample
per metric into its `MetricHistory` rings, and plots them. `SimulationPanel::Draw` loads/stores
`GameThreadConfig` (Seqlock) — the GameThread picks up changes next tick, exactly as the Hello panel
did. No new cross-thread channel.

## Build / verification

Build preset `msvc-win64-vs2026-community`. No `GAME_API_VERSION` bump.

- **Unit test (`test_metrichistory`)**: push fewer-than-capacity then assert `Count`, `Min`, `Max`,
  `Avg`, `Last`; push more-than-capacity and assert it wraps (drops oldest, `Count==capacity`, the
  window min/max/avg reflect only the retained samples); empty-history accessors are safe (no div-by-
  zero). Prints `All metric history tests passed.`
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum`/`test_input`/
  `test_picking`/`test_editorcam` stay green; no `StatsPanel`/`"Hello, world!"` references remain
  (grep clean).
- **GUI smoke (user-run):** "Performance" panel shows four live sparklines (FPS, CPU ms, GPU ms, TPS)
  with current + min/max/avg that update and scroll; "Simulation" panel's Target TPS presets/slider,
  Spin Threshold, and tracking toggle change game pacing exactly as the old Hello panel did; no
  "Hello, world!" window; default layout docks both new panels where Hello was; `runtime.exe`
  unaffected.

## Risks

- **Lost functionality on delete** — the relocate must carry over every *useful* Hello control
  (Target TPS presets + slider, Spin Threshold, tracking toggle) with identical `GameThreadConfig`
  semantics; only the dead "Reset Stats" button is dropped. Mitigated by diffing the moved controls
  against the original.
- **PlotLines offset/scale** — using the ring write-index as `values_offset` so the plot scrolls
  correctly; auto-scale guarded against an all-zero/empty window (fixed fallback range).
- **Null `Snapshot`** — guard the TPS sample when `ctx.Snapshot` is null (first frame) to avoid a
  deref.
- **Stale dock layout** — users with an existing `imgui.ini` keep their old layout (the new docks
  only apply on a fresh layout / "Reset Layout"); acceptable, noted for the smoke test.
