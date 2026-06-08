# Animator Node-Graph Editor Design

**Status:** Design approved, pre-implementation.
**Branch:** `feat/anim-graph-editor`
**Depends on:** SP5 data-driven animator (merged to main, `79c898e`) — `AnimatorController` data graph, `AnimatorControllerStore`, `AnimatorComponent`, the `EvaluateAnimator` evaluator, and `<model>.animctrl.json` loading.

## Goal

The deferred SP5 authoring UI: a **visual node-graph editor** for `AnimatorController` assets. States are nodes, transitions are links you draw between them; conditions, params, clips, and flags are edited in-panel. Save writes the controller back to its `.animctrl.json` and **live-reloads** it so running entities update immediately. The active state/transition of a selected entity is **highlighted live** on the graph, turning the editor into a debugging view of the running FSM.

This replaces the SP5 stopgap (JSON-by-hand + the `AnimatorEditor` assign-picker). The `AnimatorEditor` inspector panel stays (it assigns a controller to an entity + shows the textual cursor readout); this panel is the graph authoring tool.

## Architecture

Three pieces, editor-side except one engine seam:

- **Vendored library:** `thedmd/imgui-node-editor` into `third_party/imgui-node-editor/`, built as a CMake target linked into the **`editor`** executable only. `Engine` and `runtime` never link it (it is an ImGui extension; ImGui is already editor-only tooling). Composes with the existing ImGui overlay.
- **`AnimatorGraphPanel`** (`src/editor/src/panels/AnimatorGraphPanel.{h,cpp}`) — a top-level panel (sibling of `MaterialManagerPanel`/`NavigationPanel`), opened from the menu bar, with its own window. It edits a shared controller **asset** (keyed `<model>#animctrl`), not a per-entity component. It holds a **mutable working copy** of the controller, decoupled from the immutable store entry; edits never touch the store until Save.
- **Engine seam — `AnimatorControllerStore` becomes shared_ptr-backed + reloadable** (below).

The controller dropdown at the top of the panel lists loaded controllers (`AnimatorControllerStore::GetAssetList`); it auto-selects the selected entity's `AnimatorComponent.ControllerId` when that entity has one.

## Engine seam: shared_ptr store + live reload

Currently `AnimatorControllerStore` holds `AnimatorController` by value (immutable, dedup-by-key `Add`), and the evaluator does `const AnimatorController* c = Get(id)` each tick on GameThread. Editing + reloading a controller in place would tear that cross-thread read.

Change:
- Store holds **`std::shared_ptr<const AnimatorController>`** per handle. `Get(handle)` returns the `shared_ptr` (under the existing mutex). The evaluator loads it once per tick and holds its own ref for the whole tick — a concurrent reload swap cannot tear the read. This mirrors the engine's existing `std::atomic<std::shared_ptr<const ECS>>` snapshot pattern.
- New `Reload(const std::string& key, AnimatorController controller)` — atomically replaces the entry's `shared_ptr` under the mutex (creating a fresh `shared_ptr<const AnimatorController>`). Distinct from `Add` (which dedups and is for first load); `Reload` always replaces.
- The store records each controller's **source file path** at load time (the sibling `.animctrl.json` path), so Save writes back to the exact file the controller came from (the repo asset, not a build-copy that gets overwritten on rebuild). Add a `SourcePathForHandle(handle) -> std::string`.
- Evaluator edit is one line: `const AnimatorController* c = Get(id)` → `std::shared_ptr<const AnimatorController> c = Get(id)`; the deref usage (`c->states` etc.) is unchanged because `EvaluateAnimator` takes `const AnimatorController&` (pass `*c`).

`Add` still returns the handle and stores the source path; existing call sites (the GameThread drain) pass the path. Backward note: SP5's drain currently calls `Add(res.assetKey + "#animctrl", std::move(ctrl))` — extend that call to also pass the source `.animctrl.json` path.

## Save / serialize

- Add `to_json(const AnimatorController&)` (editor-side round-trip of params/states/transitions) — symmetric with the existing `from_json`. The runtime `AnimatorController` struct stays **layout-free**: node positions live ONLY in the on-disk JSON's `editorLayout` block, handled by the editor reading/writing the raw `nlohmann::json`, never in the struct. Runtime `from_json` ignores `editorLayout` (nlohmann skips unknown keys), so the runtime path is untouched.
- `editorLayout` JSON shape (optional block in `.animctrl.json`):
  ```json
  "editorLayout": { "nodes": { "Idle": [120, 80], "Walk": [320, 80], "Run": [520, 80], "__any__": [120, 240] }, "pan": [0,0], "zoom": 1.0 }
  ```
  Keyed by state name; `"__any__"` is the anyState node's position. Missing entries → auto-placed.
- **Save flow:** serialize the working-copy graph (`to_json`) + merge the current `editorLayout` (node positions read back from imgui-node-editor) into the raw JSON → write the controller's source `.animctrl.json` → `AnimatorControllerStore::Reload(key, ctrl)` (which re-resolves bare clip names → `stateClipIds` exactly like the SP5 startup drain) → live entities pick up the new graph next tick.
- **Reload-from-disk** (discard working copy): re-read the source file into a fresh working copy.

## Panel UI

### Nodes = states
Each node shows: editable **name**; **clip dropdown** (lists that model's clips — `AnimationStore` entries whose key matches `<assetKey>#anim/`, where `<assetKey>` derives from the controller key `<assetKey>#animctrl`); **cyclic** + **loop** checkboxes. The **entry state** (states index 0) shows a badge; right-click "Set as entry" moves it to index 0. A single special **anyState** node is the source for `from:"*"` transitions (added via toolbar if absent; cannot be a transition target).

### Links = transitions
Dragging from a state's out-pin to another state's in-pin (or from the anyState node) creates a transition. Selecting a link opens an inspector strip showing `duration` and the **condition rows** — each row = param dropdown (from declared params) + op dropdown (`Greater/Less/GreaterEqual/LessEqual/Equal`) + value field; add/remove rows. Delete a link via select + Del. Transition **priority = creation/declared order** (which `SelectTransition` honors: anyState first, then declared order); the selected link can be moved up/down to reorder.

### Params region
A docked side region lists declared params (name + type `Float/Bool/Trigger`); add/remove. The condition param dropdowns read this list.

### Toolbar
Controller picker · Save · Reload-from-disk · Add State · Add anyState (if absent) · validation status text.

## Live highlight

When the selected entity uses the open controller (its `AnimatorComponent.ControllerId` matches), read its cursor via the editor's live snapshot path (the `WorldSnapshot->GetComponent<AnimatorComponent>` access the `AnimatorEditor` already uses): highlight the `CurrentState` node; if `FromState >= 0`, light the active transition link and show its weight `w`. Pure read-only overlay, refreshed each frame, no edit coupling.

## Editing operations + validation

**Ops:** add / delete / rename state; set clip; toggle cyclic / loop; set entry; add anyState; create / delete / reorder transition; set duration; add / remove / edit conditions; declare / remove params.

- **Working-copy isolation:** edits never touch the store until Save, so a half-built graph never breaks live entities.
- **Rename safety:** renaming a state rewrites all transitions referencing it (`from`/`to`) in the working copy so links never dangle. Implemented as a pure helper `RenameState(ctrl, old, new)`.
- **Validation** (`ValidateController(ctrl) -> std::vector<std::string>`, pure): duplicate state names; a transition `from`/`to` referencing a missing state; a condition referencing an undeclared param; a state whose clip name doesn't resolve in `AnimationStore`; empty graph (no states). Surfaced in the toolbar + logged `SM_WARN` — never silent. Save is allowed with warnings (early-dev policy), but they are shown.

## Testing

Pure-fn / data tests (no ImGui — the canvas itself is manual-smoke only), in `test_animator` (extend) or a new `test_animgraph`:
- `to_json` → `from_json` round-trip of a full controller (params/states/transitions) equals the original.
- `editorLayout` round-trip: write graph + layout, re-read; graph intact + layout preserved; runtime `from_json` ignores `editorLayout`.
- `RenameState(ctrl, old, new)`: state renamed + all transitions' `from`/`to` updated; anyState (`*`) left alone.
- `ValidateController`: each rule fires (dup name, dangling from/to, undeclared param, unresolved clip, empty graph) and a clean controller yields no warnings.
- Store: `Reload(key, ctrl2)` swaps so `Get` returns the new graph; a previously-held `shared_ptr` still reads the old graph (no tear); `SourcePathForHandle` returns the recorded path.

Existing suites stay green.

## Scope

**In:** vendored imgui-node-editor; `AnimatorGraphPanel`; `AnimatorControllerStore` → `shared_ptr<const>` + `Reload` + source-path tracking; editing existing controllers with full structural ops; JSON write-back + `editorLayout`; live active-state/transition highlight; validation; pure-fn tests.

**Out (later):**
- Create a brand-new controller for a model that lacks one (thin follow-up — needs model/clip enumeration UI).
- Blend-tree nodes, sub-state-machines, layers.
- **Undo/redo** — v1 relies on Reload-from-disk to discard the working copy (explicitly out per design review).
- Copy/paste of nodes; multi-controller tabs.

## Smoke test

1. Build (`msvc-win64-vs2026-community`), launch editor. Open the Animator Graph panel from the menu bar.
2. Pick `models/Fox.gltf#animctrl` → graph renders (Idle / Walk / Run nodes + their transition links, positioned from `editorLayout` or auto-placed).
3. Rename a state, drag a new transition between two states, edit a condition's value, move nodes around.
4. Save → the source `assets/models/Fox.animctrl.json` is updated (incl. `editorLayout`) → a live Fox entity using the controller reflects the change next tick (no restart).
5. Select that Fox entity → its current state node highlights and the active transition lights up + shows weight as it moves.
6. Reload-from-disk discards an unsaved edit (working copy reverts to the file).

## Risks / notes

- **imgui-node-editor integration** is the main unknown (vendoring, CMake, save/restore of node positions vs our own `editorLayout`). We use the library for canvas/interaction but own persistence via `editorLayout` (don't rely on the library's native settings file) so the layout lives in the controller JSON.
- **Source-path tracking** is essential: without it, Save would write to whichever `assets/` copy the running exe used (possibly a build-output copy overwritten on rebuild). The store must remember the loaded path and Save must target it.
- **No undo** — the working-copy + Reload-from-disk model is the safety net; a destructive edit is recovered by reloading before saving. Acceptable for v1; undo is a known follow-up.
- The `shared_ptr<const>` store change touches the SP5 evaluator's one `Get` line + the drain's `Add` call — small, contained.
