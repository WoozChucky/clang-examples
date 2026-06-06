# Game-component hot-reload safety — design

**Date:** 2026-06-06
**Branch:** `feat/game-component-reload-safety`
**Status:** DESIGN

## Goal

Make game-defined ECS components safe across `Game.dll` hot-reload. The engine/game boundary (Piece 1) let `Game.dll` define + use its own component types, but their `ComponentArray<T>` objects carry a vtable + instantiated template methods that live in `Game.dll`. The array objects, however, live in the host-owned ECS world (and ride along in RenderThread snapshots). On reload the old DLL is `FreeLibrary`'d; a later virtual call or destructor on one of those arrays (e.g. snapshot recycle on the RenderThread, or the editor Memory panel's `MemoryBytes`) executes in an unmapped module → crash.

This is a **prerequisite** for the login UI (whose `LoginForm` is the first live game-defined component) and benefits every future game component. It is the proper fix for the risk surfaced in `2026-06-06-login-ui-design.md`.

**Decision (locked during design):** keep game-defined arrays IN snapshots (so the Piece-5 editor can still inspect/edit game components via `ctx.WorldSnapshot`), and make reload safe with a GameThread→RenderThread barrier — rather than excluding game arrays from snapshots.

## Context (as-built, verified)

- **Snapshot publish:** `GameThread::PublishSnapshot` (`GameThread.cpp:592-614`) → `ECS::CreateSnapshot()` (`ecs.cpp:95-107`) → `ComponentStore::CopyArraysFrom` (`ecs.cpp:159-161`) does `m_ComponentArrays = other.m_ComponentArrays` — a **shallow copy of the whole array map**, bumping every `ComponentArray<T>` shared_ptr (built-in AND game). Stored into `m_AppContext->LatestWorldSnapshot` — type `std::atomic<std::shared_ptr<const ECS>>` (`ApplicationContext.h:228`).
- **Snapshot consume:** `RenderThread` (`RenderThread.cpp:160-208`) `LatestWorldSnapshot.load(acquire)` into a frame-local `shared_ptr<const ECS>` (held for the whole frame, dropped at loop-iteration end). Loads the ECS snapshot **before** the `SimulationSnapshot` (ordering rule, `docs/ECS_Threading_Architecture.md` issue 1).
- **Snapshot recycle:** `SnapshotPool::Recycle` (`ecs.cpp:60-65`) fires from the shared_ptr deleter **on whatever thread drops the last ref** → `ECS::ResetForRecycle()` (`ECS.h:1054-1055`) → `ComponentStore::Cleanup()` (`ECS.h:713-715`) `m_ComponentArrays.clear()` (releases all array refs; destroys uniquely-owned ones — **the destructor runs on that thread**). So a snapshot's last ref dropping on the RenderThread *after* `FreeLibrary` destroys a `ComponentArray<GameType>` with a dead vtable.
- **Reload site:** `GameThread.cpp:203-212` — on `m_ReloadPending`, calls `NetSubsystem::ReleaseGameResidentConnections()` then `m_GameLib.LoadOrReload("Game.dll", &gameState)`. `GameLibrary::LoadOrReload`/`Unload` (`GameLibrary.cpp`) do `Scheduler->Clear()` then `FreeLibrary(m_Module)` then delete the DLL file (so the file lock is released for rebuild — **`FreeLibrary` is mandatory**, can't defer-forever). The `GameState.World` persists across reload (not reconstructed; `WorldLoaded` guard).
- **Existing barrier pattern:** renderer hot-swap uses `SwapInProgress` (set by RenderThread) + `GameThreadPaused` (acked by GameThread) (`ApplicationContext.h:146-152`, `GameThread.cpp:187-199`, `RenderThread.cpp:60-74`) — RenderThread waits up to 5s for the ack else hard-errors. This is the template for B's barrier (opposite direction).
- **Builtin set:** `ECS_FOR_EACH_REGISTERED_COMPONENT` (`ECS.h:376-408`) lists the 33 built-in types. **No exported `type_index` set exists.** The map is `unordered_map<type_index, shared_ptr<IComponentArray>>` (`ECS.h:788`); `ComponentStore` has no public iterate-type_indexes or remove-by-type_index. `Cleanup()` clears the whole map; `Clear()` removes active entities' components but keeps the map.

## Components

### 1. Builtin component type set (ecs.dll, exported)
`ECS_API const std::unordered_set<std::type_index>& BuiltinComponentTypes();` — a function-local static built once from the X-macro:
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
```
Defined in an ecs.dll TU (e.g. `ecs.cpp`). Single source of "engine-owned component types."

### 2. `ECS::RemoveNonBuiltinComponentArrays()` (ecs.dll, exported)
Erases array-map entries whose `type_index` is not in `BuiltinComponentTypes()`:
```cpp
void ECS::RemoveNonBuiltinComponentArrays() {
    m_ComponentStore.RemoveArraysNotIn(BuiltinComponentTypes());
}
```
`ComponentStore::RemoveArraysNotIn(const std::unordered_set<std::type_index>&)` iterates `m_ComponentArrays` and erases non-member entries (releasing/destroying the game `ComponentArray<T>` objects). **GameThread-only**; the caller guarantees the `Game.dll` is still mapped when this runs, so the destructors execute against live code. (Does not touch built-in arrays or entity data of built-ins.)

### 3. Reload barrier (ApplicationContext atomics)
Add, mirroring the swap handshake:
```cpp
std::atomic<bool> ReloadInProgress{false};         // set by GameThread during reload
std::atomic<bool> RenderThreadPausedForReload{false}; // acked by RenderThread
```

### 4. RenderThread pause point
At the **top of the render loop, before** `LatestWorldSnapshot.load(...)` (so it holds no snapshot while paused), add a check mirroring `GameThread`'s swap-pause:
```cpp
if (m_AppContext->ReloadInProgress.load(acquire)) {
    m_AppContext->RenderThreadPausedForReload.store(true, release);
    while (m_AppContext->ReloadInProgress.load(acquire) && /* not shutting down */) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m_AppContext->RenderThreadPausedForReload.store(false, release);
    continue; // re-loop; acquire a fresh snapshot
}
```
Because the check is at loop top, the RenderThread finishes its current frame (dropping its frame-local snapshot) before it loops and observes the flag — so the pause lands at a no-snapshot-held point.

### 5. GameThread reload sequence (replaces `GameThread.cpp:203-212`)
```cpp
if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
    NetSubsystem::Instance().ReleaseGameResidentConnections();

    // Barrier: pause RenderThread at a no-snapshot-held point.
    m_AppContext->ReloadInProgress.store(true, std::memory_order_release);
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_AppContext->RenderThreadPausedForReload.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                SM_ERROR("GameThread: RenderThread did not pause for reload; proceeding (reload may be unsafe)");
                break; // best-effort; do not deadlock the reload
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // RenderThread now holds no snapshot. Drop game arrays + the published snapshot
    // that still references them, destroying those ComponentArray<GameType> objects
    // HERE on GameThread while Game.dll is still mapped.
    gameState.World.RemoveNonBuiltinComponentArrays();                       // master drops them
    m_AppContext->LatestWorldSnapshot.store(gameState.World.CreateSnapshot(),
                                            std::memory_order_release);       // drops the old snapshot's refs → recycle on GameThread
    // (Built-in-only snapshot now; CreateSnapshot is ecs.dll code, no Game.dll needed.)

    m_GameLib.LoadOrReload("Game.dll", &gameState);                          // FreeLibrary now safe

    m_AppContext->ReloadInProgress.store(false, std::memory_order_release);  // resume RenderThread
}
```

## Why this is safe (lifetime argument)

At the moment of `FreeLibrary`, every `ComponentArray<GameType>` object must already be destroyed. The holders of such arrays are exactly:
- **The master world** — released by `RemoveNonBuiltinComponentArrays()` (its refs dropped; if it were the only holder the object dies now).
- **The published snapshot** in `LatestWorldSnapshot` — shares the *same* array objects (shallow copy). Replacing the atomic with a fresh built-in-only snapshot drops that snapshot's refs; combined with the master release, the `ComponentArray<GameType>` refcount reaches 0 → destroyed during the old snapshot's recycle, which runs **on GameThread** (the thread doing `store`).
- **RenderThread's frame-local snapshot** — eliminated by the barrier: RenderThread is paused holding none.
- **Pooled (free-list) snapshot shells** — already cleared (`Cleanup` on prior recycle); hold nothing.

So all game-array destructors run on GameThread before `FreeLibrary`. After reload, the new DLL re-seeds its components on its next tick; the subsequent snapshot re-includes them (now backed by the new module). The editor inspector (Piece 5) continues to see game components in the snapshot.

## Error handling

- **Barrier timeout (5s):** log `SM_ERROR` and proceed best-effort (do not deadlock the reload). Matches the spirit of the swap handshake; chosen softer than the swap's `ExitProcess` because a missed reload barrier degrades to the pre-existing risk rather than warranting a hard process kill. (A stuck/absent RenderThread — e.g. `runtime.exe` headless with no render loop — must not hang the GameThread; see below.)
- **Headless / no RenderThread:** if no RenderThread runs (or it never acks), the timeout path proceeds. In that configuration there are no RenderThread-held snapshots anyway, so clearing the master + replacing the atomic is still safe. Document this.

## Testing

- **Unit (`test_ecs`):** define a component type **in the test TU** (outside `ecs.dll`, not in the X-macro — reuse/extend the `ExternProbeComponent` pattern). Assert:
  - `BuiltinComponentTypes()` contains a built-in (e.g. `TransformComponent`) and does **not** contain the test type.
  - Add the test component + a built-in to a world; `RemoveNonBuiltinComponentArrays()` → the test array is gone (`GetComponent` returns null / `HasComponent` false), the built-in survives.
  - A snapshot taken **before** the clear still resolves the test component (proves snapshots keep game arrays — the Piece-5-preserving property); after clear + a fresh snapshot, the new snapshot lacks it.
- **Manual reload smoke (human-owned):** rebuild `Game.dll` and trigger reload several times in the editor with the Memory panel open — no hang, no crash, RenderThread visibly continues (the barrier pauses+resumes). This works even before any game component exists (the barrier runs regardless; it just clears nothing). Full "game component survives reload" validation arrives with the login UI's `LoginForm`.

## Out of scope (deliberate)

- The login UI (separate spec; builds on this).
- Excluding game components from snapshots (rejected in favor of the barrier, to keep Piece-5 editor editing of game components working).
- Deferring `FreeLibrary` (rejected — the DLL file lock must release for rebuild).
- A general renderer/GameThread reload framework beyond what reload safety needs.

## Decisions locked

- Keep game arrays in snapshots; make reload safe via a GameThread→RenderThread barrier + clear-master + republish-clean-snapshot before `FreeLibrary`.
- `BuiltinComponentTypes()` (X-macro-derived, exported from ecs.dll) is the builtin/game discriminator; `ECS::RemoveNonBuiltinComponentArrays()` does the clear.
- New `ReloadInProgress` / `RenderThreadPausedForReload` atomics in `ApplicationContext`; RenderThread pause check at loop top before snapshot acquire.
- Barrier timeout is best-effort (log + proceed), not a hard process kill.

## Build / test note

Build & test with the `msvc-win64-vs2026-community` preset only. Touches `ecs.dll` (`ECS.h`/`ecs.cpp`: builtin set + `RemoveNonBuiltinComponentArrays` + `ComponentStore::RemoveArraysNotIn`), `ApplicationContext.h` (atomics), `RenderThread.cpp` + `GameThread.cpp` (barrier) — all engine/common, so rebuild `ecs` + `Engine` + editor + runtime; no `Game.dll` API change, no `GAME_API_VERSION` bump. Commit identity: `Nuno Silva <nuno.levezinho@live.com.pt>`.
