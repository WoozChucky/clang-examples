# ECS Systems Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **MODEL NOTE:** Task 6 (GameLibrary hot-reload wiring) is high-risk — it governs destruction of `game.dll`-owned `ISystem` instances across `FreeLibrary` (dangling-vtable territory). Dispatch Task 6's implementer and reviewer with **Opus 4.7** (`model: opus`). Other tasks may use a standard model, but Opus is acceptable throughout.

**Goal:** Add a Systems layer to the ECS — `ISystem` + `SystemScheduler` in `ecs.dll`, concrete systems in `game.dll` registered via a new `GameRegisterSystems` export, driven by `GameThread` after `GameUpdate` — and migrate the text-rotation + day/night logic out of `game.cpp`'s `GameUpdate` into systems.

**Architecture:** `SystemScheduler` (owned by `GameThread`, never inside the ECS/snapshot) holds `unique_ptr<ISystem>` instances created by `game.dll`. The reload lifecycle clears the scheduler **before** `FreeLibrary` (so `ISystem` dtors run while their vtables are still mapped) and re-registers after each load. Systems run sequentially on GameThread; they read time + mutate the ECS via COW (`Modify`/`MutateArray`).

**Tech Stack:** C++23, CMake presets (`msvc-win64-vs2026-community`), MSVC/VS2026, the existing `ecs.dll` / `game.dll` (SHARED, hot-reloaded) / `editor.exe` / `test_ecs.exe` targets.

**Spec:** `docs/superpowers/specs/2026-05-20-ecs-systems-design.md` — read first, especially the "DLL memory boundary & hot-reload safety" section.

> **Testing note:** The scheduler is pure `ecs.dll` (no game.dll/GPU) and IS unit-testable — Task 3 adds real tests to `test_ecs.exe`. The game-side migration + hot-reload wiring are verified by build + manual smoke (Task 8).

---

## File Structure

**Created:**
- `src/common/include/Systems.h` — `SystemContext`, `SystemPhase`, `ISystem`, `SystemScheduler` (public `ecs.dll` interface).
- `src/ecs/src/systems.cpp` — `SystemScheduler` method definitions + `ISystem` out-of-line anchor (single vtable/typeinfo home in `ecs.dll`).

**Modified:**
- `src/ecs/CMakeLists.txt` — add `src/systems.cpp` to the `ecs` target.
- `tests/test_ecs.cpp` — unit tests for `SystemScheduler`.
- `src/game/include/game.h` — include `Systems.h`; add `GameRegisterSystems` export decl + `GameRegisterSystemsFunc`; bump `GAME_API_VERSION` 3→4.
- `src/game/src/game.cpp` — `TextRotationSystem`, `DayNightSystem`, `GameRegisterSystems`; remove the migrated blocks from `GameUpdate`'s `MainMenu` case.
- `src/editor/src/threading/GameLibrary.h` / `.cpp` — `SystemScheduler*` member + `SetScheduler`; resolve `GameRegisterSystems`; `Clear`/`Register` at the load/unload seams. **(Task 6 — Opus.)**
- `src/editor/src/threading/GameThread.h` / `.cpp` — own `SystemScheduler m_Scheduler`; `SetScheduler` before initial load; `m_Scheduler.Run(ctx)` after `m_GameLib.Update`.

---

## Build / run commands

- Build ecs only: `cmake --build --preset msvc-win64-vs2026-community --target ecs`
- Build test_ecs: `cmake --build --preset msvc-win64-vs2026-community --target test_ecs`
- Build all in-scope: `cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs`
- Run tests: `./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe`
- Run editor: `./out/build/msvc-win64-vs2026-community/bin/Debug/editor.exe`
- Re-configure (after CMakeLists change): `cmake --preset msvc-win64-vs2026-community`

`runtime.exe` is broken on `main` (pre-existing, unrelated) — do not build it.

---

## Task 1: `Systems.h` interface

**Files:**
- Create: `src/common/include/Systems.h`

- [ ] **Step 1: Write the header**

Create `src/common/include/Systems.h`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ECS.h"   // ECS + ECS_API

// Minimal per-tick context handed to every system.
struct SystemContext {
    ECS&   world;
    double dt;        // seconds since last tick (clamped by GameThread)
    double gameTime;  // absolute time
};

// Coarse run-order buckets. Systems sort by (phase, registration index).
enum class SystemPhase : uint8_t {
    Input          = 0,   // (future) input-derived state
    Simulation     = 1,   // gameplay logic — where v1 systems live
    PostSimulation = 2,   // reactions to simulation
    PreRender      = 3,    // last-chance ECS prep before snapshot
};

// A unit of gameplay logic. Concrete systems live in game.dll; their vtables
// are unmapped on game.dll reload, so SystemScheduler::Clear() MUST run before
// FreeLibrary (see the spec's hot-reload section).
class ECS_API ISystem {
public:
    virtual ~ISystem();   // out-of-line anchor in systems.cpp (single vtable home)
    virtual void Update(SystemContext& ctx) = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
    [[nodiscard]] virtual SystemPhase Phase() const = 0;
};

// Owns registered systems; runs them ordered by (phase, registration index).
// Owned by GameThread — never stored inside ECS and never snapshotted.
class ECS_API SystemScheduler {
public:
    SystemScheduler() = default;
    ~SystemScheduler();

    SystemScheduler(const SystemScheduler&) = delete;
    SystemScheduler& operator=(const SystemScheduler&) = delete;

    // Takes ownership. Within a phase, runs in registration order.
    void Register(std::unique_ptr<ISystem> system);

    // Destroys all systems. MUST be called while game.dll is still loaded
    // (system dtors dispatch through vtables that live in game.dll).
    void Clear();

    // Runs each system once, ordered by (phase, registration index).
    void Run(SystemContext& ctx);

    [[nodiscard]] size_t Count() const { return m_Systems.size(); }

private:
    struct Entry {
        std::unique_ptr<ISystem> system;
        SystemPhase phase;
        uint32_t    order;   // registration index, for stable within-phase order
    };
    std::vector<Entry> m_Systems;
    uint32_t m_NextOrder = 0;
    bool     m_Sorted    = true;
};
```

- [ ] **Step 2: Commit**

```bash
git add src/common/include/Systems.h
git commit -m "Add Systems.h: ISystem + SystemScheduler interface"
```

---

## Task 2: `SystemScheduler` implementation + `ISystem` anchor

**Files:**
- Create: `src/ecs/src/systems.cpp`
- Modify: `src/ecs/CMakeLists.txt`

- [ ] **Step 1: Write the implementation**

Create `src/ecs/src/systems.cpp`:

```cpp
#include "Systems.h"

#include <algorithm>

#include "lib.h"   // SM_TRACE

// Out-of-line anchor: gives ISystem's vtable + typeinfo a single home in ecs.dll.
ISystem::~ISystem() = default;

SystemScheduler::~SystemScheduler() {
    // Defensive: by contract Clear() runs before game.dll unload. If a scheduler
    // is destroyed with systems still registered AND game.dll already unloaded,
    // this would dispatch dead vtables — so callers must Clear() first. We still
    // clear here for the in-process-teardown (game.dll loaded) case.
    Clear();
}

void SystemScheduler::Register(std::unique_ptr<ISystem> system) {
    if (!system) return;
    const SystemPhase phase = system->Phase();
    m_Systems.push_back(Entry{ std::move(system), phase, m_NextOrder++ });
    m_Sorted = false;
}

void SystemScheduler::Clear() {
    m_Systems.clear();
    m_NextOrder = 0;
    m_Sorted = true;
}

void SystemScheduler::Run(SystemContext& ctx) {
    if (!m_Sorted) {
        std::stable_sort(m_Systems.begin(), m_Systems.end(),
            [](const Entry& a, const Entry& b) {
                if (a.phase != b.phase)
                    return static_cast<uint8_t>(a.phase) < static_cast<uint8_t>(b.phase);
                return a.order < b.order;
            });
        m_Sorted = true;
    }
    for (Entry& e : m_Systems) {
        e.system->Update(ctx);
    }
}
```

(`stable_sort` over `(phase, order)` is belt-and-suspenders; `order` already makes it total, but stable_sort keeps it robust if two entries ever compare equal.)

- [ ] **Step 2: Add the source to the ecs target**

Edit `src/ecs/CMakeLists.txt`. Find the `add_library(ecs SHARED ...)` source list (currently `src/ecs.cpp`) and add `src/systems.cpp`:

```cmake
add_library(ecs SHARED
    src/ecs.cpp
    src/systems.cpp
)
```

- [ ] **Step 3: Re-configure + build ecs**

```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target ecs
```

Expected: clean build; `ecs.dll` links with the new symbols exported.

- [ ] **Step 4: Commit**

```bash
git add src/ecs/src/systems.cpp src/ecs/CMakeLists.txt
git commit -m "Implement SystemScheduler + ISystem anchor in ecs.dll"
```

---

## Task 3: Unit tests for `SystemScheduler`

**Files:**
- Modify: `tests/test_ecs.cpp`

- [ ] **Step 1: Add a `#include "Systems.h"` near the existing includes**

In `tests/test_ecs.cpp`, after `#include "ECS.h"` (line 7), add:

```cpp
#include "Systems.h"
```

- [ ] **Step 2: Add recording/mutating test systems + the test functions**

Add these above `int main()` (e.g. after `T12_concurrent_smoke`). They use the existing `EXPECT`/`EXPECT_EQ` macros.

```cpp
// --- Systems layer tests ---

// Records its name into a shared sink when run; carries a configurable phase.
struct RecordingSystem final : ISystem {
    RecordingSystem(const char* name, SystemPhase phase, std::vector<std::string>* sink)
        : m_Name(name), m_Phase(phase), m_Sink(sink) {}
    void Update(SystemContext&) override { m_Sink->push_back(m_Name); }
    const char* Name() const override { return m_Name; }
    SystemPhase Phase() const override { return m_Phase; }
    const char* m_Name;
    SystemPhase m_Phase;
    std::vector<std::string>* m_Sink;
};

// Increments a counter on destruction (proves Clear() runs dtors).
struct DtorCounterSystem final : ISystem {
    explicit DtorCounterSystem(int* counter) : m_Counter(counter) {}
    ~DtorCounterSystem() override { ++(*m_Counter); }
    void Update(SystemContext&) override {}
    const char* Name() const override { return "DtorCounter"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
    int* m_Counter;
};

// Adds a TransformComponent to entity 1 and bumps its X each Update.
struct MutateSystem final : ISystem {
    void Update(SystemContext& ctx) override {
        if (!ctx.world.HasComponent<TransformComponent>(1)) {
            ctx.world.AddComponent(1, TransformComponent{{0, 0, 0}, {}, {1, 1, 1}});
        }
        ctx.world.Modify<TransformComponent>(1, [](TransformComponent& t) {
            t.Position.x += 1.0f;
        });
    }
    const char* Name() const override { return "Mutate"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

static void TS01_register_and_count()
{
    SystemScheduler sched;
    EXPECT_EQ(sched.Count(), 0u);
    std::vector<std::string> sink;
    sched.Register(std::make_unique<RecordingSystem>("a", SystemPhase::Simulation, &sink));
    sched.Register(std::make_unique<RecordingSystem>("b", SystemPhase::Simulation, &sink));
    EXPECT_EQ(sched.Count(), 2u);
}

static void TS02_runs_in_phase_then_registration_order()
{
    SystemScheduler sched;
    std::vector<std::string> sink;
    // Register out of phase order on purpose.
    sched.Register(std::make_unique<RecordingSystem>("sim1",   SystemPhase::Simulation,     &sink));
    sched.Register(std::make_unique<RecordingSystem>("input1", SystemPhase::Input,          &sink));
    sched.Register(std::make_unique<RecordingSystem>("sim2",   SystemPhase::Simulation,     &sink));
    sched.Register(std::make_unique<RecordingSystem>("pre1",   SystemPhase::PreRender,      &sink));
    sched.Register(std::make_unique<RecordingSystem>("post1",  SystemPhase::PostSimulation, &sink));

    ECS world;
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);

    // Input < Simulation(reg order sim1,sim2) < PostSimulation < PreRender
    EXPECT_EQ(sink.size(), 5u);
    EXPECT(sink[0] == "input1");
    EXPECT(sink[1] == "sim1");
    EXPECT(sink[2] == "sim2");
    EXPECT(sink[3] == "post1");
    EXPECT(sink[4] == "pre1");
}

static void TS03_clear_destroys_systems()
{
    int dtorCount = 0;
    {
        SystemScheduler sched;
        sched.Register(std::make_unique<DtorCounterSystem>(&dtorCount));
        sched.Register(std::make_unique<DtorCounterSystem>(&dtorCount));
        EXPECT_EQ(sched.Count(), 2u);
        sched.Clear();
        EXPECT_EQ(sched.Count(), 0u);
        EXPECT_EQ(dtorCount, 2);   // dtors actually ran
    }
    EXPECT_EQ(dtorCount, 2);       // no double-destroy from scheduler dtor
}

static void TS04_empty_run_is_noop()
{
    SystemScheduler sched;
    ECS world;
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);               // must not crash
    EXPECT_EQ(sched.Count(), 0u);
}

static void TS05_system_mutates_ecs()
{
    SystemScheduler sched;
    sched.Register(std::make_unique<MutateSystem>());
    ECS world;
    world.CreateEntity();         // id 1
    SystemContext ctx{ world, 0.016, 1.0 };
    sched.Run(ctx);
    sched.Run(ctx);               // run twice → x == 2
    const auto* t = world.GetComponent<TransformComponent>(1);
    EXPECT_NE(t, nullptr);
    if (t) EXPECT_EQ(t->Position.x, 2.0f);
}
```

(If `CreateEntity()` does not return id `1` on a fresh `ECS`, adjust `TS05` to capture the returned id and use it; the entity store starts `m_NextEntityId = 1`, so the first id is `1`.)

- [ ] **Step 3: Register the tests in `main()`**

In `tests/test_ecs.cpp`'s `main()`, after `T12_concurrent_smoke();`, add:

```cpp
    TS01_register_and_count();
    TS02_runs_in_phase_then_registration_order();
    TS03_clear_destroys_systems();
    TS04_empty_run_is_noop();
    TS05_system_mutates_ecs();
```

- [ ] **Step 4: Build + run tests**

```
cmake --build --preset msvc-win64-vs2026-community --target test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```

Expected: `All ECS tests passed.` (exit 0). If `TS02` fails, check the phase enum ordering and the `stable_sort` comparator.

- [ ] **Step 5: Commit**

```bash
git add tests/test_ecs.cpp
git commit -m "Add SystemScheduler unit tests (order, clear, mutate)"
```

---

## Task 4: game.dll API — export decl + version bump

**Files:**
- Modify: `src/game/include/game.h`

- [ ] **Step 1: Include Systems.h + declare the export + bump the version**

In `src/game/include/game.h`:

Add near the top includes (after `#include "ECS.h"`):
```cpp
#include "Systems.h"
```

Bump the version (line 19):
```cpp
#define GAME_API_VERSION 4u
```

In the `extern "C" { ... }` export block (alongside `GameExit`), add:
```cpp
    EXPORT_FN void GameRegisterSystems(SystemScheduler* scheduler);
```

With the other `using ...Func` typedefs, add:
```cpp
using GameRegisterSystemsFunc = void(*)(SystemScheduler*);
```

- [ ] **Step 2: Do NOT build `game` yet**

`game.cpp` won't define `GameRegisterSystems` until Task 5, so a link would fail. This is a header-only declaration change; the commit below does not rebuild the `game` target. Proceed to commit, then implement Task 5.

- [ ] **Step 3: Commit (header change)**

```bash
git add src/game/include/game.h
git commit -m "game.h: add GameRegisterSystems export, include Systems.h, bump API to 4"
```

---

## Task 5: Concrete systems + migrate logic out of GameUpdate

**Files:**
- Modify: `src/game/src/game.cpp`

- [ ] **Step 1: Add the two systems + GameRegisterSystems**

In `src/game/src/game.cpp`, add near the top (after the existing includes) the two system classes. They reproduce the exact logic currently in `GameUpdate`'s `MainMenu` case.

```cpp
#include "Systems.h"

namespace {

// Spins every text entity and cycles its color. (Was GameUpdate/MainMenu.)
class TextRotationSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        for (EntityId e : ctx.world.View<TextComponent, TransformComponent>()) {
            ctx.world.Modify<TransformComponent>(e, [&](auto& transform) {
                constexpr float TWO_PI = 6.28318530718f;
                transform.Rotation.z = fmodf(
                    transform.Rotation.z + glm::radians(180.0f) * static_cast<float>(ctx.dt),
                    TWO_PI);
            });
            ctx.world.Modify<TextComponent>(e, [&](auto& text) {
                const auto time = static_cast<float>(ctx.dt);
                const float red = (sinf(time) + 1.0f) / 2.0f;
                const float green = (cosf(ctx.gameTime) + 1.0f) / 2.0f;
                const float blue = 1.0f - red;
                text.Color = glm::vec4(red, green, blue, 1.0f);
            });
        }
    }
    const char* Name() const override { return "TextRotationSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

// Drives the SunMarker directional light over a day/night cycle.
// NOTE: cycle length is a compile-time constant in v1 (was g_GameState->DayNightCycleSeconds,
// default 10.0f). Runtime config waits for the deferred singleton-component pass.
class DayNightSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        constexpr float kDayNightCycleSeconds = 10.0f;
        for (EntityId sun : ctx.world.View<SunMarker, LightningComponent>()) {
            ctx.world.Modify<LightningComponent>(sun, [&](auto& l) {
                if (l.Type != LightningType::Directional) return;

                const float cycle = glm::max(kDayNightCycleSeconds, 0.001f);
                const double gameTime = ctx.gameTime;
                const auto phase = static_cast<float>(std::fmod(gameTime, static_cast<double>(cycle)) / static_cast<double>(cycle));
                const float theta = phase * 6.28318530718f;
                const glm::vec3 dir = glm::normalize(glm::vec3(0.0f, -cosf(theta), sinf(theta)));
                l.Direction = glm::vec4(dir, 0.0f);

                const float elevation = glm::clamp(-dir.y, 0.0f, 1.0f);
                const float horizon = 1.0f - glm::abs(dir.y);
                const float horizonSmooth = glm::smoothstep(0.0f, 0.5f, horizon);

                const glm::vec3 dayColor  = glm::vec3(1.00f, 0.98f, 0.90f);
                const glm::vec3 warmColor = glm::vec3(1.00f, 0.68f, 0.35f);
                const glm::vec3 nightColor= glm::vec3(0.15f, 0.20f, 0.40f);

                const glm::vec3 dayWarm   = glm::mix(dayColor, warmColor, horizonSmooth);
                const glm::vec3 baseColor = glm::mix(nightColor, dayWarm, elevation);
                const float brightness = 0.75f + 0.75f * elevation;
                const glm::vec3 finalColor = baseColor * brightness;
                l.Color = glm::vec4(finalColor, 1.0f);
            });
        }
    }
    const char* Name() const override { return "DayNightSystem"; }
    SystemPhase Phase() const override { return SystemPhase::Simulation; }
};

} // namespace

void GameRegisterSystems(SystemScheduler* scheduler) {
    if (!scheduler) return;
    scheduler->Register(std::make_unique<TextRotationSystem>());
    scheduler->Register(std::make_unique<DayNightSystem>());
}
```

- [ ] **Step 2: Remove the migrated blocks from `GameUpdate`'s `MainMenu` case**

In `GameUpdate`'s `case GameStateId::MainMenu:`, DELETE the text-rotation loop (`for (EntityId e : ... View<TextComponent, TransformComponent>())`) and the day/night loop (`for (EntityId sun : ... View<SunMarker, LightningComponent>())`). Leave the `case` with a `break;` (and any non-migrated state-specific code, if present). The systems now own that logic.

Read the current `MainMenu` case first to delete exactly those two loops and nothing else. The `Uninitialized` case (entity spawning, including the `SunMarker` sun) stays unchanged — systems need those entities to exist.

- [ ] **Step 3: Build game**

```
cmake --build --preset msvc-win64-vs2026-community --target game
```

Expected: clean build; `GameRegisterSystems` now defined; `Game.dll` produced.

- [ ] **Step 4: Commit**

```bash
git add src/game/src/game.cpp
git commit -m "Migrate text-rotation + day/night from GameUpdate into systems"
```

---

## Task 6: GameLibrary hot-reload wiring **(HIGH-RISK — dispatch with Opus 4.7)**

This task governs `ISystem` destruction across `game.dll` reload. Get the ordering exactly right: **`Clear()` must run after `GameExit` and before `FreeLibrary`, and only after the new module has validated.**

**Files:**
- Modify: `src/editor/src/threading/GameLibrary.h`
- Modify: `src/editor/src/threading/GameLibrary.cpp`

- [ ] **Step 1: Header — add scheduler pointer, setter, register-fn member**

In `src/editor/src/threading/GameLibrary.h`:

Add an include at top: `#include "Systems.h"` (for `SystemScheduler` / `GameRegisterSystemsFunc`). (`game.h` already includes it transitively, but be explicit.)

Add a public setter (after `LoadOrReload`):
```cpp
    /** @brief Sets the scheduler GameLibrary clears before unload + repopulates
     *         after load via the game's GameRegisterSystems export. */
    void SetScheduler(SystemScheduler* scheduler) { m_Scheduler = scheduler; }
```

Add private members (alongside the other `m_pGame*` pointers):
```cpp
    GameRegisterSystemsFunc m_pGameRegisterSystems = nullptr;
    SystemScheduler*        m_Scheduler            = nullptr;  // not owned
```

- [ ] **Step 2: `.cpp` — resolve the optional symbol**

In `GameLibrary::LoadOrReload`, where the other optional symbols are resolved (next to `pResize`/`pExit`, ~line 79-80), add:
```cpp
    auto pRegisterSystems = reinterpret_cast<GameRegisterSystemsFunc>(
        GetProcAddress(newModule, "GameRegisterSystems"));
```

- [ ] **Step 3: `.cpp` — Clear before FreeLibrary in the teardown block**

In the teardown block that currently reads:
```cpp
    if (m_Module) {
        if (m_pGameExit) m_pGameExit(state);
        FreeLibrary(m_Module);
        fs::remove(m_LoadedDllPath, ec);  // best-effort
        SM_TRACE("GameLibrary: unloaded previous module '%s'", m_LoadedDllPath.c_str());
    }
```
change it to:
```cpp
    if (m_Module) {
        if (m_pGameExit) m_pGameExit(state);
        // Destroy game.dll-owned ISystem instances while their vtables are still
        // mapped (this module is about to be FreeLibrary'd). See spec hot-reload section.
        if (m_Scheduler) m_Scheduler->Clear();
        FreeLibrary(m_Module);
        fs::remove(m_LoadedDllPath, ec);  // best-effort
        SM_TRACE("GameLibrary: unloaded previous module '%s'", m_LoadedDllPath.c_str());
    }
```
This is correct relative to the validation gate: validation (`GameGetVersion` mismatch / missing required exports) already returned early *before* this block, so a failed reload never reaches `Clear()` — the old module and its systems stay intact.

- [ ] **Step 4: `.cpp` — install the new register-fn pointer + register after swap**

In the "install new function pointers" block (after `m_pGameGetVersion = pVersion;`), add:
```cpp
    m_pGameRegisterSystems = pRegisterSystems;
```

Then immediately after the install block (after the `SM_TRACE("GameLibrary: loaded ...")` line, before `return true;`), add:
```cpp
    if (m_Scheduler && m_pGameRegisterSystems) {
        m_pGameRegisterSystems(m_Scheduler);
        SM_TRACE("GameLibrary: registered %zu system(s)", m_Scheduler->Count());
    }
```

- [ ] **Step 5: `.cpp` — Clear before FreeLibrary in `Unload`**

In `GameLibrary::Unload`, change:
```cpp
void GameLibrary::Unload(GameState* state) {
    if (!m_Module) return;
    if (m_pGameExit) m_pGameExit(state);
    FreeLibrary(m_Module);
    ...
```
to insert the Clear between `GameExit` and `FreeLibrary`:
```cpp
void GameLibrary::Unload(GameState* state) {
    if (!m_Module) return;
    if (m_pGameExit) m_pGameExit(state);
    if (m_Scheduler) m_Scheduler->Clear();   // before FreeLibrary — vtables still mapped
    FreeLibrary(m_Module);
    ...
```
Also null the new pointer in the same function alongside the others:
```cpp
    m_pGameRegisterSystems = nullptr;
```

- [ ] **Step 6: Build editor**

```
cmake --build --preset msvc-win64-vs2026-community --target editor
```

Expected: clean build (also builds `ecs` + `game` deps).

- [ ] **Step 7: Self-review the ordering**

Confirm by reading the final `GameLibrary.cpp`:
- `Clear()` appears only AFTER the version/required-export validation early-returns.
- In `LoadOrReload`: order is `GameExit(old) → Clear → FreeLibrary(old) → install → Register(new)`.
- In `Unload`: order is `GameExit → Clear → FreeLibrary`.
- A reload whose new module fails validation never calls `Clear()` (old systems survive).

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/threading/GameLibrary.h src/editor/src/threading/GameLibrary.cpp
git commit -m "GameLibrary: clear+re-register systems across game.dll reload"
```

---

## Task 7: GameThread — own the scheduler + run it

**Files:**
- Modify: `src/editor/src/threading/GameThread.h`
- Modify: `src/editor/src/threading/GameThread.cpp`

- [ ] **Step 1: Header — own the scheduler**

In `src/editor/src/threading/GameThread.h`, add an include `#include "Systems.h"` and add a member next to `GameLibrary m_GameLib;`:
```cpp
    SystemScheduler m_Scheduler;
```

- [ ] **Step 2: `.cpp` — wire the scheduler to GameLibrary before the initial load**

In `GameThread::RunLoop`, BEFORE the initial `m_GameLib.LoadOrReload("Game.dll", &gameState)` call (~line 71), add:
```cpp
    m_GameLib.SetScheduler(&m_Scheduler);
```
This ensures the initial load already registers systems, and every reload thereafter clears/re-registers.

- [ ] **Step 3: `.cpp` — run the scheduler after GameUpdate**

In the tick body, immediately after:
```cpp
            if (m_GameLib.IsValid()) {
                m_GameLib.Update(&gameState);
            }
```
add:
```cpp
            {
                SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime };
                m_Scheduler.Run(sysCtx);
            }
```

- [ ] **Step 4: Verify shutdown ordering**

`GameThread::RunLoop` already calls `m_GameLib.Unload(&gameState)` at shutdown (~line 393). With Task 6, `Unload` now clears the scheduler before `FreeLibrary`. Confirm `m_Scheduler` (a `GameThread` member) outlives that call — it does, since `Unload` runs inside `RunLoop` before `GameThread` is destroyed. No code change; just confirm by reading.

- [ ] **Step 5: Build all in-scope targets**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
```

Expected: clean build of all four.

- [ ] **Step 6: Commit**

```bash
git add src/editor/src/threading/GameThread.h src/editor/src/threading/GameThread.cpp
git commit -m "GameThread: own SystemScheduler and run it after GameUpdate"
```

---

## Task 8: Verify — tests + manual smoke

**Files:** none (verification).

- [ ] **Step 1: Build everything + run ECS tests**

```
cmake --build --preset msvc-win64-vs2026-community --target ecs editor game test_ecs
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
```

Expected: clean build; `All ECS tests passed.` (the 5 new `TS0x` tests included).

- [ ] **Step 2: Manual smoke (user-driven; the editor is GUI)**

Launch `editor.exe`. Confirm:
1. The "Hello, Game!" text still spins + cycles color (now via `TextRotationSystem`).
2. The directional light still cycles day/night (now via `DayNightSystem`).
3. No duplicate behavior / no crash at startup.

- [ ] **Step 3: Manual smoke — hot-reload**

With the editor running, edit a system constant in `game.cpp` (e.g. change `glm::radians(180.0f)` to `glm::radians(90.0f)` in `TextRotationSystem`), then:
```
cmake --build --preset msvc-win64-vs2026-community --target game
```
Confirm: the editor hot-reloads (`GameLibrary: registered 2 system(s)` in the log), the text now spins slower, **no crash**, and the scheduler count stays 2 (no duplicate registration). Revert the constant + rebuild if desired.

- [ ] **Step 4: Manual smoke — clean exit**

Close the editor normally. Confirm no crash on exit (validates `Clear()` before the final `FreeLibrary` in `Unload`).

- [ ] **Step 5: Commit any fixes**

If smoke surfaces fixes, commit them. If all green, no commit.

---

## Task 9: Wrap up

- [ ] **Step 1: Confirm tree + branch state**

```
git status
git log --oneline origin/main..HEAD
```

- [ ] **Step 2: Decide integration**

If on a feature branch: push + open a PR summarizing the Systems layer (interface in ecs.dll, game-side systems + GameRegisterSystems, GameThread wiring, the migrated logic, and the hot-reload Clear/Register discipline; note the unit tests). If committing directly to `main` per the project's recent flow, push `main`. Hand the result (PR URL or pushed SHA) back to the user.

---

## Out-of-scope reminders (do not implement here)

- Parallel/multithreaded systems; system dependency/topo ordering.
- Singleton components (input/cameras/settings into the ECS) — the follow-up that lets input/camera become Input-phase systems.
- Editor UI to enable/disable/reorder systems; system state-gating by `GameStateId`.
- `View<>()` allocation optimization.
