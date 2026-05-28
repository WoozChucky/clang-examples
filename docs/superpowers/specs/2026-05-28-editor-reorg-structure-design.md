# Editor Folder Restructure — Design Spec

**Date:** 2026-05-28
**Status:** Approved
**Type:** Refactor / housekeeping — pure mechanical move, zero behavior change
**Sequence:** Spec A of 2 (Spec B = EcsInspectorPanel decomposition, depends on this landing first)

---

## Goal

The editor's source files mostly live under `src/editor/src/rendering/imgui/`, but the bulk are editor *tooling* (panels, dialogs, gizmos, viewport interaction), not rendering. The `rendering/` parent is a misnomer. This spec restructures the editor source tree into responsibility-named topic directories directly under `src/`, deleting the `rendering/` nesting.

**Out of scope:** EcsInspectorPanel decomposition (that's Spec B), any namespace additions, any logic change. This is a move-and-rewire-build only.

---

## Architecture

Pure mechanical restructure. `git mv` every file to its topic directory (preserves blame/history), rewrite `src/editor/CMakeLists.txt` source list + include dirs, sweep the 4 affected `tests/CMakeLists.txt` targets. Flat `#include "Foo.h"` lines stay unchanged because every topic dir is added to the editor's include path (strategy A below). Success criterion: identical editor runtime behavior, clean build, all editor-linked tests green.

---

## Target layout

```
src/
  main.cpp
  alloc.h
  app/        ImGuiRenderer.{cpp,h}, ImGuiOverlay.{cpp,h},
              EditorContext.h, EditorPreferences.{cpp,h}
  panels/     EcsInspectorPanel.{cpp,h}, MaterialManagerPanel.{cpp,h},
              MeshManagerPanel.{cpp,h}, NavigationPanel.{cpp,h},
              MemoryPanel.{cpp,h}, DayNightPanel.{cpp,h},
              RenderStatsPanel.{cpp,h}, SimulationPanel.{cpp,h},
              PerformancePanel.{cpp,h}, MainMenuBar.{cpp,h}
  viewport/   SceneViewport.{cpp,h}, ViewportPicker.{cpp,h},
              GizmoController.{cpp,h}, EditorCamera.{cpp,h},
              EditorCameraState.h
  imgui/      imgui_nvrhi.{cpp,h}, registered_font.{cpp,h}
  preview/    MeshPreviewRenderer.{cpp,h}
  dialogs/    EditorFileDialog.{cpp,h}
  util/       MetricHistory.h, TransientStatus.h
```

`src/rendering/` and `src/rendering/imgui/` are deleted (empty after the move).

### File → destination mapping (complete)

| Current | Destination |
|---|---|
| `src/EditorPreferences.{cpp,h}` | `src/app/` |
| `src/rendering/imgui/ImGuiRenderer.{cpp,h}` | `src/app/` |
| `src/rendering/imgui/ImGuiOverlay.{cpp,h}` | `src/app/` |
| `src/rendering/imgui/EditorContext.h` | `src/app/` |
| `src/rendering/imgui/EcsInspectorPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/MaterialManagerPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/MeshManagerPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/NavigationPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/MemoryPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/DayNightPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/RenderStatsPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/SimulationPanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/PerformancePanel.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/MainMenuBar.{cpp,h}` | `src/panels/` |
| `src/rendering/imgui/SceneViewport.{cpp,h}` | `src/viewport/` |
| `src/rendering/imgui/ViewportPicker.{cpp,h}` | `src/viewport/` |
| `src/rendering/imgui/GizmoController.{cpp,h}` | `src/viewport/` |
| `src/rendering/EditorCamera.{cpp,h}` | `src/viewport/` |
| `src/rendering/EditorCameraState.h` | `src/viewport/` |
| `src/rendering/imgui/imgui_nvrhi.{cpp,h}` | `src/imgui/` |
| `src/rendering/imgui/registered_font.{cpp,h}` | `src/imgui/` |
| `src/rendering/imgui/MeshPreviewRenderer.{cpp,h}` | `src/preview/` |
| `src/rendering/imgui/EditorFileDialog.{cpp,h}` | `src/dialogs/` |
| `src/rendering/MetricHistory.h` | `src/util/` |
| `src/rendering/TransientStatus.h` | `src/util/` |

`src/main.cpp` and `src/alloc.h` stay put.

---

## Include strategy (A)

Add every topic dir to the editor target's include path:

```cmake
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

All existing flat `#include "Foo.h"` lines keep resolving — no include-line edits in any `.cpp`/`.h`. This matches the current flat-include convention (today only `src` + `src/rendering` are on the path, and same-dir quoted-include lookup carries the rest). Trade-off accepted: include path becomes "soup" (any editor header findable from anywhere) — invisible cost; the win is folder clarity for humans.

No namespaces added. `EditorFileDialog.h` keeps its existing namespace; nothing else gains one.

---

## CMakeLists changes

### `src/editor/CMakeLists.txt`

1. Rewrite the `add_executable(editor ...)` source list with new paths (every `src/rendering/imgui/X.cpp` → `src/<topic>/X.cpp`, `src/rendering/EditorCamera.cpp` → `src/viewport/EditorCamera.cpp`, `src/EditorPreferences.cpp` → `src/app/EditorPreferences.cpp`).
2. Replace `target_include_directories(editor PRIVATE src src/rendering)` with the 8-line list above.

### `tests/CMakeLists.txt` (4 targets)

| Target | Current ref | New ref |
|---|---|---|
| `test_editorcam` | source `src/editor/src/rendering/EditorCamera.cpp` + include `src/editor/src/rendering` | source `src/editor/src/viewport/EditorCamera.cpp` + include `src/editor/src/viewport` |
| `test_metrichistory` | include `src/editor/src/rendering` | include `src/editor/src/util` |
| `test_transientstatus` | include `src/editor/src/rendering` | include `src/editor/src/util` |
| `test_editorprefs` | include `src/editor/src` + `src/editor/src/rendering` | include `src/editor/src` + `src/editor/src/app` |

(test_editorprefs only needs `EditorPreferences.h` which moves to `app/`; the `src/editor/src` entry stays for any root-level header it transitively pulls.)

---

## Testing

- `cmake --preset msvc-win64-vs2026-community` reconfigure (new dirs require regen, not just rebuild).
- Build `editor` — clean.
- Build + run the 4 affected tests: `test_editorcam`, `test_metrichistory`, `test_transientstatus`, `test_editorprefs` — all green.
- Full regression sweep of remaining suites (test_ecs, test_alloc, test_collision, test_worldserial, test_menu, test_navmesh, test_navagent, test_followcam, test_playermove) — confirm none broke (they don't touch editor src, expected trivially green).
- User GUI smoke: editor launches; every panel opens + functions; viewport gizmo + picking work; mesh preview thumbnails render. No behavior delta vs pre-reorg.

No new automated tests — this is a move; behavior is unchanged and existing tests cover the moved units.

---

## Risks

1. **Same-dir include breakage.** Files that included a sibling via same-dir lookup now cross a dir boundary. Mitigated by strategy A (every dir on the include path). If a build error surfaces a missing header, the fix is confirming its dir is in the include list.
2. **Missed file in CMake source list.** Build fails loudly (unresolved external / missing source). Caught immediately at build time.
3. **Windows case-insensitive FS + git mv.** Earlier in this codebase `Game.h`/`game.h` index-cased oddly. Use exact existing casing in `git mv` commands; verify with `git status` after each batch.
4. **Stale build cache.** CMake caches old source paths; a bare rebuild without reconfigure fails. Plan must reconfigure first.
5. **Behavior drift.** This is mechanical. If the editor behaves differently post-move, something beyond a move happened — treat as a bug, not an accepted change.
6. **Spec B coupling.** EcsInspectorPanel lands in `src/panels/` here; Spec B decomposes it there. No conflict — B builds on A's structure.

---

## Out-of-scope (explicit)

- **EcsInspectorPanel decomposition** — Spec B.
- **Namespace introduction** — not now; the editor is mostly global-namespace today, changing that is a separate concern.
- **Dir-qualified includes** (`#include "app/EditorContext.h"`) — rejected in favor of strategy A (lower churn).
- **Splitting genuinely-large non-inspector files** (ImGuiRenderer 672, imgui_nvrhi 423, MeshPreviewRenderer 421) — out of scope; move only.
- **Renaming files** — keep current filenames; only their directory changes.

---

## Commit estimate

3-4 commits:
1. Move `app/` + `panels/` files (git mv) — build broken mid-commit acceptable, or do all moves + CMake in one commit to keep build green.
2. Move `viewport/` + `imgui/` + `preview/` + `dialogs/` + `util/` files.
3. Rewrite `src/editor/CMakeLists.txt` + sweep `tests/CMakeLists.txt`.
4. Reconfigure + build + test verification (+ any include fixups).

**Note:** Because a half-moved tree doesn't build, the cleanest approach is ONE commit doing all `git mv` + both CMakeLists edits together, so every commit builds green. The plan will decide commit granularity; green-at-every-commit is the constraint.
