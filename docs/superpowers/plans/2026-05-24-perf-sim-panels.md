# Performance & Simulation Panels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the demo "Hello, world!" panel by splitting it into a graph-driven **Performance** panel (FPS/CPU ms/GPU ms/TPS sparklines) and a **Simulation** panel (the relocated game-thread controls).

**Architecture:** A pure, unit-tested `MetricHistory<N>` rolling ring feeds `ImGui::PlotLines` in a new `PerformancePanel`; a new `SimulationPanel` hosts the Target-TPS/spin/tracking controls over the existing `GameThreadConfig` seqlock. `StatsPanel` is deleted and the dock layout + `ImGuiRenderer` rewired. Editor-only; no engine/runtime/threading change.

**Tech Stack:** C++23, Dear ImGui (`PlotLines`), CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-perf-sim-panels-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump** (editor-only; no `GameState`/export/ECS-component change).
- Commit author is the repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`, no `-c`/`--author`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.
- New files/targets require a CMake reconfigure (`cmake --preset msvc-win64-vs2026-community`) before building; edits to existing files do not.

---

### Task 1: `MetricHistory<N>` ring + `test_metrichistory` (TDD)

**Files:**
- Create: `src/editor/src/rendering/MetricHistory.h` (header-only)
- Create: `tests/test_metrichistory.cpp`
- Modify: `tests/CMakeLists.txt`

TDD: write the test, watch it fail (no header/impl), implement, watch it pass.

- [ ] **Step 1: Write the header (interface the test compiles against)**

`src/editor/src/rendering/MetricHistory.h`:
```cpp
#pragma once

#include <array>
#include <algorithm>

// Fixed-capacity rolling ring of float samples for live metric graphs. Newest overwrites oldest
// when full. Pure (no ImGui/glm) so it is unit-testable. Data()/Count()/Offset() map directly onto
// ImGui::PlotLines(values, values_count, values_offset) for a correctly-scrolling plot.
template <int N>
class MetricHistory {
    static_assert(N > 0, "MetricHistory capacity must be positive");
public:
    void Push(float v) {
        m_Data[m_Write] = v;
        m_Write = (m_Write + 1) % N;
        if (m_Count < N) ++m_Count;
    }

    int  Count()    const { return m_Count; }
    int  Capacity() const { return N; }
    // values_offset for ImGui::PlotLines: 0 until the ring wraps, then the write cursor (oldest).
    int  Offset()   const { return (m_Count < N) ? 0 : m_Write; }
    const float* Data() const { return m_Data.data(); }

    float Last() const { return m_Count == 0 ? 0.0f : m_Data[(m_Write + N - 1) % N]; }

    float Min() const {
        if (m_Count == 0) return 0.0f;
        float m = m_Data[0];
        for (int i = 1; i < m_Count; ++i) m = std::min(m, m_Data[i]);
        return m;
    }
    float Max() const {
        if (m_Count == 0) return 0.0f;
        float m = m_Data[0];
        for (int i = 1; i < m_Count; ++i) m = std::max(m, m_Data[i]);
        return m;
    }
    float Avg() const {
        if (m_Count == 0) return 0.0f;
        float s = 0.0f;
        for (int i = 0; i < m_Count; ++i) s += m_Data[i];
        return s / static_cast<float>(m_Count);
    }

private:
    std::array<float, N> m_Data{}; // zero-initialized
    int m_Count = 0;
    int m_Write = 0;
};
```
(Valid samples occupy slots `[0, m_Count)` both before the wrap — where `m_Write == m_Count` — and after, where `m_Count == N`. So Min/Max/Avg iterate `[0, m_Count)` in all cases.)

- [ ] **Step 2: Write the failing test**

`tests/test_metrichistory.cpp`:
```cpp
#include <cstdio>
#include <cmath>

#include "MetricHistory.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)
#define EXPECT_NEAR(a, b) EXPECT(std::fabs((a) - (b)) < 1e-4f)

static void T00_empty_is_safe()
{
    MetricHistory<4> h;
    EXPECT(h.Count() == 0);
    EXPECT(h.Capacity() == 4);
    EXPECT(h.Offset() == 0);
    EXPECT(h.Min() == 0.0f);
    EXPECT(h.Max() == 0.0f);
    EXPECT(h.Avg() == 0.0f);
    EXPECT(h.Last() == 0.0f);
}

static void T01_partial_fill()
{
    MetricHistory<4> h;
    h.Push(1.0f); h.Push(2.0f); h.Push(3.0f);
    EXPECT(h.Count() == 3);
    EXPECT(h.Offset() == 0);     // not wrapped yet
    EXPECT(h.Last() == 3.0f);
    EXPECT(h.Min() == 1.0f);
    EXPECT(h.Max() == 3.0f);
    EXPECT_NEAR(h.Avg(), 2.0f);
}

static void T02_exactly_full()
{
    MetricHistory<4> h;
    h.Push(1.0f); h.Push(2.0f); h.Push(3.0f); h.Push(4.0f);
    EXPECT(h.Count() == 4);
    EXPECT(h.Last() == 4.0f);
    EXPECT(h.Min() == 1.0f);
    EXPECT(h.Max() == 4.0f);
    EXPECT_NEAR(h.Avg(), 2.5f);
}

static void T03_wraps_drops_oldest()
{
    MetricHistory<4> h;
    for (float v : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) h.Push(v); // retains 3,4,5,6
    EXPECT(h.Count() == 4);
    EXPECT(h.Last() == 6.0f);
    EXPECT(h.Min() == 3.0f);      // 1,2 dropped
    EXPECT(h.Max() == 6.0f);
    EXPECT_NEAR(h.Avg(), 4.5f);
    EXPECT(h.Offset() == 2);      // write cursor = oldest slot after wrap
}

int main()
{
    T00_empty_is_safe();
    T01_partial_fill();
    T02_exactly_full();
    T03_wraps_drops_oldest();

    if (g_Failures == 0) { std::printf("All metric history tests passed.\n"); return 0; }
    std::printf("%d metric history test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_metrichistory` CMake target**

In `tests/CMakeLists.txt`, after the `test_picking` block (and the `test_editorcam` block if present), append:
```cmake
add_executable(test_metrichistory
    test_metrichistory.cpp
)

target_include_directories(test_metrichistory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

set_target_properties(test_metrichistory PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(No `glm` / GLM defines — `MetricHistory` is pure float math.)

- [ ] **Step 4: Reconfigure + build → expect FAIL**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_metrichistory
```
Expected: fails — before the header exists it cannot find `MetricHistory.h` (CMake validates listed sources, or the compile errors on the missing include). Confirm it fails for that reason. (If you wrote Step 1's header before this step, instead temporarily expect a clean build; the intent is TDD ordering — prefer writing the test first.)

- [ ] **Step 5: Ensure the header from Step 1 exists, then build → expect PASS**

```
cmake --build out/build/msvc-win64-vs2026-community --target test_metrichistory
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
```
Expected: `All metric history tests passed.` If any assertion fails, fix the **header** (not the test — the assertions encode the spec).

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/rendering/MetricHistory.h tests/test_metrichistory.cpp tests/CMakeLists.txt
git commit -m "feat: add MetricHistory rolling ring + test_metrichistory"
```

---

### Task 2: `PerformancePanel`

**Files:**
- Create: `src/editor/src/rendering/imgui/PerformancePanel.h`
- Create: `src/editor/src/rendering/imgui/PerformancePanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

No unit test (ImGui draw glue); verified by a clean editor build + the GUI smoke. The panel is created here but not yet wired into `ImGuiRenderer` (Task 4) — it compiles standalone.

- [ ] **Step 1: Write `PerformancePanel.h`**

`src/editor/src/rendering/imgui/PerformancePanel.h`:
```cpp
#pragma once

#include "MetricHistory.h"

struct EditorContext;

// Read-only live performance graphs: FPS, CPU frame time, GPU time, TPS. Samples one value per
// metric each Draw into a rolling MetricHistory and plots it with ImGui::PlotLines.
class PerformancePanel {
public:
    void Draw(const EditorContext& ctx);

private:
    static constexpr int kSamples = 240; // ~4 s at 60 fps
    MetricHistory<kSamples> m_Fps;
    MetricHistory<kSamples> m_CpuMs;
    MetricHistory<kSamples> m_GpuMs;
    MetricHistory<kSamples> m_Tps;
};
```

- [ ] **Step 2: Write `PerformancePanel.cpp`**

`src/editor/src/rendering/imgui/PerformancePanel.cpp`:
```cpp
#include "PerformancePanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include <cstdio>
#include <cfloat>

#include "ApplicationContext.h"

namespace {
// Push the latest sample, then draw "<header>" + a scrolling sparkline with a cur/min/max/avg
// overlay. `id` is a hidden ImGui id (e.g. "##fps") so the four plots don't collide.
template <int N>
void DrawMetric(const char* header, const char* id, MetricHistory<N>& h, float current)
{
    h.Push(current);
    char overlay[96];
    std::snprintf(overlay, sizeof(overlay), "cur %.2f   min %.2f   max %.2f   avg %.2f",
                  h.Last(), h.Min(), h.Max(), h.Avg());
    ImGui::TextUnformatted(header);
    ImGui::PlotLines(id, h.Data(), h.Count(), h.Offset(), overlay,
                     FLT_MAX, FLT_MAX, ImVec2(-1.0f, 40.0f)); // auto-scale, full width, 40px tall
}
} // namespace

void PerformancePanel::Draw(const EditorContext& ctx)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Begin("Performance");

    const float fps   = io.Framerate;
    const float cpuMs = io.DeltaTime * 1000.0f;
    const float gpuMs = ctx.GpuFrameTimeMs;
    const float tps   = ctx.Snapshot ? static_cast<float>(ctx.Snapshot->ActualTPS) : 0.0f;

    DrawMetric("FPS",    "##fps", m_Fps,   fps);
    ImGui::Separator();
    DrawMetric("CPU ms", "##cpu", m_CpuMs, cpuMs);
    ImGui::Separator();
    DrawMetric("GPU ms", "##gpu", m_GpuMs, gpuMs);
    ImGui::Separator();

    char tpsHeader[48];
    std::snprintf(tpsHeader, sizeof(tpsHeader), "TPS (target %.0f)",
                  ctx.Snapshot ? ctx.Snapshot->TargetTPS : 0.0);
    DrawMetric(tpsHeader, "##tps", m_Tps, tps);

    ImGui::End();
}
```

- [ ] **Step 3: Add the source to the editor target**

In `src/editor/CMakeLists.txt`, in the editor source list (the `add_executable(editor ...)` block), add after `src/rendering/imgui/RenderStatsPanel.cpp`:
```cmake
    src/rendering/imgui/PerformancePanel.cpp
```
(`MetricHistory.h` is header-only — no source entry.)

- [ ] **Step 4: Reconfigure + build the editor**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean (the class is unused until Task 4, which is fine).

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/PerformancePanel.h src/editor/src/rendering/imgui/PerformancePanel.cpp src/editor/CMakeLists.txt
git commit -m "feat: add PerformancePanel (FPS/CPU/GPU/TPS live graphs)"
```

---

### Task 3: `SimulationPanel`

**Files:**
- Create: `src/editor/src/rendering/imgui/SimulationPanel.h`
- Create: `src/editor/src/rendering/imgui/SimulationPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`

No unit test (ImGui glue). This is a straight move of the working game-thread controls from `StatsPanel` (same `GameThreadConfig` seqlock round-trip) minus the dead "Reset Stats" button. Created here, wired in Task 4.

- [ ] **Step 1: Write `SimulationPanel.h`**

`src/editor/src/rendering/imgui/SimulationPanel.h`:
```cpp
#pragma once

struct EditorContext;

// Game-thread pacing controls (relocated from the old Hello panel): Target TPS presets + slider,
// spin-threshold slider, and the frame-time-tracking toggle. Reads/writes App->GameThreadConfig.
class SimulationPanel {
public:
    void Draw(const EditorContext& ctx);
};
```

- [ ] **Step 2: Write `SimulationPanel.cpp`**

`src/editor/src/rendering/imgui/SimulationPanel.cpp`:
```cpp
#include "SimulationPanel.h"
#include "EditorContext.h"

#include <imgui.h>
#include <cstdint>

#include "ApplicationContext.h"

void SimulationPanel::Draw(const EditorContext& ctx)
{
    ImGui::Begin("Simulation");

    GameThreadSettings settings = ctx.App->GameThreadConfig.load();
    bool settingsChanged = false;

    // Target TPS: presets + slider.
    ImGui::Text("Target TPS:");
    ImGui::SameLine();
    if (ImGui::Button("60"))  { settings.TargetTPS = 60.0;  settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("120")) { settings.TargetTPS = 120.0; settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("144")) { settings.TargetTPS = 144.0; settingsChanged = true; }
    ImGui::SameLine();
    if (ImGui::Button("165")) { settings.TargetTPS = 165.0; settingsChanged = true; }

    float targetTpsFloat = static_cast<float>(settings.TargetTPS);
    if (ImGui::SliderFloat("##TargetTPS", &targetTpsFloat, 60.0f, 240.0f, "%.1f Hz")) {
        settings.TargetTPS = static_cast<double>(targetTpsFloat);
        settingsChanged = true;
    }

    // Spin threshold (microseconds).
    int spinThresholdInt = static_cast<int>(settings.SpinThresholdMicros);
    if (ImGui::SliderInt("Spin Threshold (us)", &spinThresholdInt, 0, 2000, "%d us")) {
        settings.SpinThresholdMicros = static_cast<uint32_t>(spinThresholdInt);
        settingsChanged = true;
    }
    ImGui::TextWrapped("Lower = more accurate timing, higher CPU usage during spin");

    // Frame-time tracking toggle.
    if (ImGui::Checkbox("Enable Frame Time Tracking", &settings.EnableFrameTimeTracking)) {
        settingsChanged = true;
    }

    if (settingsChanged) {
        ctx.App->GameThreadConfig.store(settings);
    }

    ImGui::End();
}
```

- [ ] **Step 3: Add the source to the editor target**

In `src/editor/CMakeLists.txt`, add after the `PerformancePanel.cpp` line from Task 2:
```cmake
    src/rendering/imgui/SimulationPanel.cpp
```

- [ ] **Step 4: Reconfigure + build the editor**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/SimulationPanel.h src/editor/src/rendering/imgui/SimulationPanel.cpp src/editor/CMakeLists.txt
git commit -m "feat: add SimulationPanel (game-thread pacing controls)"
```

---

### Task 4: Wire the new panels, delete `StatsPanel`

**Files:**
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.h`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`
- Modify: `src/editor/CMakeLists.txt`
- Delete: `src/editor/src/rendering/imgui/StatsPanel.h`
- Delete: `src/editor/src/rendering/imgui/StatsPanel.cpp`

No unit test (wiring); verified by clean editor+runtime build, the full regression suite, and a grep showing no `StatsPanel`/`"Hello, world!"` references remain.

- [ ] **Step 1: Swap includes + members in `ImGuiRenderer.h`**

Replace the line `#include "StatsPanel.h"` with:
```cpp
#include "PerformancePanel.h"
#include "SimulationPanel.h"
```
Replace the member line `StatsPanel m_StatsPanel;` with:
```cpp
    PerformancePanel m_Performance;
    SimulationPanel  m_Simulation;
```

- [ ] **Step 2: Swap the draw calls in `ImGuiRenderer.cpp`**

Find the line `m_StatsPanel.Draw(ctx);` and replace it with:
```cpp
        m_Performance.Draw(ctx);
        m_Simulation.Draw(ctx);
```

- [ ] **Step 3: Update the default dock layout**

In `ImGuiRenderer.cpp`, in `BuildDefaultDockLayout`, replace the line:
```cpp
    ImGui::DockBuilderDockWindow("Hello, world!",          leftBottom);
```
with:
```cpp
    ImGui::DockBuilderDockWindow("Performance",            leftBottom);
    ImGui::DockBuilderDockWindow("Simulation",             leftBottom);
```

- [ ] **Step 4: Remove `StatsPanel.cpp` from the editor target + delete the files**

In `src/editor/CMakeLists.txt`, remove the line:
```cmake
    src/rendering/imgui/StatsPanel.cpp
```
Delete the files:
```bash
git rm src/editor/src/rendering/imgui/StatsPanel.h src/editor/src/rendering/imgui/StatsPanel.cpp
```

- [ ] **Step 5: Reconfigure, build editor + runtime, run the full regression suite**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking test_editorcam test_metrichistory
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
```
Expected: all build clean; every test prints its `All ... passed.` line (test_alloc prints one intentional ERROR line before its pass).

- [ ] **Step 6: Confirm no stale references remain**

Grep the source tree (excluding `docs/` and build dirs) for `StatsPanel` and `Hello, world!` — expect zero matches in `src/`.

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/rendering/imgui/ImGuiRenderer.h src/editor/src/rendering/imgui/ImGuiRenderer.cpp src/editor/CMakeLists.txt src/editor/src/rendering/imgui/StatsPanel.h src/editor/src/rendering/imgui/StatsPanel.cpp
git commit -m "feat: wire Performance + Simulation panels, retire the Hello panel"
```
(`git rm` already staged the deletions; `git add` stages the edits. Verify `git status` shows only these five paths.)

---

## Final verification (after all tasks)

- [ ] Full build: `cmake --build out/build/msvc-win64-vs2026-community` — all targets green (editor, runtime, game, all test_*).
- [ ] All unit tests print their pass line:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
```
Expected: `All ... passed.` from each (test_alloc prints one intentional ERROR line first).

- [ ] **GUI smoke (user-run; surface to the user — do not self-approve):**
  - No "Hello, world!" window exists.
  - **Performance** panel: four sparklines (FPS, CPU ms, GPU ms, TPS), each scrolling with a live `cur/min/max/avg` overlay; TPS header shows the target.
  - **Simulation** panel: Target TPS presets (60/120/144/165) + slider, Spin Threshold slider, Enable Frame Time Tracking toggle — all change game pacing exactly as the old Hello panel did (watch TPS in the Performance panel react).
  - On a fresh layout / "View → Reset Layout", both new panels dock where Hello used to be. (Users with an existing `imgui.ini` keep their old layout — expected.)
  - `runtime.exe` unaffected.

## Notes / non-goals
- No `GAME_API_VERSION` bump (editor-only).
- One series per plot (no overlaid multi-series); auto-scaled; ~240-sample (~4 s) window.
- Render Stats + Memory panels unchanged. The dead "Reset Stats" button is dropped, not relocated.
- Graph history is render-side + session-only (not persisted). Existing-layout users won't see the new docks until they reset the layout — acceptable.
