# ECS Systems Layer Design

**Date:** 2026-05-20
**Status:** Draft — pending user review
**Scope:** Add a "Systems" layer to the ECS: a lightweight `ISystem` interface + `SystemScheduler` in `ecs.dll`, concrete systems living in `game.dll` and registered on load, driven by `GameThread` each tick. Migrate the existing ad-hoc gameplay logic in `game.cpp`'s `GameUpdate` (text rotation, day/night cycle) into systems as the first consumers.

## Motivation

The ECS has entities + components (COW `ComponentStore`, `ComponentArray<T>`, `View<T...>`), but no Systems. All gameplay logic lives ad-hoc inside `game.cpp`'s `GameUpdate`. This blocks composability, ordering control, and eventual editor introspection. A Systems layer gives gameplay logic a first-class home with explicit ordering, while preserving the project's hard constraints: single-writer GameThread, snapshot isolation for the RenderThread, and `game.dll` hot-reload.

## Decisions (from brainstorming)

1. **Hybrid ownership:** the `ISystem` interface + `SystemScheduler` live in `ecs.dll` (engine, always loaded). Concrete systems live in `game.dll` (hot-reloadable) and self-register on load.
2. **System shape:** a class implementing `ISystem` with a virtual `Update`. Per-system state allowed. (Hot-reload lifetime discipline below.)
3. **Context:** `Update` receives a minimal `SystemContext { ECS& world; double dt; double gameTime; }`. Global state (input, cameras, settings) reaches systems only through the ECS; that migration is deferred (see Out of scope).
4. **Ordering:** explicit `SystemPhase` enum; within a phase, registration order. Scheduler sorts by `(phase, registrationIndex)`.
5. **Registration lifecycle:** `GameThread` owns the `SystemScheduler` (survives reload, never inside the ECS/snapshot). A new optional `game.dll` export `GameRegisterSystems(SystemScheduler*)` repopulates it on load; the scheduler is cleared before each unload.
6. **Tick wiring:** `GameThread` drives the scheduler **after** `GameUpdate`, anticipating the eventual move of input/camera into Input-phase systems.
7. **v1 migration:** only pure-ECS logic (text rotation, day/night). Input/camera/state stay in `GameUpdate` for now.

## Architecture

```
ecs.dll  (engine, stays loaded)
  ISystem            interface: Update(SystemContext&), Name(), Phase()
  SystemContext      { ECS& world; double dt; double gameTime; }
  SystemPhase        enum: Input, Simulation, PostSimulation, PreRender
  SystemScheduler    owns registered systems; Run(ctx) executes by (phase, regIndex)

editor.exe / GameThread  (owns the scheduler — survives game.dll reload)
  SystemScheduler m_Scheduler        // NOT inside ECS → never copied into render snapshots

game.dll  (hot-reloadable, owns concrete systems)
  class TextRotationSystem : ISystem
  class DayNightSystem     : ISystem
  export GameRegisterSystems(SystemScheduler*)   // creates + registers instances

Per-tick (GameThread):
  drain ECS commands / model loads / render responses
  → m_GameLib.Update(&gameState)              // transitional: input, camera, state machine, spawn
  → m_Scheduler.Run({world, dt, gameTime})    // migrated sim systems, ordered by phase
  → SimulateStep / PublishSnapshot
```

**Threading:** the scheduler runs entirely on GameThread (single-writer). Systems mutate via `Modify`/`MutateArray` (COW). No parallelism in v1. RenderThread is untouched; snapshots never contain the scheduler.

## Interfaces (new header `src/common/include/Systems.h`, part of `ecs.dll`)

```cpp
#pragma once
#include "ECS.h"

struct SystemContext {
    ECS&   world;
    double dt;        // seconds since last tick (clamped by GameThread)
    double gameTime;  // absolute time
};

enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay logic — where v1 systems live
    PostSimulation = 2,   // reactions to sim
    PreRender      = 3,    // last-chance ECS prep before snapshot
};

class ECS_API ISystem {
public:
    virtual ~ISystem() = default;
    virtual void Update(SystemContext& ctx) = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
    [[nodiscard]] virtual SystemPhase Phase() const = 0;
};

class ECS_API SystemScheduler {
public:
    void Register(std::unique_ptr<ISystem> system);   // takes ownership; within-phase = insertion order
    void Clear();                                      // destroys all systems; MUST run while game.dll still loaded
    void Run(SystemContext& ctx);                      // runs each system once, ordered by (phase, regIndex)
    [[nodiscard]] size_t Count() const;

private:
    std::vector<std::unique_ptr<ISystem>> m_Systems;
    bool m_Dirty = false;   // re-sort lazily on first Run after Register/Clear
};
```

`SystemScheduler` is `ECS_API`-exported but is **not** part of `ECS` and is never snapshotted — it is a standalone engine type owned by `GameThread`.

**game.dll export** (added to `game.h`; include `Systems.h`; bump `GAME_API_VERSION`):

```cpp
EXPORT_FN void GameRegisterSystems(SystemScheduler* scheduler);   // optional export (nullptr-tolerant)
using GameRegisterSystemsFunc = void(*)(SystemScheduler*);
```

```cpp
// game.cpp
void GameRegisterSystems(SystemScheduler* s) {
    s->Register(std::make_unique<TextRotationSystem>());
    s->Register(std::make_unique<DayNightSystem>());
}
```

## DLL memory boundary & hot-reload safety

When `game.dll` runs `std::make_unique<TextRotationSystem>()`, three memory pieces are involved:

1. **The `unique_ptr` + vector buffer** (`m_Systems`) — allocated by `SystemScheduler::Register` (ecs.dll code) in the **shared CRT heap** (`CMAKE_MSVC_RUNTIME_LIBRARY` is locked to `MultiThreaded…DLL` → one heap across modules). Survives reload.
2. **The concrete object's bytes** — `new`'d by game.dll but also in the shared CRT heap. The block survives `FreeLibrary`.
3. **The object's vtable + virtual function code** (`Update`, `~`, `Name`, `Phase`) — live **inside game.dll's image**. `FreeLibrary` unmaps them. **Gone after reload.**

Consequently, if systems are still registered when `game.dll` is unloaded, every `unique_ptr<ISystem>` holds a valid heap pointer with a **dangling vtable**. Any virtual call crashes, and — critically — *destroying* the `unique_ptr` calls the virtual `~ISystem()` through the dead vtable, so it cannot even be cleaned up afterward.

**Rule:** `SystemScheduler::Clear()` must run **before** `FreeLibrary`, while game.dll is still mapped, so each `~Concrete()` dispatches through a valid vtable and frees its block on the shared heap. This is *why* `unique_ptr` is correct here — it gives deterministic destruction at the `Clear()` point we control, rather than at some later unsafe moment. The load/unload lifecycle (below) enforces this at every seam.

## Lifecycle wiring

`GameThread` owns both `m_GameLib` (existing `GameLibrary`) and `m_Scheduler`. Because the `Clear()`-before-`FreeLibrary` ordering happens inside `GameLibrary::LoadOrReload`'s atomic teardown, `GameLibrary` holds a `SystemScheduler*` (set once by `GameThread`) and resolves the optional `GameRegisterSystems` symbol.

**`LoadOrReload` sequence** (ordered so a failed reload preserves the old module *and* its systems):
```
1. copy dll, LoadLibrary, resolve symbols (incl. optional GameRegisterSystems), validate GAME_API_VERSION
   — on any failure: free the new module, return false; old module + its systems untouched
2. tear down OLD module (if present):
     m_pGameExit(state)
     m_Scheduler->Clear()        // destroy ISystem instances — vtables still valid
     FreeLibrary(m_Module)
3. install new function pointers
4. if (m_pGameRegisterSystems) m_pGameRegisterSystems(m_Scheduler);   // fresh instances
```

**Initial load:** no old module → only step 4 registers.

**App shutdown** (`GameLibrary::Unload` / dtor):
```
m_pGameExit(state)
m_Scheduler->Clear()    // before the final FreeLibrary, else ~SystemScheduler later
FreeLibrary(m_Module)   // destroys dangling-vtable objects → crash on exit
```

**GameThread tick** gains, immediately after the existing `m_GameLib.Update(&gameState)`:
```cpp
SystemContext ctx{ gameState.World, gameState.DeltaTime, gameState.GameTime };
m_Scheduler.Run(ctx);
```

## Migration

**Moves into systems (Simulation phase), in `game.dll`:**

- **`TextRotationSystem`** — current `MainMenu` loop over `View<TextComponent, TransformComponent>`: wrap `Rotation.z += rate*dt` mod 2π, cycle text color from `dt`/`gameTime`. Fully expressible with the minimal context.
- **`DayNightSystem`** — current `View<SunMarker, LightningComponent>` loop driving `Direction` + `Color`.
  - **Caveat:** today it reads `g_GameState->DayNightCycleSeconds` (a `GameState` float not in the ECS). The minimal context cannot reach it, so v1 uses a compile-time constant equal to the current `10.0f` default. Runtime-configurable cycle length waits for the deferred singleton-component pass.

**Stays in `GameUpdate` (transitional):** state machine (`Uninitialized → MainMenu`), default-scene entity spawn, `DrainInput`, `HandleCameraMovement`, `HandleFreeLook`, mouse-aim, F12-spawn.

**Behavioral change to note:** the migrated systems previously ran only in the `MainMenu` state; the scheduler runs them **every tick regardless of `StateId`**. With one effective state today this matches current behavior. System state-gating is deferred.

After migration the `MainMenu` case in `GameUpdate` loses the rotation/day-night blocks; `GameRegisterSystems` lists the two systems in run order.

## Error handling

| Source | Behavior |
|---|---|
| `game.dll` lacks `GameRegisterSystems` (old module) | Optional export → scheduler stays empty, `Run` is a no-op, editor runs. |
| Empty scheduler | `Run` no-op. |
| A system throws | Propagates up GameThread (process dies) — same policy as `GameUpdate` today. No per-system try/catch / SEH in v1. Keep systems benign. |
| System adds/removes entities mid-iteration | Safe: `View<>()` returns a copy of the matching `EntityId` vector. New entities appear next tick; removed ones make `Modify` a no-op. |
| Reload mid-tick | Cannot happen: reload is drained at tick top; `Clear`/`Register` complete inside `LoadOrReload` before `Run` later in the same tick. |
| Reload validation failure | Old module + its registered systems untouched (Clear happens only after the new module validates). |

**Invariants:**
- `Clear()` always precedes `FreeLibrary` (hot-reload and shutdown).
- The `SystemScheduler` is never part of `ECS` and never enters a render snapshot.
- System execution order is deterministic: stable sort by `(phase, registrationIndex)`.

## Testing

Unlike most of this project, the scheduler is **pure `ecs.dll` (no game.dll, no GPU) and genuinely unit-testable.** Add to `test_ecs.exe`:

- Register + `Count`.
- Run order: recording systems append their `Name()` to a shared vector; assert ordering across phases and insertion order within a phase.
- `Clear` destroys instances: `Count()` → 0, and a dtor-counter system confirms destructors actually run.
- Empty-scheduler `Run` is a no-op.
- A system that mutates the ECS via `Modify` takes effect (assert component value after `Run`).

**Manual smoke:** editor — text still spins and day/night still cycles, now via systems. Edit a system constant, rebuild `game` → hot-reload re-registers, no crash, behavior updates, `Count()` stable (no duplicate registration). Restart cleanly (no crash on exit from `Clear`-before-`FreeLibrary`).

## Build / rebuild

`Systems.h` is part of `ecs.dll`'s public headers (in `src/common/include`). `SystemScheduler`/`ISystem` are `ECS_API`-exported from `ecs.dll`. `game.h` includes `Systems.h` and declares `GameRegisterSystems`; `GAME_API_VERSION` is bumped. Rebuild `ecs` + `editor` + `game` together; restart the editor (header/ABI change).

## Out of scope (v1)

- Parallel / multithreaded system execution.
- System dependency declarations / topological ordering.
- Singleton components (moving input, cameras, settings into the ECS) — the deferred follow-up that lets input/camera become Input/Simulation-phase systems and shrinks `GameUpdate` to nothing.
- Editor UI to enable/disable or reorder systems.
- System state-gating (running a system only in certain `GameStateId`s).
- `View<>()` allocation optimization (it still returns a fresh `vector<EntityId>` per call).
