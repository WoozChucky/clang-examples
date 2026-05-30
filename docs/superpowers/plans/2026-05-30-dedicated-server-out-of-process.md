# Phase 3 — Dedicated Server (Out-of-Process) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the editor spawn a real, separate `server.exe` process that boots headless (no window, no renderer), loads `Game.dll`, and runs the gameplay simulation + networking; the in-editor client connects to it over loopback TCP and round-trips a message; "Stop" shuts it down cleanly and an editor crash never orphans the child.

**Architecture:** A slim `ServerApplication` (engine) owns its own fixed-step loop running only `GameThread`-equivalent gameplay (world load, `Game.dll` + `SystemScheduler`, `NavMeshSystem::Tick`, `NetSubsystem`) — no `PlatformThread`/`RenderThread`/NVRHI/GLFW. A `server.exe` target (a trimmed `runtime.exe` sibling) boots it. The single `Game.dll` learns its role (`Client` vs `Server`) through a new `SystemContext::role` field set by each bootstrap; `NetDemoSystem` branches: server binds + echoes, client connects + pings. The editor owns a `ServerSupervisor` (Win32 `CreateProcess` + Job Object with `KILL_ON_JOB_CLOSE` + a named-event clean-shutdown signal) driven from a new **Dedicated Server** panel.

**Tech Stack:** C++23, Win32 (`CreateProcess`, Job Objects, named events, `SetConsoleCtrlHandler`), the existing `Engine`/`ecs`/`netlib`/`Game.dll` machinery, CMake (`msvc-win64-vs2026-community` preset), ImGui (editor panel).

**Decisions locked (from brainstorming):**
- Boot path = **slim `ServerApplication`** (not an `Application` headless mode).
- Supervision = **Job Object (`KILL_ON_JOB_CLOSE`) + named event** for clean stop.
- Server navmesh = **pre-baked disk** (`NavMeshSystem::TryLoadFromDisk`); mesh-source rebake unavailable headless (documented limitation).
- Demo uses a **fixed loopback port constant** (`kDedicatedServerDefaultPort`); variable-port editor→client wiring is a documented follow-up.

**Build/test note (project memory `project_build_preset`):** build and test ONLY with `msvc-win64-vs2026-community`. The configure command is `cmake --preset msvc-win64-vs2026-community`; build a target with `cmake --build --preset msvc-win64-vs2026-community --target <t>`. Binaries land in `out/build/msvc-win64-vs2026-community/bin/<Config>/` (this is `RUNTIME_DIR`, the CWD for all exes — assets are copied there post-build).

**Commit identity (project memory `feedback_git_identity`):** commit as `Nuno Silva <nuno.levezinho@live.com.pt>`. Never `--no-verify`. Work happens on the existing branch `feat/networking-design` (do NOT branch or merge).

---

## File Structure

**New files:**
- `src/common/include/AppRole.h` — `enum class AppRole { Client, Server }`. Included by both `Game.h` and `Systems.h` (so the value can live on `GameState` and flow through `SystemContext`). Header-only, no deps.
- `src/common/include/ServerControl.h` — shared client/server/editor constants + pure helpers: `kDedicatedServerDefaultPort`, `ServerShutdownEventName(uint16_t port)` (builds the Win32 named-event string). Pure/testable; no Win32 includes.
- `src/engine/src/core/ServerApplication.h` / `.cpp` — the headless bootstrap + fixed-step loop. `ENGINE_API`.
- `src/server/src/main.cpp` — `server.exe` entry: parse `--port`/`--world`, install console-ctrl + named-event shutdown, run `ServerApplication`.
- `src/server/CMakeLists.txt` — the trimmed target.
- `src/editor/src/app/ServerSupervisor.h` / `.cpp` — editor-side process spawn/supervise (Job Object + named-event stop + status). Pure arg-builder + status mapping are unit-tested.
- `src/editor/src/panels/DedicatedServerPanel.h` / `.cpp` — Start/Stop/status UI.
- `tests/test_servercontrol.cpp` — unit tests for the pure helpers (`ServerShutdownEventName`, supervisor arg-builder, status mapping).
- `tests/test_dedicated_server.cpp` — one focused end-to-end test: spawn `server.exe`, connect a `netlib` TCP client, assert connect succeeds (proves headless boot + listen), signal shutdown, assert clean exit.

**Modified files:**
- `src/game/include/Game.h` — add `AppRole Role` to `GameState`; bump `GAME_API_VERSION` 18→19.
- `src/common/include/Systems.h` — add `AppRole role` to `SystemContext` + `#include "AppRole.h"`.
- `src/engine/src/threading/GameThread.cpp` — set `gameState.Role = AppRole::Client` and pass `role` into the per-tick `SystemContext`.
- `src/game/src/game.cpp` — rework `NetDemoSystem` to branch on `ctx.role` (server binds+echoes / client connects+pings with bounded retry).
- `src/engine/CMakeLists.txt` — add `src/core/ServerApplication.cpp`.
- `src/editor/CMakeLists.txt` — add `ServerSupervisor.cpp` + `DedicatedServerPanel.cpp`.
- `src/editor/src/app/ImGuiRenderer.cpp` (+ `.h`) — own a `ServerSupervisor`, draw the panel.
- `src/editor/src/panels/MainMenuBar.cpp` — add a "Dedicated Server" window toggle (optional but include it).
- `CMakeLists.txt` (root) — `add_subdirectory(src/server)`.
- `tests/CMakeLists.txt` — add `test_servercontrol` + `test_dedicated_server`.
- `docs/superpowers/specs/2026-05-30-dedicated-server-out-of-process-design.md` — tick the deliverables checklist + record the manual-smoke result.

---

## Task 1: Role + shared control constants (`AppRole.h`, `ServerControl.h`, GameState/SystemContext wiring)

**Files:**
- Create: `src/common/include/AppRole.h`
- Create: `src/common/include/ServerControl.h`
- Create: `tests/test_servercontrol.cpp`
- Modify: `src/game/include/Game.h` (add field, bump version)
- Modify: `src/common/include/Systems.h` (add field + include)
- Modify: `src/engine/src/threading/GameThread.cpp` (set Client role, pass into SystemContext)
- Modify: `tests/CMakeLists.txt`

This is the cross-cutting data plumbing every later task builds on. It touches `Game.h` (struct layout → `GAME_API_VERSION` bump) AND `Systems.h` (compiled into both `ecs.dll` and `Game.dll`), so after this task you must rebuild `ecs`, `Engine`, `game`, `editor` and restart the editor (per CLAUDE.md hot-reload rules). That restart is expected, not a regression.

- [ ] **Step 1: Write the failing test**

Create `tests/test_servercontrol.cpp`:

```cpp
// Unit tests for the pure Phase-3 control helpers (no Win32, no sockets).
#include <cassert>
#include <cstdio>
#include <string>

#include "AppRole.h"
#include "ServerControl.h"

int main() {
    // Default port is the documented demo constant.
    assert(kDedicatedServerDefaultPort == 27015);

    // Event name is deterministic, non-empty, and port-specific.
    const std::string a = ServerShutdownEventName(27015);
    const std::string b = ServerShutdownEventName(27016);
    assert(!a.empty());
    assert(a != b);
    // Must be a usable Win32 object name (no backslashes beyond an optional
    // "Local\\" prefix — we use a flat name) and contain the port.
    assert(a.find("27015") != std::string::npos);

    // AppRole default is Client (editor/runtime are clients).
    AppRole r = AppRole::Client;
    assert(r == AppRole::Client);
    r = AppRole::Server;
    assert(r == AppRole::Server);

    std::printf("All server-control tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Create `AppRole.h`**

Create `src/common/include/AppRole.h`:

```cpp
#pragma once
#include <cstdint>

// Which role this process plays in the networking topology. Set by the bootstrap
// (GameThread => Client, ServerApplication => Server), carried on GameState and
// threaded into SystemContext so game systems can branch without linking Engine.
enum class AppRole : uint8_t {
    Client = 0,   // editor.exe / runtime.exe — connects out
    Server = 1,   // server.exe — listens
};
```

- [ ] **Step 3: Create `ServerControl.h`**

Create `src/common/include/ServerControl.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>

// Shared client/server/editor networking-control constants + pure helpers.
// Header-only and Win32-free so tests and all three exes can include it.

// Fixed loopback port for the Phase-3 dedicated-server demo. The editor spawns
// server.exe bound here and the in-editor client connects here. (Variable-port
// editor->client wiring is a documented follow-up; for the demo both sides
// compile in this constant.)
inline constexpr uint16_t kDedicatedServerDefaultPort = 27015;

// Name of the Win32 manual-reset event the editor sets to ask the server to
// shut down cleanly, and that the server waits on. Flat (session-local) name;
// port-qualified so multiple servers don't collide. Keep ASCII, no backslashes.
inline std::string ServerShutdownEventName(uint16_t port) {
    return "smengine_server_shutdown_" + std::to_string(port);
}
```

- [ ] **Step 4: Add the test target**

In `tests/CMakeLists.txt`, append:

```cmake
add_executable(test_servercontrol
    test_servercontrol.cpp
)

target_include_directories(test_servercontrol PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

set_target_properties(test_servercontrol PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 5: Configure + build the test, verify it passes**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_servercontrol
./out/build/msvc-win64-vs2026-community/bin/Debug/test_servercontrol.exe
```
Expected: `All server-control tests passed.`

- [ ] **Step 6: Add the role field to `GameState` and bump the API version**

In `src/game/include/Game.h`:
- Add `#include "AppRole.h"` near the other includes (after `#include "Systems.h"`).
- Change `#define GAME_API_VERSION 18u` to `#define GAME_API_VERSION 19u`.
- Add a trailing field to `struct GameState` (after `bool WorldLoaded = false;`):

```cpp
    AppRole  Role                   = AppRole::Client;  // set by bootstrap; Server only in server.exe
```

- [ ] **Step 7: Add the role to `SystemContext`**

In `src/common/include/Systems.h`:
- Add `#include "AppRole.h"` after the existing includes.
- Add a trailing field to `struct SystemContext` (after the `Net` pointer):

```cpp
    AppRole role = AppRole::Client;    // process role; Server only inside server.exe
```

- [ ] **Step 8: Set Client role + pass it through in GameThread**

In `src/engine/src/threading/GameThread.cpp`:
- After `gameState.Settings = &m_AppContext->Settings;` (line ~63), add:

```cpp
    gameState.Role = AppRole::Client;   // editor.exe / runtime.exe boot through GameThread
```

- Update the per-tick `SystemContext` construction (line ~457) from:

```cpp
                SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices, &netServices };
```
to:
```cpp
                SystemContext sysCtx{ gameState.World, gameState.DeltaTime, gameState.GameTime, &navServices, &netServices, gameState.Role };
```

- [ ] **Step 9: Rebuild everything affected + verify it still builds**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target ecs
cmake --build --preset msvc-win64-vs2026-community --target Engine
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: all four build clean. (No behavior change yet — `NetDemoSystem` still does the old loopback; role branch lands in Task 7.)

- [ ] **Step 10: Commit**

```bash
git add src/common/include/AppRole.h src/common/include/ServerControl.h \
        tests/test_servercontrol.cpp tests/CMakeLists.txt \
        src/game/include/Game.h src/common/include/Systems.h \
        src/engine/src/threading/GameThread.cpp
git commit -m "feat(net): AppRole + server-control constants; thread role through SystemContext"
```

---

## Task 2: `ServerApplication` — headless boot + fixed-step loop (engine)

**Files:**
- Create: `src/engine/src/core/ServerApplication.h`
- Create: `src/engine/src/core/ServerApplication.cpp`
- Modify: `src/engine/CMakeLists.txt` (add the `.cpp`)

`ServerApplication` is the headless analog of `Application` + the gameplay core of `GameThread::RunLoop`, with every renderer/window coupling removed: NO `PlatformThread`, NO `RenderThread`, NO model-load worker, NO `GRCommandRing`/`RGCommandRing` mesh round-trip, NO viewport sync, NO plugins. It DOES: load settings, load the world, load `Game.dll` (with filewatch hot-reload), run the `SystemScheduler` + `GameUpdate`, tick `NavMeshSystem`, own `NetSubsystem`, and pace a fixed 60 Hz loop until shutdown is requested.

To stay unit-testable without threads or timing, the loop is split into `Init()` / `Tick()` / `Shutdown()`, with `Run()` = pace-and-loop calling `Tick()` until `RequestShutdown()` is observed.

Critical ordering (the close-crash gotcha from Phase 2, project memory `project_networking_roadmap`): `Shutdown()` MUST call `NetSubsystem::Instance().Shutdown()` BEFORE `m_GameLib.Unload(...)`. The net adapters' IOCP worker threads are spun up by `Game.dll` (via `NetServices`); joining them after `FreeLibrary(Game.dll)` runs per-thread teardown into unmapped code → DEP/execute access violation.

- [ ] **Step 1: Write `ServerApplication.h`**

Create `src/engine/src/core/ServerApplication.h`:

```cpp
#pragma once
#include <atomic>
#include <memory>
#include <string>

#include "Engine.h"
#include "ApplicationContext.h"
#include "GameLibrary.h"
#include "Systems.h"        // SystemScheduler
#include "Game.h"           // GameState

// Headless server bootstrap. Runs the gameplay simulation + networking with NO
// renderer, window, or GPU device. Owns its own fixed-step loop. Lives in Engine
// so it can reach NetSubsystem / NavMeshSystem / WorldManager directly.
//
// Lifecycle: Init() -> (Run() | repeated Tick()) -> Shutdown().
// Run() blocks on a 60 Hz pace loop until RequestShutdown() is called (from a
// signal handler, a named-event waiter, or another thread). Tick() runs exactly
// one simulation step (used by tests to drive the loop deterministically).
class ENGINE_API ServerApplication {
public:
    struct Config {
        uint16_t    port = 0;       // 0 => use the world/default; the demo passes kDedicatedServerDefaultPort
        std::string worldPath;      // empty => WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH
        double      targetTps = 60.0;
    };

    ServerApplication() = default;
    ~ServerApplication();

    ServerApplication(const ServerApplication&) = delete;
    ServerApplication& operator=(const ServerApplication&) = delete;

    // Load settings/world/Game.dll, init NetSubsystem + service tables, seed
    // singletons, try the disk navmesh bake. Returns false on fatal failure.
    bool Init(const Config& cfg);

    // One fixed simulation step: drain reload flag, GameUpdate, run scheduler,
    // NavMeshSystem::Tick. Safe to call only between Init() and Shutdown().
    void Tick();

    // Pace a fixed-step loop calling Tick() until RequestShutdown() observed.
    void Run();

    // Ask Run() to exit at the next loop boundary. Thread-safe / signal-safe.
    void RequestShutdown() { m_StopRequested.store(true, std::memory_order_relaxed); }
    bool StopRequested() const { return m_StopRequested.load(std::memory_order_relaxed); }

    // Tear down: NetSubsystem::Shutdown() BEFORE Game.dll unload (ordering gotcha),
    // then clear the world. Idempotent.
    void Shutdown();

private:
    bool InstallFilewatch();

    std::shared_ptr<ApplicationContext> m_AppContext;
    GameLibrary       m_GameLib;
    SystemScheduler   m_Scheduler;
    GameState         m_GameState{};
    Config            m_Config{};

    NavServices       m_NavServices{};
    NetServices       m_NetServices{};

    std::atomic<bool> m_StopRequested{false};
    std::atomic<bool> m_ReloadPending{false};
    bool              m_Initialized = false;

    // filewatch handle (opaque to header to avoid pulling the dep in)
    void*             m_Watcher = nullptr;
};
```

- [ ] **Step 2: Write `ServerApplication.cpp`**

Create `src/engine/src/core/ServerApplication.cpp`. Mirror the gameplay-relevant parts of `GameThread::RunLoop` (lines 47-569) but strip all renderer coupling. Use `WorldManager`, `NavMeshSystem`, `NetSubsystem`, `NavServicesImpl`, `NetServicesImpl` (same includes GameThread uses).

```cpp
#include "ServerApplication.h"

#include <chrono>
#include <filesystem>
#include <regex>
#include <thread>

#include <FileWatch.hpp>   // same dep GameThread uses for hot-reload

#include "lib.h"
#include "Timing.h"
#include "WorldManager.h"
#include "ServerControl.h"
#include "navigation/NavMeshSystem.h"
#include "navigation/NavServicesImpl.h"
#include "network/NetSubsystem.h"
#include "network/NetServicesImpl.h"
#include "ECSCommands.h"

using Clock = std::chrono::steady_clock;

ServerApplication::~ServerApplication() { Shutdown(); }

bool ServerApplication::Init(const Config& cfg) {
    if (m_Initialized) return true;
    m_Config = cfg;
    if (m_Config.worldPath.empty())
        m_Config.worldPath = WorldManager::DEFAULT_WORLD_SNAPSHOT_PATH;

    m_AppContext = std::make_shared<ApplicationContext>();
    SettingsManager::Load(SettingsManager::DEFAULT_SETTINGS_PATH, &m_AppContext->Settings);

    m_GameState.Settings = &m_AppContext->Settings;
    m_GameState.Role     = AppRole::Server;   // <-- the whole point
    m_GameState.TargetTPS = m_Config.targetTps;

    // World load (mirrors GameThread). On success, try the pre-baked navmesh from
    // disk. A headless server CANNOT rebuild mesh-source navmesh (no GPU round-trip
    // for CPU vertex data), so on a disk-bake miss we WARN and continue without nav
    // rather than posting a RebuildNavMesh that would silently no-op for mesh sources.
    if (WorldManager::LoadWorldSnapshot(m_Config.worldPath.c_str(), &m_GameState.World)) {
        m_GameState.WorldLoaded = true;
        SM_TRACE("ServerApplication: world loaded from '%s'", m_Config.worldPath.c_str());
        if (!NavMeshSystem::Instance().TryLoadFromDisk(m_Config.worldPath.c_str())) {
            SM_WARN("ServerApplication: no fresh disk navmesh for '%s'; running without nav "
                    "(headless server cannot rebake mesh-source geometry)", m_Config.worldPath.c_str());
        }
    } else {
        SM_WARN("ServerApplication: world '%s' not loaded (missing/invalid)", m_Config.worldPath.c_str());
    }

    // Seed singletons game code expects (no viewport/UI camera needed headless, but
    // InputStateComponent is read by some systems; seed an empty one).
    m_GameState.World.SetSingleton(InputStateComponent{});

    // Service tables + networking subsystem (same as GameThread).
    NavServicesImpl::Init(m_NavServices);
    NetServicesImpl::Init(m_NetServices);
    NetSubsystem::Instance().Init();

    // Game.dll load + filewatch hot-reload.
    m_GameLib.SetScheduler(&m_Scheduler);
    if (!m_GameLib.LoadOrReload("Game.dll", &m_GameState)) {
        SM_ERROR("ServerApplication: initial Game.dll load failed; server will idle without game logic");
    }
    InstallFilewatch();

    m_Initialized = true;
    SM_TRACE("ServerApplication: initialized (role=Server, port=%u)", (unsigned)m_Config.port);
    return true;
}

bool ServerApplication::InstallFilewatch() {
    try {
        m_Watcher = new filewatch::FileWatch<std::string>(
            std::string("."),
            std::regex(R"(^Game\.dll$)"),
            [this](const std::string&, const filewatch::Event evt) {
                if (evt == filewatch::Event::modified || evt == filewatch::Event::added)
                    m_ReloadPending.store(true, std::memory_order_release);
            });
        SM_TRACE("ServerApplication: filewatch installed on './Game.dll'");
        return true;
    } catch (const std::exception& e) {
        SM_ERROR("ServerApplication: filewatch setup failed: %s. Hot-reload disabled.", e.what());
        return false;
    }
}

void ServerApplication::Tick() {
    if (!m_Initialized) return;

    // Game.dll hot-reload (Model-B: release game-resident net adapters first).
    if (m_ReloadPending.exchange(false, std::memory_order_acquire)) {
        NetSubsystem::Instance().ReleaseGameResidentConnections();
        if (m_GameLib.LoadOrReload("Game.dll", &m_GameState))
            SM_TRACE("ServerApplication: Game.dll reloaded");
    }

    const double dt = 1.0 / m_Config.targetTps;
    m_GameState.DeltaTime = dt;
    m_GameState.GameTime  = TimeNowSec();

    if (m_GameLib.IsValid())
        m_GameLib.Update(&m_GameState);

    SystemContext ctx{ m_GameState.World, m_GameState.DeltaTime, m_GameState.GameTime,
                       &m_NavServices, &m_NetServices, m_GameState.Role };
    m_Scheduler.Run(ctx);

    NavMeshSystem::Instance().Tick(static_cast<float>(dt));

    // In-world quit (e.g. AppControlComponent) also stops the server.
    if (const auto* app = m_GameState.World.GetSingleton<AppControlComponent>(); app && app->QuitRequested)
        RequestShutdown();
}

void ServerApplication::Run() {
    if (!m_Initialized) { SM_ERROR("ServerApplication::Run before Init"); return; }
    SM_TRACE("ServerApplication: entering run loop (%.0f TPS)", m_Config.targetTps);

    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / m_Config.targetTps));
    auto next = Clock::now();

    while (!m_StopRequested.load(std::memory_order_relaxed)) {
        Tick();
        next += period;
        const auto now = Clock::now();
        if (now < next) std::this_thread::sleep_for(next - now);
        else            next = now;   // fell behind; don't spiral
    }
    SM_TRACE("ServerApplication: run loop exited");
}

void ServerApplication::Shutdown() {
    if (!m_Initialized) return;
    m_Initialized = false;

    if (m_Watcher) {
        delete static_cast<filewatch::FileWatch<std::string>*>(m_Watcher);
        m_Watcher = nullptr;
    }

    // ORDERING GOTCHA (Phase 2): tear down networking (joins IOCP worker threads
    // created while Game.dll was loaded) BEFORE unloading Game.dll, or thread
    // teardown dispatches into the unmapped module -> DEP/execute crash.
    NetSubsystem::Instance().Shutdown();

    m_GameLib.Unload(&m_GameState);
    m_GameState.World.Clear();
    SM_TRACE("ServerApplication: shutdown complete");
}
```

> Implementation note for the engineer: confirm the exact filewatch include path GameThread uses (`#include <FileWatch.hpp>` or similar) and match it — copy the include line verbatim from `GameThread.cpp`. Confirm `SettingsManager`, `TimeNowSec`, `WorldManager`, `AppControlComponent`, and `InputStateComponent` are all reachable with the includes shown (they are all used by `GameThread.cpp` / `Application.cpp`; add the matching include if the compiler disagrees). Do NOT add a `RebuildNavMesh` path — the headless server has no GPU round-trip for mesh CPU data, which is the documented limitation.

- [ ] **Step 3: Add the source to the engine build**

In `src/engine/CMakeLists.txt`, add to the engine source list (next to `src/core/Application.cpp`):

```cmake
    src/core/ServerApplication.cpp
```

- [ ] **Step 4: Build the engine, verify it compiles**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target Engine
```
Expected: `Engine.dll` builds clean. (Driven by a real process in Task 3; a direct unit test for the loop lands in Task 8's end-to-end test.)

- [ ] **Step 5: Commit**

```bash
git add src/engine/src/core/ServerApplication.h src/engine/src/core/ServerApplication.cpp \
        src/engine/CMakeLists.txt
git commit -m "feat(net): headless ServerApplication (Init/Tick/Run/Shutdown, no renderer)"
```

---

## Task 3: `server.exe` target + entry point

**Files:**
- Create: `src/server/src/main.cpp`
- Create: `src/server/CMakeLists.txt`
- Modify: `CMakeLists.txt` (root — `add_subdirectory(src/server)`)

`server.exe` mirrors `runtime.exe` but boots `ServerApplication` instead of `Application`. It parses `--port` / `--world`, installs a clean-shutdown path (Win32 named event + `SetConsoleCtrlHandler` for Ctrl-C), and runs. It provides its own `platform_debug_break` like the other exes.

Engine PUBLIC-links nvrhi/glfw/etc., so `server.exe` will transitively pull those import libs — that is acceptable for a first cut (the headless win is that no window/device is ever *created*, not fewer link deps). Keep the explicit link list minimal; do not add nvrhi/glfw directly.

- [ ] **Step 1: Write `src/server/src/main.cpp`**

```cpp
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "lib.h"
#include "core/ServerApplication.h"
#include "ServerControl.h"

// Per-module SM_ASSERT backend (mirrors runtime.exe / editor.exe).
void platform_debug_break(const char* expr, const char* file, int line, const char* message) {
    char buffer[2048] = {};
    sprintf_s(buffer, "Assertion failed!\n\nExpression: %s\nFile: %s\nLine: %d\n\n%s",
              (expr ? expr : "<none>"), (file ? file : "<unknown>"), line,
              (message ? message : "<no message>"));
    // Headless: log instead of MessageBox (no user at the console necessarily).
    SM_ERROR("%s", buffer);
    DEBUG_BREAK();
}

namespace {
    ServerApplication* g_App = nullptr;   // for the console-ctrl handler

    BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
            type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT) {
            if (g_App) g_App->RequestShutdown();
            return TRUE;
        }
        return FALSE;
    }

    struct Cli { uint16_t port = kDedicatedServerDefaultPort; std::string world; bool ok = true; };

    Cli ParseCli(int argc, char** argv) {
        Cli c;
        for (int i = 1; i < argc; ++i) {
            std::string_view a = argv[i];
            constexpr std::string_view kPort = "--port=";
            constexpr std::string_view kWorld = "--world=";
            if (a.substr(0, kPort.size()) == kPort) {
                c.port = static_cast<uint16_t>(std::stoi(std::string(a.substr(kPort.size()))));
            } else if (a.substr(0, kWorld.size()) == kWorld) {
                c.world = std::string(a.substr(kWorld.size()));
            } else {
                std::printf("server.exe: unknown arg '%.*s'\n", (int)a.size(), a.data());
                c.ok = false; return c;
            }
        }
        return c;
    }
}

int main(int argc, char** argv) {
    SM_TRACE("server.exe working dir: %s", std::filesystem::current_path().string().c_str());
    const Cli cli = ParseCli(argc, argv);
    if (!cli.ok) {
        std::printf("Usage: server.exe [--port=<n>] [--world=<path>]\n");
        return 1;
    }

    ServerApplication app;
    g_App = &app;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    ServerApplication::Config cfg;
    cfg.port = cli.port;
    cfg.worldPath = cli.world;
    if (!app.Init(cfg)) {
        SM_ERROR("server.exe: ServerApplication init failed");
        return -1;
    }

    // Background waiter: the editor sets this named event to ask for a clean stop.
    const std::string evName = ServerShutdownEventName(cli.port);
    HANDLE shutdownEvent = CreateEventA(nullptr, /*manualReset*/ TRUE, /*initial*/ FALSE, evName.c_str());
    HANDLE waiter = nullptr;
    if (shutdownEvent) {
        waiter = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            HANDLE ev = static_cast<HANDLE>(p);
            WaitForSingleObject(ev, INFINITE);
            if (g_App) g_App->RequestShutdown();
            return 0;
        }, shutdownEvent, 0, nullptr);
    } else {
        SM_WARN("server.exe: could not create shutdown event '%s' (err %lu); Ctrl-C only",
                evName.c_str(), GetLastError());
    }

    app.Run();          // blocks until RequestShutdown (event, Ctrl-C, or in-world quit)
    app.Shutdown();
    g_App = nullptr;

    if (shutdownEvent) {
        SetEvent(shutdownEvent);   // unblock the waiter so it can exit
        if (waiter) { WaitForSingleObject(waiter, 2000); CloseHandle(waiter); }
        CloseHandle(shutdownEvent);
    }
    SM_TRACE("server.exe: clean exit");
    return 0;
}
```

- [ ] **Step 2: Write `src/server/CMakeLists.txt`** (mirror `src/runtime/CMakeLists.txt`, trimmed)

```cmake
add_executable(server
    src/main.cpp
)

target_include_directories(server PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/src/engine/src        # core/ServerApplication.h
)

target_compile_definitions(server PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RIGHT_HANDED
    GLM_ENABLE_EXPERIMENTAL
)

target_link_libraries(server PRIVATE
    Engine
    ecs
    netlib
    CommonHeaders
    GameHeaders
    glm::glm
)

set_target_properties(server PROPERTIES
    OUTPUT_NAME server
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Applications
    VS_DEBUGGER_WORKING_DIRECTORY "${RUNTIME_DIR}"
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(server PRIVATE -Wno-switch -Wno-writable-strings -Wno-sign-compare -Wno-deprecated-declarations -Wno-format-security -Wmissing-braces)
endif()

# Build the hot-reloadable Game.dll before the server runs.
add_dependencies(server game)

# Ship assets next to server.exe (same RUNTIME_DIR as the other exes; harmless if already copied).
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets ${RUNTIME_DIR}/assets
)
```

> Note: `core/ServerApplication.h` is included via `src/engine/src` on the include path. If Engine already exposes `src/engine/src` as a PUBLIC include dir (check `src/engine/CMakeLists.txt`), the extra `target_include_directories` line is redundant but harmless. If the header isn't found, that explicit include dir resolves it.

- [ ] **Step 3: Register the subdirectory in the root CMake**

In `CMakeLists.txt` (root), after `add_subdirectory(src/runtime)` (line ~38), add:

```cmake
# headless dedicated-server application (loads Game.dll; no renderer/window)
add_subdirectory(src/server)
```

- [ ] **Step 4: Configure + build `server`, verify it links**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target server
```
Expected: `server.exe` links and lands in `out/build/msvc-win64-vs2026-community/bin/Debug/`.

- [ ] **Step 5: Manual smoke — run the server standalone**

Run (from the session, so output is captured):
```
./out/build/msvc-win64-vs2026-community/bin/Debug/server.exe --port=27015
```
Expected log lines: working dir, "world loaded", navmesh disk-bake result, "Game.dll" load, "initialized (role=Server, port=27015)", "entering run loop". **USER ACTION:** a Windows Firewall prompt may fire when the game's server adapter calls `listen()` — accept it once for `server.exe`. Then press Ctrl-C; expect "run loop exited" → "shutdown complete" → "clean exit" with no crash. (The listen socket itself comes online in Task 7 once `NetDemoSystem` learns the Server role; for now this proves the headless boot + clean shutdown.)

- [ ] **Step 6: Commit**

```bash
git add src/server/src/main.cpp src/server/CMakeLists.txt CMakeLists.txt
git commit -m "feat(net): server.exe target — headless boot of ServerApplication"
```

---

## Task 4: Game-side role branch — server listens, client connects (with retry)

**Files:**
- Modify: `src/game/src/game.cpp` (rework `NetDemoSystem`)

Currently `NetDemoSystem` stands up BOTH a loopback server and client in every process. Split the behavior by `ctx.role`:
- **Server role:** create a server bound to `127.0.0.1:kDedicatedServerDefaultPort`; on each `Message` (opcode 1 ping) reply with opcode 2 echo. `gameResident = true`.
- **Client role:** create a client targeting `127.0.0.1:kDedicatedServerDefaultPort`; ping opcode 1 every 2s once connected; on `Connected` log it; on `Error`/`Disconnected`, mark disconnected and bounded-retry the `CreateClient` (the `netlib` `TcpClient` emits an `Error` event on connect failure — see `project_networking_roadmap`). `gameResident = true`.

The fixed port comes from `ServerControl.h` (already linkable — `game` links `CommonHeaders`, which puts `src/common/include` on the path). Add `#include "ServerControl.h"` to `game.cpp`.

- [ ] **Step 1: Add the include**

In `src/game/src/game.cpp`, near the existing `#include <netlib/netlib.h>`, add:

```cpp
#include "ServerControl.h"   // kDedicatedServerDefaultPort
```

- [ ] **Step 2: Rework `NetDemoSystem::Update`**

Replace the body of `NetDemoSystem::Update` (the `m_Started` block and the poll/ping logic) with role-branched logic. Keep the class members; add a few for client retry/state. Full replacement for the class:

```cpp
class NetDemoSystem final : public ISystem {
public:
    void Update(SystemContext& ctx) override {
        const NetServices* net = ctx.Net;
        if (!net) return;   // nullptr in tests / before engine init

        if (ctx.role == AppRole::Server) UpdateServer(ctx, net);
        else                             UpdateClient(ctx, net);
    }
    const char* Name() const override { return "NetDemoSystem"; }
    SystemPhase Phase() const override { return SystemPhase::PreRender; }

private:
    // ---- Server: bind once, echo pings (opcode 1 -> opcode 2) ----
    void UpdateServer(SystemContext& ctx, const NetServices* net) {
        if (!m_ServerStarted) {
            m_ServerStarted = true;
            NetServerConfig sc{};
            sc.bind = netlib::Endpoint{ "127.0.0.1", kDedicatedServerDefaultPort };
            sc.gameResident = true;
            m_Server = net->CreateServer(&netlib::MakeTcpServer, sc);
            if (m_Server != NetHandle::Invalid)
                SM_TRACE("NetDemo[server]: listening on 127.0.0.1:%u", (unsigned)kDedicatedServerDefaultPort);
            else
                SM_WARN("NetDemo[server]: failed to bind 127.0.0.1:%u", (unsigned)kDedicatedServerDefaultPort);
        }
        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Server) continue;
            if (ev.kind == NetEventKind::Message && ev.opcode == 1) {
                net->Send(m_Server, ev.conn, /*opcode*/ 2, ev.payload, ev.len);   // echo
                SM_TRACE("NetDemo[server]: echoed ping from conn %llu", (unsigned long long)ev.conn);
            } else if (ev.kind == NetEventKind::Connected) {
                SM_TRACE("NetDemo[server]: client connected (conn %llu)", (unsigned long long)ev.conn);
            }
        }
    }

    // ---- Client: connect (bounded retry), ping every 2s ----
    void UpdateClient(SystemContext& ctx, const NetServices* net) {
        if (m_Client == NetHandle::Invalid) {
            // (Re)connect with a small backoff so a not-yet-listening server doesn't spam.
            m_RetryAccum += ctx.dt;
            if (m_RetryAccum < kRetryIntervalSec) return;
            m_RetryAccum = 0.0;
            if (m_RetryCount >= kMaxRetries) {
                if (!m_GaveUpLogged) { SM_WARN("NetDemo[client]: gave up connecting after %d tries", kMaxRetries); m_GaveUpLogged = true; }
                return;
            }
            NetClientConfig cc{};
            cc.target = netlib::Endpoint{ "127.0.0.1", kDedicatedServerDefaultPort };
            cc.gameResident = true;
            m_Client = net->CreateClient(&netlib::MakeTcpClient, cc);
            m_RetryCount++;
            if (m_Client == NetHandle::Invalid)
                SM_WARN("NetDemo[client]: CreateClient failed (try %d/%d)", m_RetryCount, kMaxRetries);
            else
                SM_TRACE("NetDemo[client]: connecting to 127.0.0.1:%u (try %d)", (unsigned)kDedicatedServerDefaultPort, m_RetryCount);
            return;
        }

        NetEvent ev{};
        while (net->PollEvent(&ev)) {
            if (ev.adapter != m_Client) continue;
            if (ev.kind == NetEventKind::Connected) {
                m_Connected = true; m_GaveUpLogged = false;
                SM_TRACE("NetDemo[client]: connected");
            } else if (ev.kind == NetEventKind::Message && ev.opcode == 2) {
                SM_TRACE("NetDemo[client]: got echo (%u bytes)", ev.len);
            } else if (ev.kind == NetEventKind::Disconnected || ev.kind == NetEventKind::Error) {
                SM_WARN("NetDemo[client]: connection lost/failed; will retry");
                net->Close(m_Client);
                m_Client = NetHandle::Invalid;
                m_Connected = false;
                m_RetryAccum = 0.0;
                return;
            }
        }

        if (m_Connected) {
            m_PingAccum += ctx.dt;
            if (m_PingAccum >= 2.0) {
                m_PingAccum = 0.0;
                const uint8_t payload[4] = { 'p','i','n','g' };
                net->Send(m_Client, kNetConnInvalid, /*opcode*/ 1, payload, sizeof(payload));
                SM_TRACE("NetDemo[client]: sent ping (opcode 1)");
            }
        }
    }

    static constexpr int    kMaxRetries        = 20;
    static constexpr double kRetryIntervalSec  = 0.5;

    bool      m_ServerStarted = false;
    NetHandle m_Server        = NetHandle::Invalid;

    NetHandle m_Client        = NetHandle::Invalid;
    bool      m_Connected     = false;
    bool      m_GaveUpLogged  = false;
    int       m_RetryCount    = 0;
    double    m_RetryAccum    = 0.0;
    double    m_PingAccum     = 0.0;
};
```

> Engineer note: keep the existing registration line (`s->Register(std::make_unique<NetDemoSystem>());`). Confirm `netlib::Endpoint`, `netlib::MakeTcpServer`, `netlib::MakeTcpClient`, `NetServerConfig`, `NetClientConfig`, `NetEvent`, `NetEventKind`, `kNetConnInvalid`, `NetHandle` are all already visible in `game.cpp` (they were used by the prior `NetDemoSystem`). `AppRole` arrives via `Systems.h` (Task 1).

- [ ] **Step 3: Rebuild the game + editor, verify build**

Run:
```
cmake --build --preset msvc-win64-vs2026-community --target game
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: both build clean. (The editor restart from Task 1's `GAME_API_VERSION` bump must already have happened; if not, restart the editor before relying on hot-reload.)

- [ ] **Step 4: Manual smoke — server now listens**

Run the server again:
```
./out/build/msvc-win64-vs2026-community/bin/Debug/server.exe --port=27015
```
Expected new log: `NetDemo[server]: listening on 127.0.0.1:27015`. Ctrl-C → clean exit. Leave it running for Task 8.

- [ ] **Step 5: Commit**

```bash
git add src/game/src/game.cpp
git commit -m "feat(net): NetDemoSystem role branch — server listens, client connects with retry"
```

---

## Task 5: Editor `ServerSupervisor` (spawn + Job Object + clean stop + status)

**Files:**
- Create: `src/editor/src/app/ServerSupervisor.h`
- Create: `src/editor/src/app/ServerSupervisor.cpp`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `tests/test_servercontrol.cpp` (add arg-builder + status tests)

`ServerSupervisor` owns the child process lifecycle. `Start(port, world)`: build the arg string, `CreateProcess(server.exe ...)`, assign the child to a Job Object created with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so the child dies if the editor exits/crashes (the editor's process handle to the job is the last ref). `Stop()`: open + `SetEvent` the named shutdown event, wait bounded, else `TerminateProcess`. `Status()`: `GetExitCodeProcess` → Running / Stopped(code) / Crashed(code). The pure arg-builder (`BuildServerArgs`) and status-mapping are factored out for unit testing.

- [ ] **Step 1: Add failing pure-helper tests**

Append to `tests/test_servercontrol.cpp` (before the final `printf`/`return`), and add the include at top: `#include "ServerSupervisor.h"`. Also add the editor app dir to the test's include path (Step 5 of this task updates `tests/CMakeLists.txt`).

```cpp
    // ServerSupervisor pure helpers (no process spawned).
    {
        const std::string args = BuildServerArgs(27015, "assets/world.json");
        assert(args.find("--port=27015") != std::string::npos);
        assert(args.find("--world=assets/world.json") != std::string::npos);

        // Empty world => no --world flag (server uses its default).
        const std::string args2 = BuildServerArgs(27016, "");
        assert(args2.find("--port=27016") != std::string::npos);
        assert(args2.find("--world=") == std::string::npos);

        // Status mapping: STILL_ACTIVE => Running; 0 => Stopped; other => Crashed.
        assert(MapExitCodeToStatus(STILL_ACTIVE) == ServerStatus::Running);
        assert(MapExitCodeToStatus(0)            == ServerStatus::Stopped);
        assert(MapExitCodeToStatus(0xC0000005)   == ServerStatus::Crashed);
    }
```

- [ ] **Step 2: Write `ServerSupervisor.h`**

```cpp
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Process status snapshot the panel renders.
enum class ServerStatus { NotStarted, Running, Stopped, Crashed };

// PURE helpers (no Win32 side effects) — unit-tested.
// Build the command-line argument tail for server.exe. Empty world => no --world.
std::string BuildServerArgs(uint16_t port, const std::string& worldPath);
// Map a GetExitCodeProcess result to a status. STILL_ACTIVE => Running, 0 =>
// Stopped, anything else => Crashed.
ServerStatus MapExitCodeToStatus(DWORD exitCode);

// Editor-side supervisor for the out-of-process dedicated server. Spawns
// server.exe into a Job Object (KILL_ON_JOB_CLOSE => child dies with the editor),
// stops it via a named event (clean) with a TerminateProcess fallback.
class ServerSupervisor {
public:
    ServerSupervisor();
    ~ServerSupervisor();

    ServerSupervisor(const ServerSupervisor&) = delete;
    ServerSupervisor& operator=(const ServerSupervisor&) = delete;

    // Spawn server.exe bound to `port`. exePath defaults to "server.exe" (same
    // dir / CWD as the editor). Returns false + logs on failure. No-op if already running.
    bool Start(uint16_t port, const std::string& worldPath = "", const std::string& exePath = "server.exe");

    // Clean shutdown (named event), bounded wait, then TerminateProcess fallback.
    void Stop(DWORD cleanWaitMs = 3000);

    // Refresh + return current status (also flips Running->Stopped/Crashed on exit).
    ServerStatus Status();
    DWORD        LastExitCode() const { return m_LastExitCode; }
    uint16_t     Port() const { return m_Port; }

private:
    void CloseHandles();

    HANDLE   m_Job     = nullptr;   // KILL_ON_JOB_CLOSE
    HANDLE   m_Process = nullptr;
    HANDLE   m_Thread  = nullptr;
    uint16_t m_Port    = 0;
    DWORD    m_LastExitCode = 0;
    ServerStatus m_Status = ServerStatus::NotStarted;
};
```

- [ ] **Step 3: Write `ServerSupervisor.cpp`**

```cpp
#include "ServerSupervisor.h"
#include "ServerControl.h"
#include "lib.h"   // SM_WARN / SM_ERROR / SM_TRACE

#include <vector>

std::string BuildServerArgs(uint16_t port, const std::string& worldPath) {
    std::string args = "--port=" + std::to_string(port);
    if (!worldPath.empty()) args += " --world=" + worldPath;
    return args;
}

ServerStatus MapExitCodeToStatus(DWORD exitCode) {
    if (exitCode == STILL_ACTIVE) return ServerStatus::Running;
    if (exitCode == 0)            return ServerStatus::Stopped;
    return ServerStatus::Crashed;
}

ServerSupervisor::ServerSupervisor() = default;
ServerSupervisor::~ServerSupervisor() {
    // Closing the job handle triggers KILL_ON_JOB_CLOSE => the child dies even if
    // we never called Stop(). Do a best-effort clean stop first.
    if (m_Process) Stop(1000);
    CloseHandles();
}

bool ServerSupervisor::Start(uint16_t port, const std::string& worldPath, const std::string& exePath) {
    if (Status() == ServerStatus::Running) {
        SM_WARN("ServerSupervisor: server already running on port %u", (unsigned)m_Port);
        return false;
    }
    CloseHandles();
    m_Port = port;
    m_LastExitCode = 0;

    m_Job = CreateJobObjectA(nullptr, nullptr);
    if (!m_Job) { SM_ERROR("ServerSupervisor: CreateJobObject failed (%lu)", GetLastError()); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(m_Job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
        SM_WARN("ServerSupervisor: SetInformationJobObject failed (%lu); orphan protection off", GetLastError());

    std::string cmd = "\"" + exePath + "\" " + BuildServerArgs(port, worldPath);
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_SUSPENDED so we can assign to the job BEFORE it runs (no escape window).
    const BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                   CREATE_SUSPENDED | CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!ok) {
        SM_ERROR("ServerSupervisor: CreateProcess('%s') failed (%lu)", cmd.c_str(), GetLastError());
        CloseHandles();
        return false;
    }
    if (!AssignProcessToJobObject(m_Job, pi.hProcess))
        SM_WARN("ServerSupervisor: AssignProcessToJobObject failed (%lu)", GetLastError());
    ResumeThread(pi.hThread);

    m_Process = pi.hProcess;
    m_Thread  = pi.hThread;
    m_Status  = ServerStatus::Running;
    SM_TRACE("ServerSupervisor: spawned server.exe (pid %lu) on port %u", pi.dwProcessId, (unsigned)port);
    return true;
}

void ServerSupervisor::Stop(DWORD cleanWaitMs) {
    if (!m_Process) return;

    // Clean: set the named event the server waits on.
    const std::string evName = ServerShutdownEventName(m_Port);
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, evName.c_str());
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else    { SM_WARN("ServerSupervisor: OpenEvent('%s') failed (%lu); will terminate", evName.c_str(), GetLastError()); }

    if (WaitForSingleObject(m_Process, cleanWaitMs) != WAIT_OBJECT_0) {
        SM_WARN("ServerSupervisor: clean stop timed out; TerminateProcess");
        TerminateProcess(m_Process, 1);
        WaitForSingleObject(m_Process, 1000);
    }
    Status();          // capture exit code
    CloseHandles();    // closing the job handle also enforces KILL_ON_JOB_CLOSE
    m_Status = ServerStatus::Stopped;
}

ServerStatus ServerSupervisor::Status() {
    if (!m_Process) return m_Status;
    DWORD code = 0;
    if (GetExitCodeProcess(m_Process, &code)) {
        m_Status = MapExitCodeToStatus(code);
        if (m_Status != ServerStatus::Running) m_LastExitCode = code;
    }
    return m_Status;
}

void ServerSupervisor::CloseHandles() {
    if (m_Thread)  { CloseHandle(m_Thread);  m_Thread  = nullptr; }
    if (m_Process) { CloseHandle(m_Process); m_Process = nullptr; }
    if (m_Job)     { CloseHandle(m_Job);     m_Job     = nullptr; }  // KILL_ON_JOB_CLOSE fires here
}
```

- [ ] **Step 4: Add `ServerSupervisor.cpp` to the editor build**

In `src/editor/CMakeLists.txt`, add to the editor source list (under "App / orchestration layer"):

```cmake
    src/app/ServerSupervisor.cpp
```

- [ ] **Step 5: Point the test at the supervisor sources**

In `tests/CMakeLists.txt`, update the `test_servercontrol` target to compile the supervisor + see its header:

```cmake
add_executable(test_servercontrol
    test_servercontrol.cpp
    ${CMAKE_SOURCE_DIR}/src/editor/src/app/ServerSupervisor.cpp
)

target_link_libraries(test_servercontrol PRIVATE
    Engine        # for SM_* logging used by ServerSupervisor.cpp
)

target_include_directories(test_servercontrol PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
    ${CMAKE_SOURCE_DIR}/src/editor/src/app
)

set_target_properties(test_servercontrol PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

> Replace the Task-1 version of this target with the above (do not duplicate the `add_executable`).

- [ ] **Step 6: Build + run the test, verify it passes**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_servercontrol
./out/build/msvc-win64-vs2026-community/bin/Debug/test_servercontrol.exe
```
Expected: `All server-control tests passed.`

- [ ] **Step 7: Commit**

```bash
git add src/editor/src/app/ServerSupervisor.h src/editor/src/app/ServerSupervisor.cpp \
        src/editor/CMakeLists.txt tests/test_servercontrol.cpp tests/CMakeLists.txt
git commit -m "feat(net): editor ServerSupervisor — spawn into Job Object, named-event clean stop"
```

---

## Task 6: Dedicated Server panel + editor wiring

**Files:**
- Create: `src/editor/src/panels/DedicatedServerPanel.h`
- Create: `src/editor/src/panels/DedicatedServerPanel.cpp`
- Modify: `src/editor/CMakeLists.txt`
- Modify: `src/editor/src/app/ImGuiRenderer.cpp` (own a `ServerSupervisor`, draw the panel)
- Modify: `src/editor/src/app/ImGuiRenderer.h` (the member)
- Modify: `src/editor/src/panels/MainMenuBar.cpp` (window toggle — optional)

A small panel with a port input, **Start**/**Stop** buttons, and a status line. The `ServerSupervisor` is owned by `ImGuiRenderer` (its destructor closes the Job Object → kills the child if the editor exits). The panel takes the supervisor by reference.

- [ ] **Step 1: Write `DedicatedServerPanel.h`**

```cpp
#pragma once
#include <cstdint>

class ServerSupervisor;

// Dedicated Server panel: Start/Stop server.exe + show status. The supervisor is
// owned by ImGuiRenderer and passed in by reference.
void DrawDedicatedServerPanel(ServerSupervisor& supervisor, bool* open);
```

- [ ] **Step 2: Write `DedicatedServerPanel.cpp`**

```cpp
#include "DedicatedServerPanel.h"
#include "ServerSupervisor.h"
#include "ServerControl.h"   // kDedicatedServerDefaultPort

#include <imgui.h>

void DrawDedicatedServerPanel(ServerSupervisor& supervisor, bool* open) {
    if (open && !*open) return;
    if (!ImGui::Begin("Dedicated Server", open)) { ImGui::End(); return; }

    static int s_Port = kDedicatedServerDefaultPort;
    ImGui::InputInt("Port", &s_Port);
    if (s_Port < 1)     s_Port = 1;
    if (s_Port > 65535) s_Port = 65535;

    const ServerStatus st = supervisor.Status();
    const bool running = (st == ServerStatus::Running);

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Start", ImVec2(120, 0))) {
        // Empty world => server uses its default world path.
        supervisor.Start(static_cast<uint16_t>(s_Port));
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(120, 0))) {
        supervisor.Stop();
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Status");
    switch (st) {
        case ServerStatus::NotStarted: ImGui::TextUnformatted("Not started"); break;
        case ServerStatus::Running:    ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "Running on port %u", (unsigned)supervisor.Port()); break;
        case ServerStatus::Stopped:    ImGui::TextUnformatted("Stopped"); break;
        case ServerStatus::Crashed:    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Crashed (exit 0x%08lX)", supervisor.LastExitCode()); break;
    }
    ImGui::TextDisabled("The in-editor client auto-connects to 127.0.0.1:%u (see log).",
                        (unsigned)kDedicatedServerDefaultPort);

    ImGui::End();
}
```

- [ ] **Step 3: Add the panel source to the editor build**

In `src/editor/CMakeLists.txt`, add under "Editor panels":

```cmake
    src/panels/DedicatedServerPanel.cpp
```

- [ ] **Step 4: Own a `ServerSupervisor` in `ImGuiRenderer` + draw the panel**

In `src/editor/src/app/ImGuiRenderer.h`: add the include and a member.
- Add near the other includes: `#include "ServerSupervisor.h"`
- Add a private member (next to other panel state): `ServerSupervisor m_ServerSupervisor;`

In `src/editor/src/app/ImGuiRenderer.cpp`:
- Add the include at the top: `#include "DedicatedServerPanel.h"`
- Near the other `static bool s_Show...Panel` declarations / draw calls (around line 497 where `DrawNavigationPanel(ctx, &s_ShowNavigationPanel);` is), add:

```cpp
        static bool s_ShowDedicatedServerPanel = false;
        DrawDedicatedServerPanel(m_ServerSupervisor, &s_ShowDedicatedServerPanel);
```

> Engineer note: match how the surrounding panels gate visibility. If panel show-flags are toggled from `MainMenuBar`, hook `s_ShowDedicatedServerPanel` there too (Step 5). If they are plain `static bool`s defaulting visible, mirror that. The panel early-returns when `*open` is false, so a default-hidden flag is fine.

- [ ] **Step 5 (optional): Add a menu toggle in `MainMenuBar.cpp`**

If `MainMenuBar` exposes a "Windows"/"View" menu listing the other panels, add a `MenuItem("Dedicated Server", ...)` that toggles a show-flag. Wire it through whatever result struct `MainMenuBar` already uses (mirror the existing Navigation/Simulation entries). If the menu pattern is non-trivial, skip this step — the panel can default to visible instead. Do NOT invent a new plumbing pattern.

- [ ] **Step 6: Build the editor, verify it compiles**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target editor
```
Expected: `editor.exe` builds clean.

- [ ] **Step 7: Manual smoke — Start/Stop from the editor**

Launch the editor. Open the Dedicated Server panel (menu toggle, or it's visible). Click **Start** → status flips to "Running"; the editor log shows the client's `NetDemo[client]: connecting...` then `connected`, and the spawned `server.exe` console shows `listening` + `echoed ping`. **USER ACTION:** accept Windows Firewall prompts for both `editor.exe` (client) and `server.exe` (listener) the first time. Click **Stop** → status "Stopped", server console exits cleanly. Close the editor with the server running → confirm `server.exe` dies with it (Job Object). 

- [ ] **Step 8: Commit**

```bash
git add src/editor/src/panels/DedicatedServerPanel.h src/editor/src/panels/DedicatedServerPanel.cpp \
        src/editor/CMakeLists.txt src/editor/src/app/ImGuiRenderer.h src/editor/src/app/ImGuiRenderer.cpp \
        src/editor/src/panels/MainMenuBar.cpp
git commit -m "feat(net): Dedicated Server editor panel + ServerSupervisor wiring"
```

---

## Task 7: End-to-end automated test (`test_dedicated_server`)

**Files:**
- Create: `tests/test_dedicated_server.cpp`
- Modify: `tests/CMakeLists.txt`

One focused process-level test: spawn `server.exe` as a child, poll until a `netlib` TCP client can connect to `127.0.0.1:kDedicatedServerDefaultPort` (proves the headless server booted, loaded `Game.dll`, ran `NetDemoSystem` in Server role, and bound the listen socket), then signal clean shutdown via the named event and assert the process exits within a timeout. This test deliberately does NOT assert the full opcode echo (that's the manual smoke) — a raw TCP connect keeps it robust and decoupled from game-protocol details.

The test must run from `RUNTIME_DIR` (where `server.exe`, `Game.dll`, and `assets/` live) — it is built into `RUNTIME_DIR` and uses a relative `server.exe`, matching the editor's spawn.

- [ ] **Step 1: Write the test**

Create `tests/test_dedicated_server.cpp`:

```cpp
// End-to-end: spawn server.exe, prove it boots headless + listens, then clean-stop.
// NOTE: requires server.exe + Game.dll + assets in CWD (RUNTIME_DIR). The first
// run may surface a Windows Firewall prompt for server.exe — accept it once.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

#include "ServerControl.h"

#pragma comment(lib, "ws2_32.lib")

static bool TryConnect(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const bool ok = (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::closesocket(s);
    return ok;
}

int main() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const uint16_t port = kDedicatedServerDefaultPort;

    // Spawn server.exe (relative path => resolved against CWD = RUNTIME_DIR).
    std::string cmd = "server.exe --port=" + std::to_string(port);
    std::string cmdMut = cmd;
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL spawned = CreateProcessA(nullptr, cmdMut.data(), nullptr, nullptr, FALSE,
                                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
    if (!spawned) {
        std::printf("FAIL: CreateProcess(server.exe) err=%lu\n", GetLastError());
        return 1;
    }

    // Poll for the listen socket (bounded ~15s; world+navmesh+dll load takes a moment).
    bool connected = false;
    for (int i = 0; i < 150 && !connected; ++i) {
        connected = TryConnect(port);
        if (!connected) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    int rc = 0;
    if (!connected) {
        std::printf("FAIL: could not connect to 127.0.0.1:%u within timeout\n", (unsigned)port);
        rc = 1;
    } else {
        std::printf("OK: server is listening on 127.0.0.1:%u\n", (unsigned)port);
    }

    // Clean shutdown via the named event.
    const std::string evName = ServerShutdownEventName(port);
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, evName.c_str());
    if (ev) { SetEvent(ev); CloseHandle(ev); }
    else    { std::printf("WARN: OpenEvent('%s') err=%lu; terminating\n", evName.c_str(), GetLastError()); }

    const DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    if (wait != WAIT_OBJECT_0) {
        std::printf("FAIL: server did not exit cleanly; terminating\n");
        TerminateProcess(pi.hProcess, 1);
        rc = 1;
    } else {
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        std::printf("OK: server exited with code %lu\n", code);
        if (code != 0) rc = 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    WSACleanup();

    if (rc == 0) std::printf("All dedicated-server e2e tests passed.\n");
    return rc;
}
```

- [ ] **Step 2: Add the test target**

In `tests/CMakeLists.txt`, append:

```cmake
add_executable(test_dedicated_server
    test_dedicated_server.cpp
)

target_link_libraries(test_dedicated_server PRIVATE
    CommonHeaders
    ws2_32
)

target_include_directories(test_dedicated_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src/common/include
)

# Needs server.exe + Game.dll + assets present in RUNTIME_DIR at run time.
add_dependencies(test_dedicated_server server game)

set_target_properties(test_dedicated_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${RUNTIME_DIR}"
    FOLDER Tests
)
```

- [ ] **Step 3: Build + run the test**

Run:
```
cmake --preset msvc-win64-vs2026-community
cmake --build --preset msvc-win64-vs2026-community --target test_dedicated_server
./out/build/msvc-win64-vs2026-community/bin/Debug/test_dedicated_server.exe
```
Expected: `OK: server is listening...` → `OK: server exited with code 0` → `All dedicated-server e2e tests passed.` **USER ACTION:** if a firewall prompt appears for `server.exe` on the first run, accept it, then re-run (the test's 15s connect window may expire while the dialog is open). Subsequent runs are unattended.

- [ ] **Step 4: Commit**

```bash
git add tests/test_dedicated_server.cpp tests/CMakeLists.txt
git commit -m "test(net): e2e dedicated-server boot+listen+clean-shutdown"
```

---

## Task 8: Docs, regression sweep, memory update

**Files:**
- Modify: `docs/superpowers/specs/2026-05-30-dedicated-server-out-of-process-design.md` (tick deliverables, record manual smoke)
- Modify: project memory `project_networking_roadmap.md` + `MEMORY.md` pointer

- [ ] **Step 1: Full regression — rebuild everything + run the existing test suite**

Run (catches any breakage from the `GAME_API_VERSION` / `SystemContext` changes):
```
cmake --build --preset msvc-win64-vs2026-community
./out/build/msvc-win64-vs2026-community/bin/Debug/test_ecs.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_net.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_netlib.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_servercontrol.exe
./out/build/msvc-win64-vs2026-community/bin/Debug/test_dedicated_server.exe
```
Expected: all green (`All ECS tests passed.`, the net suites pass, `All server-control tests passed.`, `All dedicated-server e2e tests passed.`).

- [ ] **Step 2: Tick the spec deliverables checklist + record the smoke**

In `docs/superpowers/specs/2026-05-30-dedicated-server-out-of-process-design.md`, check off the "Deliverables checklist" items now satisfied and append a short "Manual smoke verified <date>: editor Start → client connect → ping/echo → Stop; editor-close kills server (Job Object)." line. Confirm the documented navmesh limitation (pre-baked/disk only on headless server) is stated.

- [ ] **Step 3: Update project memory**

Update `C:\Users\nunol\.claude\projects\C--dev-clang-examples\memory\project_networking_roadmap.md`: mark Phase 3 DONE with the commit range, note the key decisions (slim `ServerApplication`, Job Object + named-event, fixed-port demo + variable-port follow-up, pre-baked-nav limitation, `SystemContext::role` mechanism), and the next milestone (Phase 4 in-process server, gated on de-singletoning). Refresh the `MEMORY.md` one-line pointer hook accordingly. Keep it to facts not derivable from the code/commits.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-30-dedicated-server-out-of-process-design.md
git commit -m "docs(net): tick Phase-3 deliverables + record dedicated-server manual smoke"
```

(The memory files live outside the repo and are not committed.)

---

## Self-Review

**Spec coverage** (against `2026-05-30-dedicated-server-out-of-process-design.md` deliverables):
- Headless boot path (slim `ServerApplication`, no window/renderer) → Task 2. ✓
- GameThread mode-gating for client-only responsibilities → handled by NOT reusing GameThread; the renderer-coupled work (model worker, GR/RG rings, viewport, swap handshake, plugins) simply doesn't exist in `ServerApplication`. The audited client-only list is enumerated in Task 2's preamble. ✓
- `server.exe` target + CMake + trimmed deps → Task 3. ✓
- Editor Start/Stop + supervision (spawn, status, clean shutdown, Job-Object parent-death) → Tasks 5 + 6. ✓
- Client-side TCP connect-to-local-server with bounded retry → Task 4 (client role). ✓
- End-to-end demo round-trip + one automated test → manual smoke (Tasks 3/4/6) + automated `test_dedicated_server` (Task 7). ✓
- Documented navmesh-from-mesh limitation → Task 2 (WARN + comment) + Task 8 (spec). ✓
- Manual firewall/UAC user gates → called out as **USER ACTION** in Tasks 3, 6, 7. ✓
- Port collision / SO_REUSEADDR → already set on the netlib listen socket (verified `TcpServer.cpp:20-21`); no extra work. ✓

**Type consistency:** `AppRole` (Task 1) used identically in `GameState.Role`, `SystemContext.role`, `ServerApplication` (`m_GameState.Role = AppRole::Server`), and `NetDemoSystem` (`ctx.role == AppRole::Server`). `ServerStatus` enum + `MapExitCodeToStatus`/`BuildServerArgs` signatures match between `ServerSupervisor.h`, `.cpp`, the panel, and the test. `ServerShutdownEventName`/`kDedicatedServerDefaultPort` used consistently across server main, supervisor, and both tests. `NetServices` method names (`CreateServer`/`CreateClient`/`BoundPort`/`Send`/`PollEvent`/`Close`) and `NetEvent`/`NetEventKind`/`kNetConnInvalid` match `NetServices.h`. `ServerApplication::Config` fields match its `.cpp` usage.

**Placeholder scan:** no TBD/TODO; every code step shows complete code; CMake snippets are concrete. The two "engineer note" callouts ask only to verify include paths against existing files (`GameThread.cpp` filewatch include, editor menu pattern) — they do not defer design decisions.

**Known restart points** (call out to the executor, not defects): Task 1 bumps `GAME_API_VERSION` (18→19) and changes `SystemContext` → rebuild `ecs`+`Engine`+`game`+`editor` and restart the running editor before relying on hot-reload (CLAUDE.md rule). All later game-only edits (Task 4) hot-reload normally.
