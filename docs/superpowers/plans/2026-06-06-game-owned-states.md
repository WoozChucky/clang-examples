# Game-Owned Game States (Boundary Piece 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the `GameStateId` enum out of `common` into a game-owned header so the game can define/rename/reorder its own application states with no engine or `ecs.dll` change, while the engine treats the current state as an opaque `uint32_t` bit index.

**Architecture:** `GameStateComponent.Current` becomes a plain `uint32_t` (the bit index the engine already used it as). The `GameStateId` enum + two `constexpr` converters move to `src/game/include/GameStates.h`, included by `game.h` via a quoted relative include (Engine compiles it transitively through `game.h`; it references nothing the engine needs). `ScopeAllows` takes `uint32_t`. The engine's only `GameStateId` consumer (`UiRenderPass`) and the editor's `StateScopeEditor` are decoupled from the enum.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community` preset), CMake. Touches `src/common/include/ECS.h`, `src/common/include/StateScope.h`, `src/game/include/`, `src/game/src/game.cpp`, `src/engine/src/rendering/passes/UiRenderPass.cpp`, `src/editor/src/panels/inspector/StateScopeEditor.cpp`, `tests/test_menu.cpp`.

**Scope:** Piece 2 of the engine/game boundary spec (`docs/superpowers/specs/2026-06-06-engine-game-boundary-design.md`). Independent of Piece 1 (already landed). Does NOT add a state-name registry — the editor's `StateScopeEditor` gets a local literal bit→label table (mirrors the game enum; a registry is Piece 5's job, noted in a comment).

---

## Background facts (verified against current code)

- `GameStateId` enum lives in `src/common/include/GameStateId.h` — values `Uninitialized=0, MainMenu=1, InLevel=2, InEditor=3, Paused=4`.
- `GameStateComponent { GameStateId Current = GameStateId::MainMenu; }` at `ECS.h:225-227`. It is a registered component (in the X-macro) but is **NOT** in `ECSCommands.h` (not editor-editable) and **NOT** in `ComponentSerialization.h` (not persisted to `world.json`) — confirmed by grep. So only the struct definition + readers/writers change.
- `ScopeAllows(uint32_t mask, GameStateId cur)` in `src/common/include/StateScope.h` does `mask == 0 || (mask & (1u << uint32_t(cur)))`. Includes `GameStateId.h`.
- Only includers of `GameStateId.h`: `ECS.h:24` and `StateScope.h:3`. (Verify with grep in Task 1 Step 1.)
- Engine consumer (read-only): `UiRenderPass.cpp:302-306` — `GameStateId uiState = GameStateId::MainMenu; if (gs) uiState = gs->Current; ... ScopeAllows(sc->StateMask, uiState)`.
- Editor: `StateScopeEditor.cpp:26-33` builds a named checkbox per state from a `{label, GameStateId}` table.
- Game writers/readers (`src/game/src/game.cpp`): compares `gs->Current != GameStateId::InLevel` (lines 126, 171, 232); `cur = gs->Current` (355); `SetState` writes `g.Current = s` (447); `g.Current = GameStateId::InLevel` (747); `g_GameState->StateId = gs->Current` (819); `SetSingleton(GameStateComponent{ GameStateId::MainMenu })` (875); `1u << uint32_t(GameStateId::MainMenu)` (897, a bit-shift — stays valid). `g_GameState->StateId` is a game-side `GameStateId` field (`game.h:29`).
- `game.h` currently gets `GameStateId` transitively via `ECS.h` (`game.h:23` comment). After this change `ECS.h` no longer provides it, so `game.h` must include the new header directly.
- `tests/test_menu.cpp` calls `ScopeAllows(..., GameStateId::X)` and includes `StateScope.h`; its CMake include path is `src/common/include` only (no game path).
- **Engine include path does NOT contain `src/game/include`** (grep of `src/engine/CMakeLists.txt`). Engine includes `game.h` (it owns `GameState`); a quoted `#include "GameStates.h"` from `game.h` resolves because the compiler searches the including file's own directory first for quoted includes. So the new header MUST sit in the same directory as `game.h` (`src/game/include/`).

## Type/symbol contract (keep exact)

- New file `src/game/include/GameStates.h`:
  ```cpp
  #pragma once
  #include <cstdint>

  // Game-owned application states. The engine never names these — it stores the
  // current state as an opaque uint32_t bit index (GameStateComponent.Current) and
  // compares it against StateScopeComponent.StateMask bits. Add/rename/reorder freely;
  // only the bit indices (0..31) are an implicit contract with authored StateMask bits.
  enum class GameStateId : uint32_t {
      Uninitialized = 0,
      MainMenu      = 1,
      InLevel       = 2,
      InEditor      = 3,
      Paused        = 4,
  };

  // Convert between the typed game enum and the engine's opaque uint32_t index.
  constexpr uint32_t   StateIndex(GameStateId s) noexcept { return static_cast<uint32_t>(s); }
  constexpr GameStateId AsGameState(uint32_t v)  noexcept { return static_cast<GameStateId>(v); }
  ```
- `GameStateComponent { uint32_t Current = 0; }` — `0` = unset/initial; the game seeds the authoritative value at startup (game.cpp). Engine treats it as an opaque bit index.
- `ScopeAllows(uint32_t mask, uint32_t cur)` — `mask == 0 || (mask & (1u << cur))`.

---

### Task 1: Move the enum to the game; make the engine/common side opaque `uint32_t`

This is one cohesive type-change ripple across 7 files — the tree only builds once all edits land, so it is a single commit. Do the sub-steps in order, then build.

**Files:**
- Create: `src/game/include/GameStates.h`
- Delete: `src/common/include/GameStateId.h`
- Modify: `src/common/include/ECS.h`, `src/common/include/StateScope.h`, `src/engine/src/rendering/passes/UiRenderPass.cpp`, `src/editor/src/panels/inspector/StateScopeEditor.cpp`, `src/game/include/game.h`, `src/game/src/game.cpp`
- Test: `tests/test_menu.cpp`

- [ ] **Step 1: Confirm the only `GameStateId.h` includers**

Run (git-bash):
```
grep -rn '#include "GameStateId.h"' /c/dev/clang-examples/src /c/dev/clang-examples/tests
```
Expected: exactly two hits — `src/common/include/ECS.h` and `src/common/include/StateScope.h`. If there are MORE, list them; each extra includer must be updated to either drop the include (if it can use `uint32_t`) or include the new game header (if it's game-side). Proceed knowing the full set.

- [ ] **Step 2: Rewrite `tests/test_menu.cpp` to the new `uint32_t` `ScopeAllows` (failing first)**

`ScopeAllows` will take a `uint32_t` state index. Update the test to stop using `GameStateId` (its CMake target sees only `src/common/include`). Replace the existing includes/usages so the test uses local `constexpr uint32_t` indices that mirror the game's bit indices. Concretely, ensure the top of the file reads:
```cpp
#include "StateScope.h" // ScopeAllows (uint32_t state index)
```
and replace the three test bodies' `GameStateId::X` usages with local constants. Add near the top of the file (after includes):
```cpp
// Mirrors the game's GameStateId bit indices (game-owned enum; values are the
// implicit contract with StateMask bits). This test exercises the pure ScopeAllows
// masking logic in common, so it uses the raw indices directly.
namespace { constexpr uint32_t kMainMenu = 1, kInLevel = 2, kPaused = 4; }
```
and rewrite the assertions to:
```cpp
    EXPECT(ScopeAllows(0u, kMainMenu));
    EXPECT(ScopeAllows(0u, kInLevel));
    ...
    const uint32_t menu = 1u << kMainMenu;
    EXPECT(ScopeAllows(menu, kMainMenu));
    EXPECT(!ScopeAllows(menu, kInLevel));
    ...
    const uint32_t mask = (1u << kMainMenu) | (1u << kPaused);
    EXPECT(ScopeAllows(mask, kMainMenu));
    EXPECT(ScopeAllows(mask, kPaused));
    EXPECT(!ScopeAllows(mask, kInLevel));
```
(Preserve the existing test-function names and the `main()` registration; only the values/types change.)

- [ ] **Step 3: Verify the test fails to build (red)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_menu
```
Expected: compile error — `ScopeAllows` still takes `GameStateId`, so passing `uint32_t` mismatches (or, if it implicitly converts, the intent is still to flip the signature next). This confirms the test now targets the new signature. (If it compiles via implicit conversion, that's acceptable — proceed; the real gate is Step 4–9 making the whole tree build with the enum moved.)

- [ ] **Step 4: Create the game-owned header**

Create `src/game/include/GameStates.h` with EXACTLY the contents from the "Type/symbol contract" section above.

- [ ] **Step 5: Make `ScopeAllows` opaque**

Replace `src/common/include/StateScope.h` entirely with:
```cpp
#pragma once
#include <cstdint>

// True if an entity scoped by `mask` is active in state index `cur`. mask == 0 means
// "always-on" (no StateScopeComponent / unscoped). Bit i set => active in state index i.
// `cur` is an opaque game-owned state index (see the game's GameStates.h); the engine
// does not name the states.
inline bool ScopeAllows(uint32_t mask, uint32_t cur) {
    return mask == 0u || (mask & (1u << cur)) != 0u;
}
```

- [ ] **Step 6: Make `GameStateComponent.Current` a `uint32_t` and drop the enum include from `ECS.h`**

In `src/common/include/ECS.h`:
- Remove the line `#include "GameStateId.h"` (line ~24).
- Change `GameStateComponent` (lines ~225-227) to:
```cpp
// Singleton: the current application state as an opaque bit index. The game owns the
// state vocabulary (see the game's GameStates.h); 0 = unset/initial, seeded by the game
// at startup. The engine compares this against StateScopeComponent.StateMask bits.
struct GameStateComponent {
    uint32_t Current = 0;
};
```

- [ ] **Step 7: Delete the common enum header**

Delete `src/common/include/GameStateId.h`:
```
git -C /c/dev/clang-examples rm src/common/include/GameStateId.h
```

- [ ] **Step 8: Update the engine consumer (`UiRenderPass`)**

In `src/engine/src/rendering/passes/UiRenderPass.cpp` (lines ~302-303), change:
```cpp
        GameStateId uiState = GameStateId::MainMenu;
        if (const auto* gs = world->GetSingleton<GameStateComponent>()) uiState = gs->Current;
```
to:
```cpp
        // Opaque state index; the game seeds the authoritative value. 0 (unset) renders
        // only unscoped entities until the game publishes its first state.
        uint32_t uiState = 0u;
        if (const auto* gs = world->GetSingleton<GameStateComponent>()) uiState = gs->Current;
```
(`ScopeAllows(sc->StateMask, uiState)` at line ~306 now type-matches; no other change. If `UiRenderPass.cpp` has its own `#include "GameStateId.h"`, remove it.)

- [ ] **Step 9: Decouple the editor `StateScopeEditor` from the enum**

In `src/editor/src/panels/inspector/StateScopeEditor.cpp`, replace the `kStates` table (lines ~26-33) so it no longer references `GameStateId`:
```cpp
    ImGui::TextDisabled("Active in states (none = always):");
    // Bit indices mirror the game-owned GameStateId (Uninitialized=0 is omitted — nothing
    // scopes to it). A registered state-name table (boundary Piece 5) will replace this
    // hardcoded mirror so the editor stops duplicating the game's state vocabulary.
    struct { const char* label; uint32_t bitIndex; } kStates[] = {
        {"Main Menu", 1},
        {"In Level",  2},
        {"In Editor", 3},
        {"Paused",    4},
    };
    for (const auto& s : kStates) {
        const uint32_t bit = 1u << s.bitIndex;
        bool on = (m_St.edit.StateMask & bit) != 0u;
        if (ImGui::Checkbox(s.label, &on)) {
            if (on) m_St.edit.StateMask |= bit; else m_St.edit.StateMask &= ~bit;
            m_St.modified = true;
        }
    }
```
(If the file has an `#include "GameStateId.h"`, remove it. It reaches `GameStateId` only through `ECS.h`/`ECSCommands.h` today; after this change it needs neither the enum nor a new include.)

- [ ] **Step 10: Point `game.h` at the new header**

In `src/game/include/game.h`:
- Update the stale comment (line ~23) from the "moved to common, included via ECS.h" note to:
```cpp
// GameStateId lives in the game-owned GameStates.h (next to this header). The engine
// stores the current state as an opaque uint32_t (GameStateComponent.Current).
```
- Add, with the other includes in `game.h`:
```cpp
#include "GameStates.h"
```
- Leave the `GameStateId StateId = GameStateId::Uninitialized;` field (line ~29) unchanged — it now resolves via the new include.

- [ ] **Step 11: Update game.cpp `GameStateComponent.Current` interactions**

In `src/game/src/game.cpp`, the game keeps using the `GameStateId` enum; only the `.Current` field reads/writes wrap through the converters:
- Lines ~126, ~171, ~232 — `gs->Current != GameStateId::InLevel` → `gs->Current != StateIndex(GameStateId::InLevel)`.
- Lines ~354-355 — the `GameStateId cur` init that reads `gs->Current`:
```cpp
        GameStateId cur = gs ? AsGameState(gs->Current) : GameStateId::MainMenu;
```
  (Replace the existing two-line `GameStateId cur = GameStateId::MainMenu; if (const auto* gs = ...) cur = gs->Current;` form; keep `cur` typed `GameStateId` and its later uses unchanged. If the `gs` pointer is fetched on a separate prior line and reused, adapt minimally so `cur` is set via `AsGameState(gs->Current)` when `gs` is non-null.)
- `SetState` (line ~447) — `g.Current = s;` → `g.Current = StateIndex(s);` (keep the signature `static void SetState(SystemContext& ctx, GameStateId s)`).
- Line ~747 — `g.Current = GameStateId::InLevel;` → `g.Current = StateIndex(GameStateId::InLevel);`.
- Line ~819 — `g_GameState->StateId = gs->Current;` → `g_GameState->StateId = AsGameState(gs->Current);`.
- Line ~875 — `SetSingleton(GameStateComponent{ GameStateId::MainMenu });` → `SetSingleton(GameStateComponent{ StateIndex(GameStateId::MainMenu) });`.
- Lines ~832, ~897, ~934 — UNCHANGED: line 832 compares the game-side `StateId` (a `GameStateId`) to `GameStateId::Uninitialized`; line 897 does `1u << static_cast<uint32_t>(GameStateId::MainMenu)` (already an index→bit shift); line 934 sets the `GameStateId StateId` field. All still valid.

- [ ] **Step 12: Build the full tree (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean build of `ecs`, `Engine`, `game`, `editor`, `runtime`, all `test_*`. Watch for: any `error` mentioning `GameStateId` (a missed reference) or `Current` type mismatch. If `error C2065: 'GameStateId': undeclared` appears in an ENGINE or COMMON TU, that file still references the enum and must be converted to `uint32_t` (engine) — do NOT add the game header to engine/common; convert to opaque index instead.

- [ ] **Step 13: Run the affected suites**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_menu --target test_ecs --target test_worldserial
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
```
Expected: `test_menu` passes (its pass line), `All ECS tests passed.`, `All world-serialization tests passed.` (`test_worldserial` exercises `StateScopeComponent` (de)serialization — confirms the mask path is intact).

- [ ] **Step 14: Commit**

```
git -C /c/dev/clang-examples add src/game/include/GameStates.h src/common/include/ECS.h src/common/include/StateScope.h src/engine/src/rendering/passes/UiRenderPass.cpp src/editor/src/panels/inspector/StateScopeEditor.cpp src/game/include/game.h src/game/src/game.cpp tests/test_menu.cpp
git -C /c/dev/clang-examples rm src/common/include/GameStateId.h
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "refactor(state): game-owned GameStateId; engine treats GameStateComponent.Current as opaque uint32_t"
```
Then verify `git -C /c/dev/clang-examples show HEAD --stat` lists exactly those files (8 modified/created + 1 deleted), no stray `assets/*.json`.

---

### Task 2: Full regression + manual UI-scope smoke

Confirm nothing that depended on `GameStateId`/state-scope filtering regressed.

**Files:** none (verification; commit only if a fixup is needed).

- [ ] **Step 1: Full clean build**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: all targets build, zero errors. (Repeat of Task 1 Step 12 to confirm a from-clean state after commit.)

- [ ] **Step 2: Run the broad suite**

Run:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_menu.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_collision.exe
```
Expected: each prints its pass line / exits 0. (`test_navagent` is a known pre-existing RED — do not run it.)

- [ ] **Step 3: Manual UI-scope smoke (editor)**

Launch `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`. Confirm:
- The main menu renders at startup (menu-scoped UI entities visible) and clicking **Play** transitions to in-level (menu hides, level UI shows) — proves `GameStateComponent.Current` round-trips as an index and `ScopeAllows` still filters by state.
- In the entity inspector, a `StateScopeComponent`'s "Active in states" checkboxes (Main Menu / In Level / In Editor / Paused) still toggle and persist — proves the decoupled `StateScopeEditor` table.
Close the editor.

- [ ] **Step 4: Commit any fixups (only if Steps 1-3 required edits)**

```
git -C /c/dev/clang-examples add -- <changed files by exact path>
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "fix(state): resolve game-owned-state fallout"
```
If no fixups were needed, skip.

---

## Done criteria

- `GameStateId` exists only in `src/game/include/GameStates.h`; `common`/`engine`/`editor` reference no game state names (engine uses `uint32_t`). `src/common/include/GameStateId.h` is deleted.
- The game can add/rename/reorder states by editing only `GameStates.h` (+ any game logic) — no `ecs.dll`/engine change for the vocabulary (the field type is already `uint32_t`).
- Full tree builds; `test_menu`/`test_ecs`/`test_worldserial`/`test_collision` green; editor menu→level transition + StateScope checkboxes work (Task 2).

## Next plans (not this one)

Piece 3 (raw input to game), Piece 4 (registered serialization), Piece 5 (generic inspector editing — replaces the hardcoded `StateScopeEditor` mirror with a registered state-name table). Each its own plan.
