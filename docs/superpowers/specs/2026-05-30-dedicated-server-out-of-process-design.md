# Phase 3 — Dedicated Server (Out-of-Process) — Context Spec

**Umbrella:** `2026-05-30-networking-architecture-design.md`
**Date:** 2026-05-30
**Depends on:** Phase 2 (engine Net plumbing + `NetServices`) + Phase 1 netlib adapters (server uses the **IOCP `TcpServer`**; the editor client uses the **async `TcpClient`**).
**Blocks:** Phase 4 (in-process server reuses `ServerHost`); real multiplayer gameplay.

---

## Goal

Let the editor stand up a **real, separate-process** dedicated server and have the in-editor client connect to it over **loopback TCP** — the production-shaped path for a server-authoritative ARPG. Introduces the headless server-role boot path, a `server.exe` target, and the editor control to spawn/supervise it.

## Proof of done

From the editor: click "Start Dedicated Server" → a `server.exe` process launches headless (no window, no renderer) → the in-editor client (via `NetServices` + the `netlib` TCP adapter) connects to `127.0.0.1:<port>` → a message round-trips both ways → "Stop" cleanly shuts the server down. Server crash or its own `Game.dll` hot-reload does **not** take down the editor.

## Scope

**In scope:**
- A **headless boot path**: run `GameThread` + `NetSubsystem` (the IO pool) **without** `PlatformThread`/`RenderThread`/NVRHI/GLFW.
- A `server.exe` target (a `runtime.exe` sibling) whose `main` boots that headless path in server role, loading `Game.dll` like the other exes.
- `ServerHost` concept formalized = `{ECS, SystemScheduler, io-server}` running headless (even if Phase 3's process simply *is* one `ServerHost`).
- Editor UI + process supervision: spawn `server.exe` (with port/args), track it, signal shutdown, surface status.
- Client-side wiring: a `NetSystem` path that creates a `netlib` TCP client to the local server.

**Out of scope:**
- In-process `ServerHost` in the editor address space (Phase 4 — gated on de-singletoning, §9 of umbrella).
- Real game replication/auth protocol (game's job; Phase 3 only needs a demo round-trip).
- Remote/internet servers, NAT, matchmaking, TLS.

## The central engine-structural problem: headless boot

`Application::Init` (`src/engine/src/core/Application.cpp:51-62`) **always** constructs `PlatformThread` (owns the GLFW window) and `RenderThread` (owns NVRHI), and `Application::Run` always spawns the render thread. `runtime.exe`'s `main` (`src/runtime/src/main.cpp`) just does `Application app; app.Init(...); app.Run();`. A headless server cannot create a window or a GPU device.

**This phase must introduce a renderer-less / windowless run path.** Options to evaluate in the plan:

1. **Headless mode on `Application`.** Add a flag (e.g. `Application::InitHeadless()` or an `AppMode { Client, Server }`) that skips `PlatformThread` + `RenderThread`, runs only `GameThread` + `NetSubsystem`, and provides a non-GLFW main loop (the process blocks on GameThread until shutdown). Pros: one `Application` class, shared lifecycle. Cons: lots of `if (mode==Server)` branches; `GameThread` currently assumes some render-thread coupling (e.g. `SwapInProgress`, `GRCommandRing` mesh uploads, `SceneViewportSize`) — audit what GameThread does that presumes a RenderThread and make it tolerate absence.
2. **Separate `ServerApplication`.** A distinct, slim bootstrap that owns `GameThread` + `NetSubsystem` only, sharing `ApplicationContext` selectively. Pros: clean separation, no client-mode branches polluting the server path. Cons: some duplication of init (settings load, `GameState`, world load).

**Recommend option 2 (separate slim server bootstrap)** for a first cut — it avoids threading headless-branches through the renderer-coupled `Application`/`GameThread`, and the duplication is small and honest. Reconsider unification later if it proves to drift.

### GameThread audit (must do before/within this phase)
`GameThread::RunLoop` today does renderer-coupled work that a headless server must not require:
- `SwapInProgress` / `GameThreadPaused` handshake (renderer hot-swap) — harmless if RenderThread never sets it, but confirm.
- Mesh/material upload via `GRCommandRing` / `RGCommandRing` and the model-load worker — a server may not need GPU meshes at all, BUT it **does** need nav-mesh CPU triangle data (which today flows through the MeshUpload round-trip — see `GameThread.cpp` `pendingMeshData` → `NavMeshSystem::StoreMeshCpuData`). **This is a real coupling:** navmesh geometry from `MeshComponent` sources currently depends on the render round-trip to get CPU vertex data. A headless server that needs navmesh from mesh sources must get that CPU data another way (load meshes CPU-side without GPU upload). Scope this carefully — the demo can sidestep it (collider-source navmesh, or a pre-baked navmesh on disk via `TryLoadFromDisk`), but document the limitation.
- `ViewportComponent` / `UICameraComponent` updates from `SceneViewportSize` — server can default/skip.

The plan must enumerate which GameThread responsibilities are client-only and gate them by mode.

## Build / target wiring

- `server.exe` target mirrors `src/runtime/CMakeLists.txt`: links `Engine`, `ecs`, `netlib`, `CommonHeaders`, `GameHeaders`, glm; loads `Game.dll`; `add_dependencies(server game)`; `VS_DEBUGGER_WORKING_DIRECTORY` = `RUNTIME_DIR`; copies assets. It likely does **not** need nvrhi/glfw/freetype/dxc if truly headless — trim the link list (a benefit of the slim-bootstrap option; verify the engine doesn't force-link them).
- Add `server` to root `CMakeLists.txt` subdirs / target lists and the relevant presets.
- Per project memory `project_build_preset`: build/test with `msvc-win64-vs2026-community` only.

## Editor control & process supervision

- A panel/button (mirror the editor-panel style under `src/editor/src/panels/`, e.g. how `NavigationPanel` triggers actions) to **Start/Stop** the server.
- Spawn `server.exe` via `CreateProcess` with args (`--port=<n>`, maybe `--world=<path>`). Capture the `PROCESS_INFORMATION`; track liveness.
- **Shutdown:** clean signal preferred over `TerminateProcess` — e.g. the server polls a shutdown condition (a `--parent-pid` it watches, a local control message, or a named event). Decide a simple mechanism (a loopback control message or a Win32 named event) in the plan.
- Surface status (running / stopped / crashed + exit code) in the panel. Per `feedback_logging_over_silent_skip`, log spawn failures (`SM_WARN`/`SM_ERROR`), never silently no-op the button.
- Editor is the **client**; it creates a `netlib` TCP client to `127.0.0.1:<port>` once the server is up. Handle connect-retry (server may take a moment to bind) — bounded retry with backoff, logged.

## Testing strategy

- **Manual smoke (primary):** editor Start → connect → round-trip → Stop; kill the server mid-session and confirm the editor survives + reports it.
- **Automated where feasible:** a test that launches `server.exe` as a child, connects a `netlib` TCP client over loopback, round-trips a message, signals shutdown, asserts clean exit. (Process-spawning tests are heavier; keep one focused end-to-end test, not a suite.)
- **Headless boot test:** the server bootstrap starts, loads the world, runs GameThread ticks, and exits cleanly on shutdown signal — with no renderer/window.

## Risks / gotchas

- **Headless boot is the bulk of the risk.** The renderer/window coupling in `Application`/`GameThread` is the unknown; the GameThread audit above must be done early. Budget for it.
- **Navmesh-from-mesh CPU data coupling** (the render round-trip dependency) — easiest mitigation for the demo: server uses a **pre-baked navmesh** (`NavMeshSystem::TryLoadFromDisk`, which the startup path already calls) or collider-source geometry, avoiding the GPU round-trip entirely. Document this; full CPU-side mesh loading for servers is a follow-up.
- **`Game.dll` hot-reload in the server process** should "just work" (the server runs the same `GameThread` + `GameLibrary` machinery), but verify the filewatch CWD logic and that a server-side reload doesn't assume renderer presence.
- **Port collisions / cleanup:** use a configurable port; on Stop ensure the socket is released so a quick restart doesn't hit `WSAEADDRINUSE` (SO_REUSEADDR or proper close in `netlib`).
- **Orphaned server processes:** if the editor crashes, the child `server.exe` must not linger. Use a parent-death detection (job object `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, or parent-pid watch) — recommend a Win32 **Job Object** so the child dies with the editor.

## Manual steps (user-owned — word as human gates, do not automate)

Phase 3 is the most likely to hit OS prompts only the user can accept; the implementation must surface them and **pause for the user**, never assume they're granted:

- **Windows Firewall allow-dialog** fires when `server.exe` first calls `listen()` (and possibly when the editor first connects out). Loopback often skips it but policy varies. **User accepts once** per binary, or pre-authorizes a firewall rule. The client's bounded connect-retry must tolerate the window during which the user hasn't yet clicked Allow — log "waiting for server / connection refused, retrying" rather than hanging or hard-failing on the first attempt.
- **UAC elevation** if a firewall rule must be added, or the machine policy requires admin to bind/listen. Surface the need; the user elevates. Suggest running such a step via `! <command>` in-session.
- **Two binaries to authorize:** both `editor.exe`/`runtime.exe` (client) and `server.exe` (listener) may each prompt the first time. The plan's first end-to-end run should include a **"USER ACTION: accept firewall prompts for editor + server"** step.
- A successful manual smoke run should be recorded once so later runs (post-authorization) are unattended.

## Deliverables checklist

- [x] Headless server boot path — slim `ServerApplication` (`src/engine/src/core/ServerApplication.{h,cpp}`), no window, no renderer; `Init/Tick/Run/Shutdown`. Commit `2036a42`.
- [x] GameThread mode-gating for client-only responsibilities — achieved by NOT reusing `GameThread`: `ServerApplication` simply omits the renderer-coupled work (model-load worker, GR/RG mesh rings, viewport/UICamera sync, swap handshake, plugins). Role flows via `SystemContext::role` (`AppRole`, commit `59acec9`).
- [x] `server.exe` target + CMake wiring; trimmed explicit link deps (`Engine ecs netlib CommonHeaders GameHeaders glm`). Commit `f629e84`. (Engine PUBLIC-propagates nvrhi/glfw import libs transitively; acceptable — no window/device is ever created. Presets unchanged: existing `msvc-win64-vs2026-community` picks up the new target.)
- [x] Editor Start/Stop control + process supervision — `ServerSupervisor` (`CreateProcess` into a Job Object with `KILL_ON_JOB_CLOSE` = orphan-proof, named-event clean stop + `TerminateProcess` fallback, status/exit-code) commit `dbad3d0`; `DedicatedServerPanel` commit `a6058b1`.
- [x] Client-side TCP connect-to-local-server wiring — `NetDemoSystem` role branch (server binds+echoes, client connects+pings, bounded connect-retry with budget reset on connect). Commit `db870a6`.
- [x] One automated end-to-end test — `tests/test_dedicated_server.cpp` (commit `25ab0ac`): spawns `server.exe`, connects over loopback TCP (proves headless boot + Game.dll load + server-role listen-socket bind), signals the named shutdown event, asserts clean exit (code 0). **Verified green.**
- [x] Documented limitation: navmesh-from-mesh on headless server uses pre-baked disk navmesh (`NavMeshSystem::TryLoadFromDisk`); on a disk-bake miss `ServerApplication::Init` SM_WARNs and runs without nav (no GPU round-trip for mesh CPU data headless). See `ServerApplication.cpp`.

**Verification (2026-05-30):** automated `test_dedicated_server` passes end-to-end (boot → listen on 127.0.0.1:27015 → clean exit 0). Phase-3 regression set green: `test_ecs`, `test_net`, `test_netlib`, `test_servercontrol`, `test_dedicated_server`.

**User-owned manual smoke (not yet performed — requires interactive GUI + firewall acceptance):** in the editor, open the Dedicated Server panel → Start → confirm the in-editor client logs `connected` + the server console logs `echoed ping` (full ping/echo round-trip) → Stop → confirm clean exit; then close the editor with the server running and confirm the `server.exe` child dies with it (Job Object). **USER ACTION:** accept the Windows Firewall prompt for `editor.exe` and `server.exe` on first run.

**Variable-port wiring — DONE 2026-05-30** (commit `af45e54`). The port now flows end-to-end: `SystemContext::serverPort` carries it; `ServerApplication` feeds `Config::port` (from `server.exe --port`); `NetDemoSystem` *binds* and *connects* to `ctx.serverPort` (no longer hardcoded). Editor side: `ApplicationContext::NetServerPort` (atomic, RenderThread→GameThread); the Dedicated Server panel writes it on Start so the in-editor client retargets (a `ctx.serverPort` change drops the handle and reconnects). The client retry is now non-permanent (20 fast tries @0.5s, then slow @5s forever) so it reconnects whenever a server appears — no editor restart. Proven: `server.exe --port=27020` binds 27020, connect succeeds, matching named-event clean-stop (exit 0). The obsolete fixed-port panel caveat was removed. `GAME_API_VERSION` 19→20 (SystemContext ABI).

**Pre-existing test breakage — `test_navagent` FIXED 2026-05-30** (commit `4d82d74`): the stub now counts `FindPathForClass` (the call `NavAgentSystem` actually makes post-refactor `7e66356`) — green again. **Still-open pre-existing breakage (NOT Phase 3): `tests/test_settings.cpp` fails to compile** — references `ApplicationSettings::fxaaEnabled`, renamed to `aaMode` in the earlier AAMode migration. Out of Phase-3 scope; needs the test updated to the `aaMode` int field separately.
