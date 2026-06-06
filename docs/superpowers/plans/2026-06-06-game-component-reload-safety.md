# Game-Component Hot-Reload Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make game-defined ECS components safe across `Game.dll` hot-reload — destroy their `ComponentArray<T>` objects (vtable/code in `Game.dll`) on the GameThread while the DLL is still mapped, including copies held by RenderThread snapshots.

**Architecture:** An ecs.dll-exported set of built-in component `type_index`es (X-macro-derived) + an `ECS::RemoveNonBuiltinComponentArrays()` clear method; plus a GameThread→RenderThread reload barrier (new atomics mirroring the existing renderer-swap handshake) so the GameThread can, at a no-snapshot-held point, clear the master + republish a built-in-only snapshot before `FreeLibrary`.

**Tech Stack:** C++23, MSVC (`msvc-win64-vs2026-community`), CMake. Touches `ecs.dll` (`ECS.h`/`ecs.cpp`), `ApplicationContext.h`, `RenderThread.cpp`, `GameThread.cpp`. No `Game.dll` API change; no `GAME_API_VERSION` bump.

**Scope:** Implements `docs/superpowers/specs/2026-06-06-game-component-reload-safety-design.md`. Prerequisite for the login UI. Touches `ECS.h` (an ecs.dll/common header) → after these commits, rebuild `ecs` + `Engine` + editor + runtime and restart the editor.

---

## Background facts (verified)

- `ComponentStore` map: `std::unordered_map<std::type_index, std::shared_ptr<IComponentArray>> m_ComponentArrays;` (`ECS.h:788`). `Cleanup()` (`ECS.h:713-715`) clears it; no remove-by-type_index / iterate-types API exists.
- X-macro `ECS_FOR_EACH_REGISTERED_COMPONENT` (`ECS.h:376-408`) lists the 33 built-in types. `ECS.h` already includes `<unordered_set>` (line 9) and `<typeindex>` (line 11).
- `ECS` is declared `class ECS_API ECS` (members exported). Exported free functions live near `ECS.h:791-801` (`GetSnapshotPoolStats`, `GetComponentArrayPoolStats`).
- `ECS::CreateSnapshot()` (`ecs.cpp:95-107`) shallow-copies the whole array map; `LatestWorldSnapshot` is `std::atomic<std::shared_ptr<const ECS>>` (`ApplicationContext.h:228`). Recycle (`ecs.cpp:60-65`) → `ResetForRecycle` → `Cleanup` runs the array destructors on whatever thread drops the last ref.
- Existing swap handshake atomics: `SwapInProgress` + `GameThreadPaused` (`ApplicationContext.h:146-152`); GameThread swap-pause (`GameThread.cpp:187-199`); RenderThread side (`RenderThread.cpp:60-74`, 5s deadline → `ExitProcess`). `ShutdownRequested` exists (`ApplicationContext.h:146`).
- RenderThread loop: drains PR/GR command rings (`RenderThread.cpp:41-158`), then **loads the snapshot at `:160-162`** (`std::shared_ptr<const ECS> worldSnapshot = LatestWorldSnapshot.load(acquire);`) held for the frame, dropped at iteration end. `<thread>`/`<chrono>` already used here.
- GameThread reload block: `GameThread.cpp:203-212` (`m_ReloadPending.exchange` → `ReleaseGameResidentConnections()` → `m_GameLib.LoadOrReload`). `gameState` is a RunLoop local; `gameState.World` persists across reload.
- `test_ecs.cpp` already defines `struct ExternProbeComponent { int Value; float Ratio; };` (Piece 1) — a type outside `ecs.dll`, not in the X-macro — plus the `EXPECT`/`EXPECT_EQ` harness and `main()` registration. Reuse it.

## Type/symbol contract (keep exact)

- `ECS_API const std::unordered_set<std::type_index>& BuiltinComponentTypes();` (declared in `ECS.h`, defined in `ecs.cpp`).
- `ComponentStore::RemoveArraysNotIn(const std::unordered_set<std::type_index>& keep)` (inline in `ECS.h`).
- `void ECS::RemoveNonBuiltinComponentArrays();` (declared in the `ECS` class in `ECS.h`, defined in `ecs.cpp`).
- `ApplicationContext`: `std::atomic<bool> ReloadInProgress{false};` + `std::atomic<bool> RenderThreadPausedForReload{false};`.

---

### Task 1: Built-in type set + `RemoveNonBuiltinComponentArrays` (ecs.dll) + unit test

**Files:**
- Modify: `src/common/include/ECS.h`
- Modify: `src/ecs/src/ecs.cpp`
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Write the failing test (extend `tests/test_ecs.cpp`)**

Add this test using the existing `ExternProbeComponent` + harness, and register it in `main()` after the other `TX*` tests:
```cpp
static void TX04_remove_non_builtin_component_arrays()
{
    // BuiltinComponentTypes() classifies built-ins vs game types.
    EXPECT(BuiltinComponentTypes().count(std::type_index(typeid(TransformComponent))) == 1);
    EXPECT(BuiltinComponentTypes().count(std::type_index(typeid(ExternProbeComponent))) == 0);

    ECS world;
    const EntityId e = world.CreateEntity();
    world.AddComponent<TransformComponent>(e, TransformComponent{});
    world.AddComponent<ExternProbeComponent>(e, ExternProbeComponent{ 5, 1.0f });

    // A snapshot taken BEFORE the clear must still resolve the game component
    // (snapshots keep game arrays — the Piece-5-preserving property).
    std::shared_ptr<const ECS> snap = world.CreateSnapshot();
    EXPECT(snap->GetComponent<ExternProbeComponent>(e) != nullptr);

    world.RemoveNonBuiltinComponentArrays();

    EXPECT(world.HasComponent<TransformComponent>(e));            // built-in survives
    EXPECT(!world.HasComponent<ExternProbeComponent>(e));         // game array removed from master
    EXPECT(snap->GetComponent<ExternProbeComponent>(e) != nullptr); // pre-clear snapshot still has it
}
```
Register `TX04_remove_non_builtin_component_arrays();` in `main()`.

- [ ] **Step 2: Build — expect RED**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
```
Expected: compile/link errors — `BuiltinComponentTypes` and `ECS::RemoveNonBuiltinComponentArrays` are undeclared.

- [ ] **Step 3: Declare the API in `ECS.h`**

- In `ComponentStore` (public section, near `Cleanup()` ~`ECS.h:713`), add an inline method:
```cpp
    // Erase component arrays whose type is not in `keep` (used to drop game-defined
    // arrays before Game.dll unload). Releases their shared_ptrs — uniquely-owned ones
    // are destroyed here on the caller's thread.
    void RemoveArraysNotIn(const std::unordered_set<std::type_index>& keep) {
        for (auto it = m_ComponentArrays.begin(); it != m_ComponentArrays.end(); ) {
            if (keep.find(it->first) == keep.end()) it = m_ComponentArrays.erase(it);
            else ++it;
        }
    }
```
- In the `ECS` class (public, near `Clear()`/`CreateSnapshot()` declarations), add:
```cpp
    // Drop all game-defined (non-built-in) component arrays from this world. Call on the
    // GameThread immediately before Game.dll unload, while the DLL is still mapped, so the
    // ComponentArray<GameType> destructors (code in Game.dll) run against live code.
    void RemoveNonBuiltinComponentArrays();
```
- Near the other exported free functions (~`ECS.h:791-801`), add:
```cpp
// The set of built-in component type_indexes (the ECS_FOR_EACH_REGISTERED_COMPONENT set).
// Anything not in here is a game-defined type. Built once; single ecs.dll instance.
ECS_API const std::unordered_set<std::type_index>& BuiltinComponentTypes();
```
(`<unordered_set>`/`<typeindex>` are already included.)

- [ ] **Step 4: Define the API in `ecs.cpp`**

Add (e.g. after the explicit-instantiation blocks / near the other free-function definitions like `GetComponentArrayPoolStats`):
```cpp
const std::unordered_set<std::type_index>& BuiltinComponentTypes() {
    static const std::unordered_set<std::type_index> s = [] {
        std::unordered_set<std::type_index> set;
        #define ECS_ADD_BUILTIN(T) set.emplace(std::type_index(typeid(T)));
        ECS_FOR_EACH_REGISTERED_COMPONENT(ECS_ADD_BUILTIN)
        #undef ECS_ADD_BUILTIN
        return set;
    }();
    return s;
}

void ECS::RemoveNonBuiltinComponentArrays() {
    m_ComponentStore.RemoveArraysNotIn(BuiltinComponentTypes());
}
```

- [ ] **Step 5: Build + run — expect GREEN**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```
Expected: links cleanly; ends with `All ECS tests passed.`

- [ ] **Step 6: Commit**

```
git -C /c/dev/clang-examples add src/common/include/ECS.h src/ecs/src/ecs.cpp tests/test_ecs.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(ecs): BuiltinComponentTypes() + RemoveNonBuiltinComponentArrays() for reload safety"
```
Verify `git -C /c/dev/clang-examples show HEAD --stat` lists exactly those three files.

---

### Task 2: Reload barrier (clear master + republish before FreeLibrary)

**Files:**
- Modify: `src/common/include/ApplicationContext.h`
- Modify: `src/engine/src/threading/RenderThread.cpp`
- Modify: `src/engine/src/threading/GameThread.cpp`

Threading wiring — verified by build + manual reload smoke (no unit test; the clear method is covered by Task 1).

- [ ] **Step 1: Add the barrier atomics (`ApplicationContext.h`)**

After the swap-coordination atomics (`GameThreadPaused`, ~line 152), add:
```cpp
    // Game.dll reload coordination. GameThread sets ReloadInProgress and waits for the
    // RenderThread to ack RenderThreadPausedForReload (a no-snapshot-held point) before
    // destroying game-defined component arrays + unloading the DLL.
    std::atomic<bool> ReloadInProgress{false};
    std::atomic<bool> RenderThreadPausedForReload{false};
```

- [ ] **Step 2: RenderThread pause point (`RenderThread.cpp`)**

Immediately BEFORE the snapshot load at `:160-162` (`std::shared_ptr<const ECS> worldSnapshot = ...`), insert:
```cpp
        // Game.dll reload barrier: pause here holding NO snapshot, so the GameThread can
        // destroy game-defined component arrays before FreeLibrary. Checked at loop top
        // (after command drains, before snapshot acquire) so the previous frame's snapshot
        // is already released.
        if (m_AppContext->ReloadInProgress.load(std::memory_order_acquire)) {
            m_AppContext->RenderThreadPausedForReload.store(true, std::memory_order_release);
            while (m_AppContext->ReloadInProgress.load(std::memory_order_acquire)
                   && !m_AppContext->ShutdownRequested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_AppContext->RenderThreadPausedForReload.store(false, std::memory_order_release);
            continue; // re-loop and acquire a fresh (clean) snapshot
        }

```
(Leave the existing `worldSnapshot` load + everything after unchanged.)

- [ ] **Step 3: GameThread reload sequence (`GameThread.cpp`)**

Replace the reload block at `:203-212` with:
```cpp
		if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
			ZoneScopedN("Game:Reload");
			// Model B: release Game.dll-resident network adapters (joins their threads) before
			// the DLL is unloaded — the networking analog of SystemScheduler::Clear().
			NetSubsystem::Instance().ReleaseGameResidentConnections();

			// Reload barrier: pause the RenderThread at a no-snapshot-held point so we can
			// destroy game-defined ComponentArray<T> objects (code in Game.dll) on THIS
			// thread while the DLL is still mapped.
			m_AppContext->ReloadInProgress.store(true, std::memory_order_release);
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!m_AppContext->RenderThreadPausedForReload.load(std::memory_order_acquire)) {
					if (std::chrono::steady_clock::now() > deadline) {
						SM_ERROR("GameThread: RenderThread did not pause for reload; proceeding (reload may be unsafe)");
						break; // best-effort; never deadlock the reload (e.g. headless/no render loop)
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}

			// Drop game-defined arrays from the master, then replace the published snapshot
			// (which shares those array objects) with a built-in-only one. Both releases
			// destroy the ComponentArray<GameType> objects HERE on the GameThread, DLL mapped.
			gameState.World.RemoveNonBuiltinComponentArrays();
			m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(),
			                                        std::memory_order_release);

			if (m_GameLib.LoadOrReload("Game.dll", &gameState)) {
				SM_TRACE("GameThread: Game.dll reloaded successfully");
			}
			// On failure, GameLibrary already logged and kept the previous module.

			m_AppContext->ReloadInProgress.store(false, std::memory_order_release); // resume RenderThread
		}
```
(Ensure `<chrono>` is available — it is used elsewhere in this file. `SM_ERROR`/`SM_TRACE` already in use.)

- [ ] **Step 4: Full build (green)**

Run:
```
cmake --build --preset msvc-win64-vs2026-community
```
Expected: clean build of all targets (`ecs`, `Engine`, `editor`, `runtime`, `game`, tests). No errors.

- [ ] **Step 5: Manual reload smoke (human-owned, interactive)**

Launch `editor.exe`; open the **Memory** panel (it iterates component arrays via `MemoryBytes` — the clearest dangling-vtable trigger). Edit `src/game/src/Game.cpp` trivially (e.g. a log line) and rebuild only the `game` target (`cmake --build --preset msvc-win64-vs2026-community --target game`) so the editor hot-reloads. Repeat 3–4 times. Confirm: no crash/hang, the editor keeps rendering (RenderThread pauses ~a frame then resumes each reload), the Memory panel stays live. (No game-defined component exists yet, so the barrier clears nothing — this verifies the barrier itself doesn't deadlock or stall; the game-component-survives-reload path is validated later by the login UI's `LoginForm`.)

- [ ] **Step 6: Commit**

```
git -C /c/dev/clang-examples add src/common/include/ApplicationContext.h src/engine/src/threading/RenderThread.cpp src/engine/src/threading/GameThread.cpp
git -C /c/dev/clang-examples commit --author="Nuno Silva <nuno.levezinho@live.com.pt>" -m "feat(reload): GameThread<->RenderThread barrier clears game component arrays before FreeLibrary"
```
Verify the `--stat` lists exactly those three files.

---

### Task 3: Full regression

**Files:** none (verification; commit fixups only if needed).

- [ ] **Step 1: Full clean build** — `cmake --build --preset msvc-win64-vs2026-community`. Expect all targets, no errors/`LNK`.

- [ ] **Step 2: Suites**
```
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_alloc.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_worldserial.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_compserial.exe
```
Expected: each prints its pass line. (`test_navagent` is a known pre-existing RED — skip.)

- [ ] **Step 3: Reload smoke** — repeat Task 2 Step 5 from a clean build if not already done (confirms the committed state). Human-owned.

- [ ] **Step 4: Commit fixups (only if Steps 1-3 required edits).**

---

## Done criteria

- `BuiltinComponentTypes()` classifies built-in vs game types; `RemoveNonBuiltinComponentArrays()` drops game arrays from the master while keeping built-ins; a pre-clear snapshot still resolves the game component (Task 1, unit-tested).
- On `Game.dll` reload the RenderThread pauses at a no-snapshot point, the GameThread clears the master + republishes a built-in-only snapshot before `FreeLibrary`, then resumes the RenderThread (Task 2). No hang/crash across repeated reloads with the Memory panel open.
- Full tree builds; `test_ecs`/`test_alloc`/`test_worldserial`/`test_compserial` green.

## Notes

- Barrier timeout is best-effort (log + proceed) — must not deadlock a headless/no-render-loop configuration (`runtime.exe`); there, no RenderThread snapshots exist, so clearing the master + replacing the atomic is still safe.
- This unblocks the **login UI** spec (`2026-06-06-login-ui-design.md`): `LoginForm` (and future game components) survive `Game.dll` reload. The login UI is the first real consumer + the full game-component-survives-reload validation.
