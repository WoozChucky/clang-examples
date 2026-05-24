# Scene File Menu (New / Reload / Save) — Design

**Date:** 2026-05-24
**Status:** Approved (design); pending implementation plan.
**Branch:** stacking on `main` (implementation on `scene-file-menu`).

## Goal

Wire the editor's File menu to the existing JSON scene persistence (`WorldManager`), against the
single default file `world.json`:
- **New** (Ctrl+N) — clear to an empty scene.
- **Reload** (Ctrl+R) — re-read `world.json` from disk, replacing the in-memory world.
- **Save** (Ctrl+S) — write the current world to `world.json`, with visible success/failure feedback.

Drop the meaningless **Save As** and the dead **Open...** stub. No file dialog (fixed file). Editor
+ GameThread only; runtime unaffected.

## Background (verified)

- **Persistence layer (works today):** `WorldManager` (`src/engine/src/utilities/WorldManager.{h,cpp}`)
  - `bool SaveWorldSnapshot(const std::string& path, const ECS* world)` — returns false on I/O error.
  - `bool LoadWorldSnapshot(const std::string& path, ECS* world)` — calls `world->Clear()` then
    recreates entities from JSON (full replace, fresh ids; singletons survive `Clear`). Returns false
    on I/O/parse error.
  - `DEFAULT_WORLD_SNAPSHOT_PATH = "world.json"`. Serializes 6 per-entity components
    (Transform/Mesh/Material/Lightning/Text/SunMarker); Parent/Child + singletons not serialized.
- **File menu** (`src/editor/src/rendering/imgui/MainMenuBar.cpp:16-28`): New/Open/Save As/Exit are
  no-ops; **Save** already calls `SaveWorldSnapshot(DEFAULT_WORLD_SNAPSHOT_PATH, ctx.WorldSnapshot.get())`
  but **ignores the return bool** (no feedback). `MainMenuBar::Draw(const EditorContext&, bool& editMode)`
  returns `bool resetLayout`. The Settings menu shows errors via a red `ImGui::TextColored` from a
  `std::string m_SettingsSaveError` member — but that's only visible while the menu is open (no use
  for Save, whose menu closes on click).
- **Threading tension (the crux):**
  - **Save** reads `ctx.WorldSnapshot` (a `shared_ptr<const ECS>` snapshot) on the RenderThread —
    read-only, already safe.
  - **New / Reload** MUTATE the ECS, which is owned exclusively by the **GameThread**
    (`gameState.World`, local to `GameThread::RunLoop`). The editor (RenderThread) must NOT touch it
    directly. The GameThread drains `ECSCommandRing` via
    `ECSCommandProcessor::ProcessCommands(gameState.World, ring)` (`GameThread.cpp:207`).
  - `ECSCommandProcessor::ProcessCommands` lives in **`src/common/include/ECSCommands.h`** (the
    `common` layer) and cannot call `WorldManager` (the `engine` layer) without a layering violation,
    and `ECSCommand` carries only `EntityId`/`ComponentData` (no string path). → Scene New/Reload are
    better signalled to the GameThread directly (which is `engine` and already includes `WorldManager`)
    via simple request flags, not through the component command ring.
- **ECS API** (`ECS.h`): `const std::vector<EntityId>& GetActiveEntities()` (excludes the reserved
  singleton entity), `void DestroyEntity(EntityId)`, `void Clear()` (used by `LoadWorldSnapshot`).
- **Editor↔engine flags pattern** (`ApplicationContext.h`): the editor already publishes RenderThread→
  other-thread state via `std::atomic` members (`SelectedEntity`, `EditorCameraActive`,
  `GameAcceptsMouse/Keyboard`). The GameThread reads such atomics each tick. Runtime never writes them.
- **Selection** (`EcsInspectorPanel`): `EntityId GetSelectedEntity()` / `void SetSelectedEntity(EntityId)`;
  republished to `ApplicationContext::SelectedEntity` each frame by `ImGuiRenderer`.
- **EditorFileDialog exists** (`EditorFileDialog::Open`, native Win32, used by the Mesh/Material
  panels) — but is **not used here** (fixed-file model, no dialog).
- `ImGui::GetTime()` gives a seconds clock; `ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_X)`
  for shortcuts (gated on `!io.WantTextInput`, the pattern used for Ctrl+D in `EcsInspectorPanel`).

## Scope

**In scope:**
1. Two GameThread request flags in `ApplicationContext`: `std::atomic<bool> RequestSceneReload`,
   `std::atomic<bool> RequestSceneNew`.
2. GameThread handling: each tick, `exchange(false)` the flags — Reload → `LoadWorldSnapshot`; New →
   destroy all active entities (singletons preserved).
3. `MainMenuBar` File menu rewrite: New / Reload / Save (+ Ctrl+N/R/S), drop Save As + Open.
4. Save success/failure feedback via a small pure, unit-tested `TransientStatus` helper shown as a
   ~3 s on-screen toast (green ok / red fail); New/Reload show an info toast.
5. Clear the editor selection when a scene change (New/Reload) is issued (stale ids).

**Out of scope / non-goals:** per-file paths / file dialog / "current scene" tracking / window-title
filename (fixed `world.json`); Save As; dirty/unsaved-changes tracking + discard confirmation;
serializing Parent/Child hierarchy or singletons (pre-existing `WorldManager` behavior, untouched);
undo; saving the live world instead of the snapshot (the snapshot is a consistent 1-frame-old copy —
fine). **No `GAME_API_VERSION` bump** (no `GameState`/export/ECS-component-layout change; the new
atomics live on host-owned `ApplicationContext`, not `GameState`).

## Design

### 1. Request flags (`ApplicationContext.h`)

Add near the other editor atomics (e.g. after `SelectedEntity`):
```cpp
// Editor scene-file requests (editor RenderThread sets; GameThread consumes via exchange() each
// tick). Reload re-reads world.json from disk (replacing the world); New clears to an empty scene.
// Runtime never sets these, so it is unaffected.
std::atomic<bool> RequestSceneReload{false};
std::atomic<bool> RequestSceneNew{false};
```

### 2. GameThread handling (`GameThread.cpp`)

Immediately after the `ProcessCommands` block (`GameThread.cpp:207-208`), add:
```cpp
            // Editor scene-file requests (fire-and-forget atomics from the File menu).
            if (m_AppContext->RequestSceneNew.exchange(false, std::memory_order_relaxed)) {
                const std::vector<EntityId> ids = gameState.World.GetActiveEntities(); // copy before destroy
                for (EntityId e : ids) gameState.World.DestroyEntity(e);
                SM_TRACE("GameThread: new (empty) scene");
            }
            if (m_AppContext->RequestSceneReload.exchange(false, std::memory_order_relaxed)) {
                if (WorldManager::LoadWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, &gameState.World))
                    SM_TRACE("GameThread: reloaded world from '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
                else
                    SM_WARN("GameThread: reload failed for '%s'", WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH);
            }
```
- **New** destroys all active entities (a copied id list, so destroy-during-iterate is safe);
  singletons (camera/viewport/input) are excluded from `GetActiveEntities()`, so they survive.
- **Reload** calls `LoadWorldSnapshot` directly (it `Clear`s + repopulates). The startup `WorldLoaded`
  guard is irrelevant here — this is an explicit reload, not the once-per-lifetime auto-load.
- `WorldManager.h`, `ECS` are already included by `GameThread.cpp`.

### 3. `TransientStatus` (pure, unit-tested)

New header-only `src/editor/src/rendering/TransientStatus.h`:
```cpp
#pragma once
#include <string>

// A short-lived status message (toast). Time is injected (ImGui::GetTime()) so it is pure + testable.
class TransientStatus {
public:
    void Set(const std::string& text, bool isError, double now, double durationSec = 3.0) {
        m_Text = text; m_IsError = isError; m_Expiry = now + durationSec;
    }
    bool Visible(double now) const { return !m_Text.empty() && now < m_Expiry; }
    const std::string& Text() const { return m_Text; }
    bool IsError() const { return m_IsError; }
private:
    std::string m_Text;
    bool   m_IsError = false;
    double m_Expiry  = 0.0;
};
```

### 4. `MainMenuBar` File menu + toast (`MainMenuBar.{h,cpp}`)

- `MainMenuBar.h`: `#include "TransientStatus.h"`; add member `TransientStatus m_SceneStatus;`. Change
  `Draw` to return a small result struct so the host can react to a scene change:
  ```cpp
  struct MainMenuBarResult { bool resetLayout = false; bool sceneChanged = false; };
  MainMenuBarResult Draw(const EditorContext& ctx, bool& editMode);
  ```
- `MainMenuBar.cpp` File menu — replace the New/Open/Save/Save As block with New / Reload / Save.
  Share the action bodies between the menu items and the Ctrl+N/R/S chords via local lambdas:
  ```cpp
  const double now = ImGui::GetTime();
  auto doNew = [&]{ ctx.App->RequestSceneNew.store(true, std::memory_order_relaxed);
                    result.sceneChanged = true; m_SceneStatus.Set("New scene", false, now); };
  auto doReload = [&]{ ctx.App->RequestSceneReload.store(true, std::memory_order_relaxed);
                       result.sceneChanged = true; m_SceneStatus.Set("Reloading world.json", false, now); };
  auto doSave = [&]{ const bool ok = ctx.WorldSnapshot &&
                       WorldManager::SaveWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, ctx.WorldSnapshot.get());
                     m_SceneStatus.Set(ok ? "Saved world.json" : "Save failed", !ok, now); };
  ```
  Menu items: `New` (Ctrl+N) → `doNew`; `Reload` (Ctrl+R) → `doReload`; separator; `Save` (Ctrl+S) →
  `doSave`; separator; `Exit` (unchanged no-op). Then, outside the menu (so shortcuts work without the
  menu open), gate on `!ImGui::GetIO().WantTextInput` and fire the same lambdas on
  `IsKeyChordPressed(Ctrl|N / Ctrl|R / Ctrl|S)`.
- **Toast render** — after `EndMainMenuBar()`, if `m_SceneStatus.Visible(now)`, draw a small
  borderless, non-interactive overlay window pinned to the main viewport's bottom-left, with the text
  in green (ok) / red (error):
  ```cpp
  if (m_SceneStatus.Visible(now)) {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + vp->WorkSize.y - 36.0f));
      ImGui::SetNextWindowBgAlpha(0.85f);
      const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
          | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing
          | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
      if (ImGui::Begin("##SceneStatusToast", nullptr, f)) {
          const ImVec4 col = m_SceneStatus.IsError() ? ImVec4(1.0f,0.4f,0.4f,1.0f) : ImVec4(0.5f,1.0f,0.5f,1.0f);
          ImGui::TextColored(col, "%s", m_SceneStatus.Text().c_str());
      }
      ImGui::End();
  }
  ```

### 5. Host wiring + selection clear (`ImGuiRenderer.cpp`)

Update the menu-bar call site (currently `if (m_MenuBar.Draw(ctx, m_EditMode)) s_ResetLayout = true;`):
```cpp
const MainMenuBarResult menuResult = m_MenuBar.Draw(ctx, m_EditMode);
if (menuResult.resetLayout) s_ResetLayout = true;
if (menuResult.sceneChanged) m_EcsInspector.SetSelectedEntity(INVALID_ENTITY); // stale ids after New/Reload
```
(The `SelectedEntity` atomic is republished from the inspector each frame, so clearing the inspector
selection clears the outline too. `INVALID_ENTITY` is from `ECS.h`, already in scope.)

## Data flow

- **Save** (RenderThread): menu/Ctrl+S → `SaveWorldSnapshot(world.json, snapshot)` → toast from the
  bool. Synchronous, no thread hop.
- **New / Reload** (RenderThread → GameThread): menu/chord sets `RequestSceneNew`/`RequestSceneReload`
  + an info toast + `sceneChanged`; `ImGuiRenderer` clears the selection. Next GameThread tick
  `exchange(false)` runs the mutation; the new world appears in the next snapshot the editor renders.

## Build / verification

Build preset `msvc-win64-vs2026-community`. `ApplicationContext.h` is shared → rebuild engine/editor/
runtime (+game). No `GAME_API_VERSION` bump.

- **Unit test (`test_transientstatus`)**: default is not visible; after `Set(text,false,now=10)` it is
  visible at now=10 and now=12.9 and not at now=13.1 (default 3 s); `IsError`/`Text` reflect the set
  values; a later `Set` extends the expiry. Prints `All transient status tests passed.`
- `editor` + `runtime` build clean; `test_ecs`/`test_alloc`/`test_frustum`/`test_input`/`test_picking`/
  `test_editorcam`/`test_metrichistory` stay green; grep shows no `Save As`/dead `Open...` stub left in
  `MainMenuBar.cpp`.
- **GUI smoke (user-run):** Save (menu or Ctrl+S) writes `world.json` and shows a green "Saved
  world.json" toast (make it unwritable → red "Save failed"); edit an entity, Reload (Ctrl+R) → the
  on-disk state returns, selection cleared, info toast; New (Ctrl+N) → empty scene (Entity Count 0,
  camera still works — singletons survived), selection cleared; `runtime.exe` unaffected (no File menu;
  it still auto-loads `world.json` at startup as before).

## Risks

- **Editor mutating GameThread ECS** → avoided: New/Reload only set atomics; all mutation runs on the
  GameThread. Save reads the immutable snapshot.
- **Stale selection after New/Reload** → cleared via `sceneChanged` → `SetSelectedEntity(INVALID_ENTITY)`;
  outline/gizmo/picking already guard against invalid ids as a backstop.
- **Save races a tick?** Save uses the published snapshot (a consistent deep copy), not the live world —
  no race. It is at most one frame behind, which is fine for a manual save.
- **Lost work on New/Reload** → no confirmation prompt (out of scope; matches the current no-undo,
  no-dirty-tracking editor). Noted for the smoke test.
- **Toast obscuring UI** → it is non-interactive (`NoInputs`), auto-sizes, auto-expires (~3 s),
  bottom-left; minimal footprint.
