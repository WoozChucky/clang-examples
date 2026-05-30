# Phase 2 — Engine Integration — Context Spec

**Umbrella:** `2026-05-30-networking-architecture-design.md`
**Date:** 2026-05-30
**Depends on:** Phase 1 (`netlib` interfaces + adapters).
**Blocks:** Phase 3 (dedicated server needs the engine's Net plumbing + `NetServices`).

---

## Goal

Wire `netlib` into the engine: an engine-owned **IO thread pool** that pumps abstract io-objects, the **ring + buffer-pool plumbing** that moves messages between IO threads and GameThread, the **`NetServices` bridge** that exposes networking to `Game.dll` (with zero `Engine.dll` link), and the **Model-B hot-reload teardown** that releases + quiesces `Game.dll`-resident adapters before every `FreeLibrary`.

## Proof of done

A game `NetSystem` (in `Game.dll`, registered via `GameRegisterSystems`) running on GameThread:
1. Asks the engine (via `NetServices`) to create a server and a client wired by the **in-memory** adapter (single process — no `server.exe` yet).
2. `Send`s a message; on a later tick `PollEvent` surfaces the `Connected` then `Message` on the peer, with correct opcode + payload bytes.
3. The whole thing **survives a `Game.dll` hot-reload** when the adapter factory is `netlib`-resident (connection stays up); and is **cleanly force-released** (no crash) when the adapter factory is `Game.dll`-resident.

That exercises every seam Phase 3 depends on, without a second process.

## Scope

**In scope:**
- `NetSubsystem` (engine-side; the networking analog of `NavMeshSystem`) owning: the IO thread pool, the io-object registry, per-connection inbound/outbound rings, and the `NetBufferPool`.
- IO thread(s): pump registered adapters' `Poll()`, copy received `IoEvent` payloads into pooled buffers, push `NetEvent` onto inbound rings; drain outbound rings and call adapter `Send()`.
- `NetBufferPool` (the `StagingBufferPool` analog for net payloads).
- `NetServices` struct (`src/common/include/NetServices.h`) + `NetServicesImpl::Init` (`src/engine/src/network/`), populated like `NavServicesImpl`.
- `SystemContext.Net` field; set every tick in `GameThread::RunLoop` (next to `&navServices`).
- Model-B pre-reload teardown hook + IO-thread quiesce.
- A throwaway/in-tree demo `NetSystem` in `Game.cpp` (or a test) to satisfy "proof of done".

**Out of scope:**
- `server.exe`, headless boot, cross-process anything (Phase 3).
- In-process second `ServerHost` (Phase 4).
- Protocol/opcode semantics beyond the demo round-trip (game's job).

## Where things hook into existing code (verified references)

| Concern | Existing anchor | What Phase 2 adds |
|---------|-----------------|-------------------|
| Bridge table | `NavServices` (`src/common/include/NavServices.h`) + `NavServicesImpl::Init` (`src/engine/src/navigation/NavServicesImpl.cpp`) | A parallel `NetServices` + `NetServicesImpl::Init`. |
| Threading per tick | `GameThread::RunLoop` builds `SystemContext sysCtx{ world, dt, gameTime, &navServices }` (`GameThread.cpp:447`) | Add `&netServices`; construct `NetServices netServices{}; NetServicesImpl::Init(netServices);` near the `NavServices` init (`GameThread.cpp:163-164`). |
| Variable-length payload across a ring | `RendererCommand` + `StagingBufferPool` (mesh-upload path in `GameThread.cpp`) | `NetEvent`/`OutMsg` carry `{opcode, ptr, len}`; `NetBufferPool` owns the bytes. **Required** — `SpscRing<T,N>` stores `T` by value (`SpscRing.h`), so payloads cannot be inline. |
| Ring type | `SpscRing<T,N>` (`src/common/include/SpscRing.h`), power-of-two, SPSC | One inbound + one outbound ring **per connection** (single IO thread ↔ GameThread = SPSC holds). |
| Hot-reload teardown | `GameLibrary::LoadOrReload` order: `GameExit` → `m_Scheduler->Clear()` → `FreeLibrary` → swap → `GameRegisterSystems` (`GameLibrary.cpp:87-110`); also `Unload` (`:114`) and `~GameLibrary` (`:23`) | Net teardown must run in the same pre-`FreeLibrary` window AND quiesce IO threads off `Game.dll` adapters. |
| Subsystem ownership/init | `NavMeshSystem::Instance()` singleton; `Application` owns the 3 threads (`Application.cpp`) | `NetSubsystem` — decide singleton (matches NavMeshSystem) vs. owned-by-Application. See decision below. |

## Key design decisions to make in the plan

### 1. IO threading model
- A small pool sized N (config; default e.g. 1–2 to start). Each registered io-object is assigned to a thread.
- **Decision:** one-thread-per-server vs. shared pool with all objects round-robined. Start simplest (single IO thread pumping all registered objects in a loop) unless perf demands otherwise — `log()` the choice; YAGNI on work-stealing.
- The IO thread loop: for each object `Poll()` → translate `IoEvent`→`NetEvent` (copy payload into `NetBufferPool`) → push inbound ring; drain outbound ring → `Send()`. Sleep/yield when idle (don't busy-spin a core; mirror GameThread's hybrid sleep or a short `WaitForMultipleObjects`/select if cheap).

### 2. `NetSubsystem` ownership & thread contract
- **Recommend a singleton** (`NetSubsystem::Instance()`) to match `NavMeshSystem` and keep `NetServicesImpl` forwarding trivial — BUT note the architecture's §9 singleton caution. A singleton is fine for Phases 1–3 (one world per process); Phase 4's de-singletoning will revisit it. Document that explicitly so Phase 4 isn't surprised.
- `NetServices` calls (`CreateClient`/`Send`/`PollEvent`/`Close`) are invoked from **GameThread only** (matches `NavServices`' GameThread-only contract). The IO threads are internal to `NetSubsystem`. So the registry needs a thread-safe handoff between GameThread (create/close/enqueue-outbound/drain-inbound) and IO threads (pump) — the rings handle data; create/close need a small lock or a command ring into the IO thread.

### 3. `NetHandle` representation
- Index + salt (like `MeshHandle`/`ModelHandle` are index-based; salt guards use-after-close), or opaque pointer. **Recommend index+salt** for safety + stable ABI across the `NetServices` boundary.

### 4. Model-B teardown placement
Two options (pick one in the plan; option B is simpler to reason about):
- **A:** `GameLibrary` gains `SetOnBeforeUnload(callback)` (like `SetScheduler`), invoked before `m_Scheduler->Clear()` in `LoadOrReload`/`Unload`/`~GameLibrary`. GameThread registers a callback that tells `NetSubsystem` to release Game.dll-resident connections.
- **B:** `GameThread`, when it drains `m_ReloadPending` (`GameThread.cpp:194`), calls `NetSubsystem::Instance().ReleaseGameResidentConnections()` *before* `m_GameLib.LoadOrReload(...)`. No `GameLibrary` change.

Either way `ReleaseGameResidentConnections()` must:
1. Mark the affected io-objects for teardown.
2. **Quiesce the IO thread(s) off them** — ensure no IO thread is inside a `Game.dll` adapter's `Poll()`/`Send()` (e.g., signal + join-to-barrier, or a per-object "in use" flag the IO thread respects) — *before* the vtable is unmapped.
3. Destroy those io-objects while `Game.dll` is still loaded (their dtors live there).
- How does the engine know an adapter is `Game.dll`-resident? The `CreateClient`/`CreateServer` factory pointer's module: the game can pass a flag, or `NetServices` exposes `CreateClientGameResident(...)` vs the stable variant, or the engine checks the factory pointer's module via `GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` against the loaded `Game.dll` handle. **Recommend an explicit `bool gameResident` / residence enum in the create config** — explicit beats magic, and the game knows its own intent.

### 5. Backpressure policy
- Inbound ring full (GameThread not draining fast enough) or `NetBufferPool` exhausted: `SM_WARN` + drop-newest or stall-IO-read (TCP backpressure naturally propagates if we stop recv'ing). Outbound ring full: `Send` returns false; game retries. **Decide + document**; per `feedback_logging_over_silent_skip`, never silently drop.

## Testing strategy

- **In-memory loopback test** (no sockets, no second process): drive a `NetSubsystem` + two in-memory-wired handles; pump the IO thread (or a manual pump in test mode); assert the `NetEvent` sequence on the peer.
- **Pool/ring stress:** flood messages, assert no leak (pool returns balanced), backpressure path logs + recovers.
- **Hot-reload teardown:** simulate a reload cycle (call the teardown path) with a fake `Game.dll`-resident adapter; assert no use-after-unload and clean release. (Real DLL reload is hard to unit-test; cover the teardown ordering logic + do a manual editor smoke test for the live reload.)
- **`NetServices` ABI:** a compile/static check that the table is append-only vs. any prior (mirror how `NavServices` is treated).

## Risks / gotchas

- **IO-thread vs GameThread races on create/close.** The rings are SPSC per connection (safe), but the *registry* (add/remove io-objects, handle table) is touched by GameThread (create/close) and read by IO threads (pump). Needs a deliberate sync design — a command queue into the IO thread is cleaner than a shared mutex on the hot pump loop.
- **Quiescing the IO thread for Game.dll-resident teardown** is the subtlest correctness point. Get the barrier right or hot-reload crashes return (exactly the class of bug Model B exists to prevent).
- **Don't regress `NavServices` ABI** while editing `SystemContext` — add `Net` as a new trailing field; don't reorder.
- **`Game.dll` must reach `netlib` for factories** but still must NOT link `Engine.dll`. Confirm `Game.dll`'s link graph: it may link `netlib` directly (to call `MakeTcpClient`) while networking *services* come through `NetServices`. That's consistent with the architecture (game depends on netlib + the bridge).

## Deliverables checklist

- [ ] `src/common/include/NetServices.h` (append-only table, GameThread-only contract documented).
- [ ] `NetEvent` + `NetHandle` + create-config types (in common include so both engine + game see them).
- [ ] `NetSubsystem` (IO pool, registry, rings, `NetBufferPool`) under `src/engine/src/network/`.
- [ ] `NetServicesImpl::Init` forwarding to `NetSubsystem`.
- [ ] `SystemContext.Net` + per-tick wiring in `GameThread::RunLoop`.
- [ ] Model-B teardown hook + IO-thread quiesce.
- [ ] CMake: new engine sources added to `src/engine/CMakeLists.txt` (explicit list, no glob); `Engine` links `netlib`.
- [ ] Demo `NetSystem` + tests proving the loopback round-trip and teardown.
