# Editor Folder Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move editor tooling out of the misleading `src/editor/src/rendering/imgui/` tree into responsibility-named topic dirs (`app/ panels/ viewport/ imgui/ preview/ dialogs/ util/`) directly under `src/editor/src/`, with zero behavior change.

**Architecture:** Pure mechanical `git mv` + CMake rewire. Because a half-moved tree won't compile (cross-file includes + CMake source paths), ALL moves + both `CMakeLists.txt` edits land in ONE commit so the build is green at every commit. Include strategy A: add every topic dir to the editor target's include path, leaving all flat `#include "Foo.h"` lines untouched.

**Tech Stack:** CMake, C++23, git mv (history-preserving). No code logic changes.

**Spec reference:** `docs/superpowers/specs/2026-05-28-editor-reorg-structure-design.md` (commit `ff2715f`).

---

## Codebase orientation (read once before Task 1)

- **Branch `feat/editor-reorg-structure` already exists**, spec committed at `ff2715f`.
- **Current editor source root:** `src/editor/src/`. Tooling lives under `src/editor/src/rendering/imgui/` (panels, dialogs, viewport, imgui backend, preview) + `src/editor/src/rendering/` (EditorCamera, EditorCameraState, MetricHistory, TransientStatus). `main.cpp`, `alloc.h`, `EditorPreferences.{cpp,h}` sit at `src/editor/src/` root.
- **Why one atomic commit:** `ImGuiRenderer` (moving to `app/`) `#include`s `SceneViewport.h`, `EditorCamera.h` (→ `viewport/`), `MeshPreviewRenderer.h` (→ `preview/`), `ViewportPicker.h` (→ `viewport/`). These only resolve once every topic dir is on the include path. Moving in partial batches would break intermediate builds. So: move everything + rewrite CMake + build, all before the single commit.
- **Include resolution today:** `target_include_directories(editor PRIVATE src src/rendering)` — note `src/rendering/imgui` is NOT listed; files there include siblings via same-dir quoted-include lookup. After the move, same-dir lookup no longer spans the new dir boundaries, so strategy A adds all 7 topic dirs to the path.
- **`git mv` does NOT create destination directories** — `mkdir` the 7 target dirs first, or git mv fails with "destination directory does not exist".
- **Windows case-insensitive FS:** use exact existing filename casing in every `git mv`; verify with `git status` after the moves.
- **4 tests reference editor src paths** (`tests/CMakeLists.txt`):
  - `test_editorcam` (line ~109): compiles `src/editor/src/rendering/EditorCamera.cpp` + include dir `src/editor/src/rendering`.
  - `test_metrichistory` (line ~137): include dir `src/editor/src/rendering` (for `MetricHistory.h`).
  - `test_transientstatus` (line ~150): include dir `src/editor/src/rendering` (for `TransientStatus.h`).
  - `test_editorprefs` (line ~257-258): include dirs `src/editor/src` + `src/editor/src/rendering`. It `#include "EditorPreferences.h"` (→ `app/`), which transitively `#include "EditorCameraState.h"` (→ `viewport/`). **So this test needs BOTH `app/` AND `viewport/` on its path — the spec table under-specified this (listed only `app/`).**
- **Reconfigure required:** CMake caches source paths. After moving + editing CMakeLists, run `cmake --preset ...` (full reconfigure), not just `cmake --build`.
- **Complete file inventory (25 units):**
  - `app/`: `EditorPreferences.{cpp,h}` (from `src/`), `ImGuiRenderer.{cpp,h}`, `ImGuiOverlay.{cpp,h}`, `EditorContext.h`
  - `panels/`: `EcsInspectorPanel.{cpp,h}`, `MaterialManagerPanel.{cpp,h}`, `MeshManagerPanel.{cpp,h}`, `NavigationPanel.{cpp,h}`, `MemoryPanel.{cpp,h}`, `DayNightPanel.{cpp,h}`, `RenderStatsPanel.{cpp,h}`, `SimulationPanel.{cpp,h}`, `PerformancePanel.{cpp,h}`, `MainMenuBar.{cpp,h}`
  - `viewport/`: `SceneViewport.{cpp,h}`, `ViewportPicker.{cpp,h}`, `GizmoController.{cpp,h}`, `EditorCamera.{cpp,h}` (from `src/rendering/`), `EditorCameraState.h` (from `src/rendering/`)
  - `imgui/`: `imgui_nvrhi.{cpp,h}`, `registered_font.{cpp,h}`
  - `preview/`: `MeshPreviewRenderer.{cpp,h}`
  - `dialogs/`: `EditorFileDialog.{cpp,h}`
  - `util/`: `MetricHistory.h` (from `src/rendering/`), `TransientStatus.h` (from `src/rendering/`)

---

## Task 0: Verify branch state

**Files:** none (git only)

- [ ] **Step 1: Confirm branch + clean tree**

```bash
git status -sb
# Expected: "## feat/editor-reorg-structure" — clean (.claude/ untracked OK).
git log --oneline -1
# Expected: ff2715f docs(editor): design spec for folder restructure (Spec A)
```

- [ ] **Step 2: Baseline build (sanity — confirm editor compiles BEFORE the move)**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug 2>&1 | tail -3
```

Expected: clean build. This is the "before" reference — the editor must behave identically after the move.

---

## Task 1: Move all editor files + rewire CMake (single green commit)

**Files:**
- Move: 25 file-units (see inventory above) via `git mv`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create the 7 topic directories**

```bash
cd C:/dev/clang-examples/src/editor/src
mkdir -p app panels viewport imgui preview dialogs util
```

- [ ] **Step 2: git mv the `app/` group**

```bash
cd C:/dev/clang-examples
git mv src/editor/src/EditorPreferences.cpp           src/editor/src/app/EditorPreferences.cpp
git mv src/editor/src/EditorPreferences.h             src/editor/src/app/EditorPreferences.h
git mv src/editor/src/rendering/imgui/ImGuiRenderer.cpp  src/editor/src/app/ImGuiRenderer.cpp
git mv src/editor/src/rendering/imgui/ImGuiRenderer.h    src/editor/src/app/ImGuiRenderer.h
git mv src/editor/src/rendering/imgui/ImGuiOverlay.cpp   src/editor/src/app/ImGuiOverlay.cpp
git mv src/editor/src/rendering/imgui/ImGuiOverlay.h     src/editor/src/app/ImGuiOverlay.h
git mv src/editor/src/rendering/imgui/EditorContext.h    src/editor/src/app/EditorContext.h
```

- [ ] **Step 3: git mv the `panels/` group**

```bash
git mv src/editor/src/rendering/imgui/EcsInspectorPanel.cpp     src/editor/src/panels/EcsInspectorPanel.cpp
git mv src/editor/src/rendering/imgui/EcsInspectorPanel.h       src/editor/src/panels/EcsInspectorPanel.h
git mv src/editor/src/rendering/imgui/MaterialManagerPanel.cpp  src/editor/src/panels/MaterialManagerPanel.cpp
git mv src/editor/src/rendering/imgui/MaterialManagerPanel.h    src/editor/src/panels/MaterialManagerPanel.h
git mv src/editor/src/rendering/imgui/MeshManagerPanel.cpp      src/editor/src/panels/MeshManagerPanel.cpp
git mv src/editor/src/rendering/imgui/MeshManagerPanel.h        src/editor/src/panels/MeshManagerPanel.h
git mv src/editor/src/rendering/imgui/NavigationPanel.cpp       src/editor/src/panels/NavigationPanel.cpp
git mv src/editor/src/rendering/imgui/NavigationPanel.h         src/editor/src/panels/NavigationPanel.h
git mv src/editor/src/rendering/imgui/MemoryPanel.cpp           src/editor/src/panels/MemoryPanel.cpp
git mv src/editor/src/rendering/imgui/MemoryPanel.h             src/editor/src/panels/MemoryPanel.h
git mv src/editor/src/rendering/imgui/DayNightPanel.cpp         src/editor/src/panels/DayNightPanel.cpp
git mv src/editor/src/rendering/imgui/DayNightPanel.h           src/editor/src/panels/DayNightPanel.h
git mv src/editor/src/rendering/imgui/RenderStatsPanel.cpp      src/editor/src/panels/RenderStatsPanel.cpp
git mv src/editor/src/rendering/imgui/RenderStatsPanel.h        src/editor/src/panels/RenderStatsPanel.h
git mv src/editor/src/rendering/imgui/SimulationPanel.cpp       src/editor/src/panels/SimulationPanel.cpp
git mv src/editor/src/rendering/imgui/SimulationPanel.h         src/editor/src/panels/SimulationPanel.h
git mv src/editor/src/rendering/imgui/PerformancePanel.cpp      src/editor/src/panels/PerformancePanel.cpp
git mv src/editor/src/rendering/imgui/PerformancePanel.h        src/editor/src/panels/PerformancePanel.h
git mv src/editor/src/rendering/imgui/MainMenuBar.cpp           src/editor/src/panels/MainMenuBar.cpp
git mv src/editor/src/rendering/imgui/MainMenuBar.h             src/editor/src/panels/MainMenuBar.h
```

- [ ] **Step 4: git mv the `viewport/` group**

```bash
git mv src/editor/src/rendering/imgui/SceneViewport.cpp     src/editor/src/viewport/SceneViewport.cpp
git mv src/editor/src/rendering/imgui/SceneViewport.h       src/editor/src/viewport/SceneViewport.h
git mv src/editor/src/rendering/imgui/ViewportPicker.cpp    src/editor/src/viewport/ViewportPicker.cpp
git mv src/editor/src/rendering/imgui/ViewportPicker.h      src/editor/src/viewport/ViewportPicker.h
git mv src/editor/src/rendering/imgui/GizmoController.cpp   src/editor/src/viewport/GizmoController.cpp
git mv src/editor/src/rendering/imgui/GizmoController.h     src/editor/src/viewport/GizmoController.h
git mv src/editor/src/rendering/EditorCamera.cpp           src/editor/src/viewport/EditorCamera.cpp
git mv src/editor/src/rendering/EditorCamera.h             src/editor/src/viewport/EditorCamera.h
git mv src/editor/src/rendering/EditorCameraState.h        src/editor/src/viewport/EditorCameraState.h
```

- [ ] **Step 5: git mv the `imgui/`, `preview/`, `dialogs/`, `util/` groups**

```bash
git mv src/editor/src/rendering/imgui/imgui_nvrhi.cpp       src/editor/src/imgui/imgui_nvrhi.cpp
git mv src/editor/src/rendering/imgui/imgui_nvrhi.h         src/editor/src/imgui/imgui_nvrhi.h
git mv src/editor/src/rendering/imgui/registered_font.cpp   src/editor/src/imgui/registered_font.cpp
git mv src/editor/src/rendering/imgui/registered_font.h     src/editor/src/imgui/registered_font.h
git mv src/editor/src/rendering/imgui/MeshPreviewRenderer.cpp src/editor/src/preview/MeshPreviewRenderer.cpp
git mv src/editor/src/rendering/imgui/MeshPreviewRenderer.h   src/editor/src/preview/MeshPreviewRenderer.h
git mv src/editor/src/rendering/imgui/EditorFileDialog.cpp  src/editor/src/dialogs/EditorFileDialog.cpp
git mv src/editor/src/rendering/imgui/EditorFileDialog.h    src/editor/src/dialogs/EditorFileDialog.h
git mv src/editor/src/rendering/MetricHistory.h            src/editor/src/util/MetricHistory.h
git mv src/editor/src/rendering/TransientStatus.h          src/editor/src/util/TransientStatus.h
```

- [ ] **Step 6: Verify the old tree is empty + moves registered**

```bash
git status -sb | head -60
# Expected: ~50 "R  old -> new" rename entries, all under src/editor/src/.
find src/editor/src/rendering -type f 2>/dev/null
# Expected: NO output (rendering/ + rendering/imgui/ now empty; git removed tracked files).
```

If `find` lists stragglers, they were missed — git mv them to their dir per the inventory before proceeding.

- [ ] **Step 7: Rewrite `src/editor/CMakeLists.txt`**

Replace the `add_executable(editor ...)` block (lines ~3-28) with:

```cmake
add_executable(editor
    src/main.cpp

    # App / orchestration layer
    src/app/EditorPreferences.cpp
    src/app/ImGuiRenderer.cpp
    src/app/ImGuiOverlay.cpp

    # Editor panels
    src/panels/EcsInspectorPanel.cpp
    src/panels/MaterialManagerPanel.cpp
    src/panels/MeshManagerPanel.cpp
    src/panels/NavigationPanel.cpp
    src/panels/MemoryPanel.cpp
    src/panels/DayNightPanel.cpp
    src/panels/RenderStatsPanel.cpp
    src/panels/SimulationPanel.cpp
    src/panels/PerformancePanel.cpp
    src/panels/MainMenuBar.cpp

    # Viewport interaction + editor camera
    src/viewport/SceneViewport.cpp
    src/viewport/ViewportPicker.cpp
    src/viewport/GizmoController.cpp
    src/viewport/EditorCamera.cpp

    # ImGui <-> NVRHI integration
    src/imgui/imgui_nvrhi.cpp
    src/imgui/registered_font.cpp

    # Offscreen mesh-thumbnail render
    src/preview/MeshPreviewRenderer.cpp

    # Dialogs
    src/dialogs/EditorFileDialog.cpp
)
```

Replace the `target_include_directories(editor PRIVATE ...)` block (lines ~32-35) with:

```cmake
# Editor include paths. Engine headers (Renderer.h, IOverlay.h, MeshSystem.h,
# etc.) are inherited transitively via Engine's PUBLIC include dirs. Editor
# topic dirs are all on the path so the existing flat #include "Foo.h" lines
# keep resolving across the new directory boundaries (strategy A).
target_include_directories(editor PRIVATE
    src
    src/app
    src/panels
    src/viewport
    src/imgui
    src/preview
    src/dialogs
    src/util
)
```

(Header-only files — `EditorContext.h`, `EditorCameraState.h`, `MetricHistory.h`, `TransientStatus.h`, and every `*.h` — are NOT listed as sources; they're found via the include dirs. This matches the pre-move CMakeLists which listed only `.cpp` sources.)

- [ ] **Step 8: Sweep `tests/CMakeLists.txt` — 4 targets**

**test_editorcam** — change the source path + include dir:

```cmake
# source line: was src/editor/src/rendering/EditorCamera.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport/EditorCamera.cpp
# include dir: was src/editor/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport
```

**test_metrichistory** — change include dir:

```cmake
# was src/editor/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src/util
```

**test_transientstatus** — change include dir:

```cmake
# was src/editor/src/rendering
    ${CMAKE_SOURCE_DIR}/src/editor/src/util
```

**test_editorprefs** — replace the two editor include dirs. `EditorPreferences.h` moved to `app/`; it transitively includes `EditorCameraState.h` which moved to `viewport/`. So BOTH are needed:

```cmake
# was:
#     ${CMAKE_SOURCE_DIR}/src/editor/src
#     ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
# now:
    ${CMAKE_SOURCE_DIR}/src/editor/src
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
    ${CMAKE_SOURCE_DIR}/src/editor/src/viewport
```

(Keep `${CMAKE_SOURCE_DIR}/src/engine/include` and `${CMAKE_SOURCE_DIR}/src/engine/src/rendering` — those are engine paths for `RenderStats.h`, unaffected by this move.)

- [ ] **Step 9: Reconfigure CMake (mandatory — caches old paths)**

```bash
cmake --preset msvc-win64-vs2026-community
```

Expected: configure succeeds, no "cannot find source file" errors.

- [ ] **Step 10: Build the editor**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target editor --config Debug 2>&1 | tail -15
```

Expected: clean build. If an include fails to resolve (`cannot open source file "Foo.h"`), confirm `Foo.h`'s new dir is in the editor `target_include_directories` list from Step 7.

- [ ] **Step 11: Build + run the 4 affected tests**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_editorcam test_metrichistory test_transientstatus test_editorprefs --config Debug
for t in test_editorcam test_metrichistory test_transientstatus test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "Editor-linked tests green."
```

Expected: all 4 pass.

- [ ] **Step 12: Regression sweep (confirm nothing else broke)**

```bash
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove --config Debug
for t in test_ecs test_alloc test_collision test_worldserial test_menu test_navmesh test_navagent test_followcam test_playermove; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "All tests green."
```

Expected: all 9 pass (none touch editor src — trivially green, but confirms no collateral).

- [ ] **Step 13: Final structure check**

```bash
git status -sb | grep -c "^R"
# Expected: ~50 (25 file-units, most being .cpp+.h pairs).
find src/editor/src -type d | sort
# Expected: src/editor/src plus app panels viewport imgui preview dialogs util — NO rendering.
```

- [ ] **Step 14: Commit (single, green)**

```bash
git add -A src/editor/CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Nuno Silva" -c user.email="nuno.levezinho@live.com.pt" commit -m "refactor(editor): restructure src into topic dirs (Spec A)

Move editor tooling out of the misleading src/editor/src/rendering/
imgui/ tree into responsibility-named dirs directly under
src/editor/src/:
  app/      ImGuiRenderer, ImGuiOverlay, EditorContext, EditorPreferences
  panels/   10 *Panel files + MainMenuBar
  viewport/ SceneViewport, ViewportPicker, GizmoController, EditorCamera(+State)
  imgui/    imgui_nvrhi, registered_font
  preview/  MeshPreviewRenderer
  dialogs/  EditorFileDialog
  util/     MetricHistory, TransientStatus

Pure mechanical git mv (history preserved) + CMake rewire. Zero
behavior change. rendering/ + rendering/imgui/ deleted.

Include strategy A: every topic dir added to the editor
target_include_directories so the existing flat #include \"Foo.h\"
lines keep resolving across the new dir boundaries — no include-line
edits in any .cpp/.h.

tests/CMakeLists.txt swept for the 4 editor-linked targets:
  test_editorcam     -> viewport/ (EditorCamera.cpp + include)
  test_metrichistory -> util/ (MetricHistory.h)
  test_transientstatus -> util/ (TransientStatus.h)
  test_editorprefs   -> app/ + viewport/ (EditorPreferences.h pulls
                        EditorCameraState.h transitively)

Editor builds clean; 4 editor-linked tests + 9 regression suites
green. Spec B (EcsInspectorPanel decomposition) builds on this."
```

Note: `git add -A` stages the renames (already tracked by git mv) plus the two CMakeLists edits. Verify the commit contains ONLY moves + the 2 CMakeLists files via `git show --stat HEAD | tail -5`.

---

## Task 2: Final review + GUI smoke handoff

- [ ] **Step 1: Verify clean tree + full sweep**

```bash
git status -sb
# Expected: clean.
cmake --build out/build/msvc-win64-vs2026-community --target editor test_editorcam test_metrichistory test_transientstatus test_editorprefs --config Debug
for t in test_editorcam test_metrichistory test_transientstatus test_editorprefs; do
  ./out/build/msvc-win64-vs2026-community/bin/Debug/$t.exe || { echo "$t FAILED"; exit 1; }
done
echo "Verified."
```

- [ ] **Step 2: Dispatch final reviewer subagent**

Provide:
- Spec: `docs/superpowers/specs/2026-05-28-editor-reorg-structure-design.md` (commit `ff2715f`)
- Plan: this file
- Diff: `git diff main..feat/editor-reorg-structure`
- Verify: (a) commit is moves + 2 CMakeLists only — no `.cpp`/`.h` CONTENT changed (git should show pure renames, `R100`); (b) no file left under `src/editor/src/rendering/`; (c) editor include dirs list all 7 topic dirs; (d) the 4 test targets point at correct new dirs; (e) builds + tests green.

Reviewer checks specifically: **did any file's content change?** A pure move shows `R100` (100% similarity) in `git diff --stat -M`. Any content delta means a non-mechanical edit slipped in — flag it.

- [ ] **Step 3: User GUI smoke checklist**

1. Editor launches clean (no restart-required gotcha — no ECS/API change here).
2. Every panel opens + renders: ECS Inspector, Material Manager, Mesh Manager, Navigation, Memory, Atmosphere (DayNight), Render Stats, Simulation, Performance, Main Menu Bar.
3. Viewport: gizmo manipulation works; entity picking (click-select) works.
4. Mesh preview thumbnails render in the Mesh/Material managers.
5. No visual or behavioral delta vs pre-reorg — this was a pure move.

---

## Self-review notes

**Spec coverage check:**

- ✅ Target layout (7 topic dirs) — Task 1 Steps 1-5.
- ✅ Complete 25-unit file→dir mapping — Task 1 Steps 2-5 (every file from the spec table has a git mv line).
- ✅ Include strategy A (all dirs on editor path, flat includes unchanged) — Task 1 Step 7.
- ✅ editor CMakeLists source list + include dirs rewrite — Task 1 Step 7.
- ✅ tests CMakeLists 4-target sweep — Task 1 Step 8 (CORRECTED: test_editorprefs needs app/ AND viewport/, not just app/ as the spec table said).
- ✅ Reconfigure before build — Task 1 Step 9.
- ✅ git mv history preservation — Task 1 Steps 2-5.
- ✅ Delete empty rendering dirs — implicit in git mv (Step 6 verifies empty).
- ✅ Green-at-every-commit constraint — single commit (Step 14) after full build+test verification.
- ✅ Build + 4 tests + regression verification — Steps 10-12.
- ✅ User GUI smoke — Task 2 Step 3.
- ✅ Final review (pure-move / R100 check) — Task 2 Step 2.

No gaps. One spec correction surfaced + fixed: test_editorprefs include-dir set (app/ + viewport/, not app/ only).

**Placeholder scan:** None. Every git mv path is explicit; every CMake edit shows the exact text; every command has expected output.

**Type/path consistency:**

- All destination paths use the same 7 dir names (`app panels viewport imgui preview dialogs util`) across mkdir (Step 1), git mv (Steps 2-5), editor include dirs (Step 7), and test sweeps (Step 8).
- `EditorCameraState.h` consistently → `viewport/` (Step 4) and referenced as a viewport-dir dependency in test_editorprefs (Step 8).
- `EditorPreferences.{cpp,h}` consistently → `app/` (Step 2), source listed in editor CMake (Step 7), include dir for test_editorprefs (Step 8).

**Commit count:** 1 implementation commit (Task 1) + Task 0 (admin) + Task 2 (review-only). Matches the green-at-every-commit constraint from the spec.
