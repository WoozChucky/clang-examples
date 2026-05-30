# Phase 2 — Engine Integration — Context Spec

**Umbrella:** `2026-05-30-networking-architecture-design.md`
**Date:** 2026-05-30
**Depends on:** Phase 1 (`netlib` interfaces + adapters).
**Blocks:** Phase 3 (dedicated server needs the engine's Net plumbing + `NetServices`).

---

## Goal

Wire `netlib` into the engine: the engine **`IIoSink`** that adapter threads push into, the **lock-free MPSC ring + `NetBufferPool`** that move messages from adapter threads to GameThread, the **`NetServices` bridge** that exposes networking to `Game.dll` (with zero `Engine.dll` link), and the **Model-B hot-reload teardown** that `Stop()`-joins `Game.dll`-resident adapters before every `FreeLibrary`. The engine owns **no** IO threads — `netlib` adapters (IOCP server, async client) own theirs.

## Proof of done

A game `NetSystem` (in `Game.dll`, registered via `GameRegisterSystems`) running on GameThread:
1. Asks the engine (via `NetServices`) to create a server and a client wired by the **in-memory** adapter (single process — no `server.exe` yet).
2. `Send`s a message; on a later tick `PollEvent` surfaces the `Connected` then `Message` on the peer, with correct opcode + payload bytes.
3. The whole thing **survives a `Game.dll` hot-reload** when the adapter factory is `netlib`-resident (connection stays up); and is **cleanly force-released** (no crash) when the adapter factory is `Game.dll`-resident.

That exercises every seam Phase 3 depends on, without a second process.

## Scope

**In scope:**
- `NetSubsystem` (engine-side; the networking analog of `NavMeshSystem`) owning: the io-object registry, the **engine `IIoSink` impl**, the **lock-free bounded MPSC ring** of inbound `NetEvent`s, and the **lock-free `NetBufferPool`**. **No IO pump thread** — adapters (netlib) own their threads and push into the sink.
- The engine `IIoSink::OnIoEvent` (runs on adapter threads, must be thread-safe + fast): copy the borrowed `IoEvent` payload into a `NetBufferPool` block, build a `NetEvent`, enqueue on the MPSC ring.
- `MpscRing<T,N>` — new lock-free bounded MPSC ring (Vyukov), `src/common/include/MpscRing.h`, beside `SpscRing.h`.
- `NetBufferPool` — lock-free fixed-block pool (atomic free-list); concurrent acquire (adapter threads) + release (GameThread).
- `NetServices` struct (`src/common/include/NetServices.h`) + `NetServicesImpl::Init` (`src/engine/src/network/`), populated like `NavServicesImpl`. `PollEvent` drains the MPSC ring; `Send` calls the adapter directly.
- `SystemContext.Net` field; set every tick in `GameThread::RunLoop` (next to `&navServices`).
- Model-B pre-reload teardown hook: call adapter `Stop()` (joins worker threads = quiesce) before `FreeLibrary` for `Game.dll`-resident adapters.
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
| Variable-length payload across a ring | `RendererCommand` + `StagingBufferPool` (mesh-upload path in `GameThread.cpp`) | `NetEvent` carries `{kind, opcode, conn, ptr, len}`; `NetBufferPool` owns the bytes. **Required** — rings store `T` by value (`SpscRing.h`), so payloads cannot be inline. |
| Ring type | `SpscRing<T,N>` (`src/common/include/SpscRing.h`), power-of-two, **SPSC only** | **New** `MpscRing<T,N>` (Vyukov bounded, lock-free) — inbound is **many adapter threads → one GameThread**, so SPSC does not hold. One MPSC ring for ALL inbound; **no** outbound ring (`Send` is direct). |
| Hot-reload teardown | `GameLibrary::LoadOrReload` order: `GameExit` → `m_Scheduler->Clear()` → `FreeLibrary` → swap → `GameRegisterSystems` (`GameLibrary.cpp:87-110`); also `Unload` (`:114`) and `~GameLibrary` (`:23`) | Net teardown in the same pre-`FreeLibrary` window: call `Stop()` on `Game.dll`-resident adapters (joins their threads). |
| Subsystem ownership/init | `NavMeshSystem::Instance()` singleton; `Application` owns the 3 threads (`Application.cpp`) | `NetSubsystem` — decide singleton (matches NavMeshSystem) vs. owned-by-Application. See decision below. |

## Key design decisions to make in the plan

### 1. Threading — the engine owns NONE
The adapters own their threads (IOCP workers / client async thread, all inside `netlib`). The engine adds **no IO pump thread**. The engine's job is:
- Provide the `IIoSink` impl. `OnIoEvent` is called concurrently from adapter threads → must be **thread-safe and cheap**: acquire a `NetBufferPool` block, `memcpy` the borrowed payload, build a `NetEvent`, `MpscRing::Enqueue`. Nothing heavy, no locks on the hot path (pool + ring are lock-free).
- Provide the lock-free MPSC ring + lock-free pool.
- GameThread drains the ring (`PollEvent`) and releases pool blocks after use.

The **lock-free MPSC ring** is the Vyukov bounded-MPMC algorithm: fixed array of cells, each with an atomic sequence number; producers CAS-advance an enqueue position and write their cell; the single consumer reads in order. No allocation per op; full → enqueue fails (backpressure, §5). The **lock-free `NetBufferPool`** is a fixed set of blocks on an atomic free-list (Treiber stack with a tagged head to dodge ABA, or a bounded index free-list).

### 2. `NetSubsystem` ownership & thread contract
- **Recommend a singleton** (`NetSubsystem::Instance()`) to match `NavMeshSystem` and keep `NetServicesImpl` forwarding trivial — BUT note the architecture's §9 singleton caution. Fine for Phases 1–3 (one world per process); Phase 4's de-singletoning revisits it. Document so Phase 4 isn't surprised.
- `NetServices` calls (`CreateClient`/`CreateServer`/`Send`/`PollEvent`/`Close`) are invoked from **GameThread only** (matches `NavServices`). `Send` forwards straight to the (thread-safe) adapter; `PollEvent` drains the MPSC ring; `Create`/`Close` mutate the registry.
- **Registry sync:** the io-object registry (add on create, remove on close) is touched by GameThread, while adapter threads call back through the sink. The sink path does **not** touch the registry (it only enqueues to the ring + pool), so the registry needs sync only against the rare create/close — a small lock there is fine (not a hot path). Keep the hot path (sink → pool → ring) lock-free.

### 3. `NetHandle` representation
- Index + salt (like `MeshHandle`/`ModelHandle` are index-based; salt guards use-after-close), or opaque pointer. **Recommend index+salt** for safety + stable ABI across the `NetServices` boundary.

### 4. Model-B teardown placement
Two options (pick one in the plan; option B is simpler to reason about):
- **A:** `GameLibrary` gains `SetOnBeforeUnload(callback)` (like `SetScheduler`), invoked before `m_Scheduler->Clear()` in `LoadOrReload`/`Unload`/`~GameLibrary`. GameThread registers a callback that tells `NetSubsystem` to release Game.dll-resident connections.
- **B:** `GameThread`, when it drains `m_ReloadPending` (`GameThread.cpp:194`), calls `NetSubsystem::Instance().ReleaseGameResidentConnections()` *before* `m_GameLib.LoadOrReload(...)`. No `GameLibrary` change.

Either way `ReleaseGameResidentConnections()` must, for each `Game.dll`-resident adapter:
1. Call the adapter's **`Stop()`** — which joins its worker threads. After it returns, no adapter thread is running, so none can be mid-call in the about-to-be-unmapped `Game.dll` code. **`Stop()` IS the quiesce** (the push/sink model makes this clean — no "is a thread mid-`Poll()`" flag-juggling).
2. Destroy the io-object (dtor lives in `Game.dll`, still mapped here).
3. Drop its registry slot; drain/recycle any of its in-flight pool blocks still on the ring (or let GameThread's next drain release them — decide in plan).
- **Subtle:** the engine sink may be mid-`OnIoEvent` on a worker when `Stop()` is called. `Stop()` must not return until those callbacks have finished (join guarantees it). The sink writes only to the (engine-owned, always-valid) pool + ring, never into `Game.dll`, so an in-flight callback during teardown is safe — it's the *adapter* code that must stop, and join ensures it.
- How does the engine know an adapter is `Game.dll`-resident? **Recommend an explicit `bool gameResident` / residence enum in the create config** — explicit beats magic (`GetModuleHandleEx` sniffing of the factory pointer), and the game knows its own intent.

### 5. Backpressure policy
- **Inbound MPSC ring full** (GameThread not draining fast enough) or **`NetBufferPool` exhausted** (in the sink, on an adapter thread): `SM_WARN` + either drop-newest or stop posting recvs so TCP flow-control naturally back-pressures the peer. Dropping a *reliable*-stream message is wrong for TCP semantics, so prefer **stop-recv** (don't post the next `WSARecv` until the ring drains) over silent drop. **Decide + document**; per `feedback_logging_over_silent_skip`, never silently drop.
- **Outbound:** `Send` returns false when the adapter's send queue is saturated; game retries next tick. No engine-side outbound ring exists.

## Testing strategy

**Logging:** same first-class requirement as Phase 1 — log lifecycle, every dropped/backpressured event (`SM_WARN`), pool/ring exhaustion, and teardown begin/end, so a failing test is diagnosable from the log. Run tests verbose.

**Manual step (user-owned):** the TCP-loopback-through-engine test calls `listen()`, so the **Windows Firewall allow-dialog / UAC** note from Phase 1 applies here too — the plan must include the same "USER ACTION: accept the firewall prompt" gate before that test runs. The pure-`MpscRing`/`NetBufferPool`/in-memory tests need no network permission and run unattended.

- **`MpscRing` unit tests:** concurrent producers (spawn K threads enqueuing M items each) + single consumer drains exactly K·M with no loss/dup; full-ring returns false; ordering-per-producer where the algorithm guarantees it. This is a lock-free structure — test it hard (stress + ThreadSanitizer-style reasoning; consider a high-iteration loop).
- **`NetBufferPool` unit tests:** concurrent acquire/release across threads, balanced (no leak, no double-free), exhaustion returns null cleanly.
- **In-memory loopback test** (no sockets, no second process): a `NetSubsystem` + two in-memory-wired handles; the in-memory adapter delivers synchronously into the engine sink; GameThread `PollEvent` observes the `NetEvent` sequence on the peer. Deterministic.
- **TCP loopback through the engine:** wire a real `netlib` IOCP server + async client (loopback) into `NetSubsystem`; assert the round-trip via `PollEvent` (wait-with-timeout — real adapter threads).
- **Hot-reload teardown:** with a fake `Game.dll`-resident adapter whose `Stop()` joins a worker, drive the teardown path; assert `Stop()` joined + object destroyed + no event fires after; plus a manual editor smoke test for a live `netlib`-resident connection surviving a reload.
- **`NetServices` ABI:** static check the table is append-only (mirror `NavServices`).

## Risks / gotchas

- **Lock-free correctness is the headline risk.** The MPSC ring + pool are the hard parts. Use the textbook Vyukov bounded-MPMC layout exactly; don't improvise memory orderings. Stress-test with many producers. A subtle bug here is a heisenbug under load — worth the extra test time.
- **Sink re-entrancy / lifetime:** `OnIoEvent` fires on adapter threads concurrently; it must only touch the engine-owned, always-valid pool + ring (never `Game.dll`, never the registry). Keep it minimal.
- **`Stop()` must fully join before `FreeLibrary`** for `Game.dll`-resident adapters — the one correctness point that gates safe hot-reload. (Cleaner than the old Poll-quiesce, but still the thing to get right.)
- **Don't regress `NavServices` ABI** while editing `SystemContext` — add `Net` as a new trailing field; don't reorder.
- **`Game.dll` reaches `netlib` for factories** but must NOT link `Engine.dll`. Confirm the link graph: `Game.dll` links `netlib` (to call `MakeTcpClient`); networking *services* come through `NetServices`. Consistent with the architecture.

## Deliverables checklist

- [ ] `src/common/include/MpscRing.h` (lock-free Vyukov bounded MPSC) + unit tests.
- [ ] Lock-free `NetBufferPool` + unit tests.
- [ ] `src/common/include/NetServices.h` (append-only table, GameThread-only contract documented).
- [ ] `NetEvent` + `NetHandle` + create-config (incl. `gameResident` flag) types (common include, shared by engine + game).
- [ ] `NetSubsystem` (registry, engine `IIoSink`, MPSC ring, `NetBufferPool`) under `src/engine/src/network/`. **No IO pump thread.**
- [ ] `NetServicesImpl::Init` forwarding (`PollEvent`=drain ring, `Send`=direct to adapter).
- [ ] `SystemContext.Net` + per-tick wiring in `GameThread::RunLoop`.
- [ ] Model-B teardown hook calling adapter `Stop()` before `FreeLibrary`.
- [ ] CMake: new engine sources in `src/engine/CMakeLists.txt` (explicit list, no glob); `Engine` links `netlib`.
- [ ] Demo `NetSystem` + tests proving the loopback round-trip and teardown.
