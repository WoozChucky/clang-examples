# Scene File Menu (New / Reload / Save) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the editor File menu to the existing `world.json` persistence — New (empty), Reload (re-read disk), Save (write + visible feedback) — dropping the dead Save As / Open stubs.

**Architecture:** Save stays editor-side (reads the render-thread snapshot, returns bool → toast). New/Reload mutate the GameThread-owned ECS via two fire-and-forget `ApplicationContext` request atomics the GameThread `exchange(false)`-checks each tick. Feedback uses a pure, unit-tested `TransientStatus` toast helper. No command-ring/ECSCommands change.

**Tech Stack:** C++23, Dear ImGui, `WorldManager` JSON persistence, lock-free atomics, CMake presets.

**Spec:** `docs/superpowers/specs/2026-05-24-scene-file-menu-design.md`

**Conventions (every task):**
- Build preset `msvc-win64-vs2026-community`; build dir `out/build/msvc-win64-vs2026-community`; exes in `bin/Debug/`.
- **No `GAME_API_VERSION` bump** (no `GameState`/export/ECS-component change; the new atomics live on host-owned `ApplicationContext`, like `SelectedEntity`/`EditorCameraActive` before them). `ApplicationContext.h` is shared → rebuild engine/editor/runtime.
- Commit author repo default (`Nuno Silva <nuno.levezinho@live.com.pt>`) — plain `git commit`, no `-c`/`--author`. Never stage `.claude/`. Stage only the files each step names. After each commit, `git log -1 --format='%an <%ae>'` must show the personal email.
- New files/targets require a CMake reconfigure (`cmake --preset msvc-win64-vs2026-community`) before building; edits to existing files do not.

---

### Task 1: `TransientStatus` helper + `test_transientstatus` (TDD)

**Files:**
- Create: `src/editor/src/rendering/TransientStatus.h` (header-only)
- Create: `tests/test_transientstatus.cpp`
- Modify: `tests/CMakeLists.txt`

TDD: write the test, build (fail — header missing), implement the header, build (pass).

- [ ] **Step 1: Write the header**

`src/editor/src/rendering/TransientStatus.h`:
```cpp
#pragma once
#include <string>

// A short-lived status message (toast). Time is injected (caller passes ImGui::GetTime()) so the
// class is pure and unit-testable. Visible() is true from Set() until `durationSec` later.
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

- [ ] **Step 2: Write the failing test**

`tests/test_transientstatus.cpp`:
```cpp
#include <cstdio>

#include "TransientStatus.h"

static int g_Failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_Failures;                                                \
        }                                                                \
    } while (0)

static void T00_default_not_visible()
{
    TransientStatus s;
    EXPECT(!s.Visible(0.0));
    EXPECT(!s.Visible(100.0));
}

static void T01_visible_within_window()
{
    TransientStatus s;
    s.Set("Saved world.json", false, 10.0); // default 3 s
    EXPECT(s.Visible(10.0));
    EXPECT(s.Visible(12.9));
    EXPECT(!s.Visible(13.1));   // expired
    EXPECT(s.Text() == "Saved world.json");
    EXPECT(s.IsError() == false);
}

static void T02_error_flag_and_custom_duration()
{
    TransientStatus s;
    s.Set("Save failed", true, 5.0, 1.0); // 1 s
    EXPECT(s.Visible(5.0));
    EXPECT(!s.Visible(6.1));
    EXPECT(s.IsError() == true);
    EXPECT(s.Text() == "Save failed");
}

static void T03_reset_extends_expiry()
{
    TransientStatus s;
    s.Set("a", false, 10.0);   // expires ~13
    s.Set("b", false, 20.0);   // expires ~23
    EXPECT(!s.Visible(13.5));   // first window passed, but...
    EXPECT(s.Visible(22.0));    // ...second Set is active
    EXPECT(s.Text() == "b");
}

int main()
{
    T00_default_not_visible();
    T01_visible_within_window();
    T02_error_flag_and_custom_duration();
    T03_reset_extends_expiry();

    if (g_Failures == 0) { std::printf("All transient status tests passed.\n"); return 0; }
    std::printf("%d transient status test(s) FAILED.\n", g_Failures);
    return 1;
}
```

- [ ] **Step 3: Wire the `test_transientstatus` CMake target**

In `tests/CMakeLists.txt`, after the last test block (e.g. `test_metrichistory`), append:
```cmake
add_executable(test_transientstatus
    test_transientstatus.cpp
)

target_include_directories(test_transientstatus PRIVATE
    ${CMAKE_SOURCE_DIR}/src/editor/src/rendering
)

set_target_properties(test_transientstatus PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```
(No glm — pure std::string/double.)

- [ ] **Step 4: Reconfigure + build → run (expect PASS)**

```
cmake --preset msvc-win64-vs2026-community
cmake --build out/build/msvc-win64-vs2026-community --target test_transientstatus
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
```
Expected: `All transient status tests passed.` (TDD: to see red, build before writing the header — it fails to find `TransientStatus.h`. Final state must be green.) If an assertion fails, fix the HEADER, not the test.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/TransientStatus.h tests/test_transientstatus.cpp tests/CMakeLists.txt
git commit -m "feat: add TransientStatus toast helper + test_transientstatus"
```

---

### Task 2: Scene request flags + GameThread New/Reload handling

**Files:**
- Modify: `src/common/include/ApplicationContext.h`
- Modify: `src/engine/src/threading/GameThread.cpp`

No unit test (threading glue over already-tested `WorldManager`/ECS); verified by build + regression + the GUI smoke. This is the shared/runtime-touching change — runtime never sets the flags, so it is inert there.

- [ ] **Step 1: Add the request atomics to `ApplicationContext`**

In `src/common/include/ApplicationContext.h`, inside `struct ApplicationContext`, after the `SelectedEntity` member block, add:
```cpp
    // Editor scene-file requests (editor RenderThread sets; GameThread consumes via exchange() each
    // tick). Reload re-reads world.json from disk (replacing the world); New clears to an empty scene.
    // Runtime never sets these, so it is unaffected.
    std::atomic<bool> RequestSceneReload{false};
    std::atomic<bool> RequestSceneNew{false};
```

- [ ] **Step 2: Handle the requests on the GameThread**

In `src/engine/src/threading/GameThread.cpp`, find the `ProcessCommands` block (inside the `ZoneScopedN("Game:FixedUpdate")` scope):
```cpp
            // Process ECS commands from RenderThread (ImGui modifications)
            {
                ZoneScopedN("Game:ProcessECSCommands");
                ECSCommandProcessor::ProcessCommands(gameState.World, m_AppContext->ECSCommandRing);
            }
```
Immediately AFTER that block, insert:
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
(`WorldManager.h` is already included in `GameThread.cpp` — it's used in the startup load at the top of `RunLoop`. `EntityId`/`std::vector`/`ECS` are in scope. `GetActiveEntities()` returns `const std::vector<EntityId>&` excluding the reserved singleton entity, so destroying them all preserves the camera/viewport/input singletons. Copying the id list first makes destroy-during-iterate safe.)

- [ ] **Step 3: Build engine + editor + runtime + run the full regression suite**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
cmake --build out/build/msvc-win64-vs2026-community --target runtime
cmake --build out/build/msvc-win64-vs2026-community --target test_ecs test_alloc test_frustum test_input test_picking test_editorcam test_metrichistory test_transientstatus
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_frustum.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_input.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_picking.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_editorcam.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_metrichistory.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
```
Expected: all build clean; every test prints its `All ... passed.` line (test_alloc prints one intentional ERROR line before its pass).

- [ ] **Step 4: Commit**

```bash
git add src/common/include/ApplicationContext.h src/engine/src/threading/GameThread.cpp
git commit -m "feat: GameThread New/Reload scene via ApplicationContext request atomics"
```

---

### Task 3: File menu rewrite (New/Reload/Save + toast + shortcuts) + host wiring

**Files:**
- Modify: `src/editor/src/rendering/imgui/MainMenuBar.h`
- Modify: `src/editor/src/rendering/imgui/MainMenuBar.cpp`
- Modify: `src/editor/src/rendering/imgui/ImGuiRenderer.cpp`

No unit test (ImGui glue); verified by clean editor build + the GUI smoke.

- [ ] **Step 1: `MainMenuBar.h` — result struct, return type, status member**

In `src/editor/src/rendering/imgui/MainMenuBar.h`:
- Add `#include "TransientStatus.h"` after the existing includes.
- Before `class MainMenuBar`, add:
```cpp
// What the host should react to after drawing the menu bar this frame.
struct MainMenuBarResult {
    bool resetLayout  = false; // "View -> Reset Layout" clicked
    bool sceneChanged = false; // New or Reload issued -> host clears the (now stale) selection
};
```
- Change the method declaration `bool Draw(const EditorContext& ctx, bool& editMode);` to:
```cpp
    MainMenuBarResult Draw(const EditorContext& ctx, bool& editMode);
```
- Add a private member:
```cpp
    TransientStatus m_SceneStatus; // Save/New/Reload feedback toast
```

- [ ] **Step 2: `MainMenuBar.cpp` — return type, File menu, shortcuts, toast**

In `src/editor/src/rendering/imgui/MainMenuBar.cpp`:

(a) Change the definition signature + the local result. Replace:
```cpp
bool MainMenuBar::Draw(const EditorContext& ctx, bool& editMode)
{
    bool resetLayoutRequested = false;
```
with:
```cpp
MainMenuBarResult MainMenuBar::Draw(const EditorContext& ctx, bool& editMode)
{
    MainMenuBarResult result;
    const double now = ImGui::GetTime();

    // Shared action bodies for the File menu items and their Ctrl shortcuts.
    auto doNew = [&] {
        ctx.App->RequestSceneNew.store(true, std::memory_order_relaxed);
        result.sceneChanged = true;
        m_SceneStatus.Set("New scene", false, now);
    };
    auto doReload = [&] {
        ctx.App->RequestSceneReload.store(true, std::memory_order_relaxed);
        result.sceneChanged = true;
        m_SceneStatus.Set("Reloading world.json", false, now);
    };
    auto doSave = [&] {
        const bool ok = ctx.WorldSnapshot &&
            WorldManager::SaveWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, ctx.WorldSnapshot.get());
        m_SceneStatus.Set(ok ? "Saved world.json" : "Save failed", !ok, now);
    };
```

(b) Replace the File menu block:
```cpp
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New", "Ctrl+N")) { /* no-op */ }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) { /* no-op */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S") && ctx.WorldSnapshot) {
                WorldManager::SaveWorldSnapshot(WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH, ctx.WorldSnapshot.get());
            }
            if (ImGui::MenuItem("Save As...")) { /* no-op */ }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { /* no-op */ }
            ImGui::EndMenu();
        }
```
with:
```cpp
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New", "Ctrl+N"))    { doNew(); }
            if (ImGui::MenuItem("Reload", "Ctrl+R")) { doReload(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))   { doSave(); }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { /* no-op */ }
            ImGui::EndMenu();
        }
```

(c) Change `return resetLayoutRequested;` — first carry the existing reset-layout result. Find where `resetLayoutRequested` was set (the `if (ImGui::MenuItem("Reset Layout")) { resetLayoutRequested = true; }` in the View menu) and change it to `result.resetLayout = true;`. Then replace the final `return resetLayoutRequested;` with handling shortcuts + the toast, then `return result;`:
```cpp
    // Ctrl+N / Ctrl+R / Ctrl+S work without the menu open (gated so they don't fire while typing).
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) doNew();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) doReload();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) doSave();
    }

    // Transient feedback toast: borderless, non-interactive, bottom-left of the main viewport.
    if (m_SceneStatus.Visible(now)) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + vp->WorkSize.y - 36.0f));
        ImGui::SetNextWindowBgAlpha(0.85f);
        const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##SceneStatusToast", nullptr, f)) {
            const ImVec4 col = m_SceneStatus.IsError() ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                                       : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            ImGui::TextColored(col, "%s", m_SceneStatus.Text().c_str());
        }
        ImGui::End();
    }

    return result;
```
(`ApplicationContext.h` + `WorldManager.h` are already included in `MainMenuBar.cpp`. `ctx.App` is the `ApplicationContext*`. The `Settings`/`About`/`View` menus are unchanged except the one `resetLayoutRequested` → `result.resetLayout` rename.)

- [ ] **Step 3: `ImGuiRenderer.cpp` — consume the result, clear stale selection**

Find the call site:
```cpp
        if (m_MenuBar.Draw(ctx)) s_ResetLayout = true;
```
(it may already read `m_MenuBar.Draw(ctx, m_EditMode)` from the editor-camera work — match the actual current text). Replace it with:
```cpp
        const MainMenuBarResult menuResult = m_MenuBar.Draw(ctx, m_EditMode);
        if (menuResult.resetLayout) s_ResetLayout = true;
        if (menuResult.sceneChanged) m_EcsInspector.SetSelectedEntity(INVALID_ENTITY); // ids invalid after New/Reload
```
(`INVALID_ENTITY` is from `ECS.h`, already in scope in this file; `m_EcsInspector.SetSelectedEntity` exists.)

- [ ] **Step 4: Build the editor + confirm no dead stub remains**

```
cmake --build out/build/msvc-win64-vs2026-community --target editor
```
Expected: builds clean. Then grep `MainMenuBar.cpp` for `Save As` and `Open...` — expect zero matches.

- [ ] **Step 5: Commit**

```bash
git add src/editor/src/rendering/imgui/MainMenuBar.h src/editor/src/rendering/imgui/MainMenuBar.cpp src/editor/src/rendering/imgui/ImGuiRenderer.cpp
git commit -m "feat: wire File menu New/Reload/Save with feedback toast + shortcuts"
```

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
./out/build/msvc-win64-vs2026-community/bin/Debug/test_transientstatus.exe
```
Expected: `All ... passed.` from each (test_alloc prints one intentional ERROR line first).

- [ ] **GUI smoke (user-run; surface to the user — do not self-approve):**
  - **Save** (File→Save or Ctrl+S): writes `world.json`, green "Saved world.json" toast (~3 s). Make `world.json` read-only → red "Save failed".
  - **Reload** (Ctrl+R): edit an entity (move it), Reload → the on-disk state returns; selection cleared (outline/gizmo gone); info toast.
  - **New** (Ctrl+N): Entity Count → 0, the scene empties but the camera still works (singletons preserved); selection cleared; info toast.
  - Ctrl+N/R/S do nothing while typing in a text field (guard); File menu has no "Save As"/"Open...".
  - `runtime.exe` unaffected — no File menu, still auto-loads `world.json` at startup as before.

## Notes / non-goals
- No `GAME_API_VERSION` bump (host-owned atomics; no GameState/export/component change).
- Fixed `world.json` — no file dialog, no per-file paths, no Save As, no window-title filename.
- No dirty/unsaved-changes tracking or discard confirmation (New/Reload discard in-memory changes silently — matches the no-undo editor).
- Parent/Child hierarchy + singletons still not serialized (pre-existing `WorldManager` behavior, untouched).
